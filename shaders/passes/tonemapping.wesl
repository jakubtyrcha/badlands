// Tonemapping/gamma resolve pass — HDR linear -> display.
// Mode 0: sRGB clamp+gamma (also the headless-capture path).
// Mode 1: HDR luminance debug visualization.
// Mode 2: Display-P3 output — the single sRGB->P3 step of the P3/HDR output
//         pipeline (the working buffer stays linear sRGB; see the
//         2026-07-24 p3-hdr-color-grading design spec). outputIsLinear picks
//         EDR passthrough (float surface) vs SDR clamp + sRGB-curve encode
//         (Display P3 uses the sRGB transfer curve).
import package::common::frame::{frame, getFullscreenTriangleVertex};
import package::common::color::linear_to_srgb;
import package::common::colorspace::linear_srgb_to_linear_p3;

@group(0) @binding(1) var hdr_texture: texture_2d<f32>;

struct VertexOutput {
    @builtin(position) Position: vec4<f32>,
}

@vertex
fn vs_main(@builtin(vertex_index) VertexIndex: u32) -> VertexOutput {
    let fullscreen = getFullscreenTriangleVertex(VertexIndex);
    var output: VertexOutput;
    output.Position = fullscreen.clipPosition;
    return output;
}

@fragment
fn fs_main(input: VertexOutput) -> @location(0) vec4<f32> {
    let texCoord = vec2<i32>(input.Position.xy);
    let hdr = textureLoad(hdr_texture, texCoord, 0);

    if (frame.tonemapMode == 0u) {
        if (frame.outputIsLinear == 1u) {
            // RGBA16Float: compositor handles gamma, pass linear through
            return hdr;
        }
        // BGRA8Unorm: clamp to [0,1] + sRGB gamma
        let mapped = clamp(hdr.rgb, vec3<f32>(0.0), vec3<f32>(1.0));
        return vec4<f32>(linear_to_srgb(mapped), hdr.a);
    }
    if (frame.tonemapMode == 1u) {
        // HDR luminance debug visualization
        // BT.709 luminance (matches Display P3 / sRGB primaries)
        let luma = 0.22897 * hdr.r + 0.69174 * hdr.g + 0.07929 * hdr.b;
        var color: vec3<f32>;
        if (luma <= 1.0) {
            // SDR range: grayscale
            color = vec3<f32>(clamp(luma, 0.0, 1.0));
        } else {
            // HDR range: green -> yellow -> red (log2 mapped)
            let t = clamp(log2(luma) / 4.0, 0.0, 1.0);
            if (t < 0.5) {
                // green to yellow
                color = vec3<f32>(t * 2.0, 1.0, 0.0);
            } else {
                // yellow to red
                color = vec3<f32>(1.0, (1.0 - t) * 2.0, 0.0);
            }
        }
        if (frame.outputIsLinear == 1u) {
            return vec4<f32>(color, 1.0);
        }
        return vec4<f32>(linear_to_srgb(color), 1.0);
    }
    if (frame.tonemapMode == 2u) {
        // Display-P3 output: one primary conversion for the whole scene.
        let p3 = linear_srgb_to_linear_p3(hdr.rgb);
        if (frame.outputIsLinear == 1u) {
            // Float surface tagged extended-linear-Display-P3: pass linear
            // through, retaining >1.0 for the EDR compositor. Clamp negatives
            // only — out-of-gamut negatives must not reach the compositor.
            return vec4<f32>(max(p3, vec3<f32>(0.0)), hdr.a);
        }
        // 8-bit surface tagged Display P3: clamp to SDR (the only place a
        // tone curve applies) + sRGB-curve encode (= the P3 transfer).
        let mapped = clamp(p3, vec3<f32>(0.0), vec3<f32>(1.0));
        return vec4<f32>(linear_to_srgb(mapped), hdr.a);
    }
    // Unknown mode — output magenta error
    return vec4<f32>(1.0, 0.0, 1.0, 1.0);
}
