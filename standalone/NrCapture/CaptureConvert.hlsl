Texture2D<float4> Source : register(t0);
RWTexture2D<float4> Target : register(u0);
cbuffer Params : register(b0) { uint Width, Height, Left, Top; uint Rotation, DecodeSdr, Padding0, Padding1; };
[numthreads(8,8,1)]
void Convert(uint3 id : SV_DispatchThreadID) {
    if (id.x >= Width || id.y >= Height) return;
    uint2 p = id.xy;
    if (Rotation == 2) p = uint2(id.y, Width - 1 - id.x);
    else if (Rotation == 3) p = uint2(Width - 1 - id.x, Height - 1 - id.y);
    else if (Rotation == 4) p = uint2(Height - 1 - id.y, id.x);
    float4 value = Source.Load(int3(p + uint2(Left, Top), 0));
    if (DecodeSdr) {
        float3 low = value.rgb / 12.92;
        float3 high = pow((value.rgb + 0.055) / 1.055, 2.4);
        value.rgb = float3(value.r <= 0.04045 ? low.r : high.r,
                          value.g <= 0.04045 ? low.g : high.g,
                          value.b <= 0.04045 ? low.b : high.b);
    }
    Target[id.xy] = float4(value.rgb, 1);
}
