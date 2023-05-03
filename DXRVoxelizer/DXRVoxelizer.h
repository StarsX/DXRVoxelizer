//*********************************************************
//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//
//*********************************************************

#pragma once

#include "DXFramework.h"
#include "StepTimer.h"
#include "Voxelizer.h"
#include "VoxelizerEZ.h"

using namespace DirectX;

// Note that while ComPtr is used to manage the lifetime of resources on the CPU,
// it has no understanding of the lifetime of resources on the GPU. Apps must account
// for the GPU lifetime of resources to avoid destroying objects that may still be
// referenced by the GPU.
// An example of this can be found in the class method: OnDestroy().

class DXRVoxelizer : public DXFramework
{
public:
	DXRVoxelizer(uint32_t width, uint32_t height, std::wstring name);
	virtual ~DXRVoxelizer();

	virtual void OnInit();
	virtual void OnUpdate();
	virtual void OnRender();
	virtual void OnDestroy();

	virtual void OnKeyUp(uint8_t /*key*/);
	virtual void OnLButtonDown(float posX, float posY);
	virtual void OnLButtonUp(float posX, float posY);
	virtual void OnMouseMove(float posX, float posY);
	virtual void OnMouseWheel(float deltaZ, float posX, float posY);
	virtual void OnMouseLeave();

	virtual void ParseCommandLineArgs(wchar_t* argv[], int argc);

private:
	static const uint8_t FrameCount = Voxelizer::FrameCount;

	// Pipeline objects.
	XUSG::DescriptorTableLib::sptr m_descriptorTableLib;

	XUSG::Viewport			m_viewport;
	XUSG::RectRange			m_scissorRect;

	XUSG::SwapChain::uptr			m_swapChain;
	XUSG::CommandAllocator::uptr	m_commandAllocators[FrameCount];
	XUSG::CommandQueue::uptr		m_commandQueue;

	bool m_isDxrSupported;

	XUSG::RayTracing::Device::uptr m_device;
	XUSG::RenderTarget::uptr m_renderTargets[FrameCount];
	XUSG::RayTracing::CommandList::uptr m_commandList;
	XUSG::RayTracing::EZ::CommandList::uptr m_commandListEZ;

	// App resources.
	std::unique_ptr<Voxelizer> m_voxelizer;
	std::unique_ptr<VoxelizerEZ> m_voxelizerEZ;
	XUSG::DepthStencil::uptr m_depth;
	DirectX::XMFLOAT4X4	m_proj;
	DirectX::XMFLOAT4X4	m_view;
	DirectX::XMFLOAT3	m_focusPt;
	DirectX::XMFLOAT3	m_eyePt;

	// Synchronization objects.
	uint32_t	m_frameIndex;
	HANDLE		m_fenceEvent;
	XUSG::Fence::uptr m_fence;
	uint64_t	m_fenceValues[FrameCount];

	// Application state
	bool		m_useEZ;
	bool		m_showFPS;
	bool		m_pausing;
	StepTimer	m_timer;

	// User camera interactions
	bool m_tracking;
	DirectX::XMFLOAT2 m_mousePt;

	// User external settings
	std::string m_meshFileName;
	XMFLOAT4 m_meshPosScale;

	// Screen-shot helpers and state
	XUSG::Buffer::uptr	m_readBuffer;
	uint32_t			m_rowPitch;
	uint8_t				m_screenShot;

	void LoadPipeline();
	void LoadAssets();
	void PopulateCommandList();
	void WaitForGpu();
	void MoveToNextFrame();
	void SaveImage(char const* fileName, XUSG::Buffer* imageBuffer,
		uint32_t w, uint32_t h, uint32_t rowPitch, uint8_t comp = 3);
	double CalculateFrameStats(float* fTimeStep = nullptr);

	// Ray tracing
	void EnableDirectXRaytracing(IDXGIAdapter1* adapter);
};
