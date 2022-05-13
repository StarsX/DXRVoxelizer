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
	m_shaderPool = ShaderPool::MakeUnique();
}

Voxelizer::~Voxelizer()
{
}

bool Voxelizer::Init(RayTracing::CommandList* pCommandList, uint32_t width, uint32_t height,
	Format rtFormat, Format dsFormat, vector<Resource::uptr>& uploaders, GeometryBuffer* pGeometry,
	const char* fileName, const XMFLOAT4& posScale)
{
	const auto pDevice = pCommandList->GetRTDevice();
	m_rayTracingPipelineCache = RayTracing::PipelineCache::MakeUnique(pDevice);
	m_graphicsPipelineCache = Graphics::PipelineCache::MakeUnique(pDevice);
	m_computePipelineCache = Compute::PipelineCache::MakeUnique(pDevice);
	m_pipelineLayoutCache = PipelineLayoutCache::MakeUnique(pDevice);
	m_descriptorTableCache = DescriptorTableCache::MakeUnique(pDevice, L"RayTracerDescriptorTableCache");

	m_viewport.x = static_cast<float>(width);
	m_viewport.y = static_cast<float>(height);
	m_posScale = posScale;

	// Load inputs
	ObjLoader objLoader;
	if (!objLoader.Import(fileName, true, true)) return false;
	XUSG_N_RETURN(createVB(pCommandList, objLoader.GetNumVertices(), objLoader.GetVertexStride(), objLoader.GetVertices(), uploaders), false);
	XUSG_N_RETURN(createIB(pCommandList, objLoader.GetNumIndices(), objLoader.GetIndices(), uploaders), false);

	// Create raytracing pipelines
	XUSG_N_RETURN(createPipelineLayouts(pDevice), false);
	XUSG_N_RETURN(createPipelines(rtFormat, dsFormat), false);

	// Extract boundary
	const auto center = objLoader.GetCenter();
	m_bound = XMFLOAT4(center.x, center.y, center.z, objLoader.GetRadius());

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
	XUSG_N_RETURN(buildAccelerationStructures(pCommandList, pGeometry), false);
	XUSG_N_RETURN(buildShaderTables(pDevice), false);

	return true;
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
	const DescriptorPool descriptorPools[] =
	{
		m_descriptorTableCache->GetDescriptorPool(CBV_SRV_UAV_POOL),
		m_descriptorTableCache->GetDescriptorPool(SAMPLER_POOL)
	};
	pCommandList->SetDescriptorPools(static_cast<uint32_t>(size(descriptorPools)), descriptorPools);

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
			pDevice, m_pipelineLayoutCache.get(), PipelineLayoutFlag::NONE,
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
			m_pipelineLayoutCache.get(), PipelineLayoutFlag::NONE, L"RayCastLayout"), false);
	}

	return true;
}

bool Voxelizer::createPipelines(Format rtFormat, Format dsFormat)
{
	{
		XUSG_N_RETURN(m_shaderPool->CreateShader(Shader::Stage::CS, 0, L"DXRVoxelizer.cso"), false);

		const auto state = RayTracing::State::MakeUnique();
		state->SetShaderLibrary(m_shaderPool->GetShader(Shader::Stage::CS, 0));
		state->SetHitGroup(0, HitGroupName, ClosestHitShaderName);
		state->SetShaderConfig(sizeof(XMFLOAT4), sizeof(XMFLOAT2));
		//state->SetLocalPipelineLayout(0, m_pipelineLayouts[RAY_GEN_LAYOUT],
			//1, reinterpret_cast<const void**>(&RaygenShaderName));
		state->SetGlobalPipelineLayout(m_pipelineLayouts[GLOBAL_LAYOUT]);
		state->SetMaxRecursionDepth(1);
		XUSG_X_RETURN(m_pipelines[RAY_TRACING], state->GetPipeline(m_rayTracingPipelineCache.get(), L"Raytracing"), false);
	}

	{
		XUSG_N_RETURN(m_shaderPool->CreateShader(Shader::Stage::VS, VS_SCREEN_QUAD, L"VSScreenQuad.cso"), false);
		XUSG_N_RETURN(m_shaderPool->CreateShader(Shader::Stage::PS, PS_RAY_CAST, L"PSRayCast.cso"), false);

		const auto state = Graphics::State::MakeUnique();
		state->SetPipelineLayout(m_pipelineLayouts[RAY_CAST_LAYOUT]);
		state->SetShader(Shader::Stage::VS, m_shaderPool->GetShader(Shader::Stage::VS, VS_SCREEN_QUAD));
		state->SetShader(Shader::Stage::PS, m_shaderPool->GetShader(Shader::Stage::PS, PS_RAY_CAST));
		state->IASetPrimitiveTopologyType(PrimitiveTopologyType::TRIANGLE);
		state->DSSetState(Graphics::DepthStencilPreset::DEPTH_STENCIL_NONE, m_graphicsPipelineCache.get());
		state->OMSetRTVFormats(&rtFormat, 1);
		XUSG_X_RETURN(m_pipelines[RAY_CAST], state->GetPipeline(m_graphicsPipelineCache.get(), L"RayCast"), false);
	}

	return true;
}

bool Voxelizer::createDescriptorTables()
{
	// Acceleration structure UAVs
	{
		Descriptor descriptors[] = { m_bottomLevelAS->GetResult()->GetUAV(), m_topLevelAS->GetResult()->GetUAV() };
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		descriptorTable->SetDescriptors(0, static_cast<uint32_t>(size(descriptors)), descriptors);
		const auto asTable = descriptorTable->GetCbvSrvUavTable(m_descriptorTableCache.get());
		XUSG_N_RETURN(asTable, false);
	}

	for (uint8_t i = 0; i < FrameCount; ++i)
	{
		// Output UAV
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		descriptorTable->SetDescriptors(0, 1, &m_grids[i]->GetUAV());
		XUSG_X_RETURN(m_uavTables[i], descriptorTable->GetCbvSrvUavTable(m_descriptorTableCache.get()), false);
	}

	// Index buffer SRV
	{
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		descriptorTable->SetDescriptors(0, 1, &m_indexBuffer->GetSRV());
		XUSG_X_RETURN(m_srvTables[SRV_TABLE_IB], descriptorTable->GetCbvSrvUavTable(m_descriptorTableCache.get()), false);
	}

	// Vertex buffer SRV
	{
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		descriptorTable->SetDescriptors(0, 1, &m_vertexBuffer->GetSRV());
		XUSG_X_RETURN(m_srvTables[SRV_TABLE_VB], descriptorTable->GetCbvSrvUavTable(m_descriptorTableCache.get()), false);
	}

	// Ray cast
	for (uint8_t i = 0; i < FrameCount; ++i)
	{
		// Get CBV
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		descriptorTable->SetDescriptors(0, 1, &m_cbPerObject->GetCBV(i));
		XUSG_X_RETURN(m_cbvTables[i], descriptorTable->GetCbvSrvUavTable(m_descriptorTableCache.get()), false);

		// Get SRV
		if (!m_srvTables[SRV_TABLE_GRID + i])
		{
			const auto descriptorTable = Util::DescriptorTable::MakeUnique();
			descriptorTable->SetDescriptors(0, 1, &m_grids[i]->GetSRV());
			XUSG_X_RETURN(m_srvTables[SRV_TABLE_GRID + i], descriptorTable->GetCbvSrvUavTable(m_descriptorTableCache.get()), false);
		}
	}

	// Create the sampler table
	{
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		const auto sampler = LINEAR_CLAMP;
		descriptorTable->SetSamplers(0, 1, &sampler, m_descriptorTableCache.get());
		XUSG_X_RETURN(m_samplerTable, descriptorTable->GetSamplerTable(m_descriptorTableCache.get()), false);
	}

	return true;
}

bool Voxelizer::buildAccelerationStructures(RayTracing::CommandList* pCommandList, GeometryBuffer* pGeometry)
{
	AccelerationStructure::SetFrameCount(FrameCount);
	const auto pDevice = pCommandList->GetRTDevice();

	// Set geometries
	BottomLevelAS::SetTriangleGeometries(*pGeometry, 1, Format::R32G32B32_FLOAT,
		&m_vertexBuffer->GetVBV(), &m_indexBuffer->GetIBV());

	// Descriptor index in descriptor pool
	const auto bottomLevelASIndex = 0u;
	const auto topLevelASIndex = bottomLevelASIndex + 1;

	// Prebuild
	m_bottomLevelAS = BottomLevelAS::MakeUnique();
	m_topLevelAS = TopLevelAS::MakeUnique();
	XUSG_N_RETURN(m_bottomLevelAS->PreBuild(pDevice, 1, *pGeometry, bottomLevelASIndex), false);
	XUSG_N_RETURN(m_topLevelAS->PreBuild(pDevice, 1, topLevelASIndex), false);

	// Create scratch buffer
	auto scratchSize = m_topLevelAS->GetScratchDataMaxSize();
	scratchSize = (max)(m_bottomLevelAS->GetScratchDataMaxSize(), scratchSize);
	m_scratch = Resource::MakeUnique();
	XUSG_N_RETURN(AccelerationStructure::AllocateUAVBuffer(pDevice, m_scratch.get(), scratchSize), false);

	// Get descriptor pool and create descriptor tables
	XUSG_N_RETURN(createDescriptorTables(), false);
	const auto& descriptorPool = m_descriptorTableCache->GetDescriptorPool(CBV_SRV_UAV_POOL);

	// Set instance
	XMFLOAT3X4 matrix;
	const auto normalizedToLocal = XMMatrixScaling(m_bound.w, m_bound.w, m_bound.w) * XMMatrixTranslation(m_bound.x, m_bound.y, m_bound.z);
	XMStoreFloat3x4(&matrix, XMMatrixInverse(nullptr, normalizedToLocal));
	float* const pTransform[] = { reinterpret_cast<float*>(&matrix) };
	m_instances = Resource::MakeUnique();
	const BottomLevelAS* ppBottomLevelAS[] = { m_bottomLevelAS.get() };
	TopLevelAS::SetInstances(pDevice, m_instances.get(), 1, ppBottomLevelAS, pTransform);

	// Build bottom level ASs
	m_bottomLevelAS->Build(pCommandList, m_scratch.get(), descriptorPool);

	// Build top level AS
	m_topLevelAS->Build(pCommandList, m_scratch.get(), m_instances.get(), descriptorPool);

	return true;
}

bool Voxelizer::buildShaderTables(const RayTracing::Device* pDevice)
{
	// Get shader identifiers.
	const auto shaderIDSize = ShaderRecord::GetShaderIDSize(pDevice);

	// Ray gen shader table
	m_rayGenShaderTable = ShaderTable::MakeUnique();
	XUSG_N_RETURN(m_rayGenShaderTable->Create(pDevice, 1, shaderIDSize, L"RayGenShaderTable"), false);
	XUSG_N_RETURN(m_rayGenShaderTable->AddShaderRecord(ShaderRecord::MakeUnique(pDevice, m_pipelines[RAY_TRACING], RaygenShaderName).get()), false);

	// Hit group shader table
	m_hitGroupShaderTable = ShaderTable::MakeUnique();
	XUSG_N_RETURN(m_hitGroupShaderTable->Create(pDevice, 1, shaderIDSize, L"HitGroupShaderTable"), false);
	XUSG_N_RETURN(m_hitGroupShaderTable->AddShaderRecord(ShaderRecord::MakeUnique(pDevice, m_pipelines[RAY_TRACING], HitGroupName).get()), false);

	// Miss shader table
	m_missShaderTable = ShaderTable::MakeUnique();
	XUSG_N_RETURN(m_missShaderTable->Create(pDevice, 1, shaderIDSize, L"MissShaderTable"), false);
	XUSG_N_RETURN(m_missShaderTable->AddShaderRecord(ShaderRecord::MakeUnique(pDevice, m_pipelines[RAY_TRACING], MissShaderName).get()), false);

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

	// Set pipline
	pCommandList->SetRayTracingPipeline(m_pipelines[RAY_TRACING]);

	// Fallback layer has no depth
	pCommandList->DispatchRays(GRID_SIZE, GRID_SIZE * GRID_SIZE, 1, m_hitGroupShaderTable.get(),
		m_missShaderTable.get(), m_rayGenShaderTable.get());
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
