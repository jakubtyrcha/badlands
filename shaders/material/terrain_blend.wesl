// Terrain-blend material (deferred): each vertex carries up to 4 compact
// (layer, weight) pairs (Uint8x4 indices + Unorm8x4 weights). The VERTEX stage
// scatters them into an 8-slot per-layer weight vector output as two vec4
// varyings, which interpolate as floats — so the mesh is INDEXABLE (shared
// vertices, no flat integer varyings). The FRAGMENT stage hands those weights
// to the shared blend in common/terrain_layers.wesl, which resolves them into
// ONE G-buffer sample; the single BRDF is done later by
// shaders/passes/deferred_lighting.wesl.
//
// The blend itself (de-tiling, height-lerp, the three arrays) is SHARED with
// material/terrain_cluster.wesl — the only difference between the two materials
// is where the weights come from (per-vertex here, a splat texture there).

// shadow_pass: depth-only render from the sun's point of view (the kShadow
// pipeline variant, material.cpp pushes the feature). It MUST project through
// the light's matrix, not the camera's — without this path the terrain writes
// camera-projected depth into the light's shadow map and the whole map shadows
// itself.
@if(!shadow_pass)
import package::common::frame::{frame, worldCameraOffsetedSpaceToClipSpace};
@if(shadow_pass)
import package::common::frame::{frame, worldCameraOffsetedSpaceToLightClipSpace};
@if(!shadow_pass)
import package::common::gbuffer_encode::{GBufferOutput, encodeOctahedron};
@if(!shadow_pass)
import package::common::terrain_layers::{sampleTerrainLayers, terrainPlanarUv, terrainPlanarTbn};

struct TerrainUniforms {
    modelMatrix: mat4x4<f32>,
}

@group(0) @binding(1) var albedo_array: texture_2d_array<f32>;
@group(0) @binding(2) var terrain_sampler: sampler;
@group(0) @binding(3) var normal_array: texture_2d_array<f32>;   // GL-convention
@group(0) @binding(4) var arm_array: texture_2d_array<f32>;      // R=AO G=rough B=metal A=displacement
@group(1) @binding(0) var<uniform> object: TerrainUniforms;

struct VertexInput {
    @location(0) pos: vec3<f32>,
    @location(1) normal: vec3<f32>,
    @location(2) layer_indices: vec4<u32>,   // Uint8x4 -> components in 0..255
    @location(3) blend_weights: vec4<f32>,   // Unorm8x4 -> components in 0..1
}

struct VertexOutput {
    @builtin(position) position: vec4<f32>,
    @location(0) worldNormal: vec3<f32>,
    @location(1) worldOffsetPos: vec3<f32>,
    @location(2) w0: vec4<f32>,   // interpolated weights for layers 0..3
    @location(3) w1: vec4<f32>,   // interpolated weights for layers 4..7
}

// Shadow pass writes depth only — no normals, no blend weights.
struct VertexOutputShadow {
    @builtin(position) position: vec4<f32>,
}

@if(shadow_pass)
@vertex
fn vs_main(input: VertexInput) -> VertexOutputShadow {
    var output: VertexOutputShadow;
    // modelMatrix is already rebased into camera-offset space on the CPU; the
    // helper undoes that before applying frame.lightViewProj.
    let world_offset_pos = (object.modelMatrix * vec4<f32>(input.pos, 1.0)).xyz;
    output.position = worldCameraOffsetedSpaceToLightClipSpace(world_offset_pos);
    return output;
}

@if(shadow_pass)
@fragment
fn fs_gbuffer(input: VertexOutputShadow) {
    // Depth is written automatically via @builtin(position).
}

@if(!shadow_pass)
@vertex
fn vs_main(input: VertexInput) -> VertexOutput {
    var output: VertexOutput;
    // modelMatrix is already rebased into camera-offset space on the CPU.
    let world_offset_pos = (object.modelMatrix * vec4<f32>(input.pos, 1.0)).xyz;
    output.position = worldCameraOffsetedSpaceToClipSpace(world_offset_pos);
    output.worldNormal = normalize((object.modelMatrix * vec4<f32>(input.normal, 0.0)).xyz);
    output.worldOffsetPos = world_offset_pos;

    // Scatter the (layer, weight) pairs into 8 per-layer weight slots.
    var w = array<f32, 8>(0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0);
    for (var k = 0u; k < 4u; k = k + 1u) {
        let li = min(input.layer_indices[k], 7u);
        w[li] = w[li] + input.blend_weights[k];
    }
    output.w0 = vec4<f32>(w[0], w[1], w[2], w[3]);
    output.w1 = vec4<f32>(w[4], w[5], w[6], w[7]);
    return output;
}

@if(!shadow_pass)
@fragment
fn fs_gbuffer(input: VertexOutput) -> GBufferOutput {
    var output: GBufferOutput;
    let uv = terrainPlanarUv(input.worldOffsetPos);
    // Derivatives MUST be taken in uniform control flow (before the blend's
    // per-layer branches) — see sampleTerrainLayers.
    let dx = dpdx(uv);
    let dy = dpdy(uv);

    let surf = sampleTerrainLayers(albedo_array, normal_array, arm_array,
                                   terrain_sampler, uv, input.w0, input.w1,
                                   dx, dy);

    let TBN = terrainPlanarTbn(input.worldNormal);
    output.normals = encodeOctahedron(normalize(TBN * surf.tangent_normal));
    output.albedo = vec4<f32>(surf.albedo, 1.0);
    // R=roughness, G=shadow(1=none), B=baked AO, A=reserved.
    output.material = vec4<f32>(surf.roughness, 1.0, surf.ao, 0.0);
    return output;
}
