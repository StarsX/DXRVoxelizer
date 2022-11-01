//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#include "Optional/XUSGObjLoader.h"
#include "VoxelizerEZ.h"

#define GRID_SIZE 64

using namespace std;
using namespace DirectX;
using namespace XUSG;
using namespace XUSG::RayTracing;

const wchar_t* VoxelizerEZ::HitGroupName = L"hitGroup";
const wchar_t* VoxelizerEZ::RaygenShaderName = L"raygenMain";
const wchar_t* VoxelizerEZ::ClosestHitShaderName = L"closestHitMain";
const wchar_t* VoxelizerEZ::MissShaderName = L"missMain";

VoxelizerEZ::VoxelizerEZ() :
	m_instances()
{
	m_shaderLib = ShaderLib::MakeUnique();
}

VoxelizerEZ::~VoxelizerEZ()
{
}

bool VoxelizerEZ::Init(RayTracing::EZ::CommandList* pCommandList, uint32_t width, uint32_t height,
	Format rtFormat, Format dsFormat, vector<Resource::uptr>& uploaders, GeometryBuffer* pGeometry,
	const char* fileName, const XMFLOAT4& posScale)
{
	const auto pDevice = pCommandList->GetRTDevice();

	m_viewport.x = static_cast<float>(width);
	m_viewport.y = static_cast<float>(height);
	m_posScale = posScale;

	// Load inputs
	ObjLoader objLoader;
	if (!objLoader.Import(fileName, true, true)) return false;
	XUSG_N_RETURN(createVB(pCommandList, objLoader.GetNumVertices(), objLoader.GetVertexStride(), objLoader.GetVertices(), uploaders), false);
	XUSG_N_RETURN(createIB(pCommandList, objLoader.GetNumIndices(), objLoader.GetIndices(), uploaders), false);

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

	XUSG_N_RETURN(buildAccelerationStructures(pCommandList, pDevice, pGeometry), false);
	XUSG_N_RETURN(createShaders(), false);

	return true;
}

void VoxelizerEZ::UpdateFrame(uint8_t frameIndex, CXMVECTOR eyePt, CXMMATRIX viewProj)
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

void VoxelizerEZ::Render(RayTracing::EZ::CommandList* pCommandList, uint8_t frameIndex,
	RenderTarget* pRenderTarget, DepthStencil* pDepthStencil)
{
	voxelize(pCommandList, frameIndex);
	renderRayCast(pCommandList, frameIndex, pRenderTarget, pDepthStencil);
}

bool VoxelizerEZ::createVB(RayTracing::EZ::CommandList* pCommandList, uint32_t numVert,
	uint32_t stride, const uint8_t* pData, vector<Resource::uptr>& uploaders)
{
	m_vertexBuffer = VertexBuffer::MakeUnique();
	XUSG_N_RETURN(m_vertexBuffer->Create(pCommandList->GetDevice(), numVert, stride,
		ResourceFlag::NONE, MemoryType::DEFAULT), false);
	uploaders.emplace_back(Resource::MakeUnique());

	return m_vertexBuffer->Upload(pCommandList->AsCommandList(),
		uploaders.back().get(), pData, stride * numVert);
}

bool VoxelizerEZ::createIB(RayTracing::EZ::CommandList* pCommandList, uint32_t numIndices,
	const uint32_t* pData, vector<Resource::uptr>& uploaders)
{
	const uint32_t byteWidth = sizeof(uint32_t) * numIndices;
	m_indexBuffer = IndexBuffer::MakeUnique();
	XUSG_N_RETURN(m_indexBuffer->Create(pCommandList->GetDevice(), byteWidth, Format::R32_UINT,
		ResourceFlag::NONE, MemoryType::DEFAULT), false);
	uploaders.emplace_back(Resource::MakeUnique());

	return m_indexBuffer->Upload(pCommandList->AsCommandList(),
		uploaders.back().get(), pData, byteWidth);
}

bool VoxelizerEZ::createCB(const XUSG::Device* pDevice)
{
	m_cbPerObject = ConstantBuffer::MakeUnique();

	return m_cbPerObject->Create(pDevice, sizeof(CBPerObject) * FrameCount, FrameCount);
}

bool VoxelizerEZ::createShaders()
{
	auto vsIndex = 0u;
	auto psIndex = 0u;
	auto csIndex = 0u;

	XUSG_N_RETURN(m_shaderLib->CreateShader(Shader::Stage::VS, vsIndex, L"VSScreenQuad.cso"), false);
	m_shaders[VS_SCREEN_QUAD] = m_shaderLib->GetShader(Shader::Stage::VS, vsIndex++);

	XUSG_N_RETURN(m_shaderLib->CreateShader(Shader::Stage::PS, psIndex, L"PSRayCast.cso"), false);
	m_shaders[PS_RAY_CAST] = m_shaderLib->GetShader(Shader::Stage::PS, psIndex++);

	XUSG_N_RETURN(m_shaderLib->CreateShader(Shader::Stage::CS, csIndex, L"DXRVoxelizer.cso"), false);
	m_shaders[DXR_VOXELIZER] = m_shaderLib->GetShader(Shader::Stage::CS, csIndex++);

	return true;
}

bool VoxelizerEZ::buildAccelerationStructures(RayTracing::EZ::CommandList* pCommandList,
	const RayTracing::Device* pDevice, GeometryBuffer* pGeometry)
{
	AccelerationStructure::SetFrameCount(FrameCount);

	// Set geometries
	auto vbv = XUSG::EZ::GetVBV(m_vertexBuffer.get());
	auto ibv = XUSG::EZ::GetIBV(m_indexBuffer.get());
	pCommandList->SetTriangleGeometries(*pGeometry, 1, Format::R32G32B32_FLOAT, &vbv, &ibv);

	// Prebuild
	m_bottomLevelAS = BottomLevelAS::MakeUnique();
	m_topLevelAS = TopLevelAS::MakeUnique();
	XUSG_N_RETURN(pCommandList->PreBuildBLAS(m_bottomLevelAS.get(), 1, *pGeometry), false);
	XUSG_N_RETURN(pCommandList->PreBuildTLAS(m_topLevelAS.get(), 1), false);

	// Set instance
	XMFLOAT3X4 matrix;
	const auto normalizedToLocal = XMMatrixScaling(m_bound.w, m_bound.w, m_bound.w) * XMMatrixTranslation(m_bound.x, m_bound.y, m_bound.z);
	XMStoreFloat3x4(&matrix, XMMatrixInverse(nullptr, normalizedToLocal));
	float* const pTransform[] = { reinterpret_cast<float*>(&matrix) };
	m_instances = Resource::MakeUnique();
	const BottomLevelAS* const ppBottomLevelAS[] = { m_bottomLevelAS.get() };
	TopLevelAS::SetInstances(pDevice, m_instances.get(), 1, &ppBottomLevelAS[0], &pTransform[0]);

	// Build bottom level ASs
	pCommandList->BuildBLAS(m_bottomLevelAS.get());

	// Build top level AS
	pCommandList->BuildTLAS(m_topLevelAS.get(), m_instances.get());

	return true;
}

void VoxelizerEZ::voxelize(RayTracing::EZ::CommandList* pCommandList, uint8_t frameIndex)
{
	// Set pipeline state
	static const void* shaders[] = { RaygenShaderName, ClosestHitShaderName, MissShaderName };
	pCommandList->RTSetShaderLibrary(0, m_shaders[DXR_VOXELIZER], static_cast<uint32_t>(size(shaders)), shaders);
	pCommandList->RTSetHitGroup(0, HitGroupName, ClosestHitShaderName);
	pCommandList->RTSetShaderConfig(sizeof(XMFLOAT4), sizeof(XMFLOAT2));
	pCommandList->RTSetMaxRecursionDepth(1);

	// Set TLAS
	pCommandList->SetTopLevelAccelerationStructure(0, m_topLevelAS.get());

	// Set UAV
	const auto uav = XUSG::EZ::GetUAV(m_grids[frameIndex].get());
	pCommandList->SetResources(Shader::Stage::CS, DescriptorType::UAV, 0, 1, &uav);

	// Set SRVs
	const XUSG::EZ::ResourceView srvs[] =
	{
		XUSG::EZ::GetSRV(m_indexBuffer.get()),
		XUSG::EZ::GetSRV(m_vertexBuffer.get()),
	};
	pCommandList->SetResources(Shader::Stage::CS, DescriptorType::SRV, 0, 1, &srvs[0], 0);
	pCommandList->SetResources(Shader::Stage::CS, DescriptorType::SRV, 0, 1, &srvs[1], 1);

	// Dispatch command
	pCommandList->DispatchRays(GRID_SIZE, GRID_SIZE * GRID_SIZE, 1, RaygenShaderName,
		MissShaderName);
}

void VoxelizerEZ::renderRayCast(RayTracing::EZ::CommandList* pCommandList, uint8_t frameIndex,
	RenderTarget* pRenderTarget, DepthStencil* pDepthStencil)
{
	// set pipeline state
	pCommandList->SetGraphicsShader(Shader::Stage::VS, m_shaders[VS_SCREEN_QUAD]);
	pCommandList->SetGraphicsShader(Shader::Stage::PS, m_shaders[PS_RAY_CAST]);
	pCommandList->DSSetState(XUSG::Graphics::DepthStencilPreset::DEPTH_STENCIL_NONE);

	// Set render target
	auto rtv = XUSG::EZ::GetRTV(pRenderTarget);
	auto dsv = XUSG::EZ::GetDSV(pDepthStencil);
	pCommandList->OMSetRenderTargets(1, &rtv, &dsv);

	// Set CBV
	const auto cbv = XUSG::EZ::GetCBV(m_cbPerObject.get());
	pCommandList->SetResources(Shader::Stage::PS, DescriptorType::CBV, 0, 1, &cbv);

	// Set SRV
	const auto srv = XUSG::EZ::GetSRV(m_grids[frameIndex].get());
	pCommandList->SetResources(Shader::Stage::PS, DescriptorType::SRV, 0, 1, &srv);

	// Set sampler
	const auto sampler = SamplerPreset::LINEAR_CLAMP;
	pCommandList->SetSamplerStates(Shader::Stage::PS, 0, 1, &sampler);

	// Set viewport
	Viewport viewport(0.0f, 0.0f, m_viewport.x, m_viewport.y);
	RectRange scissorRect(0, 0, static_cast<long>(m_viewport.x), static_cast<long>(m_viewport.y));
	pCommandList->RSSetViewports(1, &viewport);
	pCommandList->RSSetScissorRects(1, &scissorRect);

	// Set IA
	pCommandList->IASetPrimitiveTopology(PrimitiveTopology::TRIANGLELIST);

	// Draw command
	pCommandList->Draw(3, 1, 0, 0);
}
