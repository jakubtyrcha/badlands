// Ground quad: textured, lit by the directional sun + constant ambient.
// Vertices are in WorldSpace; frame helpers apply the camera offset.
import package::common::frame::{frame, worldSpaceToClipSpace};

@group(0) @binding(1) var ground_texture: texture_2d<f32>;
@group(0) @binding(2) var ground_sampler: sampler;

struct VertexInput {
    @location(0) position: vec3<f32>,
    @location(1) uv: vec2<f32>,
    @location(2) normal: vec3<f32>,
    @location(3) tangent: vec3<f32>,
}

struct VertexOutput {
    @builtin(position) position: vec4<f32>,
    @location(0) uv: vec2<f32>,
    @location(1) normal: vec3<f32>,
}

@vertex
fn vs_main(input: VertexInput) -> VertexOutput {
    var output: VertexOutput;
    output.position = worldSpaceToClipSpace(input.position);
    output.uv = input.uv;
    output.normal = input.normal;
    return output;
}

@fragment
fn fs_main(input: VertexOutput) -> @location(0) vec4<f32> {
    let albedo = textureSample(ground_texture, ground_sampler, input.uv).rgb;
    let n = normalize(input.normal);
    let l = normalize(frame.sunDirection.xyz);
    let ndotl = max(dot(n, l), 0.0);
    let ambient = vec3<f32>(0.12, 0.13, 0.16);
    let color = albedo * (frame.sunColor.xyz * ndotl + ambient);
    return vec4<f32>(color, 1.0);
}
