// Dear ImGui, drawn through the RHI.
//
// Ported from the WGSL in src/engine/ui/imgui_impl_wgpu_custom.cpp, with two
// changes forced by the RHI's shape and one by the target:
//
//   * NO VERTEX INPUT LAYOUT. The RHI has no vertex buffers, so vertices are
//     pulled by SV_VertexID. That works with DrawIndexed because a vertex id in
//     an indexed draw is the fetched index plus base_vertex, which is exactly
//     the addressing ImGui's per-command offsets assume.
//
//   * VERTICES ARE READ AS RAW UINTS, deliberately. ImDrawVert is
//     {float2 pos, float2 uv, uint col} = 20 bytes PACKED, but a Slang struct
//     with those members aligns to 8 and strides to 24 -- so every vertex after
//     the first would be read from the wrong offset, producing skewed geometry
//     rather than an error. Unpacking by hand makes the 20-byte layout explicit
//     at the point that depends on it.
//
//   * NO sRGB CONVERSION. The Dawn path had a branch for a linear RGBA16Float
//     surface; this one renders straight to an 8-bit surface, where converting
//     would darken every glyph by the gamma curve.

module imgui;

public struct ImGuiParams {
    // xy = scale, zw = translate. Packed into one float4 rather than two
    // float2s because MSL aligns a float2 to 8 bytes and the host struct would
    // have to match that by accident rather than by construction.
    public float4 scale_translate;
}

ConstantBuffer<ImGuiParams> params;
StructuredBuffer<uint> vertices;   // ImDrawVert, five uints each
Texture2D<float4> tex;
SamplerState samp;

struct VOut {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
}

[shader("vertex")]
VOut vs_imgui(uint vid : SV_VertexID) {
    uint base = vid * 5u;
    float2 pos = float2(asfloat(vertices[base + 0u]), asfloat(vertices[base + 1u]));
    float2 uv = float2(asfloat(vertices[base + 2u]), asfloat(vertices[base + 3u]));
    uint packed = vertices[base + 4u];

    VOut o;
    o.pos = float4(pos * params.scale_translate.xy + params.scale_translate.zw,
                   0.0, 1.0);
    o.uv = uv;
    // IM_COL32 packs R in the low byte, then G, B, A.
    o.color = float4(float(packed & 0xFFu),
                     float((packed >> 8u) & 0xFFu),
                     float((packed >> 16u) & 0xFFu),
                     float((packed >> 24u) & 0xFFu)) / 255.0;
    return o;
}

[shader("fragment")]
float4 fs_imgui(VOut i) : SV_Target {
    let c = i.color * tex.Sample(samp, i.uv);
    // PREMULTIPLIED, because this draws into the UI overlay rather than onto
    // the surface. The overlay is composited with "over" in encoded space, and
    // premultiplied accumulation is what lets overlapping translucent panels
    // and antialiased glyphs blend correctly among themselves first. Over an
    // opaque background the composited RGB is identical either way.
    return float4(c.rgb * c.a, c.a);
}
