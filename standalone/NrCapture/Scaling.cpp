#include "Scaling.h"
#include <cmath>
#define A_CPU
#include <shaders/output_scaling/fsr1/ffx_fsr1.h>
#include <shaders/output_scaling/fsr1/FSR_EASU_Shader.h>
#include <shaders/output_scaling/precompile/BCUS_Shader.h>
#include <shaders/output_scaling/precompile/bcds_bicubic_Shader.h>
#include <shaders/output_scaling/precompile/bcds_catmull_Shader.h>
#include <shaders/output_scaling/precompile/bcds_lanczos2_Shader.h>
#include <shaders/output_scaling/precompile/bcds_lanczos3_Shader.h>
#include <shaders/output_scaling/precompile/bcds_kaiser2_Shader.h>
#include <shaders/output_scaling/precompile/bcds_kaiser3_Shader.h>
#include <shaders/output_scaling/precompile/bcds_magc_Shader.h>
namespace nr {
Scaling::Scaling(Gpu& g) : gpu(g) {
    D3D12_DESCRIPTOR_RANGE ranges[]{{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0}, {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 1}};
    D3D12_ROOT_PARAMETER parameters[2]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; parameters[0].DescriptorTable = {2, ranges};
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    D3D12_STATIC_SAMPLER_DESC sampler{}; sampler.Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP; sampler.MaxLOD = D3D12_FLOAT32_MAX;
    D3D12_ROOT_SIGNATURE_DESC signature{2, parameters, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> blob, error;
    Check(D3D12SerializeRootSignature(&signature, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error), "Scaling root signature");
    Check(gpu.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root)), "Scaling root");
    const D3D12_SHADER_BYTECODE codes[]{
        {fsr_easu_cso, sizeof(fsr_easu_cso)}, {bcds_bicubic_cso, sizeof(bcds_bicubic_cso)},
        {bcds_catmull_cso, sizeof(bcds_catmull_cso)}, {bcds_lanczos2_cso, sizeof(bcds_lanczos2_cso)},
        {bcds_lanczos3_cso, sizeof(bcds_lanczos3_cso)}, {bcds_kaiser2_cso, sizeof(bcds_kaiser2_cso)},
        {bcds_kaiser3_cso, sizeof(bcds_kaiser3_cso)}, {bcds_magc_cso, sizeof(bcds_magc_cso)}, {bcus_cso, sizeof(bcus_cso)}};
    for (unsigned i = 0; i < pipelines.size(); ++i) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc{}; desc.pRootSignature = root.Get(); desc.CS = codes[i];
        Check(gpu.device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pipelines[i])), "Spatial filter");
    }
    D3D12_DESCRIPTOR_HEAP_DESC h{}; h.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; h.NumDescriptors = 6; h.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    Check(gpu.device->CreateDescriptorHeap(&h, IID_PPV_ARGS(&heap)), "Scaling descriptors");
    stride = gpu.device->GetDescriptorHandleIncrementSize(h.Type); constants = gpu.Buffer(3 * 256, D3D12_HEAP_TYPE_UPLOAD);
}
void Scaling::Run(unsigned slot, ID3D12Resource* input, ID3D12Resource* output, unsigned filter, bool enlarge) {
    if (slot >= 3 || filter >= 8) throw std::runtime_error("Invalid spatial filter/dispatch slot.");
    const auto in = input->GetDesc(), out = output->GetDesc();
    const unsigned pipeline = filter == 0 ? 0 : enlarge ? 8 : filter;
    auto cpu = heap->GetCPUDescriptorHandleForHeapStart(); cpu.ptr += SIZE_T(slot) * 2 * stride;
    auto descriptor = heap->GetGPUDescriptorHandleForHeapStart(); descriptor.ptr += UINT64(slot) * 2 * stride;
    D3D12_SHADER_RESOURCE_VIEW_DESC view{}; view.Format = in.Format; view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; view.Texture2D.MipLevels = 1;
    gpu.device->CreateShaderResourceView(input, &view, cpu); cpu.ptr += stride;
    gpu.device->CreateUnorderedAccessView(output, nullptr, nullptr, cpu);
    void* mapped = nullptr; Check(constants->Map(0, nullptr, &mapped), "Scaling constants");
    auto* destination = static_cast<BYTE*>(mapped) + slot * 256;
    if (filter == 0) {
        UpscaleShaderConstants values{};
        FsrEasuCon(values.const0, values.const1, values.const2, values.const3,
            float(in.Width), float(in.Height), float(in.Width), float(in.Height), float(out.Width), float(out.Height));
        memcpy(destination, &values, sizeof(values));
    } else {
        const int values[]{int(in.Width), int(in.Height), int(out.Width), int(out.Height)};
        memcpy(destination, values, sizeof(values));
    }
    constants->Unmap(0, nullptr);
    auto* c = gpu.commands.Get(); ID3D12DescriptorHeap* heaps[]{heap.Get()}; c->SetDescriptorHeaps(1, heaps);
    c->SetComputeRootSignature(root.Get()); c->SetPipelineState(pipelines[pipeline].Get());
    c->SetComputeRootDescriptorTable(0, descriptor); c->SetComputeRootConstantBufferView(1, constants->GetGPUVirtualAddress() + slot * 256);
    const UINT tile = pipeline == 0 || pipeline == 8 ? 16 : 8;
    c->Dispatch((UINT(out.Width) + tile - 1) / tile, (out.Height + tile - 1) / tile, 1);
}
}
