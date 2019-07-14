//--------------------------------------------------------------------------------------
// By Stars XU Tianchen
//--------------------------------------------------------------------------------------

#pragma once

#include "Core/XUSG.h"
#include "RayTracing/XUSGRayTracing.h"

class Voxelizer
{
public:
	Voxelizer(const XUSG::RayTracing::Device& device);
	virtual ~Voxelizer();

	bool Init(const XUSG::RayTracing::CommandList& commandList, uint32_t width, uint32_t height,
		XUSG::Format rtFormat, XUSG::Format dsFormat, std::vector<XUSG::Resource>& uploaders,
		XUSG::RayTracing::Geometry& geometry, const char* fileName);

	void UpdateFrame(uint32_t frameIndex, DirectX::CXMVECTOR eyePt, DirectX::CXMMATRIX viewProj);
	void Render(const XUSG::RayTracing::CommandList& commandList, uint32_t frameIndex,
		const XUSG::RenderTargetTable& rtvs, const XUSG::Descriptor& dsv);

	static const uint32_t FrameCount = 3;

protected:
	enum PipelineLayoutIndex : uint8_t
	{
		GLOBAL_LAYOUT,
		//RAY_GEN_LAYOUT,
		RAY_CAST_LAYOUT,

		NUM_PIPELINE_LAYOUT
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

	bool createVB(const XUSG::RayTracing::CommandList& commandList, uint32_t numVert,
		uint32_t stride, const uint8_t* pData, std::vector<XUSG::Resource>& uploaders);
	bool createIB(const XUSG::RayTracing::CommandList& commandList, uint32_t numIndices,
		const uint32_t* pData, std::vector<XUSG::Resource>& uploaders);
	bool createCB();
	bool createPipelineLayouts();
	bool createPipelines(XUSG::Format rtFormat, XUSG::Format dsFormat);
	bool createDescriptorTables();
	bool buildAccelerationStructures(const XUSG::RayTracing::CommandList& commandList,
		XUSG::RayTracing::Geometry* geometries);
	bool buildShaderTables();

	void voxelize(const XUSG::RayTracing::CommandList& commandList, uint32_t frameIndex);
	void renderRayCast(const XUSG::RayTracing::CommandList& commandList, uint32_t frameIndex,
		const XUSG::RenderTargetTable& rtvs, const XUSG::Descriptor& dsv);

	XUSG::RayTracing::Device m_device;

	static const uint32_t NumUAVs = 2 + FrameCount;
	XUSG::RayTracing::BottomLevelAS m_bottomLevelAS;
	XUSG::RayTracing::TopLevelAS m_topLevelAS;

	XUSG::PipelineLayout		m_pipelineLayouts[NUM_PIPELINE_LAYOUT];
	XUSG::RayTracing::Pipeline	m_rayTracingPipeline;
	XUSG::Pipeline				m_pipeline;

	XUSG::DescriptorTable		m_cbvTables[FrameCount];
	XUSG::DescriptorTable		m_srvTables[NUM_SRV_TABLE];
	XUSG::DescriptorTable		m_uavTables[FrameCount];
	XUSG::DescriptorTable		m_samplerTable;

	XUSG::VertexBuffer			m_vertexBuffer;
	XUSG::IndexBuffer			m_indexBuffer;

	XUSG::ConstantBuffer		m_cbPerObject;

	XUSG::Texture3D				m_grids[FrameCount];

	XUSG::Resource				m_scratch;
	XUSG::Resource				m_instances;

	// Shader tables
	static const wchar_t* HitGroupName;
	static const wchar_t* RaygenShaderName;
	static const wchar_t* ClosestHitShaderName;
	static const wchar_t* MissShaderName;
	XUSG::RayTracing::ShaderTable	m_missShaderTable;
	XUSG::RayTracing::ShaderTable	m_hitGroupShaderTable;
	XUSG::RayTracing::ShaderTable	m_rayGenShaderTable;

	XUSG::ShaderPool				m_shaderPool;
	XUSG::RayTracing::PipelineCache	m_rayTracingPipelineCache;
	XUSG::Graphics::PipelineCache	m_graphicsPipelineCache;
	XUSG::Compute::PipelineCache	m_computePipelineCache;
	XUSG::PipelineLayoutCache		m_pipelineLayoutCache;
	XUSG::DescriptorTableCache		m_descriptorTableCache;

	DirectX::XMFLOAT2				m_viewport;
	DirectX::XMFLOAT4				m_bound;
};
