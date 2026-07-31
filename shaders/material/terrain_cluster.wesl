// Terrain cluster-LOD material (deferred): PBR terrain drawn as many indexed
// sub-ranges of one shared mesh (see game/geometry/terrain_clusters.*).
//
// Per-biome materials come from three texture_2d_arrays blended by weights read
// from a biome SPLAT texture sampled in world XZ — NOT from the vertex data.
// That is deliberate: the cluster DAG decimates vertices, so vertex-carried
// weights would smear biome boundaries at coarse LODs, while a splat gives the
// coarsest cluster full-resolution biome detail. The blend itself is shared with
// terrain_blend.wesl (common/terrain_layers.wesl).
//
// Vertex layout (kTerrainCluster): pos(vec3) + normal(vec3) + color(Unorm8x4) +
// meta(Uint8x4). meta = {biome_id, position_hash_byte, lod_level, 0}. `color`
// (the baked biome-palette colour) is retained in the format but no longer
// feeds albedo — removing it would churn the DAG build and its tests for no
// runtime win.
//
// debug_params.x selects a debug source: 0 = normal shading (blended materials),
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
@if(!shadow_pass)
import package::common::terrain_layers::{sampleTerrainLayers, terrainPlanarUv, terrainPlanarTbn};

struct TerrainClusterUniforms {
    modelMatrix: mat4x4<f32>,
    debug_params: vec4<f32>,   // x = debug source mode (0 shade / 1 hash / 2 lod)
    // World XZ -> splat UV: uv = worldXZ * splat_uv.xy + splat_uv.zw. The scale
    // is inset by half a texel on each side so the outermost sample lands on a
    // texel CENTRE rather than half-way off the raster.
    splat_uv: vec4<f32>,
}

@group(0) @binding(1) var albedo_array: texture_2d_array<f32>;
@group(0) @binding(2) var terrain_sampler: sampler;
@group(0) @binding(3) var normal_array: texture_2d_array<f32>;   // GL-convention
@group(0) @binding(4) var arm_array: texture_2d_array<f32>;      // R=AO G=rough B=metal A=displacement
@group(0) @binding(5) var biome_splat0: texture_2d<f32>;         // weights, slots 0..3
@group(0) @binding(6) var biome_splat1: texture_2d<f32>;         // weights, slots 4..7
@group(0) @binding(7) var splat_sampler: sampler;                // linear + CLAMP

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
    @location(3) worldOffsetPos: vec3<f32>,
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
    output.worldOffsetPos = world_offset_pos;
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

    // Biome weights from the splat, in TRUE world XZ (re-add cameraWorldPos, as
    // the material UV does) so the lookup is camera-stable.
    let world = input.worldOffsetPos + frame.cameraWorldPos.xyz;
    let suv = world.xz * object.splat_uv.xy + object.splat_uv.zw;
    let w0 = textureSample(biome_splat0, splat_sampler, suv);
    let w1 = textureSample(biome_splat1, splat_sampler, suv);

    let uv = terrainPlanarUv(input.worldOffsetPos);
    // textureSample and the derivatives MUST be taken in uniform control flow —
    // hence here, above the debug branches, and hence textureSampleGrad inside
    // the blend's per-layer loop.
    let dx = dpdx(uv);
    let dy = dpdy(uv);
    let surf = sampleTerrainLayers(albedo_array, normal_array, arm_array,
                                   terrain_sampler, uv, w0, w1, dx, dy);

    var albedo = surf.albedo;
    let mode = i32(round(object.debug_params.x));
    if (mode == 1) {
        // Tint by the per-vertex position-hash byte (meta.y).
        albedo = tint_from_unit(f32(input.packed_meta.y) / 255.0);
    } else if (mode == 2) {
        // Tint by LOD level (meta.z). A golden-ratio step keeps adjacent levels
        // visually distinct rather than a slow ramp.
        albedo = tint_from_unit(fract(f32(input.packed_meta.z) * 0.61803399));
    }

    let TBN = terrainPlanarTbn(input.worldNormal);
    output.normals = encodeOctahedron(normalize(TBN * surf.tangent_normal));
    output.albedo = vec4<f32>(albedo, 1.0);
    // R=roughness, G=shadow(1=none), B=baked AO, A=reserved.
    output.material = vec4<f32>(surf.roughness, 1.0, surf.ao, 0.0);
    return output;
}
