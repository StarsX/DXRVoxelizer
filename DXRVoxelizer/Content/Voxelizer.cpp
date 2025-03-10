//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#include "Optional/XUSGObjLoader.h"
#include "Voxelizer.h"

#define GRID_SIZE 64

using namespace std;
using namespace DirectX;
using namespace XUSG;
using namespace XUSG::RayTracing;

const wchar_t* Voxelizer::HitGroupName = L"hitGroup";
const wchar_t* Voxelizer::RaygenShaderName = L"raygenMain";
const wchar_t* Voxelizer::ClosestHitShaderName = L"closestHitMain";
const wchar_t* Voxelizer::MissShaderName = L"missMain";

Voxelizer::Voxelizer() :
	m_instances()
{
	m_shaderLib = ShaderLib::MakeUnique();
}

Voxelizer::~Voxelizer()
{
}

bool Voxelizer::Init(RayTracing::CommandList* pCommandList, const XUSG::DescriptorTableLib::sptr& descriptorTableLib,
	uint32_t width, uint32_t height, Format rtFormat, Format dsFormat, vector<Resource::uptr>& uploaders,
	GeometryBuffer* pGeometry, const char* fileName, const XMFLOAT4& posScale)
{
	const auto pDevice = pCommandList->GetRTDevice();
	m_rayTracingPipelineLib = RayTracing::PipelineLib::MakeUnique(pDevice);
	m_graphicsPipelineLib = Graphics::PipelineLib::MakeUnique(pDevice);
	m_computePipelineLib = Compute::PipelineLib::MakeUnique(pDevice);
	m_pipelineLayoutLib = PipelineLayoutLib::MakeUnique(pDevice);
	m_descriptorTableLib = descriptorTableLib;

	m_viewport.x = static_cast<float>(width);
	m_viewport.y = static_cast<float>(height);
	m_posScale = posScale;

	// Load inputs
	ObjLoader objLoader;
	if (!objLoader.Import(fileName, true, true)) return false;
	XUSG_N_RETURN(createVB(pCommandList, objLoader.GetNumVertices(), objLoader.GetVertexStride(), objLoader.GetVertices(), uploaders), false);
	XUSG_N_RETURN(createIB(pCommandList, objLoader.GetNumIndices(), objLoader.GetIndices(), uploaders), false);

	// Extract boundary
	const auto& aabb = objLoader.GetAABB();
	const XMFLOAT3 ext(aabb.Max.x - aabb.Min.x, aabb.Max.y - aabb.Min.y, aabb.Max.z - aabb.Min.z);
	m_bound.x = (aabb.Max.x + aabb.Min.x) / 2.0f;
	m_bound.y = (aabb.Max.y + aabb.Min.y) / 2.0f;
	m_bound.z = (aabb.Max.z + aabb.Min.z) / 2.0f;
	m_bound.w = (max)(ext.x, (max)(ext.y, ext.z)) / 2.0f;

	XUSG_N_RETURN(createCB(pDevice), false);

	// Create output grids and build acceleration structures
	for (auto& grid : m_grids)
	{
		grid = Texture3D::MakeUnique();
		XUSG_N_RETURN(grid->Create(pDevice, GRID_SIZE, GRID_SIZE, GRID_SIZE, Format::R10G10B10A2_UNORM,
			ResourceFlag::ALLOW_UNORDERED_ACCESS), false);
	}

	//m_depth = DepthStencil::MakeUnique();
	//m_depth.Create(m_device, width, height, Format::D24_UNORM_S8_UINT, ResourceFlag::DENY_SHADER_RESOURCE);

	// Build accelerationStructures and create raytracing pipelines
	XUSG_N_RETURN(buildAccelerationStructures(pCommandList, pGeometry), false);
	XUSG_N_RETURN(createPipelineLayouts(pDevice), false);
	XUSG_N_RETURN(createPipelines(rtFormat, dsFormat), false);

	// Build shader tables
	return buildShaderTables(pDevice);
}

void Voxelizer::UpdateFrame(uint8_t frameIndex, CXMVECTOR eyePt, CXMMATRIX viewProj)
{
	// General matrices
	const auto world = XMMatrixScaling(m_bound.w, m_bound.w, m_bound.w) *
		XMMatrixTranslation(m_bound.x, m_bound.y, m_bound.z) *
		XMMatrixScaling(m_posScale.w, m_posScale.w, m_posScale.w) *
		XMMatrixTranslation(m_posScale.x, m_posScale.y, m_posScale.z);
	const auto worldI = XMMatrixInverse(nullptr, world);
	const auto worldViewProj = world * viewProj;

	// Screen space matrices
	const auto pCbPerObject = reinterpret_cast<CBPerObject*>(m_cbPerObject->Map(frameIndex));
	pCbPerObject->localSpaceLightPt = XMVector3TransformCoord(XMVectorSet(-10.0f, 45.0f, -75.0f, 0.0f), worldI);
	pCbPerObject->localSpaceEyePt = XMVector3TransformCoord(eyePt, worldI);

	const auto mToScreen = XMMATRIX
	(
		0.5f * m_viewport.x, 0.0f, 0.0f, 0.0f,
		0.0f, -0.5f * m_viewport.y, 0.0f, 0.0f,
		0.0f, 0.0f, 1.0f, 0.0f,
		0.5f * m_viewport.x, 0.5f * m_viewport.y, 0.0f, 1.0f
	);
	const auto localToScreen = XMMatrixMultiply(worldViewProj, mToScreen);
	const auto screenToLocal = XMMatrixInverse(nullptr, localToScreen);
	pCbPerObject->screenToLocal = XMMatrixTranspose(screenToLocal);
}

void Voxelizer::Render(RayTracing::CommandList* pCommandList, uint8_t frameIndex,
	const Descriptor& rtv, const Descriptor& dsv)
{
	voxelize(pCommandList, frameIndex);
	renderRayCast(pCommandList, frameIndex, rtv, dsv);
}

bool Voxelizer::createVB(RayTracing::CommandList* pCommandList, uint32_t numVert,
	uint32_t stride, const uint8_t* pData, vector<Resource::uptr>& uploaders)
{
	m_vertexBuffer = VertexBuffer::MakeUnique();
	XUSG_N_RETURN(m_vertexBuffer->Create(pCommandList->GetDevice(), numVert, stride,
		ResourceFlag::NONE, MemoryType::DEFAULT), false);
	uploaders.emplace_back(Resource::MakeUnique());

	return m_vertexBuffer->Upload(pCommandList, uploaders.back().get(), pData,
		stride * numVert, 0, ResourceState::NON_PIXEL_SHADER_RESOURCE);
}

bool Voxelizer::createIB(RayTracing::CommandList* pCommandList, uint32_t numIndices,
	const uint32_t* pData, vector<Resource::uptr>& uploaders)
{
	const uint32_t byteWidth = sizeof(uint32_t) * numIndices;
	m_indexBuffer = IndexBuffer::MakeUnique();
	XUSG_N_RETURN(m_indexBuffer->Create(pCommandList->GetDevice(), byteWidth, Format::R32_UINT,
		ResourceFlag::NONE, MemoryType::DEFAULT), false);
	uploaders.emplace_back(Resource::MakeUnique());

	return m_indexBuffer->Upload(pCommandList, uploaders.back().get(), pData,
		byteWidth, 0, ResourceState::NON_PIXEL_SHADER_RESOURCE);
}

bool Voxelizer::createCB(const XUSG::Device* pDevice)
{
	m_cbPerObject = ConstantBuffer::MakeUnique();

	return m_cbPerObject->Create(pDevice, sizeof(CBPerObject) * FrameCount, FrameCount);
}

bool Voxelizer::createPipelineLayouts(const RayTracing::Device* pDevice)
{
	// Global pipeline layout
	// This is a pipeline layout that is shared across all raytracing shaders invoked during a DispatchRays() call.
	{
		const auto pipelineLayout = RayTracing::PipelineLayout::MakeUnique();
		pipelineLayout->SetRange(OUTPUT_GRID, DescriptorType::UAV, 1, 0);
		pipelineLayout->SetRange(INDEX_BUFFERS, DescriptorType::SRV, 1, 0, 0);
		pipelineLayout->SetRange(VERTEX_BUFFERS, DescriptorType::SRV, 1, 0, 1);
		pipelineLayout->SetRootSRV(ACCELERATION_STRUCTURE, 0, 2, DescriptorFlag::DATA_STATIC);
		XUSG_X_RETURN(m_pipelineLayouts[GLOBAL_LAYOUT], pipelineLayout->GetPipelineLayout(
			pDevice, m_pipelineLayoutLib.get(), PipelineLayoutFlag::NONE,
			L"RayTracerGlobalPipelineLayout"), false);
	}

	{
		// Get pipeline layout
		const auto pipelineLayout = Util::PipelineLayout::MakeUnique();
		pipelineLayout->SetRange(0, DescriptorType::CBV, 1, 0, 0, DescriptorFlag::DATA_STATIC);
		pipelineLayout->SetRange(1, DescriptorType::SRV, 1, 0);
		pipelineLayout->SetRange(2, DescriptorType::SAMPLER, 1, 0);
		pipelineLayout->SetShaderStage(0, Shader::Stage::PS);
		pipelineLayout->SetShaderStage(1, Shader::Stage::PS);
		pipelineLayout->SetShaderStage(2, Shader::Stage::PS);
		XUSG_X_RETURN(m_pipelineLayouts[RAY_CAST_LAYOUT], pipelineLayout->GetPipelineLayout(
			m_pipelineLayoutLib.get(), PipelineLayoutFlag::NONE, L"RayCastLayout"), false);
	}

	return true;
}

bool Voxelizer::createPipelines(Format rtFormat, Format dsFormat)
{
	{
		XUSG_N_RETURN(m_shaderLib->CreateShader(Shader::Stage::CS, 0, L"DXRVoxelizer.cso"), false);
		const wchar_t* shaderNames[] = { RaygenShaderName, ClosestHitShaderName, MissShaderName };

		const auto state = RayTracing::State::MakeUnique();
		state->SetShaderLibrary(0, m_shaderLib->GetShader(Shader::Stage::CS, 0),
			static_cast<uint32_t>(size(shaderNames)), shaderNames);
		state->SetHitGroup(0, HitGroupName, ClosestHitShaderName);
		state->SetShaderConfig(sizeof(XMFLOAT4), sizeof(XMFLOAT2));
		//state->SetLocalPipelineLayout(0, m_pipelineLayouts[RAY_GEN_LAYOUT], 1, &RaygenShaderName);
		state->SetGlobalPipelineLayout(m_pipelineLayouts[GLOBAL_LAYOUT]);
		state->SetMaxRecursionDepth(1);
		XUSG_X_RETURN(m_pipelines[RAY_TRACING], state->GetPipeline(m_rayTracingPipelineLib.get(), L"Raytracing"), false);
	}

	{
		XUSG_N_RETURN(m_shaderLib->CreateShader(Shader::Stage::VS, VS_SCREEN_QUAD, L"VSScreenQuad.cso"), false);
		XUSG_N_RETURN(m_shaderLib->CreateShader(Shader::Stage::PS, PS_RAY_CAST, L"PSRayCast.cso"), false);

		const auto state = Graphics::State::MakeUnique();
		state->SetPipelineLayout(m_pipelineLayouts[RAY_CAST_LAYOUT]);
		state->SetShader(Shader::Stage::VS, m_shaderLib->GetShader(Shader::Stage::VS, VS_SCREEN_QUAD));
		state->SetShader(Shader::Stage::PS, m_shaderLib->GetShader(Shader::Stage::PS, PS_RAY_CAST));
		state->IASetPrimitiveTopologyType(PrimitiveTopologyType::TRIANGLE);
		state->DSSetState(Graphics::DepthStencilPreset::DEPTH_STENCIL_NONE, m_graphicsPipelineLib.get());
		state->OMSetRTVFormats(&rtFormat, 1);
		XUSG_X_RETURN(m_pipelines[RAY_CAST], state->GetPipeline(m_graphicsPipelineLib.get(), L"RayCast"), false);
	}

	return true;
}

bool Voxelizer::createDescriptorTables()
{
	for (uint8_t i = 0; i < FrameCount; ++i)
	{
		// Output UAV
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		descriptorTable->SetDescriptors(0, 1, &m_grids[i]->GetUAV());
		XUSG_X_RETURN(m_uavTables[i], descriptorTable->GetCbvSrvUavTable(m_descriptorTableLib.get()), false);
	}

	// Index buffer SRV
	{
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		descriptorTable->SetDescriptors(0, 1, &m_indexBuffer->GetSRV());
		XUSG_X_RETURN(m_srvTables[SRV_TABLE_IB], descriptorTable->GetCbvSrvUavTable(m_descriptorTableLib.get()), false);
	}

	// Vertex buffer SRV
	{
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		descriptorTable->SetDescriptors(0, 1, &m_vertexBuffer->GetSRV());
		XUSG_X_RETURN(m_srvTables[SRV_TABLE_VB], descriptorTable->GetCbvSrvUavTable(m_descriptorTableLib.get()), false);
	}

	// Ray cast
	for (uint8_t i = 0; i < FrameCount; ++i)
	{
		// Get CBV
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		descriptorTable->SetDescriptors(0, 1, &m_cbPerObject->GetCBV(i));
		XUSG_X_RETURN(m_cbvTables[i], descriptorTable->GetCbvSrvUavTable(m_descriptorTableLib.get()), false);

		// Get SRV
		if (!m_srvTables[SRV_TABLE_GRID + i])
		{
			const auto descriptorTable = Util::DescriptorTable::MakeUnique();
			descriptorTable->SetDescriptors(0, 1, &m_grids[i]->GetSRV());
			XUSG_X_RETURN(m_srvTables[SRV_TABLE_GRID + i], descriptorTable->GetCbvSrvUavTable(m_descriptorTableLib.get()), false);
		}
	}

	// Create the sampler table
	{
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		const auto sampler = LINEAR_CLAMP;
		descriptorTable->SetSamplers(0, 1, &sampler, m_descriptorTableLib.get());
		XUSG_X_RETURN(m_samplerTable, descriptorTable->GetSamplerTable(m_descriptorTableLib.get()), false);
	}

	return true;
}

bool Voxelizer::buildAccelerationStructures(RayTracing::CommandList* pCommandList, GeometryBuffer* pGeometry)
{
	const auto pDevice = pCommandList->GetRTDevice();

	// Set geometries
	BottomLevelAS::SetTriangleGeometries(*pGeometry, 1, Format::R32G32B32_FLOAT,
		&m_vertexBuffer->GetVBV(), &m_indexBuffer->GetIBV());

	// Prebuild
	m_bottomLevelAS = BottomLevelAS::MakeUnique();
	m_topLevelAS = TopLevelAS::MakeUnique();
	XUSG_N_RETURN(m_bottomLevelAS->Prebuild(pDevice, 1, *pGeometry), false);
	XUSG_N_RETURN(m_topLevelAS->Prebuild(pDevice, 1), false);

	// Calculate buffer size and offsets
	uintptr_t dstBufferFirstElements[2] = {};
	uintptr_t dstBufferOffsets[2] = {};
	size_t dstBufferSize = m_topLevelAS->GetResultDataMaxByteSize();

	dstBufferFirstElements[1] = dstBufferSize / sizeof(uint32_t);
	dstBufferOffsets[1] = dstBufferSize;
	dstBufferSize += m_bottomLevelAS->GetResultDataMaxByteSize();

	// Create AS buffer storage
	Buffer::sptr dstBuffer = Buffer::MakeShared();
	XUSG_N_RETURN(AccelerationStructure::AllocateDestBuffer(pDevice, dstBuffer.get(),
		dstBufferSize, 1, nullptr, static_cast<uint32_t>(size(dstBufferFirstElements)),
		dstBufferFirstElements), false);

	// Set dest buffer for top-level AS and bottom-level ASes
	m_topLevelAS->SetDestination(pDevice, dstBuffer, 0, 0, 0, m_descriptorTableLib.get());
	m_bottomLevelAS->SetDestination(pDevice, dstBuffer, dstBufferOffsets[1], 1, m_descriptorTableLib.get());

	// Create scratch buffer
	auto scratchSize = m_topLevelAS->GetScratchDataByteSize();
	scratchSize = (max)(m_bottomLevelAS->GetScratchDataByteSize(), scratchSize);
	m_scratch = Buffer::MakeUnique();
	XUSG_N_RETURN(AccelerationStructure::AllocateUAVBuffer(pDevice, m_scratch.get(), scratchSize), false);

	// Set instance
	XMFLOAT3X4 matrix;
	const auto normalizedToLocal = XMMatrixScaling(m_bound.w, m_bound.w, m_bound.w) * XMMatrixTranslation(m_bound.x, m_bound.y, m_bound.z);
	XMStoreFloat3x4(&matrix, XMMatrixInverse(nullptr, normalizedToLocal));
	const float* const pTransform[] = { reinterpret_cast<const float*>(&matrix) };
	m_instances = Buffer::MakeUnique();
	const BottomLevelAS* ppBottomLevelAS[] = { m_bottomLevelAS.get() };
	TopLevelAS::SetInstances(pDevice, m_instances.get(), 1, ppBottomLevelAS, pTransform);

	// Build bottom level AS
	m_bottomLevelAS->Build(pCommandList, m_scratch.get());

	const ResourceBarrier barrier = { nullptr, ResourceState::UNORDERED_ACCESS };
	pCommandList->Barrier(1, &barrier);

	// Create descriptor tables
	XUSG_N_RETURN(createDescriptorTables(), false);

	// Build top level AS
	m_topLevelAS->Build(pCommandList, m_scratch.get(), m_instances.get(),
		m_descriptorTableLib->GetDescriptorHeap(CBV_SRV_UAV_HEAP));

	return true;
}

bool Voxelizer::buildShaderTables(const RayTracing::Device* pDevice)
{
	// Get shader identifiers.
	const auto shaderIDSize = ShaderRecord::GetShaderIdentifierSize(pDevice);

	// Ray gen shader table
	m_rayGenShaderTable = ShaderTable::MakeUnique();
	XUSG_N_RETURN(m_rayGenShaderTable->Create(pDevice, 1, shaderIDSize, MemoryFlag::NONE, L"RayGenShaderTable"), false);
	m_rayGenShaderTable->AddShaderRecord(ShaderRecord::MakeUnique(pDevice, m_pipelines[RAY_TRACING], RaygenShaderName).get());

	// Hit group shader table
	m_hitGroupShaderTable = ShaderTable::MakeUnique();
	XUSG_N_RETURN(m_hitGroupShaderTable->Create(pDevice, 1, shaderIDSize, MemoryFlag::NONE, L"HitGroupShaderTable"), false);
	m_hitGroupShaderTable->AddShaderRecord(ShaderRecord::MakeUnique(pDevice, m_pipelines[RAY_TRACING], HitGroupName).get());

	// Miss shader table
	m_missShaderTable = ShaderTable::MakeUnique();
	XUSG_N_RETURN(m_missShaderTable->Create(pDevice, 1, shaderIDSize, MemoryFlag::NONE, L"MissShaderTable"), false);
	m_missShaderTable->AddShaderRecord(ShaderRecord::MakeUnique(pDevice, m_pipelines[RAY_TRACING], MissShaderName).get());

	return true;
}

void Voxelizer::voxelize(RayTracing::CommandList* pCommandList, uint8_t frameIndex)
{
	// Set resource barrier
	ResourceBarrier barrier;
	const auto numBarriers = m_grids[frameIndex]->SetBarrier(&barrier, ResourceState::UNORDERED_ACCESS);
	pCommandList->Barrier(numBarriers, &barrier);

	// Set descriptor tables
	pCommandList->SetComputePipelineLayout(m_pipelineLayouts[GLOBAL_LAYOUT]);
	pCommandList->SetComputeDescriptorTable(OUTPUT_GRID, m_uavTables[frameIndex]);
	pCommandList->SetTopLevelAccelerationStructure(ACCELERATION_STRUCTURE, m_topLevelAS.get());
	pCommandList->SetComputeDescriptorTable(INDEX_BUFFERS, m_srvTables[SRV_TABLE_IB]);
	pCommandList->SetComputeDescriptorTable(VERTEX_BUFFERS, m_srvTables[SRV_TABLE_VB]);

	// Fallback layer has no depth
	pCommandList->SetRayTracingPipeline(m_pipelines[RAY_TRACING]);
	pCommandList->DispatchRays(GRID_SIZE, GRID_SIZE * GRID_SIZE, 1,
		m_rayGenShaderTable.get(), m_hitGroupShaderTable.get(), m_missShaderTable.get());
}

void Voxelizer::renderRayCast(RayTracing::CommandList* pCommandList, uint8_t frameIndex,
	const Descriptor& rtv, const Descriptor& dsv)
{
	// Set resource barrier
	ResourceBarrier barrier;
	const auto numBarriers = m_grids[frameIndex]->SetBarrier(&barrier, ResourceState::PIXEL_SHADER_RESOURCE);
	pCommandList->Barrier(numBarriers, &barrier);

	// Set descriptor tables
	pCommandList->SetGraphicsPipelineLayout(m_pipelineLayouts[RAY_CAST_LAYOUT]);
	pCommandList->SetGraphicsDescriptorTable(0, m_cbvTables[frameIndex]);
	pCommandList->SetGraphicsDescriptorTable(1, m_srvTables[SRV_TABLE_GRID + frameIndex]);
	pCommandList->SetGraphicsDescriptorTable(2, m_samplerTable);

	// Set pipeline state
	pCommandList->SetPipelineState(m_pipelines[RAY_CAST]);

	// Set viewport
	Viewport viewport(0.0f, 0.0f, m_viewport.x, m_viewport.y);
	RectRange scissorRect(0, 0, static_cast<long>(m_viewport.x), static_cast<long>(m_viewport.y));
	pCommandList->RSSetViewports(1, &viewport);
	pCommandList->RSSetScissorRects(1, &scissorRect);

	pCommandList->OMSetRenderTargets(1, &rtv);

	// Record commands.
	pCommandList->IASetPrimitiveTopology(PrimitiveTopology::TRIANGLESTRIP);
	pCommandList->Draw(3, 1, 0, 0);
}
