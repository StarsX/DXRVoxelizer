//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#pragma once

#include "Core/XUSG.h"
#include "Helper/XUSGRayTracing-EZ.h"
#include "RayTracing/XUSGRayTracing.h"

class VoxelizerEZ
{
public:
	VoxelizerEZ();
	virtual ~VoxelizerEZ();

	bool Init(XUSG::RayTracing::EZ::CommandList* pCommandList, uint32_t width, uint32_t height,
		XUSG::Format rtFormat, XUSG::Format dsFormat, std::vector<XUSG::Resource::uptr>& uploaders,
		XUSG::RayTracing::GeometryBuffer* pGeometry, const char* fileName, const DirectX::XMFLOAT4& posScale);

	void UpdateFrame(uint8_t frameIndex, DirectX::CXMVECTOR eyePt, DirectX::CXMMATRIX viewProj);
	void Render(XUSG::RayTracing::EZ::CommandList* pCommandList, uint8_t frameIndex,
		XUSG::RenderTarget* pRenderTarget, XUSG::DepthStencil* pDepthStencil);

	static const uint8_t FrameCount = 3;

protected:
	struct CBPerObject
	{
		DirectX::XMVECTOR localSpaceLightPt;
		DirectX::XMVECTOR localSpaceEyePt;
		DirectX::XMMATRIX screenToLocal;
	};

	bool createVB(XUSG::RayTracing::EZ::CommandList* pCommandList, uint32_t numVert,
		uint32_t stride, const uint8_t* pData, std::vector<XUSG::Resource::uptr>& uploaders);
	bool createIB(XUSG::RayTracing::EZ::CommandList* pCommandList, uint32_t numIndices,
		const uint32_t* pData, std::vector<XUSG::Resource::uptr>& uploaders);
	bool createCB(const XUSG::Device* pDevice);
	bool createShaders();
	bool buildAccelerationStructures(XUSG::RayTracing::EZ::CommandList* pCommandList,
		const XUSG::RayTracing::Device* pDevice,
		XUSG::RayTracing::GeometryBuffer* pGeometries);

	void voxelize(XUSG::RayTracing::EZ::CommandList* pCommandList, uint8_t frameIndex);
	void renderRayCast(XUSG::RayTracing::EZ::CommandList* pCommandList, uint8_t frameIndex,
		XUSG::RenderTarget* pRenderTarget, XUSG::DepthStencil* pDepthStencil);

	XUSG::RayTracing::BottomLevelAS::uptr m_bottomLevelAS;
	XUSG::RayTracing::TopLevelAS::uptr m_topLevelAS;

	XUSG::VertexBuffer::uptr	m_vertexBuffer;
	XUSG::IndexBuffer::uptr		m_indexBuffer;

	XUSG::ConstantBuffer::uptr	m_cbPerObject;

	XUSG::Texture3D::uptr		m_grids[FrameCount];

	XUSG::Resource::uptr		m_instances;

	// Shader tables
	static const wchar_t* HitGroupName;
	static const wchar_t* RaygenShaderName;
	static const wchar_t* ClosestHitShaderName;
	static const wchar_t* MissShaderName;

	enum ShaderIndex : uint8_t
	{
		VS_SCREEN_QUAD,
		PS_RAY_CAST,
		DXR_VOXELIZER,

		NUM_SHADER
	};

	XUSG::ShaderPool::uptr					m_shaderPool;
	XUSG::Blob m_shaders[NUM_SHADER];

	DirectX::XMFLOAT2				m_viewport;
	DirectX::XMFLOAT4				m_bound;
	DirectX::XMFLOAT4				m_posScale;
};
