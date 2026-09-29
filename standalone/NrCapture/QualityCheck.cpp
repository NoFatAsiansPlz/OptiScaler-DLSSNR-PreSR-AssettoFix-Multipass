#include "Processor.h"
#include <dlssnr/DlssNr_Proxy.h>
#include <DirectXPackedVector.h>
#include <d3d12sdklayers.h>
#include <iostream>
#include <vector>
#include <cmath>

namespace nr {
namespace {
using DirectX::PackedVector::XMConvertHalfToFloat;
std::vector<float> Read(Gpu& gpu, ID3D12Resource* texture, D3D12_RESOURCE_STATES state) {
    const auto desc = texture->GetDesc();
    if (desc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT) throw std::runtime_error("Quality readback expects RGBA16F.");
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{}; UINT64 bytes = 0;
    gpu.device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
    auto buffer = gpu.Buffer(bytes, D3D12_HEAP_TYPE_READBACK);
    gpu.Begin(); Barrier(gpu.commands.Get(), texture, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION from{}, to{};
    from.pResource = texture; from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    to.pResource = buffer.Get(); to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; to.PlacedFootprint = footprint;
    gpu.commands->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    Barrier(gpu.commands.Get(), texture, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
    gpu.Submit(); if (!gpu.Drain()) throw std::runtime_error("Quality readback timeout.");
    void* mapped = nullptr; Check(buffer->Map(0, nullptr, &mapped), "Quality readback map");
    std::vector<float> values(size_t(desc.Width) * desc.Height * 4);
    for (UINT y = 0; y < desc.Height; ++y) {
        const auto* row = reinterpret_cast<const uint16_t*>(static_cast<const BYTE*>(mapped) + footprint.Offset + size_t(y) * footprint.Footprint.RowPitch);
        for (size_t x = 0; x < desc.Width * 4; ++x) values[size_t(y) * desc.Width * 4 + x] = XMConvertHalfToFloat(row[x]);
    }
    buffer->Unmap(0, nullptr); return values;
}
double Difference(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() != b.size()) throw std::runtime_error("Quality extents differ.");
    double sum = 0;
    for (size_t i = 0; i < a.size(); ++i) if (i % 4 != 3) {
        if (!std::isfinite(a[i]) || !std::isfinite(b[i])) throw std::runtime_error("Nonfinite quality output.");
        sum += std::abs(a[i] - b[i]);
    }
    return sum / (a.size() / 4 * 3);
}
}

struct QualityProbe {
    static void TemporalReference(Gpu& gpu, Processor& p, ID3D12Resource* input, Settings s) {
        s.estimateMotion = true; p.Resize(p.width, p.height, s);
        std::array<DlssNr::Proxy::Context, 2> models;
        std::array<ComPtr<ID3D12Resource>, 2> answers;
        auto clamped = gpu.Texture(p.modelWidth, p.modelHeight, DXGI_FORMAT_R16G16B16A16_FLOAT);
        auto submit = [&](DlssNr::Proxy::Context& context) {
            gpu.Submit(); ID3D12CommandList* lists[]{gpu.commands.Get()}; context.Submitted(gpu.queue.Get(), 1, lists);
            if (!gpu.Drain()) throw std::runtime_error("Temporal reference timeout.");
            context.ResetRecording(gpu.commands.Get()); context.Collect();
        };
        for (unsigned pass = 0; pass < s.passes; ++pass) {
            answers[pass] = gpu.Texture(p.modelWidth, p.modelHeight, DXGI_FORMAT_R16G16B16A16_FLOAT);
            auto model = s.model; if (pass) model.localTone = 0;
            gpu.Begin(); bool ready = false;
            const auto result = models[pass].Prepare(gpu.commands.Get(), gpu.device.Get(), p.modelWidth, p.modelHeight, model, 0, &ready);
            submit(models[pass]); Runtime::CheckResult((NVSDK_NGX_Result)result, "Temporal reference creation");
        }
        double maximum = 0;
        for (unsigned frame = 0; frame < 12; ++frame) {
            gpu.Begin(); p.Run(input, s, s.inputColour == 2, frame == 0, 1);
            gpu.Submit(); if (!gpu.Drain()) throw std::runtime_error("Standalone temporal timeout.");
            if (p.historyReset != (frame == 0)) throw std::runtime_error("Unexpected static-frame history reset.");
            const auto actual = Read(gpu, p.answer.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            auto* colour = p.proxy.Get();
            for (unsigned pass = 0; pass < s.passes; ++pass) {
                auto model = s.model; if (pass) model.localTone = 0;
                const DlssNr::Proxy::Frame f{colour, p.depth.Get(), p.opticalFlow->Vectors(), answers[pass].Get(),
                    {p.modelWidth, p.modelHeight}, {{0, 0, p.modelWidth, p.modelHeight}, {0, 0, p.modelWidth, p.modelHeight}}, false, frame == 0, 1, 1};
                gpu.Begin(); Barrier(gpu.commands.Get(), colour, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                bool evaluated = false;
                const auto result = models[pass].Run(gpu.commands.Get(), gpu.device.Get(), f, model, frame + 1, &evaluated);
                Barrier(gpu.commands.Get(), colour, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                submit(models[pass]); Runtime::CheckResult((NVSDK_NGX_Result)result, "Temporal reference evaluation");
                if (!evaluated) throw std::runtime_error("Temporal reference skipped evaluation.");
                if (pass + 1 < s.passes) {
                    gpu.Begin(); Barrier(gpu.commands.Get(), answers[pass].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                    DlssNrConstants clamp{}; clamp.Mode = DlssNrMode_ClampProxy; clamp.Width = p.modelWidth; clamp.Height = p.modelHeight;
                    p.Dispatch(14, clamp, answers[pass].Get(), nullptr, nullptr, clamped.Get(), nullptr);
                    Barrier(gpu.commands.Get(), answers[pass].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                    gpu.Submit(); if (!gpu.Drain()) throw std::runtime_error("Temporal clamp timeout.");
                    colour = clamped.Get();
                }
            }
            const auto expected = Read(gpu, answers[s.passes - 1].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            maximum = std::max(maximum, Difference(actual, expected));
            if (actual != expected) throw std::runtime_error(std::format("Temporal model mismatch on frame {}: {:.10f}", frame, maximum));
        }
        std::cout << std::format("OptiScaler temporal wrapper vs standalone: {} passes, 12 retained-history frames, max mean RGB difference {:.10f}\n", s.passes, maximum);
    }
    static void Reference(Gpu& gpu, Processor& p, ID3D12Resource* input, const Settings& s) {
        gpu.Begin(); p.Run(input, s, s.inputColour == 2, true, 1);
        gpu.Submit(); if (!gpu.Drain()) throw std::runtime_error("Standalone reference timeout.");
        const auto actual = Read(gpu, p.answer.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        auto reference = gpu.Texture(p.modelWidth, p.modelHeight, DXGI_FORMAT_R16G16B16A16_FLOAT);
        auto scratch = gpu.Texture(p.modelWidth, p.modelHeight, DXGI_FORMAT_R16G16B16A16_FLOAT);
        auto* colour = p.proxy.Get();
        std::array<DlssNr::Proxy::Context, 2> models;
        for (unsigned pass = 0; pass < s.passes; ++pass) {
            auto model = s.model; if (pass) model.localTone = 0;
            auto& context = models[pass]; bool ready = false, evaluated = false;
            auto submit = [&] {
                gpu.Submit(); ID3D12CommandList* lists[]{gpu.commands.Get()};
                context.Submitted(gpu.queue.Get(), 1, lists);
                if (!gpu.Drain()) throw std::runtime_error("OptiScaler reference timeout.");
                context.ResetRecording(gpu.commands.Get()); context.Collect();
            };
            gpu.Begin();
            const auto created = context.Prepare(gpu.commands.Get(), gpu.device.Get(), p.modelWidth, p.modelHeight, model, 0, &ready);
            submit(); Runtime::CheckResult((NVSDK_NGX_Result)created, "OptiScaler reference creation");
            const DlssNr::Proxy::Frame frame{colour, p.depth.Get(), p.motion.Get(), reference.Get(),
                {p.modelWidth, p.modelHeight}, {{0, 0, p.modelWidth, p.modelHeight}, {0, 0, p.modelWidth, p.modelHeight}}, false, true, 1, 1};
            gpu.Begin(); Barrier(gpu.commands.Get(), colour, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            const auto result = context.Run(gpu.commands.Get(), gpu.device.Get(), frame, model, 1, &evaluated);
            Barrier(gpu.commands.Get(), colour, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            submit(); Runtime::CheckResult((NVSDK_NGX_Result)result, "OptiScaler reference evaluation");
            if (!evaluated) throw std::runtime_error("OptiScaler reference did not evaluate.");
            if (pass + 1 < s.passes) {
                gpu.Begin(); Barrier(gpu.commands.Get(), reference.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                DlssNrConstants clamp{}; clamp.Mode = DlssNrMode_ClampProxy; clamp.Width = p.modelWidth; clamp.Height = p.modelHeight;
                p.Dispatch(14, clamp, reference.Get(), nullptr, nullptr, scratch.Get(), nullptr);
                Barrier(gpu.commands.Get(), reference.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                gpu.Submit(); if (!gpu.Drain()) throw std::runtime_error("Reference interpass timeout.");
                colour = scratch.Get();
            }
        }
        const auto expected = Read(gpu, reference.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        const auto difference = Difference(actual, expected);
        std::cout << std::format("Actual OptiScaler wrapper vs standalone: {} pass(es), model {}x{}, mean RGB difference {:.10f}\n",
            s.passes, p.modelWidth, p.modelHeight, difference);
        if (actual != expected) throw std::runtime_error("OptiScaler/standalone model output is not bit-identical.");
        if (p.modelWidth < p.width || p.modelHeight < p.height) {
            std::cout << "Subnative composition uses relative reconstruction; the native-reference invariants are checked by --resize-check.\n";
            return;
        }
        const auto actualFrame = Read(gpu, p.output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE);
        auto composed = gpu.Texture(p.width, p.height, DXGI_FORMAT_R16G16B16A16_FLOAT);
        auto linear = gpu.Texture(p.width, p.height, DXGI_FORMAT_R16G16B16A16_FLOAT);
        // Rebuild the in-game resolve constants independently of Processor::Run.
        DlssNrConstants resolve{}; resolve.Mode = DlssNrMode_Resolve; resolve.ApplyModel = 1;
        resolve.Width = p.width; resolve.Height = p.height; resolve.WhitePoint = s.referenceWhiteNits > 0 ? s.referenceWhiteNits / 80 : 1;
        resolve.ReversibleMode = s.HdrMode(); resolve.Passthrough = s.inputColour != 2;
        resolve.Transfer = s.transfer; resolve.TransferStrength = s.strength; resolve.ColourStrength = s.colourStrength;
        resolve.MaxRatio = 2; resolve.CompareZoom = 1; resolve.CompareSplit = 0.5f;
        resolve.SkinProtection = s.separateSkin; resolve.ShowSkinMask = s.previewSkin;
        resolve.SkinDetail = s.skinDetail; resolve.SkinColour = s.skinColour;
        resolve.EnvironmentDetail = s.environmentDetail; resolve.EnvironmentColour = s.environmentColour;
        gpu.Begin();
        for (auto* resource : {p.proxy.Get(), reference.Get(), p.original.Get()})
            Barrier(gpu.commands.Get(), resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        p.Dispatch(7, resolve, p.proxy.Get(), reference.Get(), p.original.Get(), composed.Get(), nullptr);
        auto* result = composed.Get();
        if (resolve.Passthrough) {
            Barrier(gpu.commands.Get(), input, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Barrier(gpu.commands.Get(), p.sdrInput.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Barrier(gpu.commands.Get(), composed.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            resolve.Mode = 12; p.Dispatch(56, resolve, composed.Get(), input, p.sdrInput.Get(), linear.Get(), nullptr);
            Barrier(gpu.commands.Get(), composed.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Barrier(gpu.commands.Get(), p.sdrInput.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Barrier(gpu.commands.Get(), input, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
            result = linear.Get();
        }
        for (auto* resource : {p.proxy.Get(), reference.Get(), p.original.Get()})
            Barrier(gpu.commands.Get(), resource, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        gpu.Submit(); if (!gpu.Drain()) throw std::runtime_error("Reference composition timeout.");
        const auto expectedFrame = Read(gpu, result, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        std::cout << std::format("OptiScaler resolve vs standalone: mean RGB difference {:.10f}\n", Difference(actualFrame, expectedFrame));
        if (actualFrame != expectedFrame) throw std::runtime_error("Reference composition is not bit-identical.");
    }
    static void Run(Gpu& gpu, Processor& p, ID3D12Resource* input, Settings s) {
        s.estimateMotion = false; s.upscaler = 0;
        p.Resize(p.width, p.height, s);
        Reference(gpu, p, input, s);
        TemporalReference(gpu, p, input, s);
        p.Resize(p.width, p.height, s);
        const auto original = Read(gpu, input, D3D12_RESOURCE_STATE_COMMON);
        std::vector<float> baseline;
        // Hold every pixel, dimension and tuning key fixed while changing only the synthetic guide.
        for (float z : {1.0f, 0.5f, 0.01f, 0.0f}) {
            gpu.Begin();
            ID3D12DescriptorHeap* heaps[]{p.heap.Get()}; gpu.commands->SetDescriptorHeaps(1, heaps);
            Barrier(gpu.commands.Get(), p.depth.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            const float value[]{z, z, z, z};
            gpu.commands->ClearUnorderedAccessViewFloat(p.GpuHandle(63), p.Cpu(63), p.depth.Get(), value, 0, nullptr);
            Barrier(gpu.commands.Get(), p.depth.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            const auto created = p.runtime->Create(gpu.commands.Get(), p.modelWidth, p.modelHeight, s.model, s.passes);
            gpu.Submit(); if (!gpu.Drain()) throw std::runtime_error("Quality feature creation timeout.");
            Runtime::CheckResult(created, "Quality feature creation");
            gpu.Begin(); p.Run(input, s, s.inputColour == 2, true, 1);
            gpu.Submit(); if (!gpu.Drain()) throw std::runtime_error("Quality evaluation timeout.");
            const auto proxy = Read(gpu, p.proxy.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            const auto model = Read(gpu, p.answer.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            const auto output = Read(gpu, p.output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE);
            if (baseline.empty()) baseline = output;
            std::cout << std::format("Depth {}: model edit {:.8f}, composed edit {:.8f}, difference from depth=1 {:.8f}\n",
                z, Difference(model, proxy), Difference(output, original), Difference(output, baseline));
        }
        const auto proxy = Read(gpu, p.proxy.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        baseline.clear();
        for (auto format : {DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R8G8B8A8_UNORM}) {
            auto colour = gpu.Texture(p.modelWidth, p.modelHeight, format);
            const auto desc = colour->GetDesc(); D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{}; UINT64 bytes = 0;
            gpu.device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
            auto upload = gpu.Buffer(bytes, D3D12_HEAP_TYPE_UPLOAD);
            auto state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            for (float alpha : {1.0f, 0.5f, 0.0f}) {
                void* mapped = nullptr; Check(upload->Map(0, nullptr, &mapped), "Quality upload");
                // Quantize RGB identically in both formats to isolate format interpretation.
                std::vector<float> supplied(proxy.size());
                for (UINT y = 0; y < p.modelHeight; ++y) for (UINT x = 0; x < p.modelWidth * 4; ++x) {
                    const size_t i = size_t(y) * p.modelWidth * 4 + x;
                    const auto value = (BYTE)std::lround(std::clamp(x % 4 == 3 ? alpha : proxy[i], 0.0f, 1.0f) * 255);
                    supplied[i] = value / 255.0f;
                    auto* row = static_cast<BYTE*>(mapped) + footprint.Offset + size_t(y) * footprint.Footprint.RowPitch;
                    if (format == DXGI_FORMAT_R8G8B8A8_UNORM) row[x] = value;
                    else reinterpret_cast<uint16_t*>(row)[x] = DirectX::PackedVector::XMConvertFloatToHalf(supplied[i]);
                }
                upload->Unmap(0, nullptr);
                gpu.Begin(); Barrier(gpu.commands.Get(), colour.Get(), state, D3D12_RESOURCE_STATE_COPY_DEST);
                D3D12_TEXTURE_COPY_LOCATION from{}, to{};
                from.pResource = upload.Get(); from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; from.PlacedFootprint = footprint;
                to.pResource = colour.Get(); to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                gpu.commands->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
                state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                Barrier(gpu.commands.Get(), colour.Get(), D3D12_RESOURCE_STATE_COPY_DEST, state);
                const auto created = p.runtime->Create(gpu.commands.Get(), p.modelWidth, p.modelHeight, s.model);
                gpu.Submit(); if (!gpu.Drain()) throw std::runtime_error("Quality colour upload timeout.");
                Runtime::CheckResult(created, "Quality colour feature");
                gpu.Begin(); p.runtime->Evaluate(gpu.commands.Get(), colour.Get(), p.depth.Get(), p.motion.Get(),
                    p.answer.Get(), p.modelWidth, p.modelHeight, true);
                gpu.Submit(); if (!gpu.Drain()) throw std::runtime_error("Quality colour evaluation timeout.");
                const auto model = Read(gpu, p.answer.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                if (baseline.empty()) baseline = model;
                std::cout << std::format("Format {} alpha {}: model edit {:.8f}, difference from FP16/opaque {:.8f}\n",
                    (unsigned)format, alpha, Difference(model, supplied), Difference(model, baseline));
            }
        }
        for (unsigned scale : {100u, 67u, 50u}) {
            auto mode = s; mode.modelScale = scale; mode.passes = 1;
            p.Resize(p.width, p.height, mode);
            gpu.Begin(); const auto created = p.runtime->Create(gpu.commands.Get(), p.modelWidth, p.modelHeight, mode.model);
            gpu.Submit(); if (!gpu.Drain()) throw std::runtime_error("Resolution probe creation timeout.");
            Runtime::CheckResult(created, "Resolution probe creation");
            gpu.Begin(); p.Run(input, mode, mode.inputColour == 2, true, 1);
            gpu.Submit(); if (!gpu.Drain()) throw std::runtime_error("Resolution probe timeout.");
            const auto result = Read(gpu, p.output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE);
            std::cout << std::format("Fixed captured frame/tuning, model {}x{} ({}%): composed mean RGB edit {:.8f}\n",
                p.modelWidth, p.modelHeight, scale, Difference(result, original));
        }
    }
};
void QualityCheck(Gpu& gpu, Processor& processor, ID3D12Resource* input, Settings settings) {
    QualityProbe::Run(gpu, processor, input, settings);
    ComPtr<ID3D12InfoQueue> diagnostics;
    if (SUCCEEDED(gpu.device.As(&diagnostics))) {
        bool failed = false;
        for (UINT64 i = 0; i < diagnostics->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
            SIZE_T size = 0; diagnostics->GetMessage(i, nullptr, &size); std::vector<BYTE> data(size);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(data.data());
            Check(diagnostics->GetMessage(i, message, &size), "Quality GPU diagnostic");
            if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) { std::cerr << message->pDescription << '\n'; failed = true; }
        }
        if (failed) throw std::runtime_error("D3D12 validation failed during the quality comparison.");
    }
}
}
