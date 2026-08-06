// Pass 4: unpack the visibility buffer into colour.
//
// Reading a uint64 is fine in Slang for Metal; only the 64-bit ATOMIC is
// missing, which is why pass 3 is hand-written MSL.
import splat_common;

ConstantBuffer<SplatParams> params;
StructuredBuffer<uint64_t> visbuffer;
StructuredBuffer<Splat> splats;

struct VOut {
    float4 pos : SV_Position;
};

[shader("vertex")]
VOut vs_fullscreen(uint vid : SV_VertexID) {
    float2 uv = float2(float((vid << 1) & 2), float(vid & 2));
    VOut o;
    o.pos = float4(uv * 2.0 - 1.0, 0.0, 1.0);
    return o;
}

[shader("fragment")]
float4 fs_resolve(VOut i) : SV_Target {
    uint w = uint(params.config.z);
    uint2 px = uint2(i.pos.xy);
    uint64_t packed = visbuffer[px.y * w + px.x];
    if (packed == 0) {
        return float4(0.0, 0.0, 0.0, 1.0);   // nothing splatted here
    }
    // Low 32 bits are the splat index; its source picks the colour, so the
    // occlusion test can tell WHICH sphere won a pixel.
    uint index = uint(packed & 0xFFFFFFFF);
    float source = splats[index].pos_source.w;
    return source < 0.5 ? float4(1.0, 0.0, 0.0, 1.0)
                        : float4(0.0, 1.0, 0.0, 1.0);
}
