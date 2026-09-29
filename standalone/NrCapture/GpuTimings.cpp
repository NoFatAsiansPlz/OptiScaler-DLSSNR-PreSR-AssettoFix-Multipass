#include "Gpu.h"
namespace nr {
void Gpu::BeginTimings() {
    if (pendingTimes) throw std::runtime_error("Read completed GPU timings before starting another frame.");
    if (!timeQueries) {
        D3D12_QUERY_HEAP_DESC desc{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, unsigned(GpuStage::Count) * 2, 0};
        Check(device->CreateQueryHeap(&desc, IID_PPV_ARGS(&timeQueries)), "GPU stage timestamps");
        timeReadback = Buffer(desc.Count * sizeof(UINT64), D3D12_HEAP_TYPE_READBACK);
        Check(queue->GetTimestampFrequency(&timeFrequency), "GPU timestamp frequency");
    }
    timed.fill(false); recordingTimes = true;
}
void Gpu::Time(GpuStage stage, bool end) {
    if (!recordingTimes) return;
    const auto index = unsigned(stage);
    commands->EndQuery(timeQueries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, index * 2 + unsigned(end));
    if (end) timed[index] = true;
}
void Gpu::ResolveTimings() {
    if (!recordingTimes) return;
    for (unsigned i = 0; i < timed.size(); ++i) if (timed[i])
        commands->ResolveQueryData(timeQueries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, i * 2, 2,
            timeReadback.Get(), i * 2 * sizeof(UINT64));
    recordingTimes = false; pendingTimes = true;
}
bool Gpu::TakeTimings(std::array<double, unsigned(GpuStage::Count)>& milliseconds) {
    if (!pendingTimes || !Complete()) return false;
    void* mapped = nullptr; D3D12_RANGE range{0, timed.size() * 2 * sizeof(UINT64)};
    Check(timeReadback->Map(0, &range, &mapped), "GPU stage timing readback");
    const auto* stamps = static_cast<const UINT64*>(mapped);
    milliseconds.fill(0);
    for (unsigned i = 0; i < timed.size(); ++i) if (timed[i])
        milliseconds[i] = double(stamps[i * 2 + 1] - stamps[i * 2]) * 1000.0 / timeFrequency;
    D3D12_RANGE written{0, 0}; timeReadback->Unmap(0, &written); pendingTimes = false;
    return true;
}
}
