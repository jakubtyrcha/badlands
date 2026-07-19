// Minimal forward material for badlands_forward_pass_test — a solid (optionally
// premultiplied) colour, used to verify the forward-opaque and
// forward-transparent (premultiplied blend) material variants + depth-write as
// a per-material property. Not used at runtime.

import package::common::frame::{frame, worldCameraOffsetedSpaceToClipSpace};

struct ForwardDebugUniforms {
    modelMatrix: mat4x4<f32>,
    color: vec4<f32>,  // premultiplied for the transparent variant
}

@group(1) @binding(0) var<uniform> object: ForwardDebugUniforms;

struct VertexInput {
    @location(0) pos: vec3<f32>,
    @location(1) uv: vec2<f32>,
    @location(2) normal: vec3<f32>,
    @location(3) tangent: vec3<f32>,
}

struct VertexOutput {
    @builtin(position) position: vec4<f32>,
}

@vertex
fn vs_main(input: VertexInput) -> VertexOutput {
    var output: VertexOutput;
    let world = (object.modelMatrix * vec4<f32>(input.pos, 1.0)).xyz;
    output.position = worldCameraOffsetedSpaceToClipSpace(world);
    return output;
}

@fragment
fn fs_main(input: VertexOutput) -> @location(0) vec4<f32> {
    return object.color;
}
