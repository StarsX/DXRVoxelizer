//--------------------------------------------------------------------------------------
// By Stars XU Tianchen
//--------------------------------------------------------------------------------------

#include "DXFrameworkHelper.h"
#include "ObjLoader.h"
#include "Voxelizer.h"

#define GRID_SIZE			64

using namespace std;
using namespace DirectX;
using namespace XUSG;
using namespace XUSG::RayTracing;

const wchar_t* Voxelizer::HitGroupName = L"hitGroup";
const wchar_t* Voxelizer::RaygenShaderName = L"raygenMain";
const wchar_t* Voxelizer::ClosestHitShaderName = L"closestHitMain";
const wchar_t* Voxelizer::MissShaderName = L"missMain";

Voxelizer::Voxelizer(const RayTracing::Device& device) :
	m_device(device),
	m_instances()
{
	m_rayTracingPipelineCache.SetDevice(device);
	m_graphicsPipelineCache.SetDevice(device.Common);
	m_computePipelineCache.SetDevice(device.Common);
	m_descriptorTableCache.SetDevice(device.Common);
	m_pipelineLayoutCache.SetDevice(device.Common);

	m_descriptorTableCache.SetName(L"RayTracerDescriptorTableCache");
}

Voxelizer::~Voxelizer()
{
}

bool Voxelizer::Init(const RayTracing::CommandList& commandList, uint32_t width, uint32_t height,
	Format rtFormat, Format dsFormat, vector<Resource>& uploaders, Geometry& geometry, const char* fileName)
{
	m_viewport.x = static_cast<float>(width);
	m_viewport.y = static_cast<float>(height);

	// Load inputs
	ObjLoader objLoader;
	if (!objLoader.Import(fileName, true, true)) return false;
	N_RETURN(createVB(commandList, objLoader.GetNumVertices(), objLoader.GetVertexStride(), objLoader.GetVertices(), uploaders), false);
	N_RETURN(createIB(commandList, objLoader.GetNumIndices(), objLoader.GetIndices(), uploaders), false);

	// Create raytracing pipelines
	N_RETURN(createPipelineLayouts(), false);
	N_RETURN(createPipelines(rtFormat, dsFormat), false);

	// Extract boundary
	const auto center = objLoader.GetCenter();
	m_bound = XMFLOAT4(center.x, center.y, center.z, objLoader.GetRadius());

	N_RETURN(createCB(), false);

	// Create output grids and build acceleration structures
	for (auto& grid : m_grids)
		N_RETURN(grid.Create(m_device.Common, GRID_SIZE, GRID_SIZE, GRID_SIZE, DXGI_FORMAT_R10G10B10A2_UNORM,
			D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS), false);
	//m_depth.Create(m_device.Common, width, height, DXGI_FORMAT_D24_UNORM_S8_UINT, D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE);
	N_RETURN(buildAccelerationStructures(commandList, &geometry), false);
	N_RETURN(buildShaderTables(), false);

	return true;
}

void Voxelizer::UpdateFrame(uint32_t frameIndex, CXMVECTOR eyePt, CXMMATRIX viewProj)
{
	// General matrices
	const auto world = XMMatrixScaling(m_bound.w, m_bound.w, m_bound.w) *
		XMMatrixTranslation(m_bound.x, m_bound.y, m_bound.z);
	const auto worldI = XMMatrixInverse(nullptr, world);
	const auto worldViewProj = world * viewProj;

	// Screen space matrices
	const auto pCbPerObject = reinterpret_cast<CBPerObject*>(m_cbPerObject.Map(frameIndex));
	pCbPerObject->localSpaceLightPt = XMVector3TransformCoord(XMVectorSet(10.0f, 45.0f, 75.0f, 0.0f), worldI);
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

void Voxelizer::Render(const RayTracing::CommandList& commandList, uint32_t frameIndex,
	const RenderTargetTable& rtvs, const Descriptor& dsv)
{
	const DescriptorPool descriptorPools[] =
	{
		m_descriptorTableCache.GetDescriptorPool(CBV_SRV_UAV_POOL),
		m_descriptorTableCache.GetDescriptorPool(SAMPLER_POOL)
	};
	commandList.SetDescriptorPools(static_cast<uint32_t>(size(descriptorPools)), descriptorPools);

	voxelize(commandList, frameIndex);
	renderRayCast(commandList, frameIndex, rtvs, dsv);
}

bool Voxelizer::createVB(const RayTracing::CommandList& commandList, uint32_t numVert,
	uint32_t stride, const uint8_t* pData, vector<Resource>& uploaders)
{
	N_RETURN(m_vertexBuffer.Create(m_device.Common, numVert, stride, D3D12_RESOURCE_FLAG_NONE,
		D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST), false);
	uploaders.push_back(nullptr);

	return m_vertexBuffer.Upload(commandList, uploaders.back(), pData, stride * numVert,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
}

bool Voxelizer::createIB(const RayTracing::CommandList& commandList, uint32_t numIndices,
	const uint32_t* pData, vector<Resource>& uploaders)
{
	const uint32_t byteWidth = sizeof(uint32_t) * numIndices;
	N_RETURN(m_indexBuffer.Create(m_device.Common, byteWidth, DXGI_FORMAT_R32_UINT,
		D3D12_RESOURCE_FLAG_NONE, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST), false);
	uploaders.push_back(nullptr);

	return m_indexBuffer.Upload(commandList, uploaders.back(), pData, byteWidth,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
}

bool Voxelizer::createCB()
{
	return m_cbPerObject.Create(m_device.Common, sizeof(CBPerObject) * FrameCount, FrameCount);
}

bool Voxelizer::createPipelineLayouts()
{
	// Global pipeline layout
	// This is a pipeline layout that is shared across all raytracing shaders invoked during a DispatchRays() call.
	{
		RayTracing::PipelineLayout pipelineLayout;
		pipelineLayout.SetRange(OUTPUT_GRID, DescriptorType::UAV, 1, 0);
		pipelineLayout.SetRootSRV(ACCELERATION_STRUCTURE, 0);
		pipelineLayout.SetRange(INDEX_BUFFERS, DescriptorType::SRV, 1, 0, 1);
		pipelineLayout.SetRange(VERTEX_BUFFERS, DescriptorType::SRV, 1, 0, 2);
		X_RETURN(m_pipelineLayouts[GLOBAL_LAYOUT], pipelineLayout.GetPipelineLayout(m_device, m_pipelineLayoutCache,
			D3D12_ROOT_SIGNATURE_FLAG_NONE, NumUAVs, L"RayTracerGlobalPipelineLayout"), false);
	}

	// Local pipeline layout for RayGen shader
	// This is a pipeline layout that enables a shader to have unique arguments that come from shader tables.
#if 0
	{
		RayTracing::PipelineLayout pipelineLayout;
		pipelineLayout.SetConstants(0, SizeOfInUint32(XMFLOAT4), 0);
		X_RETURN(m_pipelineLayouts[RAY_GEN_LAYOUT], pipelineLayout.GetPipelineLayout(m_device, m_pipelineLayoutCache,
			D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE, NumUAVs, L"RayTracerRayGenPipelineLayout"), false);
	}
#endif

	{
		// Get pipeline layout
		Util::PipelineLayout pipelineLayout;
		pipelineLayout.SetRange(0, DescriptorType::CBV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC);
		pipelineLayout.SetRange(1, DescriptorType::SRV, 1, 0);
		pipelineLayout.SetRange(2, DescriptorType::SAMPLER, 1, 0);
		pipelineLayout.SetShaderStage(0, Shader::Stage::PS);
		pipelineLayout.SetShaderStage(1, Shader::Stage::PS);
		pipelineLayout.SetShaderStage(2, Shader::Stage::PS);
		X_RETURN(m_pipelineLayouts[RAY_CAST_LAYOUT], pipelineLayout.GetPipelineLayout(m_pipelineLayoutCache,
			D3D12_ROOT_SIGNATURE_FLAG_NONE, L"RayCastLayout"), false);
	}

	return true;
}

bool Voxelizer::createPipelines(Format rtFormat, Format dsFormat)
{
	{
		Blob shaderLib;
		V_RETURN(D3DReadFileToBlob(L"DXRVoxelizer.cso", &shaderLib), cerr, false);

		RayTracing::State state;
		state.SetShaderLibrary(shaderLib);
		state.SetHitGroup(0, HitGroupName, ClosestHitShaderName);
		state.SetShaderConfig(sizeof(XMFLOAT4), sizeof(XMFLOAT2));
		//state.SetLocalPipelineLayout(0, m_pipelineLayouts[RAY_GEN_LAYOUT],
			//1, reinterpret_cast<const void**>(&RaygenShaderName));
		state.SetGlobalPipelineLayout(m_pipelineLayouts[GLOBAL_LAYOUT]);
		state.SetMaxRecursionDepth(1);
		m_rayTracingPipeline = state.GetPipeline(m_rayTracingPipelineCache, L"Raytracing");

		N_RETURN(m_rayTracingPipeline.Native || m_rayTracingPipeline.Fallback, false);
	}

	{
		N_RETURN(m_shaderPool.CreateShader(Shader::Stage::VS, VS_SCREEN_QUAD, L"VSScreenQuad.cso"), false);
		N_RETURN(m_shaderPool.CreateShader(Shader::Stage::PS, PS_RAY_CAST, L"PSRayCast.cso"), false);

		Graphics::State state;
		state.SetPipelineLayout(m_pipelineLayouts[RAY_CAST_LAYOUT]);
		state.SetShader(Shader::Stage::VS, m_shaderPool.GetShader(Shader::Stage::VS, VS_SCREEN_QUAD));
		state.SetShader(Shader::Stage::PS, m_shaderPool.GetShader(Shader::Stage::PS, PS_RAY_CAST));
		state.IASetPrimitiveTopologyType(D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
		state.DSSetState(Graphics::DepthStencilPreset::DEPTH_STENCIL_NONE, m_graphicsPipelineCache);
		state.OMSetRTVFormats(&rtFormat, 1);
		X_RETURN(m_pipeline, state.GetPipeline(m_graphicsPipelineCache, L"RayCast"), false);
	}

	return true;
}

bool Voxelizer::createDescriptorTables()
{
	// Acceleration structure UAVs
	{
		Descriptor descriptors[] = { m_bottomLevelAS.GetResult().GetUAV(), m_topLevelAS.GetResult().GetUAV() };
		Util::DescriptorTable descriptorTable;
		descriptorTable.SetDescriptors(0, static_cast<uint32_t>(size(descriptors)), descriptors);
		const auto asTable = descriptorTable.GetCbvSrvUavTable(m_descriptorTableCache);
		N_RETURN(asTable, false);
	}

	for (auto i = 0u; i < FrameCount; ++i)
	{
		// Output UAV
		Util::DescriptorTable descriptorTable;
		descriptorTable.SetDescriptors(0, 1, &m_grids[i].GetUAV());
		X_RETURN(m_uavTables[i], descriptorTable.GetCbvSrvUavTable(m_descriptorTableCache), false);
	}

	// Index buffer SRV
	{
		Util::DescriptorTable descriptorTable;
		descriptorTable.SetDescriptors(0, 1, &m_indexBuffer.GetSRV());
		X_RETURN(m_srvTables[SRV_TABLE_IB], descriptorTable.GetCbvSrvUavTable(m_descriptorTableCache), false);
	}

	// Vertex buffer SRV
	{
		Util::DescriptorTable descriptorTable;
		descriptorTable.SetDescriptors(0, 1, &m_vertexBuffer.GetSRV());
		X_RETURN(m_srvTables[SRV_TABLE_VB], descriptorTable.GetCbvSrvUavTable(m_descriptorTableCache), false);
	}

	// Ray cast
	for (auto i = 0ui8; i < FrameCount; ++i)
	{
		// Get CBV
		Util::DescriptorTable cbvTable;
		cbvTable.SetDescriptors(0, 1, &m_cbPerObject.GetCBV(i));
		X_RETURN(m_cbvTables[i], cbvTable.GetCbvSrvUavTable(m_descriptorTableCache), false);

		// Get SRV
		if (!m_srvTables[SRV_TABLE_GRID + i])
		{
			Util::DescriptorTable srvTable;
			srvTable.SetDescriptors(0, 1, &m_grids[i].GetSRV());
			X_RETURN(m_srvTables[SRV_TABLE_GRID + i], srvTable.GetCbvSrvUavTable(m_descriptorTableCache), false);
		}
	}

	// Create the sampler table
	{
		Util::DescriptorTable samplerTable;
		const auto sampler = LINEAR_CLAMP;
		samplerTable.SetSamplers(0, 1, &sampler, m_descriptorTableCache);
		X_RETURN(m_samplerTable, samplerTable.GetSamplerTable(m_descriptorTableCache), false);
	}

	return true;
}

bool Voxelizer::buildAccelerationStructures(const RayTracing::CommandList& commandList, Geometry* geometries)
{
	AccelerationStructure::SetFrameCount(FrameCount);

	// Set geometries
	BottomLevelAS::SetGeometries(geometries, 1, DXGI_FORMAT_R32G32B32_FLOAT,
		&m_vertexBuffer.GetVBV(), &m_indexBuffer.GetIBV());

	// Descriptor index in descriptor pool
	const uint32_t bottomLevelASIndex = 0;
	const uint32_t topLevelASIndex = bottomLevelASIndex + 1;

	// Prebuild
	N_RETURN(m_bottomLevelAS.PreBuild(m_device, 1, geometries,
		bottomLevelASIndex, NumUAVs), false);
	N_RETURN(m_topLevelAS.PreBuild(m_device, 1, topLevelASIndex, NumUAVs), false);

	// Create scratch buffer
	auto scratchSize = m_topLevelAS.GetScratchDataMaxSize();
	scratchSize = (max)(m_bottomLevelAS.GetScratchDataMaxSize(), scratchSize);
	N_RETURN(AccelerationStructure::AllocateUAVBuffer(m_device, m_scratch, scratchSize), false);

	// Get descriptor pool and create descriptor tables
	N_RETURN(createDescriptorTables(), false);
	const auto& descriptorPool = m_descriptorTableCache.GetDescriptorPool(CBV_SRV_UAV_POOL);

	// Set instance
	XMFLOAT4X4 matrix;
	const auto normalizedToLocal = XMMatrixScaling(m_bound.w, m_bound.w, m_bound.w) * XMMatrixTranslation(m_bound.x, m_bound.y, m_bound.z);
	XMStoreFloat4x4(&matrix, XMMatrixTranspose(XMMatrixInverse(nullptr, normalizedToLocal)));
	float* const pTransform[] = { reinterpret_cast<float*>(&matrix) };
	TopLevelAS::SetInstances(m_device, m_instances, 1, &m_bottomLevelAS, pTransform);

	// Build bottom level ASs
	m_bottomLevelAS.Build(commandList, m_scratch, descriptorPool, NumUAVs);

	// Build top level AS
	m_topLevelAS.Build(commandList, m_scratch, m_instances, descriptorPool, NumUAVs);

	return true;
}

bool Voxelizer::buildShaderTables()
{
	// Get shader identifiers.
	const auto shaderIDSize = ShaderRecord::GetShaderIDSize(m_device);

	// Ray gen shader table
	N_RETURN(m_rayGenShaderTable.Create(m_device, 1, shaderIDSize, L"RayGenShaderTable"), false);
	N_RETURN(m_rayGenShaderTable.AddShaderRecord(ShaderRecord(m_device, m_rayTracingPipeline, RaygenShaderName)), false);

	// Hit group shader table
	N_RETURN(m_hitGroupShaderTable.Create(m_device, 1, shaderIDSize, L"HitGroupShaderTable"), false);
	N_RETURN(m_hitGroupShaderTable.AddShaderRecord(ShaderRecord(m_device, m_rayTracingPipeline, HitGroupName)), false);

	// Miss shader table
	N_RETURN(m_missShaderTable.Create(m_device, 1, shaderIDSize, L"MissShaderTable"), false);
	N_RETURN(m_missShaderTable.AddShaderRecord(ShaderRecord(m_device, m_rayTracingPipeline, MissShaderName)), false);

	return true;
}

void Voxelizer::voxelize(const RayTracing::CommandList& commandList, uint32_t frameIndex)
{
	// Set resource barrier
	ResourceBarrier barrier;
	const auto numBarriers = m_grids[frameIndex].SetBarrier(&barrier, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	commandList.Barrier(numBarriers, &barrier);

	// Set descriptor tables
	commandList.SetComputePipelineLayout(m_pipelineLayouts[GLOBAL_LAYOUT]);
	commandList.SetComputeDescriptorTable(OUTPUT_GRID, m_uavTables[frameIndex]);
	commandList.SetTopLevelAccelerationStructure(ACCELERATION_STRUCTURE, m_topLevelAS);
	commandList.SetComputeDescriptorTable(INDEX_BUFFERS, m_srvTables[SRV_TABLE_IB]);
	commandList.SetComputeDescriptorTable(VERTEX_BUFFERS, m_srvTables[SRV_TABLE_VB]);

	// Fallback layer has no depth
	commandList.DispatchRays(m_rayTracingPipeline, GRID_SIZE, GRID_SIZE * GRID_SIZE, 1,
		m_hitGroupShaderTable, m_missShaderTable, m_rayGenShaderTable);
}

void Voxelizer::renderRayCast(const RayTracing::CommandList& commandList, uint32_t frameIndex,
	const RenderTargetTable& rtvs, const Descriptor& dsv)
{
	// Set resource barrier
	ResourceBarrier barrier;
	const auto numBarriers = m_grids[frameIndex].SetBarrier(&barrier, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	commandList.Barrier(numBarriers, &barrier);

	// Set descriptor tables
	commandList.SetGraphicsPipelineLayout(m_pipelineLayouts[RAY_CAST_LAYOUT]);
	commandList.SetGraphicsDescriptorTable(0, m_cbvTables[frameIndex]);
	commandList.SetGraphicsDescriptorTable(1, m_srvTables[SRV_TABLE_GRID + frameIndex]);
	commandList.SetGraphicsDescriptorTable(2, m_samplerTable);

	// Set pipeline state
	commandList.SetPipelineState(m_pipeline);

	// Set viewport
	Viewport viewport(0.0f, 0.0f, m_viewport.x, m_viewport.y);
	RectRange scissorRect(0, 0, static_cast<long>(m_viewport.x), static_cast<long>(m_viewport.y));
	commandList.RSSetViewports(1, &viewport);
	commandList.RSSetScissorRects(1, &scissorRect);

	commandList.OMSetRenderTargets(1, rtvs, nullptr);

	// Record commands.
	commandList.IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	commandList.Draw(3, 1, 0, 0);
}
