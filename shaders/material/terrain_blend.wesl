// Terrain-blend material (deferred): each vertex carries up to 4 compact
// (layer, weight) pairs (Uint8x4 indices + Unorm8x4 weights). The VERTEX stage
// scatters them into an 8-slot per-layer weight vector output as two vec4
// varyings, which interpolate as floats — so the mesh is INDEXABLE (shared
// vertices, no flat integer varyings). The FRAGMENT stage blends the layers of
// a texture_2d_array by those interpolated weights into ONE G-buffer albedo;
// the single BRDF is done later by shaders/passes/deferred_lighting.wesl.
//
// A forward variant (blend + one inline BRDF, e.g. a future water material) can
// be added as a second fragment entry when a forward-opaque pass exists.

import package::common::frame::{frame, worldCameraOffsetedSpaceToClipSpace};
import package::common::gbuffer_encode::{GBufferOutput, encodeOctahedron};

struct TerrainUniforms {
    modelMatrix: mat4x4<f32>,
}

@group(0) @binding(1) var albedo_array: texture_2d_array<f32>;
@group(0) @binding(2) var terrain_sampler: sampler;
@group(1) @binding(0) var<uniform> object: TerrainUniforms;

const UV_SCALE: f32 = 0.1;            // 1 texture tile per 10 world meters
const TERRAIN_ROUGHNESS: f32 = 0.9;

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

// Planar UV from world XZ (re-add cameraWorldPos so tiling is world-stable).
fn planar_uv(world_offset_pos: vec3<f32>) -> vec2<f32> {
    let world = world_offset_pos + frame.cameraWorldPos.xyz;
    return world.xz * UV_SCALE;
}

// Blend the 8 array layers by their interpolated weights; normalized. Layers
// with zero weight contribute nothing (their array_index is clamped in range),
// so terrain with fewer real layers just leaves the high slots at weight 0.
fn sample_blended(uv: vec2<f32>, w0: vec4<f32>, w1: vec4<f32>) -> vec3<f32> {
    var c = vec3<f32>(0.0);
    var wsum = 0.0;
    for (var i = 0u; i < 4u; i = i + 1u) {
        c = c + w0[i] * textureSample(albedo_array, terrain_sampler, uv, i).rgb;
        c = c + w1[i] * textureSample(albedo_array, terrain_sampler, uv, i + 4u).rgb;
        wsum = wsum + w0[i] + w1[i];
    }
    return c / max(wsum, 1e-4);
}

@fragment
fn fs_gbuffer(input: VertexOutput) -> GBufferOutput {
    var output: GBufferOutput;
    let uv = planar_uv(input.worldOffsetPos);
    let albedo = sample_blended(uv, input.w0, input.w1);
    output.normals = encodeOctahedron(normalize(input.worldNormal));
    output.albedo = vec4<f32>(albedo, 1.0);
    // R=roughness, G=shadow(1=none), B=baked AO, A=reserved.
    output.material = vec4<f32>(TERRAIN_ROUGHNESS, 1.0, 1.0, 0.0);
    return output;
}
