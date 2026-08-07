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

    // THE TONE CURVE IS A FIT TO THE DISPLAY'S RANGE, so an extended-range
    // display needs none.
    //
    // Reinhard maps every input into [0,1) BY CONSTRUCTION -- x/(x+1) < 1 for
    // all x -- so running it on the EDR path would destroy exactly the headroom
    // the float target exists to carry, and the surface would never show a value
    // above SDR white no matter how bright the scene. On an 8-bit surface the
    // curve is what stops bright content clipping flat, so it stays there.
    // shaders/passes/tonemapping.wesl reaches the same split.
    //
    // The flag is also OFF for every debug view, whose value is already
    // display-referred: curving it would turn "roughness is 0.35" into a preview
    // showing 0.26.
    let extended = uint(params.mode.x) == kOutputExtendedLinearP3;
    var lin = hdr.rgb;
    if (params.mode.y > 0.5 && !extended) { lin = lin / (lin + 1.0); }

    // Guarded, because the composite is what costs the headroom: encoding
    // clamps, so a texel the UI does not cover must not go through it.
    if (overlay.a > 0.0)
    {
        let scene_enc = LinearToSrgb(clamp(lin, 0.0, 1.0));
        lin = SrgbToLinear(CompositeUiOverEncoded(scene_enc, overlay));
    }

    return EncodeOutput(lin, 1.0, uint(params.mode.x));
}
