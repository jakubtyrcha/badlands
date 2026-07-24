// Leaf card material: forward-opaque, alpha-tested (cutout), double-sided, lit
// (sun + SH ambient) with alpha-tested (leaf-shaped) shadow CASTING.
// Variants (MaterialPassType -> features): "_fwd_opaque" (kForward) + shadow_pass
// (kShadow). Vertex layout: pos(3)+uv(2)+normal(3)+tangent(3).
//
// NO @group(2): this shader deliberately does NOT bind the engine forward
// resources. The forward-OPAQUE pass renders with the G-buffer depth as a
// WRITABLE depth attachment (leaf cards occlude one another), and Dawn forbids
// a texture being both a writable render attachment and a sampled bind-group
// entry in one render pass. The engine's group-2 array always includes that
// depth as `sceneDepth` (binding 0) — fine for the water surface, whose
// transparent pass is depth-READ-ONLY, but a hard sync-scope violation for a
// depth-writing opaque pass. So leaves receive no shadow-map / IBL here; they
// are lit by the shadowed sun contribution's UNshadowed twin (sun + SH ambient).
// Leaves still CAST leaf-shaped shadows: the shadow_pass variant below (no
// group 2) alpha-tests into the sun shadow map, which the bark/floor sample.

@if(!shadow_pass)
import package::common::frame::{frame, worldCameraOffsetedSpaceToClipSpace};
@if(shadow_pass)
import package::common::frame::{frame, worldCameraOffsetedSpaceToLightClipSpace};
@if(!shadow_pass)
import package::common::sh_lighting::{evaluateAmbientSHL2};

// Material leaf texture — group 0, mirroring normalmapped.wesl's albedo slot.
@group(0) @binding(1) var albedo_texture: texture_2d<f32>;
@group(0) @binding(2) var material_sampler: sampler;

struct LeafUniforms {
    modelMatrix: mat4x4<f32>,
    tintCutoff: vec4<f32>,   // xyz = tint, w = alpha cutoff
}
@group(1) @binding(0) var<uniform> object: LeafUniforms;

struct VertexInput {
    @location(0) pos: vec3<f32>,
    @location(1) uv: vec2<f32>,
    @location(2) normal: vec3<f32>,
    @location(3) tangent: vec3<f32>,
}
@if(!shadow_pass)
struct VertexOutput {
    @builtin(position) position: vec4<f32>,
    @location(0) uv: vec2<f32>,
    @location(1) worldNormal: vec3<f32>,
}
@if(shadow_pass)
struct VertexOutputShadow {
    @builtin(position) position: vec4<f32>,
    @location(0) uv: vec2<f32>,
}

@if(!shadow_pass)
@vertex
fn vs_main(input: VertexInput) -> VertexOutput {
    var output: VertexOutput;
    let world = (object.modelMatrix * vec4<f32>(input.pos, 1.0)).xyz;
    output.position = worldCameraOffsetedSpaceToClipSpace(world);
    output.uv = input.uv;
    output.worldNormal = (object.modelMatrix * vec4<f32>(input.normal, 0.0)).xyz;
    return output;
}
@if(shadow_pass)
@vertex
fn vs_main(input: VertexInput) -> VertexOutputShadow {
    var output: VertexOutputShadow;
    let world = (object.modelMatrix * vec4<f32>(input.pos, 1.0)).xyz;
    output.position = worldCameraOffsetedSpaceToLightClipSpace(world);
    output.uv = input.uv;
    return output;
}

@if(!shadow_pass)
@fragment
fn fs_main(input: VertexOutput, @builtin(front_facing) front: bool) -> @location(0) vec4<f32> {
    let tex = textureSample(albedo_texture, material_sampler, input.uv);
    if (tex.a < object.tintCutoff.w) { discard; }

    // Double-sided: flip the interpolated normal toward the viewer for back faces
    // so a card facing away from the sun is still shaded (not black).
    var N = normalize(input.worldNormal);
    if (!front) { N = -N; }

    let L = normalize(frame.sunDirection.xyz);
    let NdotL = max(dot(N, L), 0.0);
    let ambient = evaluateAmbientSHL2(N, frame.ambientSH);
    let sun = NdotL * frame.sunColor.rgb;
    let albedo = tex.rgb * object.tintCutoff.xyz;

    let color = albedo * (ambient + sun);
    return vec4<f32>(color, 1.0);
}

@if(shadow_pass)
@fragment
fn fs_main(input: VertexOutputShadow) {
    // Leaf-shaped shadows: alpha-test in the depth-only pass (explicit LOD — no
    // derivatives in a depth-only fragment shader).
    let a = textureSampleLevel(albedo_texture, material_sampler, input.uv, 0.0).a;
    if (a < object.tintCutoff.w) { discard; }
}
