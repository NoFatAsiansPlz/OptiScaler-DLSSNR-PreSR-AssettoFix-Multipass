#include "FrameGeneration.h"
#include "AdaMfgUnlock.h"
#include <DirectXMath.h>
namespace nr {
namespace {
template<class T> T* Function(HMODULE module, const char* name) {
    auto address = GetProcAddress(module, name);
    if (!address) throw std::runtime_error(std::string("Streamline export missing: ") + name);
    return reinterpret_cast<T*>(address);
}
void StreamlineLog(sl::LogType, const char* text) noexcept {
    try { if (text) Log(std::string("Streamline: ") + text); }
    catch (...) { /* Logging must not turn a vendor error into another exception. */ }
}
}
void FrameGeneration::CheckSl(sl::Result result, const char* operation) {
    if (result == sl::Result::eOk) return;
    const char* detail = "See the Streamline log in the application data folder.";
    if (result == sl::Result::eErrorExceptionHandler)
        detail = "Streamline caught an internal exception. Restart Display Filter before retrying. Crash logs are in %ProgramData%/NVIDIA/Streamline/display_filter.";
    else if (result == sl::Result::eErrorOSDisabledHWS)
        detail = "DLSS Frame Generation requires Hardware-accelerated GPU scheduling in Windows Graphics settings.";
    else if (result == sl::Result::eErrorDriverOutOfDate)
        detail = "The installed NVIDIA driver is too old for this frame-generation runtime.";
    throw std::runtime_error(std::format("{} failed (Streamline {}). {}", operation, int(result), detail));
}
FrameGeneration::FrameGeneration(Gpu& g, const std::filesystem::path& directory, const std::filesystem::path& nrRuntime) : gpu(g) {
    wchar_t executable[32768]{}; GetModuleFileNameW(nullptr, executable, DWORD(std::size(executable)));
    const auto folder = directory.empty() ? std::filesystem::path(executable).parent_path() / L"runtime/streamline" : directory;
    for (auto name : {L"sl.interposer.dll", L"sl.common.dll", L"sl.dlss_g.dll", L"sl.reflex.dll", L"sl.pcl.dll", L"nvngx_dlssg.dll"})
        if (!std::filesystem::is_regular_file(folder / name))
            throw std::runtime_error("Frame generation runtime is incomplete: " + Narrow((folder / name).wstring()));
    module = LoadLibraryExW((folder / L"sl.interposer.dll").c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) Check(HRESULT_FROM_WIN32(GetLastError()), "Load Streamline");
    // Keep module code available for any asynchronous driver callbacks until process exit.
    HMODULE pinned = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(module), &pinned)) {
        const auto error = HRESULT_FROM_WIN32(GetLastError()); FreeLibrary(module); module = nullptr;
        Check(error, "Retain Streamline");
    }
    try {
        const auto init = Function<PFun_slInit>(module, "slInit");
        shutdown = Function<PFun_slShutdown>(module, "slShutdown");
        upgrade = Function<PFun_slUpgradeInterface>(module, "slUpgradeInterface");
        newToken = Function<PFun_slGetNewFrameToken>(module, "slGetNewFrameToken");
        setConstants = Function<PFun_slSetConstants>(module, "slSetConstants");
        setTags = Function<PFun_slSetTagForFrame>(module, "slSetTagForFrame");
        const auto plugins = folder.wstring(), bundled = folder.parent_path().wstring(), logs = DataDirectory().wstring();
        const auto models = nrRuntime.empty() ? bundled : std::filesystem::canonical(nrRuntime).parent_path().wstring();
        // Keep the same search paths as the application's earlier NGX initialization.
        const wchar_t* paths[]{plugins.c_str(), models.c_str(), bundled.c_str()};
        const sl::Feature features[]{sl::kFeatureDLSS_G, sl::kFeatureReflex, sl::kFeaturePCL};
        sl::Preferences pref{}; pref.pathsToPlugins = paths; pref.numPathsToPlugins = UINT(std::size(paths));
        pref.pathToLogsAndData = logs.c_str(); pref.logMessageCallback = StreamlineLog;
        pref.flags = sl::PreferenceFlags::eUseManualHooking | sl::PreferenceFlags::eUseDXGIFactoryProxy |
            sl::PreferenceFlags::eDisableCLStateTracking | sl::PreferenceFlags::eUseFrameBasedResourceTagging;
        pref.featuresToLoad = features; pref.numFeaturesToLoad = UINT(std::size(features));
        pref.engine = sl::EngineType::eCustom; pref.engineVersion = "OptiScalerNR-1";
        pref.projectId = "969c9e2d-5cbf-4d4a-9693-c2ac4f8f52de";
        pref.renderAPI = sl::RenderAPI::eD3D12;
        CheckSl(init(pref, sl::kSDKVersion), "Streamline initialization"); initialized = true;
        DXGI_ADAPTER_DESC3 desc{}; Check(g.adapter->GetDesc3(&desc), "FG adapter");
        sl::AdapterInfo adapter{}; adapter.deviceLUID = reinterpret_cast<uint8_t*>(&desc.AdapterLuid);
        adapter.deviceLUIDSizeInBytes = sizeof(desc.AdapterLuid);
        CheckSl(Function<PFun_slIsFeatureSupported>(module, "slIsFeatureSupported")(sl::kFeatureDLSS_G, adapter), "DLSS Frame Generation support");
        CheckSl(Function<PFun_slSetD3DDevice>(module, "slSetD3DDevice")(g.device.Get()), "Streamline device");
        const auto featureFunction = Function<PFun_slGetFeatureFunction>(module, "slGetFeatureFunction");
        void* commonFunction = nullptr;
        CheckSl(featureFunction(sl::kFeatureCommon, "slSetTagForFrame", commonFunction), "Streamline common plugin");
        auto feature = [&](sl::Feature id, const char* name, auto& destination) {
            void* address = nullptr; CheckSl(featureFunction(id, name, address), name);
            destination = reinterpret_cast<std::remove_reference_t<decltype(destination)>>(address);
        };
        feature(sl::kFeatureDLSS_G, "slDLSSGSetOptions", setOptions);
        feature(sl::kFeatureDLSS_G, "slDLSSGGetState", getState);
        AdaMfgUnlock::PreparePlugin(reinterpret_cast<void*>(getState));
        feature(sl::kFeatureReflex, "slReflexSetOptions", reflexOptions);
        feature(sl::kFeatureReflex, "slReflexSleep", sleep);
        feature(sl::kFeaturePCL, "slPCLSetMarker", marker);
        focus = std::make_unique<FrameGenerationFocus>(commonFunction);
        // Register our presenting queue through the documented device proxy.
        proxyDevice = g.device; auto* raw = proxyDevice.Detach();
        const auto result = upgrade(reinterpret_cast<void**>(&raw)); proxyDevice.Attach(raw); CheckSl(result, "Streamline device proxy");
        D3D12_COMMAND_QUEUE_DESC q{}; ComPtr<ID3D12CommandQueue> queue;
        Check(proxyDevice->CreateCommandQueue(&q, IID_PPV_ARGS(&queue)), "FG presenting queue");
        g.queue = queue;
        sl::ReflexOptions reflex{}; reflex.mode = sl::ReflexMode::eLowLatency;
        // Our interruptible pacer applies target/multiplier. Avoid a second driver FPS limiter.
        CheckSl(reflexOptions(reflex), "Enable Reflex");
        sl::DLSSGState state{}; CheckSl(getState(viewport, state, &options), "DLSS FG capabilities");
        maximum = state.numFramesToGenerateMax + 1;
        if (AdaMfgUnlock::RequestedOnAda())
            maximum = std::min(maximum, AdaMfgUnlock::MaximumMultiplier());
        if (AdaMfgUnlock::RequestedOnAda() && !AdaMfgUnlock::Active()) {
            maximum = std::min(maximum, 2u);
            Log("RTX 40 MFG unlock is incomplete for this runtime; retaining the 2x limit. See earlier MFG diagnostics.");
        }
        Log(std::format("DLSS FG initialized; maximum multiplier {}x (synthetic guides).", maximum));
    } catch (...) {
        focus.reset();
        if (initialized) shutdown(); initialized = false;
        FreeLibrary(module); module = nullptr; throw;
    }
}
FrameGeneration::~FrameGeneration() {
    if (initialized) {
        try { Disable(); } catch (const std::exception& e) { Log(e.what()); }
        focus.reset();
        const auto result = shutdown(); if (result != sl::Result::eOk) Log("Streamline shutdown failed.");
    }
    if (module) FreeLibrary(module);
}
void FrameGeneration::CreateSwapchain(HWND window, DXGI_SWAP_CHAIN_DESC1 desc, IDXGISwapChain3** swapchain) {
    outputWindow = window;
    if (!proxyFactory) {
        Check(gpu.factory.As(&proxyFactory), "FG factory");
        auto* raw = proxyFactory.Detach(); const auto result = upgrade(reinterpret_cast<void**>(&raw)); proxyFactory.Attach(raw);
        CheckSl(result, "Streamline factory proxy");
    }
    ComPtr<IDXGISwapChain1> chain;
    Check(proxyFactory->CreateSwapChainForHwnd(gpu.queue.Get(), window, &desc, nullptr, nullptr, &chain), "DLSS FG swapchain");
    Check(chain->QueryInterface(IID_PPV_ARGS(swapchain)), "DLSS FG swapchain3");
}
void FrameGeneration::Configure(UINT w, UINT h, DXGI_FORMAT format, unsigned multiplier, bool active) {
    if (multiplier < 2 || multiplier > maximum)
        throw std::runtime_error(std::format("DLSS FG {}x is unavailable; verified maximum is {}x. See capture.log for runtime and RTX 40 MFG diagnostics.", multiplier, maximum));
    width = w; height = h; options.mode = active ? sl::DLSSGMode::eOn : sl::DLSSGMode::eOff; options.numFramesToGenerate = multiplier - 1;
    options.numBackBuffers = 3; options.mvecDepthWidth = options.colorWidth = w;
    options.mvecDepthHeight = options.colorHeight = h;
    options.colorBufferFormat = options.hudLessBufferFormat = UINT(format);
    options.depthBufferFormat = DXGI_FORMAT_R32_FLOAT; options.mvecBufferFormat = DXGI_FORMAT_R16G16_FLOAT;
    options.enableUserInterfaceRecomposition = sl::Boolean::eFalse;
    options.flags = sl::DLSSGFlags::eRetainResourcesWhenOff;
    options.queueParallelismMode = sl::DLSSGQueueParallelismMode::eBlockPresentingClientQueue;
    if (active) focus->Enable(outputWindow); else focus->Disable();
    optionsDirty = true; enabled = active;
}
void FrameGeneration::Disable() {
    if (focus) focus->Disable();
    if (!enabled) return;
    options.mode = sl::DLSSGMode::eOff; CheckSl(setOptions(viewport, options), "Disable DLSS FG"); enabled = false;
    optionsDirty = false;
}
void FrameGeneration::BeginFrame(float targetFps) {
    CheckSl(newToken(token, &frameIndex), "Frame generation token"); ++frameIndex;
    // Fixed FG normally ignores this. If NVIDIA App forces Dynamic FG, retain the
    // user's output target rather than silently targeting the monitor refresh rate.
    if (options.dynamicTargetFrameRate != targetFps) {
        options.dynamicTargetFrameRate = targetFps;
        optionsDirty = true;
    }
    CheckSl(sleep(*token), "Reflex sleep");
    CheckSl(marker(sl::PCLMarker::eSimulationStart, *token), "Reflex simulation start");
    CheckSl(marker(sl::PCLMarker::eSimulationEnd, *token), "Reflex simulation end");
    if (optionsDirty) {
        CheckSl(setOptions(viewport, options), "DLSS FG options"); optionsDirty = false;
    }
    CheckSl(marker(sl::PCLMarker::eRenderSubmitStart, *token), "Reflex render start");
}
void FrameGeneration::Quiesce(IDXGISwapChain3* swapchain) {
    if (!enabled) return;
    Disable();
    // SetOptions takes effect only at Present. Consume Off before manipulating the
    // HWND, so Streamline returns presentation to this thread and flushes its worker.
    if (swapchain) {
        swapchain->GetCurrentBackBufferIndex();
        Check(swapchain->Present(0, 0), "Quiesce DLSS FG presentation");
    }
    if (!gpu.Drain()) throw std::runtime_error("GPU timeout quiescing DLSS FG.");
}
void FrameGeneration::Tag(ID3D12Resource* colour, ID3D12Resource* depth, ID3D12Resource* motion, bool reset) {
    using namespace DirectX;
    sl::Constants c{};
    const auto projection = XMMatrixPerspectiveFovRH(XM_PIDIV4, float(width) / height, .1f, 1000.f);
    XMFLOAT4X4 matrix; XMStoreFloat4x4(&matrix, projection); memcpy(&c.cameraViewToClip, &matrix, sizeof(matrix));
    XMStoreFloat4x4(&matrix, XMMatrixInverse(nullptr, projection)); memcpy(&c.clipToCameraView, &matrix, sizeof(matrix));
    XMStoreFloat4x4(&matrix, XMMatrixIdentity());
    for (auto* target : {&c.clipToLensClip, &c.clipToPrevClip, &c.prevClipToClip}) memcpy(target, &matrix, sizeof(matrix));
    c.cameraPos = {0,0,0}; c.cameraUp = {0,1,0}; c.cameraRight = {1,0,0}; c.cameraFwd = {0,0,-1};
    c.cameraNear = .1f; c.cameraFar = 1000; c.cameraFOV = XM_PIDIV4; c.cameraAspectRatio = float(width) / height;
    c.jitterOffset = {0,0}; c.cameraPinholeOffset = {0,0}; c.mvecScale = {1.f / width, 1.f / height};
    c.depthInverted = sl::eFalse; c.cameraMotionIncluded = sl::eTrue; c.motionVectors3D = sl::eFalse;
    c.motionVectorsDilated = sl::eTrue; c.motionVectorsJittered = sl::eFalse; c.orthographicProjection = sl::eFalse;
    c.reset = reset ? sl::eTrue : sl::eFalse;
    CheckSl(setConstants(c, *token, viewport), "DLSS FG synthetic constants");
    sl::Resource colourTag{sl::ResourceType::eTex2d, colour, nullptr, nullptr, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
    sl::Resource depthTag{sl::ResourceType::eTex2d, depth, nullptr, nullptr, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
    sl::Resource motionTag{sl::ResourceType::eTex2d, motion, nullptr, nullptr, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
    const sl::Extent extent{0,0,width,height};
    sl::ResourceTag tags[]{
        {&colourTag, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eValidUntilPresent, &extent},
        {&depthTag, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent, &extent},
        {&motionTag, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent, &extent}};
    CheckSl(setTags(*token, viewport, tags, UINT(std::size(tags)), gpu.commands.Get()), "DLSS FG input tags");
}
void FrameGeneration::BeforePresent() {
    CheckSl(marker(sl::PCLMarker::eRenderSubmitEnd, *token), "Reflex render end");
    CheckSl(marker(sl::PCLMarker::ePresentStart, *token), "Reflex present start");
}
void FrameGeneration::AfterPresent() {
    CheckSl(marker(sl::PCLMarker::ePresentEnd, *token), "Reflex present end");
    sl::DLSSGState state{}; CheckSl(getState(viewport, state, nullptr), "DLSS FG state");
    if (state.status != sl::DLSSGStatus::eOk)
        throw std::runtime_error(std::format("DLSS FG reports status 0x{:X}; generation is not working correctly.", unsigned(state.status)));
    presented += state.numFramesActuallyPresented;
    // Chain the SDK's explicit input-consumption fence into our completion fence.
    // Present returning (and our original render submission completing) is insufficient
    // to retire textures/descriptors while the asynchronous FG worker still uses them.
    if (state.inputsProcessingCompletionFence && state.lastPresentInputsProcessingCompletionFenceValue)
        Check(gpu.queue->Wait(static_cast<ID3D12Fence*>(state.inputsProcessingCompletionFence),
            state.lastPresentInputsProcessingCompletionFenceValue), "Wait for FG input consumption");
    Check(gpu.queue->Signal(gpu.fence.Get(), ++gpu.serial), "FG presentation completion fence");
}
}
