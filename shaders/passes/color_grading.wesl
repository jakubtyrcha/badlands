// Color-grading pass (Task: P3/HDR color grading) — an HDR->HDR fullscreen
// remap on the linear-sRGB working buffer, after projected decals and before
// debug lines. Reads the snapshot copy of the HDR colour (a target can't be
// sampled while bound) and writes the graded result back to the HDR target.
//
// Operates in Oklab (perceptual): crush blacks below a lightness threshold,
// desaturate midtones while sparing saturated colors (chroma mask) and HDR
// highlights (L > 1 falls outside the midtone band, so the luminance masks
// preserve them completely).
//
// Deliberately does NOT import package::common::frame: this pass needs no
// frame state, so it declares its own tiny params UBO (like ui.wesl) and
// inlines the fullscreen triangle.
import package::common::colorspace::{linear_srgb_to_oklab, oklab_to_linear_srgb};

// Must match ColorGradingParamsUniform in color_grading.cpp (8 x f32 = 32 B).
struct ColorGradingParams {
    blackCrushThreshold: f32,        // crush below this Oklab lightness
    blackCrushStrength: f32,         // crush exponent (~1.5 - 3)
    midtoneLuminanceStart: f32,      // midtone band lower edge
    midtoneLuminanceEnd: f32,        // midtone band upper edge
    midtoneDesatStrength: f32,       // 0..1 chroma removal in the band
    saturationPreservationMask: f32, // chroma at which desat fades out
    _pad0: f32,
    _pad1: f32,
}

@group(0) @binding(0) var hdr_texture: texture_2d<f32>;
@group(0) @binding(1) var<uniform> params: ColorGradingParams;

struct VertexOutput {
    @builtin(position) Position: vec4<f32>,
}

@vertex
fn vs_main(@builtin(vertex_index) VertexIndex: u32) -> VertexOutput {
    // Fullscreen triangle: (-1,-1), (3,-1), (-1,3). Same math as
    // frame.wesl's getFullscreenTriangleVertex, inlined to keep this module
    // free of the frame UBO.
    let uv = vec2<f32>(f32((VertexIndex << 1u) & 2u), f32(VertexIndex & 2u));
    var output: VertexOutput;
    output.Position = vec4<f32>(uv * 2.0 - 1.0, 0.0, 1.0);
    return output;
}

@fragment
fn fs_main(input: VertexOutput) -> @location(0) vec4<f32> {
    let texCoord = vec2<i32>(input.Position.xy);
    let hdr = textureLoad(hdr_texture, texCoord, 0);

    // Linear sRGB -> Oklab. HDR highlights land at lightness > 1.0.
    var lab = linear_srgb_to_oklab(hdr.rgb);

    // 1. Crush blacks: below the threshold, remap lightness with a power
    // curve. Threshold is division-guarded (mirrors the editor's 0.01 floor).
    let crush_threshold = max(params.blackCrushThreshold, 0.01);
    if (lab.x < crush_threshold && lab.x > 0.0) {
        let l_norm = lab.x / crush_threshold;
        lab.x = crush_threshold * pow(l_norm, params.blackCrushStrength);
    }

    // 2. Desaturate midtones. The luminance mask targets only the SDR midtone
    // band — HDR highlights (L > 1) get 0 and are preserved completely. The
    // chroma mask spares already-saturated colors.
    let chroma = length(lab.yz);
    let midtoneMask =
        smoothstep(params.midtoneLuminanceStart - 0.1,
                   params.midtoneLuminanceStart, lab.x) *
        (1.0 - smoothstep(params.midtoneLuminanceEnd,
                          params.midtoneLuminanceEnd + 0.1, lab.x));
    let desatMask = 1.0 - smoothstep(params.saturationPreservationMask * 0.5,
                                     params.saturationPreservationMask, chroma);
    let strength = params.midtoneDesatStrength * midtoneMask * desatMask;
    lab = vec3<f32>(lab.x, lab.y * (1.0 - strength), lab.z * (1.0 - strength));

    // Back to linear sRGB. Oklab edits can push slightly out of gamut; clamp
    // negatives so they never reach downstream passes (or the EDR compositor).
    let rgb = oklab_to_linear_srgb(lab);
    return vec4<f32>(max(rgb, vec3<f32>(0.0)), hdr.a);
}
