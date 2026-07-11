// Building box: per-object model matrix + color via dynamic-offset uniforms
// at group(1), lit by the directional sun + constant ambient.
import package::common::frame::{frame};

// Port of ObjectUniforms (transform.hpp) + per-building color.
struct ObjectUniforms {
    modelMatrix: mat4x4<f32>, // LocalSpace -> WorldCameraOffsetedSpace
    color: vec4<f32>,
}

@group(1) @binding(0) var<uniform> object: ObjectUniforms;

struct VertexInput {
    @location(0) position: vec3<f32>,
    @location(1) uv: vec2<f32>,
    @location(2) normal: vec3<f32>,
    @location(3) tangent: vec3<f32>,
}

struct VertexOutput {
    @builtin(position) position: vec4<f32>,
    @location(0) normal: vec3<f32>,
}

@vertex
fn vs_main(input: VertexInput) -> VertexOutput {
    var output: VertexOutput;
    let posOffseted = object.modelMatrix * vec4<f32>(input.position, 1.0);
    output.position = frame.projMatrix * frame.viewMatrix * posOffseted;
    // Uniform scale per axis only; normalize handles the scaling.
    let normalMatrix = mat3x3<f32>(
        object.modelMatrix[0].xyz,
        object.modelMatrix[1].xyz,
        object.modelMatrix[2].xyz
    );
    output.normal = normalMatrix * input.normal;
    return output;
}

@fragment
fn fs_main(input: VertexOutput) -> @location(0) vec4<f32> {
    let n = normalize(input.normal);
    let l = normalize(frame.sunDirection.xyz);
    let ndotl = max(dot(n, l), 0.0);
    let ambient = vec3<f32>(0.12, 0.13, 0.16);
    let color = object.color.rgb * (frame.sunColor.xyz * ndotl + ambient);
    return vec4<f32>(color, 1.0);
}
