#pragma once
#include "Gpu.h"
#include <array>
namespace nr {
// Owns only shader resources; uses OptiScaler's compiled spatial filters without its hooks/configuration.
class Scaling {
public:
    explicit Scaling(Gpu& gpu);
    void Run(unsigned slot, ID3D12Resource* input, ID3D12Resource* output, unsigned filter, bool enlarge);
private:
    Gpu& gpu;
    ComPtr<ID3D12RootSignature> root;
    std::array<ComPtr<ID3D12PipelineState>, 9> pipelines;
    ComPtr<ID3D12DescriptorHeap> heap;
    ComPtr<ID3D12Resource> constants;
    UINT stride = 0;
};
}
