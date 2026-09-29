#pragma once
#include "Gpu.h"
#include "FrameGenerationFocus.h"
#include <sl.h>
#include <sl_dlss_g.h>
#include <sl_reflex.h>
#include <sl_pcl.h>
namespace nr {
// Streamline only wraps this application's interfaces. Never loads into a captured process.
class FrameGeneration {
public:
    explicit FrameGeneration(Gpu& gpu, const std::filesystem::path& directory = {}, const std::filesystem::path& nrRuntime = {});
    ~FrameGeneration();
    FrameGeneration(const FrameGeneration&) = delete;
    void CreateSwapchain(HWND window, DXGI_SWAP_CHAIN_DESC1 desc, IDXGISwapChain3** swapchain);
    void Configure(UINT width, UINT height, DXGI_FORMAT format, unsigned multiplier, bool active = true);
    void Disable();
    void Quiesce(IDXGISwapChain3* swapchain);
    void BeginFrame(float targetFps = 0);
    void Tag(ID3D12Resource* colour, ID3D12Resource* depth, ID3D12Resource* motion, bool reset);
    void BeforePresent();
    void AfterPresent();
    uint64_t PresentedFrames() const { return presented; }
    unsigned MaximumMultiplier() const { return maximum; }
    bool Enabled() const { return enabled; }
private:
    Gpu& gpu;
    HMODULE module = nullptr;
    std::unique_ptr<FrameGenerationFocus> focus;
    HWND outputWindow = nullptr;
    ComPtr<ID3D12Device> proxyDevice;
    ComPtr<IDXGIFactory4> proxyFactory;
    bool initialized = false, enabled = false, optionsDirty = false;
    unsigned maximum = 1, width = 0, height = 0;
    uint32_t frameIndex = 0;
    uint64_t presented = 0;
    sl::ViewportHandle viewport{0};
    sl::FrameToken* token = nullptr;
    sl::DLSSGOptions options{};
    PFun_slShutdown* shutdown = nullptr;
    PFun_slUpgradeInterface* upgrade = nullptr;
    PFun_slGetNewFrameToken* newToken = nullptr;
    PFun_slSetConstants* setConstants = nullptr;
    PFun_slSetTagForFrame* setTags = nullptr;
    PFun_slDLSSGSetOptions* setOptions = nullptr;
    PFun_slDLSSGGetState* getState = nullptr;
    PFun_slReflexSetOptions* reflexOptions = nullptr;
    PFun_slReflexSleep* sleep = nullptr;
    PFun_slPCLSetMarker* marker = nullptr;
    static void CheckSl(sl::Result result, const char* operation);
};
}
