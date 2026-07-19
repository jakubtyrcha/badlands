// Thick antialiased debug line. The CPU (UploadDebugLines) expands each segment
// into a screen-aligned quad (2 triangles) in WORLD space, carrying a signed
// perpendicular distance + half-thickness per vertex; the vertex stage just
// projects (world -> clip via the frame helper's single camera offset), and the
// fragment stage applies a 1px conical AA fringe. Ported verbatim from sampo's
// src/shaders/geometry/thick_line.wesl.

import package::common::frame::{frame, worldSpaceToClipSpace};
import package::common::color::srgb_to_linear;

struct VertexInput {
    @location(0) pos: vec3<f32>,
    @location(1) color: vec3<f32>,
    @location(2) dist_and_half_thickness: vec2<f32>,
}

struct VertexOutput {
    @builtin(position) Position: vec4<f32>,
    @location(0) color: vec3<f32>,
    @location(1) dist: f32,
    @location(2) half_thickness: f32,
}

@vertex
fn vs_main(input: VertexInput) -> VertexOutput {
    var output: VertexOutput;
    output.Position = worldSpaceToClipSpace(input.pos);
    output.color = input.color;
    output.dist = input.dist_and_half_thickness.x;
    output.half_thickness = input.dist_and_half_thickness.y;
    return output;
}

@fragment
fn fs_main(input: VertexOutput) -> @location(0) vec4<f32> {
    let d = abs(input.dist);
    let half_w = input.half_thickness;
    let alpha = 1.0 - smoothstep(half_w, half_w + 1.0, d);
    if (alpha < 0.01) { discard; }
    return vec4<f32>(srgb_to_linear(input.color), alpha);
}
