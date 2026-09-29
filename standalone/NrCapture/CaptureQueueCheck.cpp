#include "CaptureQueue.h"
#include "CapturePacer.h"
#include <DirectXPackedVector.h>
#include <d3d12sdklayers.h>
#include <thread>
#include <iostream>
#include <vector>
#include <cmath>
namespace nr {
void CaptureQueueCheck() {
    using namespace DirectX::PackedVector;
    Gpu gpu; CaptureQueue queue(gpu);
    auto make = [&](UINT w, UINT h, bool hdr, unsigned serial) {
        std::vector<HALF> half(size_t(w) * h * 4);
        std::vector<unsigned char> bytes(size_t(w) * h * 4);
        for (UINT i = 0; i < w * h; ++i) {
            const float value = hdr ? (i + serial) / 16.f : ((i + serial) % 256) / 255.f;
            for (UINT c = 0; c < 3; ++c) {
                half[i * 4 + c] = XMConvertFloatToHalf(value);
                bytes[i * 4 + c] = static_cast<unsigned char>((i + serial) % 256);
            }
            half[i * 4 + 3] = XMConvertFloatToHalf(1); bytes[i * 4 + 3] = 255;
        }
        D3D11_TEXTURE2D_DESC desc{}; desc.Width = w; desc.Height = h;
        desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
        desc.Format = hdr ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.Usage = D3D11_USAGE_DEFAULT;
        D3D11_SUBRESOURCE_DATA data{hdr ? static_cast<void*>(half.data()) : bytes.data(), w * (hdr ? 8u : 4u), 0};
        CaptureFrame frame; Check(queue.Device()->CreateTexture2D(&desc, &data, &frame.texture), "Queue fixture");
        frame.crop = {0, 0, 0, w, h, 1}; frame.timestamp = serial;
        return frame;
    };
    auto read = [&] {
        auto* input = queue.Input(); const auto desc = input->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{}; UINT64 bytes = 0;
        gpu.device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
        auto buffer = gpu.Buffer(bytes, D3D12_HEAP_TYPE_READBACK);
        gpu.Begin(); Barrier(gpu.commands.Get(), input, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION src{}, dst{}; src.pResource = input; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.pResource = buffer.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = footprint;
        gpu.commands->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        Barrier(gpu.commands.Get(), input, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
        gpu.Submit(); if (!gpu.Drain()) throw std::runtime_error("Queue readback timeout.");
        void* mapped = nullptr; Check(buffer->Map(0, nullptr, &mapped), "Queue readback");
        std::vector<HALF> result(size_t(queue.width) * queue.height * 4);
        for (UINT y = 0; y < queue.height; ++y)
            memcpy(result.data() + size_t(y) * queue.width * 4, static_cast<BYTE*>(mapped) + footprint.Offset + size_t(y) * footprint.Footprint.RowPitch, queue.width * 8);
        buffer->Unmap(0, nullptr); return result;
    };
    for (bool hdr : {false, true}) for (unsigned rotation = 1; rotation <= 4; ++rotation) {
        auto frame = make(5, 3, hdr, 100); frame.rotation = DXGI_MODE_ROTATION(rotation);
        queue.Publish(frame);
        if (!queue.Next()) throw std::runtime_error("Missing published frame.");
        const auto pixels = read();
        for (UINT y = 0; y < queue.height; ++y) for (UINT x = 0; x < queue.width; ++x) {
            UINT sx = x, sy = y;
            if (rotation == 2) { sx = y; sy = 2 - x; }
            if (rotation == 3) { sx = 4 - x; sy = 2 - y; }
            if (rotation == 4) { sx = 4 - y; sy = x; }
            float expected = hdr ? (sy * 5 + sx + 100) / 16.f : (sy * 5 + sx + 100) / 255.f;
            if (!hdr) expected = expected <= .04045f ? expected / 12.92f : std::pow((expected + .055f) / 1.055f, 2.4f);
            const size_t offset = (size_t(y) * queue.width + x) * 4;
            for (unsigned c = 0; c < 3; ++c)
                if (std::abs(XMConvertHalfToFloat(pixels[offset + c]) - expected) > (hdr ? .004f : .0005f))
                    throw std::runtime_error("Capture conversion/rotation changed pixels incorrectly.");
            if (XMConvertHalfToFloat(pixels[offset + 3]) != 1) throw std::runtime_error("Capture alpha mismatch.");
        }
    }
    auto cropped = make(5, 3, true, 100); cropped.crop = {1, 1, 0, 4, 3, 1};
    queue.Publish(cropped); queue.Next();
    if (queue.width != 3 || queue.height != 2 || XMConvertHalfToFloat(read()[0]) != 106.f / 16)
        throw std::runtime_error("Capture crop mismatch.");
    const auto pinned = read();
    std::exception_ptr failure;
    std::jthread producer([&] {
        try { for (unsigned i = 0; i < 60; ++i) queue.Publish(make(16, 8, true, i)); }
        catch (...) { failure = std::current_exception(); }
    });
    // Publishing/resizing free slots must never overwrite the processing slot.
    for (unsigned i = 0; i < 12; ++i) if (read() != pinned) throw std::runtime_error("Producer overwrote pinned capture.");
    producer.join(); if (failure) std::rethrow_exception(failure);
    if (!queue.Next() || queue.timestamp != 59 || queue.width != 16 || queue.height != 8)
        throw std::runtime_error("Capture queue did not keep newest frame.");
    if (queue.Next()) throw std::runtime_error("Capture queue replayed an old frame.");
    if (XMConvertHalfToFloat(read()[0]) != 59.f / 16) throw std::runtime_error("Newest capture corrupted.");
    // Exercise the producer's real timer and GPU publication using owned images.
    // Source updates are sampled by timestamp: no desktop capture or game is used.
    for (unsigned sourceFps : {120u, 240u}) {
        Settings paced; paced.maxFps = 116; paced.frameGeneration = 2;
        CapturePacer pacing(paced.ProcessingFps());
        auto frame = make(64, 32, true, 0);
        const auto start = ClockMs();
        std::vector<double> starts;
        std::stop_source stop;
        while (starts.size() < 18) {
            if (!pacing.Wait(stop.get_token())) throw std::runtime_error("Unexpected capture stop.");
            const auto now = ClockMs();
            frame.timestamp = std::floor((now - start) * sourceFps / 1000.0);
            queue.Publish(frame); pacing.Submitted(now); starts.push_back(now);
        }
        const double rate = 17000.0 / (starts.back() - starts.front());
        if (rate > paced.ProcessingFps() + .2 || rate < 45 || !queue.Next() || queue.timestamp != frame.timestamp)
            throw std::runtime_error("Capture pacing lost its rate bound or newest frame.");
        std::cout << std::format("PASS simulated {} fps source: {:.1f} GPU copies/s at 116/2x, newest source frame {}\n",
            sourceFps, rate, unsigned(frame.timestamp));
    }
    {
        CapturePacer pacing(1);
        std::stop_source stop;
        pacing.Wait(stop.get_token()); pacing.Submitted(ClockMs());
        std::jthread change([&] { Sleep(10); pacing.SetRate(240); });
        const auto start = ClockMs();
        if (!pacing.Wait(stop.get_token()) || ClockMs() - start > 200)
            throw std::runtime_error("Capture did not wake when its FPS target increased.");
        change.join();
        pacing.SetRate(0);
        if (!pacing.Wait(stop.get_token())) throw std::runtime_error("Uncapped capture stopped.");
        pacing.Submitted(ClockMs()); pacing.SetRate(1);
        std::jthread cancel([&] { Sleep(10); stop.request_stop(); });
        const auto stopping = ClockMs();
        if (pacing.Wait(stop.get_token()) || ClockMs() - stopping > 200)
            throw std::runtime_error("Stop did not interrupt capture pacing.");
        std::cout << "PASS capture FPS changes, uncapped mode and stop during a low-rate wait\n";
    }
    ComPtr<ID3D12InfoQueue> diagnostics; gpu.device.As(&diagnostics);
    if (diagnostics) for (UINT64 i = 0; i < diagnostics->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
        SIZE_T size = 0; diagnostics->GetMessage(i, nullptr, &size); std::vector<BYTE> bytes(size);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(bytes.data());
        Check(diagnostics->GetMessage(i, message, &size), "Queue diagnostics");
        if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) throw std::runtime_error(message->pDescription);
    }
    auto settings = Settings{}; settings.captureBackend = 1;
    const auto file = std::filesystem::temp_directory_path() / std::format(L"capture-backend-{}.ini", GetCurrentProcessId());
    SaveSettings(settings, file); const auto loaded = LoadSettings(file); std::filesystem::remove(file);
    if (loaded.captureBackend != 1) throw std::runtime_error("Capture backend setting did not persist.");
    std::cout << "PASS capture queue: SDR linearization, FP16 HDR highlights, four rotations, crop, concurrent publication, pinned-frame protection, newest-only selection, resize and backend persistence\n";
}
}
