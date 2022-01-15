//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#pragma once

#include "Core/XUSG.h"
#include "RayTracing/XUSGRayTracing.h"

class Voxelizer
{
public:
	Voxelizer();
	virtual ~Voxelizer();

	bool Init(XUSG::RayTracing::CommandList* pCommandList, uint32_t width, uint32_t height,
		XUSG::Format rtFormat, XUSG::Format dsFormat, std::vector<XUSG::Resource::uptr>& uploaders,
		XUSG::RayTracing::GeometryBuffer* pGeometry, const char* fileName, const DirectX::XMFLOAT4& posScale);

	void UpdateFrame(uint8_t frameIndex, DirectX::CXMVECTOR eyePt, DirectX::CXMMATRIX viewProj);
	void Render(XUSG::RayTracing::CommandList* pCommandList, uint8_t frameIndex,
		const XUSG::Descriptor& rtv, const XUSG::Descriptor& dsv);

	static const uint8_t FrameCount = 3;

protected:
	enum PipelineLayoutIndex : uint8_t
	{
		GLOBAL_LAYOUT,
		RAY_CAST_LAYOUT,

		NUM_PIPELINE_LAYOUT
	};

	enum PipelineIndex : uint8_t
	{
		RAY_TRACING,
		RAY_CAST,

		NUM_PIPELINE
	};

	enum GlobalPipelineLayoutSlot : uint8_t
	{
		OUTPUT_GRID,
		SHADER_RESOURCES,
		ACCELERATION_STRUCTURE = SHADER_RESOURCES,
		INDEX_BUFFERS,
		VERTEX_BUFFERS
	};

	enum SRVTable : uint8_t
	{
		SRV_TABLE_IB,
		SRV_TABLE_VB,
		SRV_TABLE_GRID,

		NUM_SRV_TABLE = SRV_TABLE_GRID + FrameCount
	};

	enum VertexShaderID : uint8_t
	{
		VS_SCREEN_QUAD
	};

	enum PixelShaderID : uint8_t
	{
		PS_RAY_CAST
	};

	struct CBPerObject
	{
		DirectX::XMVECTOR localSpaceLightPt;
		DirectX::XMVECTOR localSpaceEyePt;
		DirectX::XMMATRIX screenToLocal;
	};

	bool createVB(XUSG::RayTracing::CommandList* pCommandList, uint32_t numVert,
		uint32_t stride, const uint8_t* pData, std::vector<XUSG::Resource::uptr>& uploaders);
	bool createIB(XUSG::RayTracing::CommandList* pCommandList, uint32_t numIndices,
		const uint32_t* pData, std::vector<XUSG::Resource::uptr>& uploaders);
	bool createCB(const XUSG::Device* pDevice);
	bool createPipelineLayouts(const XUSG::RayTracing::Device* pDevice);
	bool createPipelines(XUSG::Format rtFormat, XUSG::Format dsFormat);
	bool createDescriptorTables();
	bool buildAccelerationStructures(XUSG::RayTracing::CommandList* pCommandList,
		XUSG::RayTracing::GeometryBuffer* pGeometries);
	bool buildShaderTables(const XUSG::RayTracing::Device* pDevice);

	void voxelize(XUSG::RayTracing::CommandList* pCommandList, uint8_t frameIndex);
	void renderRayCast(XUSG::RayTracing::CommandList* pCommandList, uint8_t frameIndex,
		const XUSG::Descriptor& rtv, const XUSG::Descriptor& dsv);

	XUSG::RayTracing::BottomLevelAS::uptr m_bottomLevelAS;
	XUSG::RayTracing::TopLevelAS::uptr m_topLevelAS;

	XUSG::PipelineLayout		m_pipelineLayouts[NUM_PIPELINE_LAYOUT];
	XUSG::Pipeline				m_pipelines[NUM_PIPELINE];

	XUSG::DescriptorTable		m_cbvTables[FrameCount];
	XUSG::DescriptorTable		m_srvTables[NUM_SRV_TABLE];
	XUSG::DescriptorTable		m_uavTables[FrameCount];
	XUSG::DescriptorTable		m_samplerTable;

	XUSG::VertexBuffer::uptr	m_vertexBuffer;
	XUSG::IndexBuffer::uptr		m_indexBuffer;

	XUSG::ConstantBuffer::uptr	m_cbPerObject;

	XUSG::Texture3D::uptr		m_grids[FrameCount];

	XUSG::Resource::uptr		m_scratch;
	XUSG::Resource::uptr		m_instances;

	// Shader tables
	static const wchar_t* HitGroupName;
	static const wchar_t* RaygenShaderName;
	static const wchar_t* ClosestHitShaderName;
	static const wchar_t* MissShaderName;
	XUSG::RayTracing::ShaderTable::uptr	m_missShaderTable;
	XUSG::RayTracing::ShaderTable::uptr	m_hitGroupShaderTable;
	XUSG::RayTracing::ShaderTable::uptr	m_rayGenShaderTable;

	XUSG::ShaderPool::uptr					m_shaderPool;
	XUSG::RayTracing::PipelineCache::uptr	m_rayTracingPipelineCache;
	XUSG::Graphics::PipelineCache::uptr		m_graphicsPipelineCache;
	XUSG::Compute::PipelineCache::uptr		m_computePipelineCache;
	XUSG::PipelineLayoutCache::uptr			m_pipelineLayoutCache;
	XUSG::DescriptorTableCache::uptr		m_descriptorTableCache;

	DirectX::XMFLOAT2				m_viewport;
	DirectX::XMFLOAT4				m_bound;
	DirectX::XMFLOAT4				m_posScale;
};
