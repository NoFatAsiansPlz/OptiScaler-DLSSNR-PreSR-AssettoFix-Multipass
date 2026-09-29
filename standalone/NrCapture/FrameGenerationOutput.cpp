#include "FrameGenerationOutput.h"
#include <FrameGenerationEncode.h>
namespace nr {
FrameGenerationOutput::FrameGenerationOutput(Gpu& g) : gpu(g), motion(g) {
    D3D12_DESCRIPTOR_RANGE ranges[]{{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,0,0,0}, {D3D12_DESCRIPTOR_RANGE_TYPE_UAV,2,0,0,1}};
    D3D12_ROOT_PARAMETER parameters[2]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; parameters[0].DescriptorTable = {2,ranges};
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; parameters[1].Constants = {0,0,3};
    D3D12_ROOT_SIGNATURE_DESC signature{2,parameters}; ComPtr<ID3DBlob> blob, error;
    Check(D3D12SerializeRootSignature(&signature,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error), "FG output signature");
    Check(g.device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root)), "FG output root");
    D3D12_COMPUTE_PIPELINE_STATE_DESC p{}; p.pRootSignature = root.Get(); p.CS = {FrameGenerationEncode,sizeof(FrameGenerationEncode)};
    Check(g.device->CreateComputePipelineState(&p,IID_PPV_ARGS(&shader)), "FG output pipeline");
    D3D12_DESCRIPTOR_HEAP_DESC desc{}; desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    desc.NumDescriptors = 3; desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    Check(g.device->CreateDescriptorHeap(&desc,IID_PPV_ARGS(&heap)), "FG output descriptors");
}
void FrameGenerationOutput::Resize(UINT w, UINT h, bool isHdr) {
    if (w == width && h == height && hdr == isHdr) return;
    if (!gpu.Drain()) throw std::runtime_error("GPU timeout resizing frame generation inputs.");
    width = w; height = h; hdr = isHdr; motion.Resize(w,h); motion.Reset();
    sharedMotion.Reset();
    colour = gpu.Texture(w,h,Format(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    depth = gpu.Texture(w,h,DXGI_FORMAT_R32_FLOAT,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
}
bool FrameGenerationOutput::Run(ID3D12Resource* input, bool reset, ID3D12Resource* fullResolutionMotion) {
    gpu.Time(GpuStage::FgMotion);
    Barrier(gpu.commands.Get(),input,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    bool cut = reset;
    if (fullResolutionMotion) {
        const auto desc = fullResolutionMotion->GetDesc();
        if (desc.Width != width || desc.Height != height || desc.Format != DXGI_FORMAT_R16G16_FLOAT)
            throw std::runtime_error("FG shared motion must match the full output resolution.");
        // As with engine-provided guides, reuse the scene motion already used by
        // NR. Its completion is covered by the same FG input-consumption fence.
        sharedMotion = fullResolutionMotion; motion.Reset();
    } else {
        sharedMotion.Reset(); cut = motion.Estimate(input,reset);
    }
    gpu.Time(GpuStage::FgMotion, true);
    gpu.Time(GpuStage::FgEncode);
    auto* c = gpu.commands.Get();
    Barrier(c,colour.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Barrier(c,depth.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    auto handle = heap->GetCPUDescriptorHandleForHeapStart(); const auto stride = gpu.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{}; srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; srv.Texture2D.MipLevels = 1; srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    gpu.device->CreateShaderResourceView(input,&srv,handle); handle.ptr += stride;
    gpu.device->CreateUnorderedAccessView(colour.Get(),nullptr,nullptr,handle); handle.ptr += stride;
    gpu.device->CreateUnorderedAccessView(depth.Get(),nullptr,nullptr,handle);
    ID3D12DescriptorHeap* heaps[]{heap.Get()}; c->SetDescriptorHeaps(1,heaps);
    c->SetComputeRootSignature(root.Get()); c->SetPipelineState(shader.Get());
    c->SetComputeRootDescriptorTable(0,heap->GetGPUDescriptorHandleForHeapStart());
    const UINT values[]{width,height,UINT(hdr)}; c->SetComputeRoot32BitConstants(1,3,values,0);
    c->Dispatch((width+7)/8,(height+7)/8,1);
    Barrier(c,input,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_SOURCE);
    Barrier(c,colour.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(c,depth.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    gpu.Time(GpuStage::FgEncode, true);
    return cut;
}
}
