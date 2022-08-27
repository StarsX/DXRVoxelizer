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

#include "DXRVoxelizer.h"

using namespace std;
using namespace XUSG;
using namespace XUSG::RayTracing;

#define	PIDIV4 0.785398163f

static const float g_FOVAngleY = PIDIV4;
static const float g_zNear = 1.0f;
static const float g_zFar = 1000.0f;

DXRVoxelizer::DXRVoxelizer(uint32_t width, uint32_t height, std::wstring name) :
	DXFramework(width, height, name),
	m_isDxrSupported(false),
	m_frameIndex(0),
	m_viewport(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height)),
	m_scissorRect(0, 0, static_cast<long>(width), static_cast<long>(height)),
	m_useEZ(true),
	m_showFPS(true),
	m_pausing(false),
	m_tracking(false),
	m_meshFileName("Assets/bunny.obj"),
	m_meshPosScale(0.0f, 0.0f, 0.0f, 1.0f)
{
#if defined (_DEBUG)
	_CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
	AllocConsole();
	FILE* stream;
	freopen_s(&stream, "CONIN$", "r+t", stdin);
	freopen_s(&stream, "CONOUT$", "w+t", stdout);
	freopen_s(&stream, "CONOUT$", "w+t", stderr);
#endif
}

DXRVoxelizer::~DXRVoxelizer()
{
#if defined (_DEBUG)
	FreeConsole();
#endif
}

void DXRVoxelizer::OnInit()
{
	LoadPipeline();
	LoadAssets();
}

// Load the rendering pipeline dependencies.
void DXRVoxelizer::LoadPipeline()
{
	auto dxgiFactoryFlags = 0u;

#if defined(_DEBUG)
	// Enable the debug layer (requires the Graphics Tools "optional feature").
	// NOTE: Enabling the debug layer after device creation will invalidate the active device.
	{
		ComPtr<ID3D12Debug1> debugController;
		if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
		{
			debugController->EnableDebugLayer();
			//debugController->SetEnableGPUBasedValidation(TRUE);

			// Enable additional debug layers.
			dxgiFactoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
		}
	}
#endif

	com_ptr<IDXGIFactory5> factory;
	ThrowIfFailed(CreateDXGIFactory2(dxgiFactoryFlags, IID_PPV_ARGS(&factory)));

	DXGI_ADAPTER_DESC1 dxgiAdapterDesc;
	com_ptr<IDXGIAdapter1> dxgiAdapter;
	auto hr = DXGI_ERROR_UNSUPPORTED;
	const auto createDeviceFlags = EnableRootDescriptorsInShaderRecords;
	for (auto i = 0u; hr == DXGI_ERROR_UNSUPPORTED; ++i)
	{
		dxgiAdapter = nullptr;
		ThrowIfFailed(factory->EnumAdapters1(i, &dxgiAdapter));
		EnableDirectXRaytracing(dxgiAdapter.get());

		m_device = RayTracing::Device::MakeUnique();
		hr = m_device->Create(dxgiAdapter.get(), D3D_FEATURE_LEVEL_11_0);
		XUSG_N_RETURN(m_device->CreateInterface(createDeviceFlags), ThrowIfFailed(E_FAIL));
	}

	dxgiAdapter->GetDesc1(&dxgiAdapterDesc);
	if (dxgiAdapterDesc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
		m_title += dxgiAdapterDesc.VendorId == 0x1414 && dxgiAdapterDesc.DeviceId == 0x8c ? L" (WARP)" : L" (Software)";
	ThrowIfFailed(hr);

	// Create the command queue.
	m_commandQueue = CommandQueue::MakeUnique();
	XUSG_N_RETURN(m_commandQueue->Create(m_device.get(), CommandListType::DIRECT, CommandQueueFlag::NONE,
		0, 0, L"CommandQueue"), ThrowIfFailed(E_FAIL));

	// Describe and create the swap chain.
	m_swapChain = SwapChain::MakeUnique();
	XUSG_N_RETURN(m_swapChain->Create(factory.get(), Win32Application::GetHwnd(), m_commandQueue->GetHandle(),
		FrameCount, m_width, m_height, Format::R8G8B8A8_UNORM, SwapChainFlag::ALLOW_TEARING), ThrowIfFailed(E_FAIL));

	// This sample does not support fullscreen transitions.
	ThrowIfFailed(factory->MakeWindowAssociation(Win32Application::GetHwnd(), DXGI_MWA_NO_ALT_ENTER));

	m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();

	// Create frame resources.
	// Create a RTV and a command allocator for each frame.
	for (uint8_t n = 0; n < FrameCount; ++n)
	{
		m_renderTargets[n] = RenderTarget::MakeUnique();
		XUSG_N_RETURN(m_renderTargets[n]->CreateFromSwapChain(m_device.get(), m_swapChain.get(), n), ThrowIfFailed(E_FAIL));

		m_commandAllocators[n] = CommandAllocator::MakeUnique();
		XUSG_N_RETURN(m_commandAllocators[n]->Create(m_device.get(), CommandListType::DIRECT,
			(L"CommandAllocator" + to_wstring(n)).c_str()), ThrowIfFailed(E_FAIL));
	}

	// Create a DSV
	m_depth = DepthStencil::MakeUnique();
	XUSG_N_RETURN(m_depth->Create(m_device.get(), m_width, m_height, Format::D24_UNORM_S8_UINT,
		ResourceFlag::DENY_SHADER_RESOURCE), ThrowIfFailed(E_FAIL));

	// Create descriptor table cache.
	m_descriptorTableCache = DescriptorTableCache::MakeShared(m_device.get(), L"DescriptorTableCache");
}

// Load the sample assets.
void DXRVoxelizer::LoadAssets()
{
	// Create the command list.
	m_commandList = RayTracing::CommandList::MakeUnique();
	const auto pCommandList = m_commandList.get();
	XUSG_N_RETURN(pCommandList->Create(m_device.get(), 0, CommandListType::DIRECT,
		m_commandAllocators[m_frameIndex].get(), nullptr), ThrowIfFailed(E_FAIL));

	// Create ray tracing interfaces
	XUSG_N_RETURN(m_commandList->CreateInterface(), ThrowIfFailed(E_FAIL));

	// TODO: create m_commandListEZ.
	AccelerationStructure::SetUAVCount(2);
	m_commandListEZ = RayTracing::EZ::CommandList::MakeUnique();
	XUSG_N_RETURN(m_commandListEZ->Create(m_commandList.get(), 1, 16, 16,
		nullptr, nullptr, nullptr, 1, 2, 1, 1, 2), ThrowIfFailed(E_FAIL));

	vector<Resource::uptr> uploaders(0);
	GeometryBuffer geometries[2];

	m_voxelizer = make_unique<Voxelizer>();
	XUSG_N_RETURN(m_voxelizer->Init(pCommandList, m_descriptorTableCache, m_width, m_height,
		m_renderTargets[0]->GetFormat(), m_depth->GetFormat(), uploaders, &geometries[0],
		m_meshFileName.c_str(), m_meshPosScale), ThrowIfFailed(E_FAIL));

	m_voxelizerEZ = make_unique<VoxelizerEZ>();
	XUSG_N_RETURN(m_voxelizerEZ->Init(m_commandListEZ.get(),
		m_width, m_height, m_renderTargets[0]->GetFormat(),
		m_depth->GetFormat(), uploaders, &geometries[1], m_meshFileName.c_str(),
		m_meshPosScale), ThrowIfFailed(E_FAIL));

	// Close the command list and execute it to begin the initial GPU setup.
	XUSG_N_RETURN(pCommandList->Close(), ThrowIfFailed(E_FAIL));
	m_commandQueue->ExecuteCommandList(pCommandList);

	// Create synchronization objects and wait until assets have been uploaded to the GPU.
	{
		if (!m_fence)
		{
			m_fence = Fence::MakeUnique();
			XUSG_N_RETURN(m_fence->Create(m_device.get(), m_fenceValues[m_frameIndex]++, FenceFlag::NONE, L"Fence"), ThrowIfFailed(E_FAIL));
		}

		// Create an event handle to use for frame synchronization.
		m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
		if (!m_fenceEvent) ThrowIfFailed(HRESULT_FROM_WIN32(GetLastError()));

		// Wait for the command list to execute; we are reusing the same command 
		// list in our main loop but for now, we just want to wait for setup to 
		// complete before continuing.
		WaitForGpu();
	}

	// Projection
	const auto aspectRatio = m_width / static_cast<float>(m_height);
	const auto proj = XMMatrixPerspectiveFovLH(g_FOVAngleY, aspectRatio, g_zNear, g_zFar);
	XMStoreFloat4x4(&m_proj, proj);

	// View initialization
	m_focusPt = XMFLOAT3(0.0f, 4.0f, 0.0f);
	m_eyePt = XMFLOAT3(8.0f, 12.0f, -14.0f);
	const auto focusPt = XMLoadFloat3(&m_focusPt);
	const auto eyePt = XMLoadFloat3(&m_eyePt);
	const auto view = XMMatrixLookAtLH(eyePt, focusPt, XMVectorSet(0.0f, 1.0f, 0.0f, 1.0f));
	XMStoreFloat4x4(&m_view, view);
}

// Update frame-based values.
void DXRVoxelizer::OnUpdate()
{
	// Timer
	static auto time = 0.0, pauseTime = 0.0;

	m_timer.Tick();
	const auto totalTime = CalculateFrameStats();
	pauseTime = m_pausing ? totalTime - time : pauseTime;
	time = totalTime - pauseTime;

	// View
	const auto eyePt = XMLoadFloat3(&m_eyePt);
	const auto view = XMLoadFloat4x4(&m_view);
	const auto proj = XMLoadFloat4x4(&m_proj);

	if (m_useEZ) m_voxelizerEZ->UpdateFrame(m_frameIndex, eyePt, view * proj);
	else m_voxelizer->UpdateFrame(m_frameIndex, eyePt, view * proj);
}

// Render the scene.
void DXRVoxelizer::OnRender()
{
	// Record all the commands we need to render the scene into the command list.
	PopulateCommandList();

	// Execute the command list.
	m_commandQueue->ExecuteCommandList(m_commandList.get());

	// Present the frame.
	XUSG_N_RETURN(m_swapChain->Present(0, PresentFlag::ALLOW_TEARING), ThrowIfFailed(E_FAIL));

	MoveToNextFrame();
}

void DXRVoxelizer::OnDestroy()
{
	// Ensure that the GPU is no longer referencing resources that are about to be
	// cleaned up by the destructor.
	WaitForGpu();

	CloseHandle(m_fenceEvent);
}

// User hot-key interactions.
void DXRVoxelizer::OnKeyUp(uint8_t key)
{
	switch (key)
	{
	case VK_SPACE:
		m_pausing = !m_pausing;
		break;
	case VK_F1:
		m_showFPS = !m_showFPS;
		break;
	case 'X':
		m_useEZ = !m_useEZ;
		break;
	}
}

// User camera interactions.
void DXRVoxelizer::OnLButtonDown(float posX, float posY)
{
	m_tracking = true;
	m_mousePt = XMFLOAT2(posX, posY);
}

void DXRVoxelizer::OnLButtonUp(float posX, float posY)
{
	m_tracking = false;
}

void DXRVoxelizer::OnMouseMove(float posX, float posY)
{
	if (m_tracking)
	{
		const auto dPos = XMFLOAT2(m_mousePt.x - posX, m_mousePt.y - posY);

		XMFLOAT2 radians;
		radians.x = XM_2PI * dPos.y / m_height;
		radians.y = XM_2PI * dPos.x / m_width;

		const auto focusPt = XMLoadFloat3(&m_focusPt);
		auto eyePt = XMLoadFloat3(&m_eyePt);

		const auto len = XMVectorGetX(XMVector3Length(focusPt - eyePt));
		auto transform = XMMatrixTranslation(0.0f, 0.0f, -len);
		transform *= XMMatrixRotationRollPitchYaw(radians.x, radians.y, 0.0f);
		transform *= XMMatrixTranslation(0.0f, 0.0f, len);

		const auto view = XMLoadFloat4x4(&m_view) * transform;
		const auto viewInv = XMMatrixInverse(nullptr, view);
		eyePt = viewInv.r[3];

		XMStoreFloat3(&m_eyePt, eyePt);
		XMStoreFloat4x4(&m_view, view);

		m_mousePt = XMFLOAT2(posX, posY);
	}
}

void DXRVoxelizer::OnMouseWheel(float deltaZ, float posX, float posY)
{
	const auto focusPt = XMLoadFloat3(&m_focusPt);
	auto eyePt = XMLoadFloat3(&m_eyePt);

	const auto len = XMVectorGetX(XMVector3Length(focusPt - eyePt));
	const auto transform = XMMatrixTranslation(0.0f, 0.0f, -len * deltaZ / 16.0f);

	const auto view = XMLoadFloat4x4(&m_view) * transform;
	const auto viewInv = XMMatrixInverse(nullptr, view);
	eyePt = viewInv.r[3];

	XMStoreFloat3(&m_eyePt, eyePt);
	XMStoreFloat4x4(&m_view, view);
}

void DXRVoxelizer::OnMouseLeave()
{
	m_tracking = false;
}

void DXRVoxelizer::ParseCommandLineArgs(wchar_t* argv[], int argc)
{
	DXFramework::ParseCommandLineArgs(argv, argc);

	for (auto i = 1; i < argc; ++i)
	{
		if (_wcsnicmp(argv[i], L"-mesh", wcslen(argv[i])) == 0 ||
			_wcsnicmp(argv[i], L"/mesh", wcslen(argv[i])) == 0)
		{
			if (i + 1 < argc)
			{
				m_meshFileName.resize(wcslen(argv[i + 1]));
				for (size_t j = 0; j < m_meshFileName.size(); ++j)
					m_meshFileName[j] = static_cast<char>(argv[i + 1][j]);
			}
			m_meshPosScale.x = i + 2 < argc ? static_cast<float>(_wtof(argv[i + 2])) : m_meshPosScale.x;
			m_meshPosScale.y = i + 3 < argc ? static_cast<float>(_wtof(argv[i + 3])) : m_meshPosScale.y;
			m_meshPosScale.z = i + 4 < argc ? static_cast<float>(_wtof(argv[i + 4])) : m_meshPosScale.z;
			m_meshPosScale.w = i + 5 < argc ? static_cast<float>(_wtof(argv[i + 5])) : m_meshPosScale.w;
			break;
		}
	}
}

void DXRVoxelizer::PopulateCommandList()
{
	// Command list allocators can only be reset when the associated 
	// command lists have finished execution on the GPU; apps should use 
	// fences to determine GPU execution progress.
	const auto pCommandAllocator = m_commandAllocators[m_frameIndex].get();
	XUSG_N_RETURN(pCommandAllocator->Reset(), ThrowIfFailed(E_FAIL));

	// Voxelizer rendering
	const auto renderTarget = m_renderTargets[m_frameIndex].get();
	if (m_useEZ)
	{
		// However, when ExecuteCommandList() is called on a particular command 
		// list, that command list can then be reset at any time and must be before 
		// re-recording.
		const auto pCommandList = m_commandListEZ.get();
		XUSG_N_RETURN(pCommandList->Reset(pCommandAllocator, nullptr), ThrowIfFailed(E_FAIL));

		auto dsv = XUSG::EZ::GetDSV(m_depth.get());
		pCommandList->ClearDepthStencilView(dsv, ClearFlag::DEPTH, 1.0f);
		m_voxelizerEZ->Render(pCommandList, m_frameIndex, renderTarget, m_depth.get());

		XUSG_N_RETURN(pCommandList->CloseForPresent(renderTarget), ThrowIfFailed(E_FAIL));
	}
	else
	{
		// However, when ExecuteCommandList() is called on a particular command 
		// list, that command list can then be reset at any time and must be before 
		// re-recording.
		const auto pCommandList = m_commandList.get();
		XUSG_N_RETURN(pCommandList->Reset(pCommandAllocator, nullptr), ThrowIfFailed(E_FAIL));

		const DescriptorPool descriptorPools[] =
		{
			m_descriptorTableCache->GetDescriptorPool(CBV_SRV_UAV_POOL),
			m_descriptorTableCache->GetDescriptorPool(SAMPLER_POOL)
		};
		pCommandList->SetDescriptorPools(static_cast<uint32_t>(size(descriptorPools)), descriptorPools);

		ResourceBarrier barrier;
		auto numBarriers = renderTarget->SetBarrier(&barrier, ResourceState::RENDER_TARGET);
		pCommandList->Barrier(numBarriers, &barrier);

		// Record commands.
		//const float clearColor[] = { 0.0f, 0.2f, 0.4f, 1.0f };
		//m_commandList.ClearRenderTargetView(*m_rtvTables[m_frameIndex], clearColor);
		pCommandList->ClearDepthStencilView(m_depth->GetDSV(), ClearFlag::DEPTH, 1.0f);

		m_voxelizer->Render(pCommandList, m_frameIndex, renderTarget->GetRTV(), m_depth->GetDSV());

		// Indicate that the back buffer will now be used to present.
		numBarriers = renderTarget->SetBarrier(&barrier, ResourceState::PRESENT);
		pCommandList->Barrier(numBarriers, &barrier);

		XUSG_N_RETURN(pCommandList->Close(), ThrowIfFailed(E_FAIL));
	}
}

// Wait for pending GPU work to complete.
void DXRVoxelizer::WaitForGpu()
{
	// Schedule a Signal command in the queue.
	XUSG_N_RETURN(m_commandQueue->Signal(m_fence.get(), m_fenceValues[m_frameIndex]), ThrowIfFailed(E_FAIL));

	// Wait until the fence has been processed, and increment the fence value for the current frame.
	XUSG_N_RETURN(m_fence->SetEventOnCompletion(m_fenceValues[m_frameIndex]++, m_fenceEvent), ThrowIfFailed(E_FAIL));
	WaitForSingleObject(m_fenceEvent, INFINITE);
}

// Prepare to render the next frame.
void DXRVoxelizer::MoveToNextFrame()
{
	// Schedule a Signal command in the queue.
	const auto currentFenceValue = m_fenceValues[m_frameIndex];
	XUSG_N_RETURN(m_commandQueue->Signal(m_fence.get(), currentFenceValue), ThrowIfFailed(E_FAIL));

	// Update the frame index.
	m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();

	// If the next frame is not ready to be rendered yet, wait until it is ready.
	if (m_fence->GetCompletedValue() < m_fenceValues[m_frameIndex])
	{
		XUSG_N_RETURN(m_fence->SetEventOnCompletion(m_fenceValues[m_frameIndex], m_fenceEvent), ThrowIfFailed(E_FAIL));
		WaitForSingleObject(m_fenceEvent, INFINITE);
	}

	// Set the fence value for the next frame.
	m_fenceValues[m_frameIndex] = currentFenceValue + 1;
}

double DXRVoxelizer::CalculateFrameStats(float* pTimeStep)
{
	static int frameCnt = 0;
	static double elapsedTime = 0.0;
	static double previousTime = 0.0;
	const auto totalTime = m_timer.GetTotalSeconds();
	++frameCnt;

	const auto timeStep = static_cast<float>(totalTime - elapsedTime);

	// Compute averages over one second period.
	if ((totalTime - elapsedTime) >= 1.0f)
	{
		float fps = static_cast<float>(frameCnt) / timeStep;	// Normalize to an exact second.

		frameCnt = 0;
		elapsedTime = totalTime;

		wstringstream windowText;
		windowText << L"    fps: ";
		if (m_showFPS) windowText << setprecision(2) << fixed << fps;
		else windowText << L"[F1]";

		windowText << L"    [X] " << (m_useEZ ? "XUSG-EZ" : "XUSGCore");
		SetCustomWindowText(windowText.str().c_str());
	}

	if (pTimeStep)*pTimeStep = static_cast<float>(totalTime - previousTime);
	previousTime = totalTime;

	return totalTime;
}

//--------------------------------------------------------------------------------------
// Ray tracing
//--------------------------------------------------------------------------------------

// Enable experimental features required for compute-based raytracing fallback.
// This will set active D3D12 devices to DEVICE_REMOVED state.
// Returns bool whether the call succeeded and the device supports the feature.
inline bool EnableComputeRaytracingFallback(IDXGIAdapter1* adapter)
{
	ComPtr<ID3D12Device> testDevice;
	UUID experimentalFeatures[] = { D3D12ExperimentalShaderModels };

	return SUCCEEDED(D3D12EnableExperimentalFeatures(1, experimentalFeatures, nullptr, nullptr))
		&& SUCCEEDED(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&testDevice)));
}

// Returns bool whether the device supports DirectX Raytracing tier.
inline bool IsDirectXRaytracingSupported(IDXGIAdapter1* adapter)
{
	ComPtr<ID3D12Device> testDevice;
	D3D12_FEATURE_DATA_D3D12_OPTIONS5 featureSupportData = {};

	return SUCCEEDED(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&testDevice)))
		&& SUCCEEDED(testDevice->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &featureSupportData, sizeof(featureSupportData)))
		&& featureSupportData.RaytracingTier != D3D12_RAYTRACING_TIER_NOT_SUPPORTED;
}

void DXRVoxelizer::EnableDirectXRaytracing(IDXGIAdapter1* adapter)
{
	// Fallback Layer uses an experimental feature and needs to be enabled before creating a D3D12 device.
	bool isFallbackSupported = EnableComputeRaytracingFallback(adapter);

	if (!isFallbackSupported)
	{
		OutputDebugString(
			L"Warning: Could not enable Compute Raytracing Fallback (D3D12EnableExperimentalFeatures() failed).\n" \
			L"         Possible reasons: your OS is not in developer mode.\n\n");
	}

	m_isDxrSupported = IsDirectXRaytracingSupported(adapter);

	if (!m_isDxrSupported)
	{
		OutputDebugString(L"Warning: DirectX Raytracing is not supported by your GPU and driver.\n\n");

		if (!isFallbackSupported)
			OutputDebugString(L"Could not enable compute based fallback raytracing support (D3D12EnableExperimentalFeatures() failed).\n"\
				L"Possible reasons: your OS is not in developer mode.\n\n");
		ThrowIfFailed(isFallbackSupported ? S_OK : E_FAIL);
	}
}
