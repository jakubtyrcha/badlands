// The one place a colour reaches a presented surface.
//
// EXACTLY ONE CALLER, and that is the design rather than an accident. Passes
// draw into an offscreen scene target in ENCODED space -- the space UI blending
// is authored for -- and a single fullscreen output pass converts that target to
// whatever the surface needs. Three producers each converting for themselves was
// the first draft, and it is wrong for a reason no SDR display can show:
//
//   Alpha blending happens in the SURFACE's space. A 50%-alpha white over black
//   lands at encoded 0.5 on an 8-bit sRGB surface -- perceptually mid-grey --
//   and at encoded 0.735 on an extended-linear one. Every translucent panel
//   washes out and every antialiased glyph edge shifts.
//
// The engine already knows this: shaders/common/ui_composite.wesl composites in
// encoded space and tonemapping.wesl converts primaries only afterwards.
module output_transform;

// Mirrors badlands::rhi::ColorSpace, checked by static_assert on the C++ side.
// The output mode IS the colour space: one question, one value.
public static const uint kOutputSrgb = 0u;
public static const uint kOutputDisplayP3 = 1u;
public static const uint kOutputExtendedLinearP3 = 2u;

// The IEC 61966-2-1 curve, not the 2.2 power approximation. Display P3 reuses
// it exactly, which is why an 8-bit surface changes gamut without changing
// encoding -- and why one function serves both 8-bit modes.
public float3 SrgbToLinear(float3 c)
{
    let lo = c / 12.92;
    let hi = pow(max(c + 0.055, 0.0) / 1.055, 2.4);
    return float3(c.x <= 0.04045 ? lo.x : hi.x,
                  c.y <= 0.04045 ? lo.y : hi.y,
                  c.z <= 0.04045 ? lo.z : hi.z);
}

public float3 LinearToSrgb(float3 c)
{
    let v = max(c, 0.0);
    let lo = v * 12.92;
    let hi = 1.055 * pow(v, 1.0 / 2.4) - 0.055;
    return float3(v.x <= 0.0031308 ? lo.x : hi.x,
                  v.y <= 0.0031308 ? lo.y : hi.y,
                  v.z <= 0.0031308 ? lo.z : hi.z);
}

// Linear sRGB primaries -> linear Display P3 primaries. Copied from
// shaders/common/colorspace.wesl, itself XYZ_TO_P3 * SRGB_TO_XYZ.
//
// BOTH SPACES ARE D65, so each row sums to 1 and a neutral maps to itself
// exactly. That is not a curiosity: it is why the greyscale debug views still
// read back as their source bytes under P3, and why a test that forgets this
// matrix passes on the scalars and fails on the chromatics.
static const float3x3 kSrgbToP3 = float3x3(
    0.8224619688, 0.1775380312, 0.0000000000,
    0.0331941989, 0.9668058012, 0.0000000000,
    0.0170826307, 0.0723974406, 0.9105199285);

public float3 LinearSrgbToLinearP3(float3 c) { return mul(kSrgbToP3, c); }

// `linear_display` is display-referred and LINEAR, in sRGB primaries. Alpha is
// passed through untouched -- it carries no colour and no transfer.
public float4 EncodeOutput(float3 linear_display, float alpha, uint mode)
{
    if (mode == kOutputExtendedLinearP3)
    {
        // Unclamped on purpose: values above 1 are the whole point of an EDR
        // surface, and clamping here would silently make it an SDR one. Only
        // the negative side is cut, because a negative primary is out of gamut
        // rather than bright.
        return float4(max(LinearSrgbToLinearP3(linear_display), 0.0), alpha);
    }
    if (mode == kOutputDisplayP3)
    {
        let p3 = clamp(LinearSrgbToLinearP3(linear_display), 0.0, 1.0);
        return float4(LinearToSrgb(p3), alpha);
    }
    // kOutputSrgb: untagged. No primaries change -- the surface is already in
    // the space the values are authored in.
    return float4(LinearToSrgb(clamp(linear_display, 0.0, 1.0)), alpha);
}

// The same, for input that is ALREADY sRGB-encoded.
//
// Two real callers, and neither is a wasted round-trip:
//
//   * the output pass, whose scene target holds encoded values because that is
//     where the UI and the lines blended;
//   * a debug view showing a raw data channel, which wants its texel's 0..1
//     value to survive a readback unchanged -- that is a claim about a CODE
//     value, not an intensity.
//
// Kept separate from EncodeOutput so "interpret this as encoded" is a decision
// made at the point the claim is made, rather than a property of the contract
// that every other caller silently inherits.
public float4 EncodeOutputFromSrgb(float3 srgb_encoded, float alpha, uint mode)
{
    return EncodeOutput(SrgbToLinear(saturate(srgb_encoded)), alpha, mode);
}
