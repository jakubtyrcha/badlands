// The output pass: tonemap the HDR scene, composite the LDR overlay, convert
// once, present.
//
// THREE SPACES MEET HERE, and keeping them apart is the whole job:
//
//   scene   RGBA16Float, LINEAR, SCENE-REFERRED. Lighting is computed with
//           headroom and values above 1 are real -- they are what reaches the
//           EDR compositor instead of clipping at SDR white.
//   ui      RGBA8Unorm, ENCODED, PREMULTIPLIED. Debug lines and ImGui blend in
//           encoded space, which is the space UI is authored for; blending them
//           into a linear surface makes a 50%-alpha panel land at encoded 0.735
//           instead of 0.5.
//   surface whatever the swapchain is, in whatever colour space it was tagged.
//
// THE UI COSTS HDR ONLY WHERE IT COVERS. Compositing requires encoding the
// scene, and encoding clamps -- so the composite runs only where ui.a > 0 and
// every other texel keeps its full range. The engine reached the same
// arrangement in shaders/passes/tonemapping.wesl; this is that, ported.

module output;

import output_transform;

// x = output mode (IS badlands::rhi::ColorSpace -- see the static_asserts beside
//     that enum)
// y = 1 to tonemap the scene, 0 to pass it through
//
// A float4 rather than two scalars because a constant buffer of one scalar is a
// 4-byte object every backend pads differently.
public struct OutputParams {
    public float4 mode;
}

ConstantBuffer<OutputParams> params;
Texture2D<float4> scene;
Texture2D<float4> ui;
SamplerState scene_sampler;

struct VOut {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
}

// The standard three-vertex fullscreen triangle: no vertex buffer, no index
// buffer, and one triangle rather than two so there is no diagonal seam where
// the quads would meet.
[shader("vertex")]
VOut vs_fullscreen(uint vid : SV_VertexID)
{
    let uv = float2(float((vid << 1) & 2), float(vid & 2));
    VOut o;
    o.pos = float4(uv * 2.0 - 1.0, 0.0, 1.0);
    o.uv = float2(uv.x, 1.0 - uv.y);
    return o;
}

// Premultiplied "over", in ENCODED space -- what 8-bit hardware blending does
// when UI is drawn straight onto the surface.
float3 CompositeUiOverEncoded(float3 scene_encoded, float4 overlay)
{
    return overlay.rgb + scene_encoded * (1.0 - overlay.a);
}

[shader("fragment")]
float4 fs_output(VOut i) : SV_Target
{
    // Load, not Sample: this is a 1:1 blit and any filtering would be a bug
    // that shows up as a half-texel softness nobody looks for.
    uint w, h;
    scene.GetDimensions(w, h);
    let px = int3(int(i.uv.x * float(w)), int(i.uv.y * float(h)), 0);
    let hdr = scene.Load(px);
    let overlay = ui.Load(px);

    // The tone curve, decided by AppliesTonemap so the CPU oracles can predict
    // the same thing. It is off on an extended-range surface (which needs no
    // fit to a range it does not have) and off for every debug view (whose
    // value is already display-referred -- curving it would turn "roughness is
    // 0.35" into a preview showing 0.26).
    let mode = uint(params.mode.x);
    let scene_is_referred = params.mode.y > 0.5;
    var lin = hdr.rgb;
    if (AppliesTonemap(scene_is_referred, mode)) { lin = lin / (lin + 1.0); }

    // THE COMPOSITE, IN THE SPACE THE SURFACE CAN ACTUALLY HOLD.
    //
    // On an 8-bit surface, encoded: that is what hardware blending would have
    // done had the UI drawn straight onto it, and it is the space UI is
    // authored for.
    //
    // On an extended-range surface, LINEAR -- because encoding CLAMPS. The
    // encoded form guarded on `overlay.a > 0` and so destroyed the scene's
    // headroom at ANY non-zero alpha: a 1/255 antialias fringe on a glyph edge
    // crushed a scene value of 4.0 to 1.0 while the UI contributed 2% of the
    // texel, drawing a hard dark ring around every window, separator, letter
    // and debug line. Linear "over" never encodes, so the scene keeps its value
    // scaled by coverage, and there is no 8-bit reference behaviour to match on
    // a surface that has none.
    //
    // The overlay's OWN blending is untouched either way -- that already
    // happened inside the 8-bit overlay, in encoded space.
    if (overlay.a > 0.0)
    {
        if (OutputIsExtendedRange(mode))
        {
            lin = SrgbToLinear(overlay.rgb) + lin * (1.0 - overlay.a);
        }
        else
        {
            let scene_enc = LinearToSrgb(clamp(lin, 0.0, 1.0));
            lin = SrgbToLinear(CompositeUiOverEncoded(scene_enc, overlay));
        }
    }

    return EncodeOutput(lin, 1.0, mode);
}
