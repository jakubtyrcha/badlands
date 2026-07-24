// Screen-space GAME UI (distinct from the Dear ImGui debug UI): quads in
// physical pixels, vertex colors in sRGB, coverage from the R8 glyph atlas (a
// white texel is reserved so solid rects share this pipeline).
// Colors are linearized when the output surface is linear (RGBA16Float).
//
// This pass runs AFTER the tonemap resolve, from an app-owned render pass that
// is outside SceneRenderer's FrameContext. That context's frame UBO is
// transient -- FrameContext::End() releases it (context/frame_context.cpp:217)
// -- so by the time this shader runs the group-0 `frame` buffer no longer
// exists. Hence a small UI-local uniform block instead of
// `import package::common::frame::{frame}`: UiRenderer owns this buffer and
// rewrites it on resize, leaving the pass independent of whether the scene
// renderer ran at all.
import package::common::color::{srgb_to_linear, linear_to_srgb};
import package::common::colorspace::{linear_srgb_to_linear_p3};

struct UiFrame {
    screen_size: vec2<f32>,   // physical pixels
    output_is_linear: u32,    // 1 when the surface is linear (RGBA16Float)
    output_is_p3: u32,        // 1 when the surface is tagged Display-P3
}

@group(0) @binding(0) var<uniform> ui_frame: UiFrame;
@group(0) @binding(1) var ui_atlas: texture_2d<f32>;
@group(0) @binding(2) var ui_sampler: sampler;

struct VertexInput {
    @location(0) position: vec2<f32>, // physical pixels, origin top-left
    @location(1) uv: vec2<f32>,
    @location(2) color: vec4<f32>, // sRGB, straight alpha
}

struct VertexOutput {
    @builtin(position) position: vec4<f32>,
    @location(0) uv: vec2<f32>,
    @location(1) color: vec4<f32>,
}

@vertex
fn vs_main(input: VertexInput) -> VertexOutput {
    var output: VertexOutput;
    let ndc = vec2<f32>(
        input.position.x / ui_frame.screen_size.x * 2.0 - 1.0,
        1.0 - input.position.y / ui_frame.screen_size.y * 2.0
    );
    output.position = vec4<f32>(ndc, 0.0, 1.0);
    output.uv = input.uv;
    output.color = input.color;
    return output;
}

@fragment
fn fs_main(input: VertexOutput) -> @location(0) vec4<f32> {
    let coverage = textureSample(ui_atlas, ui_sampler, input.uv).r;
    var rgb = input.color.rgb;
    if (ui_frame.output_is_p3 == 1u) {
        // sRGB-authored colors onto a Display-P3-tagged surface: decode,
        // convert primaries, then match the surface transfer (linear on the
        // float/EDR surface; sRGB-curve-encoded on the 8-bit SDR-P3 surface).
        // sRGB is inside the P3 gamut, so the clamps are pure safety.
        let lin_p3 = linear_srgb_to_linear_p3(srgb_to_linear(rgb));
        if (ui_frame.output_is_linear == 1u) {
            rgb = max(lin_p3, vec3<f32>(0.0));
        } else {
            rgb = linear_to_srgb(clamp(lin_p3, vec3<f32>(0.0), vec3<f32>(1.0)));
        }
    } else if (ui_frame.output_is_linear == 1u) {
        rgb = srgb_to_linear(rgb);
    }
    return vec4<f32>(rgb, input.color.a * coverage);
}
