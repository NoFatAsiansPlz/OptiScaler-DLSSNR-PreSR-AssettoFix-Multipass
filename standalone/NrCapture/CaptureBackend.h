#pragma once
#include "CaptureSource.h"
#include <d3d11_4.h>
#include <memory>
namespace nr {
struct CaptureFrame {
    ComPtr<ID3D11Texture2D> texture;
    D3D11_BOX crop{};
    DXGI_MODE_ROTATION rotation = DXGI_MODE_ROTATION_IDENTITY;
    double timestamp = 0;
};
// The producer retains a returned surface until its copy finishes, before calling Next again.
class CaptureBackend {
public:
    virtual ~CaptureBackend() = default;
    virtual bool Next(CaptureFrame& frame) = 0;
    virtual bool Closed() const = 0;
    uint64_t receivedFrames = 0;
};
std::unique_ptr<CaptureBackend> CreateWgcCapture(ID3D11Device* device, CaptureSource source);
std::unique_ptr<CaptureBackend> CreateDxgiCapture(ID3D11Device* device, IDXGIAdapter4* adapter, CaptureSource source);
}
