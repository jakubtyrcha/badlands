// The app layer's debug-UI composite: an encoded, premultiplied overlay laid
// over whatever the app already presented.
//
// PORTED, NOT RE-DERIVED, from object_viewer's output.slang. The two forms
// below have been got wrong in this repo once already, and re-deriving them is
// how that happens again.
//
// WHY THE OVERLAY IS ITS OWN TARGET. ImGui blends in ENCODED space -- that is
// the space UI is authored for -- and drawing a 50%-alpha panel straight into a
// linear surface lands it at encoded 0.735 instead of 0.5. So the layer gives
// ImGui an RGBA8 encoded target and composites it here, rather than letting it
// touch the backbuffer.
//
// AND PREMULTIPLIED, because the overlay is a LAYER: it accumulates several
// translucent draws before this pass sees it, and only the premultiplied form
// composes associatively. With straight alpha two overlapping half-alpha draws
// double-count their colour against the layer's own accumulated alpha --
// identical over an opaque background, which is exactly why it stays hidden
// until a layer exists.
module ui_composite;

import output_transform;

// x = the surface's ColorSpace (the enum IS the constant -- see the
// static_asserts beside rhi::ColorSpace).
//
// A float4 rather than one scalar because a constant buffer of a single scalar
// is a 4-byte object every backend pads differently.
struct CompositeParams {
    float4 mode;
}

ConstantBuffer<CompositeParams> params;
Texture2D<float4> ui;

struct VOut {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
}

// One triangle, not two: a quad re-shades every 2x2 rasterizer quad along the
// diagonal. See "Screen-space work is a cost" in src/engine/CLAUDE.md.
[shader("vertex")]
VOut vs_ui_composite(uint vid : SV_VertexID)
{
    let uv = float2(float((vid << 1) & 2), float(vid & 2));
    VOut o;
    o.pos = float4(uv * 2.0 - 1.0, 0.0, 1.0);
    o.uv = float2(uv.x, 1.0 - uv.y);
    return o;
}

[shader("fragment")]
float4 fs_ui_composite(VOut i) : SV_Target
{
    // Load, not Sample: a 1:1 blit, and any filtering here is a bug that shows
    // as a half-texel softness nobody goes looking for.
    uint w, h;
    ui.GetDimensions(w, h);
    let overlay = ui.Load(int3(int(i.uv.x * float(w)), int(i.uv.y * float(h)), 0));

    // Nothing to do where the overlay is empty, which is most of the screen.
    // Returning zero with premultiplied blending leaves the backbuffer exactly
    // as it was.
    if (overlay.a <= 0.0 && all(overlay.rgb <= 0.0)) { return float4(0.0); }

    let mode = uint(params.mode.x);

    // THE TWO FORMS, AND WHY THERE ARE TWO.
    //
    // On an extended-range surface the backbuffer holds LINEAR values above 1,
    // so the overlay is decoded to linear and blended there. Doing it the other
    // way -- encoding the scene to composite in encoded space -- CLAMPS, and
    // clamping destroys the headroom at ANY non-zero alpha: a 1/255 glyph
    // fringe draws a dark ring around every window on an HDR display.
    //
    // On an 8-bit surface the backbuffer already holds encoded bytes, which is
    // what hardware blending assumes, so the overlay goes out as it is.
    //
    // The two agree at alpha 0 and alpha 1 and differ only in between, which is
    // why the difference stays invisible until something translucent exists.
    if (OutputIsExtendedRange(mode)) {
        // Premultiplied in, premultiplied out: decode the colour and keep the
        // alpha, so the One/OneMinusSrcAlpha blend below is still correct.
        return float4(SrgbToLinear(overlay.rgb), overlay.a);
    }
    return overlay;
}
