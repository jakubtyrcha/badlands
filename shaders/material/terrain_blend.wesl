// Terrain-blend material (deferred): blends up to 4 albedo layers of a
// texture_2d_array by per-vertex (index, weight) pairs into ONE G-buffer
// albedo. The single BRDF is done later by shaders/passes/deferred_lighting.wesl.
//
// Vertex layout kTerrainBlend: pos(3) + normal(3) + blend_weights(vec4) +
// layer_indices(uvec4). blend_weights interpolate; layer_indices are integer
// varyings so they MUST be @interpolate(flat) — the mesh builder gives all three
// vertices of a triangle the same index set (weights carry the blend).
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
    @location(2) blend_weights: vec4<f32>,
    @location(3) layer_indices: vec4<u32>,
}

struct VertexOutput {
    @builtin(position) position: vec4<f32>,
    @location(0) worldNormal: vec3<f32>,
    @location(1) worldOffsetPos: vec3<f32>,
    @location(2) blend_weights: vec4<f32>,
    @location(3) @interpolate(flat) layer_indices: vec4<u32>,
}

@vertex
fn vs_main(input: VertexInput) -> VertexOutput {
    var output: VertexOutput;
    // modelMatrix is already rebased into camera-offset space on the CPU.
    let world_offset_pos = (object.modelMatrix * vec4<f32>(input.pos, 1.0)).xyz;
    output.position = worldCameraOffsetedSpaceToClipSpace(world_offset_pos);
    output.worldNormal = normalize((object.modelMatrix * vec4<f32>(input.normal, 0.0)).xyz);
    output.worldOffsetPos = world_offset_pos;
    output.blend_weights = input.blend_weights;
    output.layer_indices = input.layer_indices;
    return output;
}

// Planar UV from world XZ (re-add cameraWorldPos so tiling is world-stable).
fn planar_uv(world_offset_pos: vec3<f32>) -> vec2<f32> {
    let world = world_offset_pos + frame.cameraWorldPos.xyz;
    return world.xz * UV_SCALE;
}

// Blend up to 4 array layers by (index, weight); normalized. THEN one BRDF
// (deferred lighting), so this writes a single blended albedo to the G-buffer.
fn sample_blended(uv: vec2<f32>, weights: vec4<f32>, indices: vec4<u32>) -> vec3<f32> {
    var c = vec3<f32>(0.0);
    var wsum = 0.0;
    for (var i = 0u; i < 4u; i = i + 1u) {
        let w = weights[i];
        c = c + w * textureSample(albedo_array, terrain_sampler, uv, indices[i]).rgb;
        wsum = wsum + w;
    }
    return c / max(wsum, 1e-4);
}

@fragment
fn fs_gbuffer(input: VertexOutput) -> GBufferOutput {
    var output: GBufferOutput;
    let uv = planar_uv(input.worldOffsetPos);
    let albedo = sample_blended(uv, input.blend_weights, input.layer_indices);
    output.normals = encodeOctahedron(normalize(input.worldNormal));
    output.albedo = vec4<f32>(albedo, 1.0);
    // R=roughness, G=shadow(1=none), B=baked AO, A=reserved.
    output.material = vec4<f32>(TERRAIN_ROUGHNESS, 1.0, 1.0, 0.0);
    return output;
}
