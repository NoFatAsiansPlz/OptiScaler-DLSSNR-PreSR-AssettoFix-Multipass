#include "CaptureBackend.h"
namespace nr {
class DxgiCapture final : public CaptureBackend {
    CaptureSource source;
    ComPtr<IDXGIOutputDuplication> duplication;
    DXGI_MODE_ROTATION rotation = DXGI_MODE_ROTATION_IDENTITY;
    bool held = false;
public:
    DxgiCapture(ID3D11Device* device, IDXGIAdapter4* adapter, CaptureSource target) : source(target) {
        if (!source.monitor) throw std::runtime_error("DXGI captures whole screens. Select a Screen source or use WGC for a window.");
        for (UINT i = 0; ; ++i) {
            ComPtr<IDXGIOutput> output;
            const auto result = adapter->EnumOutputs(i, &output);
            if (result == DXGI_ERROR_NOT_FOUND) break;
            Check(result, "Enumerate capture output");
            DXGI_OUTPUT_DESC desc{}; Check(output->GetDesc(&desc), "Capture output description");
            if (desc.Monitor != source.monitor) continue;
            ComPtr<IDXGIOutput5> extended; Check(output.As(&extended), "HDR-capable duplication");
            const DXGI_FORMAT formats[]{DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_B8G8R8A8_UNORM};
            Check(extended->DuplicateOutput1(device, 0, UINT(std::size(formats)), formats, &duplication), "DXGI desktop duplication");
            DXGI_OUTDUPL_DESC capture{}; duplication->GetDesc(&capture); rotation = capture.Rotation;
            Log(std::format("DXGI duplication format={}, rotation={}", UINT(capture.ModeDesc.Format), UINT(rotation)));
            return;
        }
        throw std::runtime_error("The selected screen is not connected to the processing GPU. Use WGC for this screen.");
    }
    ~DxgiCapture() override { if (held) duplication->ReleaseFrame(); }
    bool Closed() const override { return !source.Exists(); }
    bool Next(CaptureFrame& frame) override {
        if (held) { Check(duplication->ReleaseFrame(), "Release desktop frame"); held = false; }
        DXGI_OUTDUPL_FRAME_INFO info{}; ComPtr<IDXGIResource> resource;
        const auto result = duplication->AcquireNextFrame(10, &info, &resource);
        if (result == DXGI_ERROR_WAIT_TIMEOUT) return false;
        if (result == DXGI_ERROR_ACCESS_LOST)
            throw std::runtime_error("DXGI capture was interrupted by a display/session change. Start capture again.");
        Check(result, "Acquire desktop frame"); held = true;
        // Hardware cursor-only updates must not drive NR's temporal history/FPS.
        if (!info.LastPresentTime.QuadPart) return false;
        ++receivedFrames; // Count acquired images, not unobserved updates accumulated by Windows.
        Check(resource.As(&frame.texture), "Desktop capture texture");
        D3D11_TEXTURE2D_DESC desc{}; frame.texture->GetDesc(&desc);
        frame.crop = {0, 0, 0, desc.Width, desc.Height, 1}; frame.rotation = rotation;
        LARGE_INTEGER frequency{}; QueryPerformanceFrequency(&frequency);
        frame.timestamp = info.LastPresentTime.QuadPart * 1000.0 / frequency.QuadPart;
        return true;
    }
};
std::unique_ptr<CaptureBackend> CreateDxgiCapture(ID3D11Device* device, IDXGIAdapter4* adapter, CaptureSource source) {
    return std::make_unique<DxgiCapture>(device, adapter, source);
}
}
