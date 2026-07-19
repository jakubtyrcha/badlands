// Terrain cluster-LOD material (deferred): flat-shaded terrain from per-vertex
// baked color + metadata, drawn as many indexed sub-ranges of one shared mesh
// (see game/geometry/terrain_clusters.*). Coarse LODs have no texture detail —
// the vertex color (baked from the biome palette) IS the albedo — so this
// shader samples no textures; group 0 carries only the frame UBO.
//
// Vertex layout (kTerrainCluster): pos(vec3) + normal(vec3) + color(Unorm8x4) +
// meta(Uint8x4). meta = {biome_id, position_hash_byte, lod_level, 0}.
//
// debug_params.x selects a debug source (branches wired now; the UI that drives
// them lands in a later milestone): 0 = normal shading (albedo = vertex color),
// 1 = tint by meta.y (position hash), 2 = tint by meta.z (LOD level).

// shadow_pass: depth-only render from the sun's point of view (the kShadow
// pipeline variant, material.cpp pushes the feature). It MUST project through
// the light's matrix, not the camera's — same mechanism as terrain_blend.
@if(!shadow_pass)
import package::common::frame::{frame, worldCameraOffsetedSpaceToClipSpace};
@if(shadow_pass)
import package::common::frame::{frame, worldCameraOffsetedSpaceToLightClipSpace};
@if(!shadow_pass)
import package::common::gbuffer_encode::{GBufferOutput, encodeOctahedron};

struct TerrainClusterUniforms {
    modelMatrix: mat4x4<f32>,
    debug_params: vec4<f32>,   // x = debug source mode (0 shade / 1 hash / 2 lod)
}

@group(1) @binding(0) var<uniform> object: TerrainClusterUniforms;

struct VertexInput {
    @location(0) pos: vec3<f32>,
    @location(1) normal: vec3<f32>,
    @location(2) color: vec4<f32>,   // Unorm8x4 -> components in 0..1
    @location(3) packed_meta: vec4<u32>,  // Uint8x4 -> components in 0..255
                                          // (`meta` is a WGSL reserved word)
}

struct VertexOutput {
    @builtin(position) position: vec4<f32>,
    @location(0) worldNormal: vec3<f32>,
    @location(1) color: vec4<f32>,
    // All three verts of a triangle belong to one cluster, so flat is safe.
    @location(2) @interpolate(flat) packed_meta: vec4<u32>,
}

// Shadow pass writes depth only — no normals, no color.
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
    output.color = input.color;
    output.packed_meta = input.packed_meta;
    return output;
}

// Map a byte (0..255) to a distinct, evenly-spread hue. Used by the debug tint
// modes so adjacent hash/level values read as clearly different colors.
fn tint_from_unit(t: f32) -> vec3<f32> {
    return 0.5 + 0.5 * cos(6.2831853 * (t + vec3<f32>(0.0, 0.3333, 0.6667)));
}

@if(!shadow_pass)
@fragment
fn fs_gbuffer(input: VertexOutput) -> GBufferOutput {
    var output: GBufferOutput;

    var albedo = input.color.rgb;
    let mode = i32(round(object.debug_params.x));
    if (mode == 1) {
        // Tint by the per-vertex position-hash byte (meta.y).
        albedo = tint_from_unit(f32(input.packed_meta.y) / 255.0);
    } else if (mode == 2) {
        // Tint by LOD level (meta.z). A golden-ratio step keeps adjacent levels
        // visually distinct rather than a slow ramp.
        albedo = tint_from_unit(fract(f32(input.packed_meta.z) * 0.61803399));
    }

    output.normals = encodeOctahedron(normalize(input.worldNormal));
    output.albedo = vec4<f32>(albedo, 1.0);
    // R=roughness (fixed 0.9), G=shadow(1=none), B=baked AO (1.0), A=reserved.
    output.material = vec4<f32>(0.9, 1.0, 1.0, 0.0);
    return output;
}
