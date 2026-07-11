// Screen-space UI: quads in physical pixels, vertex colors in sRGB, coverage
// from the R8 glyph atlas (a white texel is reserved for solid rects).
// Colors are linearized when the output surface is linear (RGBA16Float).
import package::common::frame::{frame};
import package::common::color::{srgb_to_linear};

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
        input.position.x / frame.screenSize.x * 2.0 - 1.0,
        1.0 - input.position.y / frame.screenSize.y * 2.0
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
    if (frame.outputIsLinear == 1u) {
        rgb = srgb_to_linear(rgb);
    }
    return vec4<f32>(rgb, input.color.a * coverage);
}
