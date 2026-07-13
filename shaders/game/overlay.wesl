// Flat, alpha-blended ground overlay for the placement grid: world-space
// triangles carrying a per-vertex color. Drawn after the opaque scene with the
// depth test on but writes off, so buildings still occlude the grid.
import package::common::frame::{frame, worldSpaceToClipSpace};

struct VertexInput {
    @location(0) position: vec3<f32>,
    @location(1) color: vec4<f32>,
}

struct VertexOutput {
    @builtin(position) position: vec4<f32>,
    @location(0) color: vec4<f32>,
}

@vertex
fn vs_main(input: VertexInput) -> VertexOutput {
    var output: VertexOutput;
    output.position = worldSpaceToClipSpace(input.position);
    output.color = input.color;
    return output;
}

@fragment
fn fs_main(input: VertexOutput) -> @location(0) vec4<f32> {
    // The accumulation target is linear HDR; the color is authored linear with
    // straight alpha for the ALPHA_BLENDING pipeline.
    return input.color;
}
