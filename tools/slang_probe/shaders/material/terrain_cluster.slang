// Slang port of shaders/material/terrain_cluster.wesl, for the RHI/Slang probe.
//
// Covers, in one shader, most of what the engine's reflection contract consumes:
// a group-1 uniform buffer with mixed members, three texture_2d_arrays, two 2D
// textures, two samplers, four vertex inputs (including a uint4), a flat
// interpolant, and a three-target G-buffer output.
//
// The WESL original gates on @if(shadow_pass); here that is the SHADOW_PASS
// preprocessor define, which probe A uses to test whether SessionDesc macros
// express the engine's 12 feature flags cleanly.
//
// Bindings carry explicit [[vk::binding(binding, set)]] matching the WESL
// @group/@binding, so probe B can check whether the requested (set, binding)
// survives to each target's reflection or is replaced by target-native indices.

import frame_uniforms;
#ifndef SHADOW_PASS
import gbuffer_encode;
import terrain_layers;
#endif

struct TerrainClusterUniforms
{
    float4x4 modelMatrix;
    float4   debug_params;  // x = debug source mode (0 shade / 1 hash / 2 lod)
    // World XZ -> splat UV: uv = worldXZ * splat_uv.xy + splat_uv.zw
    float4   splat_uv;
};

[[vk::binding(1, 0)]] Texture2DArray<float4> albedo_array;
[[vk::binding(2, 0)]] SamplerState terrain_sampler;
[[vk::binding(3, 0)]] Texture2DArray<float4> normal_array;   // GL-convention
[[vk::binding(4, 0)]] Texture2DArray<float4> arm_array;      // R=AO G=rough B=metal A=disp
[[vk::binding(5, 0)]] Texture2D<float4> biome_splat0;        // weights, slots 0..3
[[vk::binding(6, 0)]] Texture2D<float4> biome_splat1;        // weights, slots 4..7
[[vk::binding(7, 0)]] SamplerState splat_sampler;            // linear + CLAMP

[[vk::binding(0, 1)]] ConstantBuffer<TerrainClusterUniforms> object;

struct VertexInput
{
    float3 pos         : POSITION;
    float3 normal      : NORMAL;
    float4 color       : COLOR;      // Unorm8x4 -> 0..1
    uint4  packed_meta : TEXCOORD0;  // Uint8x4 -> 0..255; {biome, hash, lod, 0}
};

struct VertexOutput
{
    float4 position    : SV_Position;
    float3 worldNormal : TEXCOORD0;
    float4 color       : TEXCOORD1;
    // All three verts of a triangle belong to one cluster, so flat is safe.
    nointerpolation uint4 packed_meta : TEXCOORD2;
    float3 worldOffsetPos : TEXCOORD3;
};

// Shadow pass writes depth only -- no normals, no color.
struct VertexOutputShadow
{
    float4 position : SV_Position;
};

#ifdef SHADOW_PASS

[shader("vertex")]
VertexOutputShadow vs_main(VertexInput input)
{
    VertexOutputShadow output;
    // modelMatrix is already rebased into camera-offset space on the CPU; the
    // helper undoes that before applying frame.lightViewProj.
    let world_offset_pos = mul(object.modelMatrix, float4(input.pos, 1.0)).xyz;
    output.position = worldCameraOffsetedSpaceToLightClipSpace(world_offset_pos);
    return output;
}

[shader("fragment")]
void fs_gbuffer(VertexOutputShadow input)
{
    // Depth is written automatically via SV_Position.
}

#else

[shader("vertex")]
VertexOutput vs_main(VertexInput input)
{
    VertexOutput output;
    let world_offset_pos = mul(object.modelMatrix, float4(input.pos, 1.0)).xyz;
    output.position = worldCameraOffsetedSpaceToClipSpace(world_offset_pos);
    output.worldNormal = normalize(mul(object.modelMatrix, float4(input.normal, 0.0)).xyz);
    output.color = input.color;
    output.packed_meta = input.packed_meta;
    output.worldOffsetPos = world_offset_pos;
    return output;
}

// Map a unit value to a distinct, evenly-spread hue for the debug tint modes.
float3 tint_from_unit(float t)
{
    return 0.5 + 0.5 * cos(6.2831853 * (t + float3(0.0, 0.3333, 0.6667)));
}

[shader("fragment")]
GBufferOutput fs_gbuffer(VertexOutput input)
{
    GBufferOutput output;

    // Biome weights from the splat, in TRUE world XZ so the lookup is
    // camera-stable.
    let world = input.worldOffsetPos + frame.cameraWorldPos.xyz;
    let suv = world.xz * object.splat_uv.xy + object.splat_uv.zw;
    let w0 = biome_splat0.Sample(splat_sampler, suv);
    let w1 = biome_splat1.Sample(splat_sampler, suv);

    let uv = terrainPlanarUv(input.worldOffsetPos);
    // Derivatives MUST be taken in uniform control flow -- hence here, above the
    // debug branches, and hence SampleGrad inside the blend's per-layer loop.
    let dx = ddx(uv);
    let dy = ddy(uv);
    let surf = sampleTerrainLayers(albedo_array, normal_array, arm_array,
                                   terrain_sampler, uv, w0, w1, dx, dy);

    float3 albedo = surf.albedo;
    let mode = int(round(object.debug_params.x));
    if (mode == 1)
    {
        albedo = tint_from_unit(float(input.packed_meta.y) / 255.0);
    }
    else if (mode == 2)
    {
        albedo = tint_from_unit(frac(float(input.packed_meta.z) * 0.61803399));
    }

    let TBN = terrainPlanarTbn(input.worldNormal);
    output.normals = encodeOctahedron(normalize(mul(TBN, surf.tangent_normal)));
    output.albedo = float4(albedo, 1.0);
    // R=roughness, G=shadow(1=none), B=baked AO, A=reserved.
    output.material = float4(surf.roughness, 1.0, surf.ao, 0.0);
    return output;
}

#endif
