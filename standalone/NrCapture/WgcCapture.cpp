#include "CaptureBackend.h"
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Security.Authorization.AppCapabilityAccess.h>
#include <atomic>
namespace nr {
using namespace winrt::Windows::Graphics::Capture;
using winrt::Windows::Graphics::DirectX::DirectXPixelFormat;
class WgcCapture final : public CaptureBackend {
    CaptureSource source;
    std::shared_ptr<Handle> arrived = std::make_shared<Handle>(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    std::shared_ptr<std::atomic_bool> closed = std::make_shared<std::atomic_bool>(false);
    GraphicsCaptureItem item{nullptr};
    Direct3D11CaptureFramePool pool{nullptr};
    GraphicsCaptureSession session{nullptr};
    Direct3D11CaptureFrame held{nullptr};
    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice captureDevice{nullptr};
    winrt::event_token closedToken{}, frameToken{};
    winrt::Windows::Graphics::SizeInt32 size{};
public:
    WgcCapture(ID3D11Device* device, CaptureSource target) : source(target) {
        if (!arrived->value) Check(HRESULT_FROM_WIN32(GetLastError()), "WGC frame event");
        if (!GraphicsCaptureSession::IsSupported()) throw std::runtime_error("Windows Graphics Capture is unavailable.");
        auto interop = winrt::get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        if (source.monitor) Check(interop->CreateForMonitor(source.monitor, winrt::guid_of<GraphicsCaptureItem>(), winrt::put_abi(item)), "Capture screen");
        else Check(interop->CreateForWindow(source.window, winrt::guid_of<GraphicsCaptureItem>(), winrt::put_abi(item)), "Capture window");
        closedToken = item.Closed([flag = closed, signal = arrived](auto&&, auto&&) { *flag = true; SetEvent(signal->value); });
        ComPtr<IDXGIDevice> dxgi; Check(device->QueryInterface(IID_PPV_ARGS(&dxgi)), "Capture DXGI device");
        winrt::com_ptr<IInspectable> inspect;
        Check(CreateDirect3D11DeviceFromDXGIDevice(dxgi.Get(), inspect.put()), "WinRT capture device");
        captureDevice = inspect.as<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();
        size = item.Size();
        pool = Direct3D11CaptureFramePool::CreateFreeThreaded(captureDevice, DirectXPixelFormat::R16G16B16A16Float, 3, size);
        frameToken = pool.FrameArrived([signal = arrived](auto&&, auto&&) { SetEvent(signal->value); });
        session = pool.CreateCaptureSession(item); session.IsCursorCaptureEnabled(false);
        using winrt::Windows::Foundation::Metadata::ApiInformation;
        if (ApiInformation::IsPropertyPresent(L"Windows.Graphics.Capture.GraphicsCaptureSession", L"MinUpdateInterval")) {
            try {
                session.MinUpdateInterval(std::chrono::milliseconds(1));
                Log(std::format("WGC minimum update interval: {:.3f} ms", session.MinUpdateInterval().count() / 10000.0));
            } catch (const winrt::hresult_error& e) { Log("WGC interval: " + winrt::to_string(e.message())); }
        }
        if (source.monitor && ApiInformation::IsPropertyPresent(L"Windows.Graphics.Capture.GraphicsCaptureSession", L"IsBorderRequired")) {
            try {
                GraphicsCaptureAccess::RequestAccessAsync(GraphicsCaptureAccessKind::Borderless).get();
                session.IsBorderRequired(false);
            } catch (const winrt::hresult_error& e) { Log("WGC border remains enabled: " + winrt::to_string(e.message())); }
        }
        session.StartCapture();
    }
    ~WgcCapture() override {
        if (session) session.Close();
        if (pool) { pool.FrameArrived(frameToken); pool.Close(); }
        if (held) held.Close();
        if (item) item.Closed(closedToken);
    }
    bool Closed() const override { return closed->load() || !source.Exists(); }
    bool Next(CaptureFrame& frame) override {
        if (held) { held.Close(); held = nullptr; }
        while (auto next = pool.TryGetNextFrame()) {
            ++receivedFrames; if (held) held.Close(); held = std::move(next);
        }
        if (!held) { WaitForSingleObject(arrived->value, 10); return false; }
        const auto extent = held.ContentSize();
        if (extent.Width <= 0 || extent.Height <= 0) return false;
        if (extent.Width != size.Width || extent.Height != size.Height) {
            held.Close(); held = nullptr; size = extent;
            pool.Recreate(captureDevice, DirectXPixelFormat::R16G16B16A16Float, 3, size); return false;
        }
        RECT bounds{}; if (!source.Bounds(bounds)) return false;
        const UINT w = bounds.right - bounds.left, h = bounds.bottom - bounds.top;
        UINT x = 0, y = 0;
        if (UINT(size.Width) != w || UINT(size.Height) != h) {
            if (source.monitor) return false;
            RECT outer{}; Check(DwmGetWindowAttribute(source.window, DWMWA_EXTENDED_FRAME_BOUNDS, &outer, sizeof(outer)), "Capture window bounds");
            if (size.Width != outer.right - outer.left || size.Height != outer.bottom - outer.top ||
                bounds.left < outer.left || bounds.top < outer.top) return false;
            x = bounds.left - outer.left; y = bounds.top - outer.top;
        }
        if (x + w > UINT(size.Width) || y + h > UINT(size.Height)) return false;
        auto access = held.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
        Check(access->GetInterface(IID_PPV_ARGS(&frame.texture)), "WGC surface");
        frame.crop = {x, y, 0, x + w, y + h, 1};
        frame.timestamp = held.SystemRelativeTime().count() / 10000.0;
        return true;
    }
};
std::unique_ptr<CaptureBackend> CreateWgcCapture(ID3D11Device* device, CaptureSource source) {
    return std::make_unique<WgcCapture>(device, source);
}
}
