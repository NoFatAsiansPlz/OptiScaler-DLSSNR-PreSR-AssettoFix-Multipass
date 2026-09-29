#include "Processor.h"
#include "Colour.h"
#include "FrameGenerationOutput.h"
#include <algorithm>
#include <iostream>
#include <numeric>
#include <vector>

namespace nr {
void PerformanceCheck(Gpu& gpu, Processor& processor, ID3D12Resource* input, Settings settings, unsigned frames) {
    const auto monitor = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    auto report = [](const char* name, std::vector<double> values) {
        const auto mean = std::accumulate(values.begin(), values.end(), 0.0) / values.size();
        std::sort(values.begin(), values.end());
        std::cout << std::format("{}: {:.3f} ms mean, {:.3f} ms p95\n", name, mean,
            values[std::min(values.size() - 1, values.size() * 95 / 100)]) << std::flush;
    };
    // Mirror the live per-frame HDR query without creating any windows/capture sessions.
    std::vector<double> hdrTimes, whiteTimes;
    for (unsigned frame = 0; frame < 20; ++frame) {
        const auto start = ClockMs();
        for (UINT i = 0; ; ++i) {
            ComPtr<IDXGIOutput> output;
            if (gpu.adapter->EnumOutputs(i, &output) == DXGI_ERROR_NOT_FOUND) break;
            ComPtr<IDXGIOutput6> extended;
            if (FAILED(output.As(&extended))) continue;
            DXGI_OUTPUT_DESC1 desc{};
            if (SUCCEEDED(extended->GetDesc1(&desc)) && desc.Monitor == monitor) break;
        }
        hdrTimes.push_back(ClockMs() - start);
        const auto whiteStart = ClockMs(); DesktopWhitePoint(monitor);
        whiteTimes.push_back(ClockMs() - whiteStart);
    }
    report("Live HDR enumeration/query", hdrTimes);
    report("Desktop white query (normally once/second)", whiteTimes);
    ComPtr<ID3D12QueryHeap> queries;
    D3D12_QUERY_HEAP_DESC queryDesc{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 2, 0};
    Check(gpu.device->CreateQueryHeap(&queryDesc, IID_PPV_ARGS(&queries)), "Performance timestamps");
    auto readback = gpu.Buffer(16, D3D12_HEAP_TYPE_READBACK);
    UINT64 frequency = 0; Check(gpu.queue->GetTimestampFrequency(&frequency), "Timestamp frequency");
    FrameGenerationOutput fgOutput(gpu);
    const bool hdr = settings.inputColour == 2;
    fgOutput.Resize(processor.width, processor.height, hdr);
    for (const int mode : {0, 1, 2, 3}) {
        const int fps = mode == 0 ? -1 : 120;
        processor.Resize(processor.width, processor.height, settings);
        std::vector<double> cpuTimes, completeTimes, gpuTimes;
        std::array<std::vector<double>, unsigned(GpuStage::Count)> stageTimes;
        for (unsigned frame = 0; frame < frames + 5; ++frame) {
            const auto start = ClockMs();
            gpu.Begin(); gpu.BeginTimings();
            gpu.commands->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
            auto* output = processor.Run(input, settings, hdr, false, 1, 8.33f, fps);
            if (mode >= 2) fgOutput.Run(output, frame == 0 || processor.historyReset,
                mode == 3 ? processor.FullResolutionMotion() : nullptr);
            gpu.commands->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
            gpu.commands->ResolveQueryData(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, readback.Get(), 0);
            gpu.ResolveTimings();
            gpu.Submit();
            const auto submitted = ClockMs();
            if (!gpu.Drain()) throw std::runtime_error("Performance check GPU timeout.");
            const auto completed = ClockMs();
            std::array<double, unsigned(GpuStage::Count)> stages{};
            if (!gpu.TakeTimings(stages)) throw std::runtime_error("GPU stage timings were not ready after completion.");
            void* data = nullptr; Check(readback->Map(0, nullptr, &data), "Performance readback");
            const auto* stamps = static_cast<const UINT64*>(data);
            const auto elapsed = (stamps[1] - stamps[0]) * 1000.0 / frequency;
            readback->Unmap(0, nullptr);
            if (frame >= 5) {
                cpuTimes.push_back(submitted - start); completeTimes.push_back(completed - start); gpuTimes.push_back(elapsed);
                for (unsigned i = 0; i < stages.size(); ++i) stageTimes[i].push_back(stages[i]);
            }
        }
        std::cout << (mode == 0 ? "NR only\n" : mode == 1 ? "NR + FPS label\n" : mode == 2 ?
            "NR + FPS label + independent FG motion/encoding (no interpolation/capture/presentation)\n" :
            "NR + FPS label + shared FG motion/encoding (no interpolation/capture/presentation)\n");
        report("CPU record including internal GPU waits", cpuTimes);
        report("Processing through GPU completion", completeTimes);
        report("GPU timestamp span", gpuTimes);
        for (unsigned i = 0; i < stageTimes.size(); ++i) report(GpuStageNames[i], stageTimes[i]);
    }
}
}
