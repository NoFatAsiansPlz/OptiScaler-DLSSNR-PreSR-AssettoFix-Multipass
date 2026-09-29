#include "Processor.h"
#include <DirectXPackedVector.h>
#include <d3d12sdklayers.h>
#include <functional>
#include <iostream>
#include <vector>
namespace nr {
namespace {
using Pixel = std::array<float,4>;
using Image = std::vector<Pixel>;
void Upload(Gpu& g, ID3D12Resource* texture, D3D12_RESOURCE_STATES state, const std::function<Pixel(UINT,UINT)>& pixel) {
    using namespace DirectX::PackedVector;
    const auto d=texture->GetDesc(); D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{}; UINT64 bytes=0;
    g.device->GetCopyableFootprints(&d,0,1,0,&layout,nullptr,nullptr,&bytes);
    auto upload=g.Buffer(bytes,D3D12_HEAP_TYPE_UPLOAD); void* mapped=nullptr;
    Check(upload->Map(0,nullptr,&mapped),"Residual fixture upload");
    for (UINT y=0;y<d.Height;++y) {
        auto* row=reinterpret_cast<HALF*>(static_cast<BYTE*>(mapped)+layout.Offset+size_t(y)*layout.Footprint.RowPitch);
        for (UINT x=0;x<d.Width;++x) { const auto value=pixel(x,y); for (UINT c=0;c<4;++c) row[x*4+c]=XMConvertFloatToHalf(value[c]); }
    }
    upload->Unmap(0,nullptr); g.Begin();
    Barrier(g.commands.Get(),texture,state,D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION src{},dst{};
    src.pResource=upload.Get(); src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint=layout;
    dst.pResource=texture; dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    g.commands->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
    Barrier(g.commands.Get(),texture,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    g.Submit(); if (!g.Drain()) throw std::runtime_error("Residual upload timeout.");
}
Image Read(Gpu& g, ID3D12Resource* texture) {
    using namespace DirectX::PackedVector;
    const auto d=texture->GetDesc(); D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{}; UINT64 bytes=0;
    g.device->GetCopyableFootprints(&d,0,1,0,&layout,nullptr,nullptr,&bytes);
    auto buffer=g.Buffer(bytes,D3D12_HEAP_TYPE_READBACK); g.Begin();
    Barrier(g.commands.Get(),texture,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION src{},dst{}; src.pResource=texture; src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.pResource=buffer.Get(); dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint=layout;
    g.commands->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
    Barrier(g.commands.Get(),texture,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    g.Submit(); if (!g.Drain()) throw std::runtime_error("Residual readback timeout.");
    void* mapped=nullptr; Check(buffer->Map(0,nullptr,&mapped),"Read residual pixels"); Image result(size_t(d.Width)*d.Height);
    for (UINT y=0;y<d.Height;++y) {
        const auto* row=reinterpret_cast<const HALF*>(static_cast<const BYTE*>(mapped)+layout.Offset+size_t(y)*layout.Footprint.RowPitch);
        for (UINT x=0;x<d.Width;++x) for (UINT c=0;c<4;++c) result[size_t(y)*d.Width+x][c]=XMConvertHalfToFloat(row[x*4+c]);
    }
    buffer->Unmap(0,nullptr); return result;
}
Pixel Grey(float value) { return {value,value,value,1}; }
}
struct ResidualProbe {
    static void Reconstruction(Gpu& g, Processor& p) {
        constexpr UINT w=400,h=8,edge=249;
        const auto readable=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        auto texture=[&](UINT width,UINT height) { return g.Texture(width,height,DXGI_FORMAT_R16G16B16A16_FLOAT,readable); };
        auto input=texture(w,h), original=texture(w,h), full=texture(w,h), nativeModel=texture(w,h);
        auto nativeResult=texture(w,h), reconstructed=texture(w,h), result=texture(w,h), enlarged=texture(w,h), bounded=texture(w,h);
        auto render=[&](const DlssNrConstants& c,ID3D12Resource* source,ID3D12Resource* model,ID3D12Resource* clean,
                        ID3D12Resource* out,ID3D12Resource* keep=nullptr) {
            g.Begin(); Barrier(g.commands.Get(),out,readable,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            if (keep) Barrier(g.commands.Get(),keep,readable,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            p.Dispatch(0,c,source,model,clean,out,keep);
            Barrier(g.commands.Get(),out,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,readable);
            if (keep) Barrier(g.commands.Get(),keep,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,readable);
            Diagnostics(g); g.Submit(); if (!g.Drain()) throw std::runtime_error("Reconstruction fixture timeout.");
        };
        auto encode=[](float v) { return v<=.0031308f ? 12.92f*v : 1.055f*std::pow(v,1/2.4f)-.055f; };
        auto decode=[](float v) { return v<=.04045f ? v/12.92f : std::pow((v+.055f)/1.055f,2.4f); };
        float worstOld=0,worstNew=0; unsigned cases=0;
        for (unsigned space=0;space<9;++space) {
            const bool sdr=space==5;
            const bool coloured=space==6 || space==7;
            DlssNrConstants c{}; c.Width=w; c.Height=h; c.WhitePoint=1; c.Passthrough=sdr; c.ReversibleMode=space>=6 ? 0 : space%5;
            c.ApplyModel=1; c.TransferStrength=c.ColourStrength=1; c.MaxRatio=8; c.CompareZoom=1;
            Upload(g,input.Get(),readable,[&](UINT x,UINT) {
                if (coloured) return x<edge ? Pixel{space==7 ? 0.f : .005f,.01f,.04f,1} : Pixel{.55f,.3f,space==7 ? 0.f : .2f,1};
                if (space==8) return Grey(x<edge ? 0.f : .55f);
                return Grey(sdr ? encode(x<edge ? .015625f : .55f) : x<edge ? .015625f : .55f);
            });
            c.Mode=DlssNrMode_Encode; render(c,input.Get(),nullptr,nullptr,full.Get(),original.Get());
            const auto fullPixels=Read(g,full.Get());
            for (unsigned scale : {99u,65u,25u}) {
                const UINT mw=(w*scale+50)/100, mh=std::max(1u,(h*scale+50)/100);
                auto low=texture(mw,mh), model=texture(mw,mh), field=texture(mw,mh);
                auto down=c; down.Mode=DlssNrMode_Downsample; down.Width=mw; down.Height=mh;
                render(down,full.Get(),nullptr,nullptr,low.Get()); const auto lowPixels=Read(g,low.Get());
                for (float gain : {.7f,1.f,1.2f}) {
                    auto change=[&](Pixel value) { for (unsigned ch=0;ch<3;++ch) value[ch]=sdr ? value[ch]*gain : encode(decode(value[ch])*gain); return value; };
                    Upload(g,model.Get(),readable,[&](UINT x,UINT y) { return change(lowPixels[size_t(y)*mw+x]); });
                    Upload(g,nativeModel.Get(),readable,[&](UINT x,UINT y) { return change(fullPixels[size_t(y)*w+x]); });
                    c.Mode=DlssNrMode_Resolve; c.Transfer=0;
                    render(c,full.Get(),nativeModel.Get(),original.Get(),nativeResult.Get()); const auto expected=Read(g,nativeResult.Get());
                    for (unsigned transfer : {0u,1u}) {
                        c.Transfer=transfer;
                        render(c,low.Get(),model.Get(),original.Get(),result.Get()); const auto legacy=Read(g,result.Get());
                        if (space==0 && scale==99 && gain==.7f) worstOld=std::max(worstOld,legacy[edge][0]-expected[edge][0]);
                        auto prepare=c; prepare.Mode=16; prepare.Width=mw; prepare.Height=mh;
                        render(prepare,low.Get(),model.Get(),nullptr,field.Get());
                        for (bool nativeCarrier : {false,true}) {
                            auto* enlargedField=field.Get();
                            if (nativeCarrier) {
                                // Emulate reconstruction excursions, then exercise the real
                                // full-resolution carrier/guard path without invoking DLSS.
                                Upload(g,enlarged.Get(),readable,[&](UINT x,UINT) { return Grey(x%2 ? 1.01f : -.01f); });
                                auto bound=c; bound.Mode=14;
                                render(bound,field.Get(),enlarged.Get(),nullptr,bounded.Get()); enlargedField=bounded.Get();
                            }
                            auto restore=c; restore.Mode=17;
                            render(restore,enlargedField,full.Get(),field.Get(),reconstructed.Get());
                            auto resolve=c; resolve.Transfer=0;
                            render(resolve,full.Get(),reconstructed.Get(),original.Get(),result.Get()); const auto actual=Read(g,result.Get());
                            for (size_t i=0;i<actual.size();++i) for (unsigned ch=0;ch<3;++ch) {
                                if (coloured && transfer==0) {
                                    // Classic intentionally retains resized chroma, but must
                                    // preserve a uniform lighting gain at a colour boundary.
                                    auto y=[](const Pixel& v) { return .2126f*v[0]+.7152f*v[1]+.0722f*v[2]; };
                                    if (std::abs(y(actual[i])-y(expected[i]))>.008f*std::max(y(expected[i]),.02f))
                                        throw std::runtime_error("Classic colour resizing introduced a lighting rim.");
                                    continue;
                                }
                                const float error=std::abs(actual[i][ch]-expected[i][ch]);
                                const float relativeError=error/std::max(expected[i][ch],.02f); worstNew=std::max(worstNew,relativeError);
                                if (!std::isfinite(actual[i][ch]) || relativeError>.008f)
                                    throw std::runtime_error(std::format("Resize mismatch: space {}, scale {}, gain {}, transfer {}, carrier {}, x {}, expected {}, actual {}",
                                        space,scale,gain,transfer,nativeCarrier,i%w,expected[i][ch],actual[i][ch]));
                            }
                            if (transfer==1 && gain==1 && Read(g,reconstructed.Get())!=fullPixels)
                                throw std::runtime_error("Neutral matched field did not preserve the full proxy exactly.");
                            ++cases;
                        }
                    }
                }
            }
        }
        if (worstOld<.02f) throw std::runtime_error("The fixture did not reproduce the old bright rim.");
        std::cout<<std::format("PASS {} shader reconstruction cases: 99/65/25%, Classic/matched, SDR/five HDR modes, bilinear/full-size carriers\n",cases);
        std::cout<<std::format("99% old bright-edge error {:.6f}; corrected worst relative error {:.4f}% across all fixtures\n",worstOld,worstNew*100);
        std::cout<<"PASS neutral matched identity, uniform brightening/darkening, cool-dark/warm-skin boundary and carrier overshoot bounds\n";
        auto lowField=texture(2,1);
        Upload(g,lowField.Get(),readable,[](UINT x,UINT){return Pixel{x ? .875f : .5f,.5f,.5f,1};});
        Upload(g,enlarged.Get(),readable,[](UINT,UINT){return Pixel{.875f,.5f,.5f,1};});
        Upload(g,full.Get(),readable,[&](UINT,UINT){return Grey(encode(.4f));});
        for (unsigned codec : {2u,4u}) {
            const unsigned transfer=1;
            DlssNrConstants c{}; c.Mode=17; c.Width=w; c.Height=h; c.ReversibleMode=codec; c.Transfer=transfer;
            render(c,enlarged.Get(),full.Get(),lowField.Get(),reconstructed.Get());
            const float value=decode(Read(g,reconstructed.Get())[149][0]);
            if (std::abs(value-.697f)>.003f)
                throw std::runtime_error("Reconstructed HDR inverse-pole fallback lost its bilinear reference.");
        }
        std::cout<<"PASS zero colour channels and both HDR inverse-pole fallbacks\n";
    }
    static void ShaderOnly() {
        Gpu g; Processor p(g); Reconstruction(g,p); Diagnostics(g);
    }
    static void Diagnostics(Gpu& gpu) {
        ComPtr<ID3D12InfoQueue> messages; gpu.device.As(&messages);
        if (messages) for (UINT64 i=0;i<messages->GetNumStoredMessagesAllowedByRetrievalFilter();++i) {
            SIZE_T bytes=0; messages->GetMessage(i,nullptr,&bytes); std::vector<BYTE> storage(bytes);
            auto* m=reinterpret_cast<D3D12_MESSAGE*>(storage.data()); messages->GetMessage(i,m,&bytes);
            if (m->Severity<=D3D12_MESSAGE_SEVERITY_ERROR) throw std::runtime_error(m->pDescription);
        }
    }
    static void Run(Gpu& g, Processor& p) {
        Reconstruction(g,p);
        // Exercise the real private DLSS runtime on a high-contrast signed edit.
        Settings settings; settings.modelScale=50; settings.transfer=1; settings.upscaler=1;
        p.Resize(1024,640,settings);
        Upload(g,p.original.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,[](UINT,UINT){return Grey(.4f);});
        uint64_t outside=0; float largest=0; bool first=true;
        for (unsigned frame=0;frame<8;++frame) {
            Upload(g,p.resizeInput.Get(),first ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                [&](UINT x,UINT y){return Grey(((x+frame*2)/19+(y/29))%2 ? .9f : .1f);});
            g.Begin();
            if (!first) Barrier(g.commands.Get(),p.enlarged.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            p.runtime->EvaluateDlss(g.commands.Get(),p.resizeInput.Get(),p.depth.Get(),p.motion.Get(),p.exposure.Get(),p.enlarged.Get(),true,16.67f);
            Barrier(g.commands.Get(),p.enlarged.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            if (!first) Barrier(g.commands.Get(),p.boundedDlss.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            DlssNrConstants c{}; c.Mode=14; c.Width=1024; c.Height=640; c.WhitePoint=1; c.ReversibleMode=4;
            p.Dispatch(77,c,p.resizeInput.Get(),p.enlarged.Get(),p.original.Get(),p.boundedDlss.Get(),nullptr);
            Barrier(g.commands.Get(),p.boundedDlss.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            g.Submit(); if (!g.Drain()) throw std::runtime_error("DLSS edge fixture timeout."); first=false;
            const auto before=Read(g,p.enlarged.Get()), after=Read(g,p.boundedDlss.Get());
            for (size_t i=0;i<before.size();++i) for (unsigned ch=0;ch<3;++ch) {
                largest=std::max(largest,before[i][ch]);
                outside+=before[i][ch]>.9001f || before[i][ch]<.0999f;
                if (!std::isfinite(after[i][ch]) || after[i][ch]>.9001f || after[i][ch]<.0999f)
                    throw std::runtime_error("Bounded DLSS residual escaped its source envelope.");
            }
        }
        std::cout<<std::format("Real DLSS edge sequence: {} out-of-envelope channel samples before correction; peak carrier {:.6f}\n",outside,largest);
        std::cout<<"PASS real-DLSS bounded carrier edges\n";
    }
};
void ResidualCheck(const std::filesystem::path& runtime) {
    Gpu gpu; Processor processor(gpu,runtime); ResidualProbe::Run(gpu,processor);
    ResidualProbe::Diagnostics(gpu);
}
void ResizeCheck() { ResidualProbe::ShaderOnly(); }
}
