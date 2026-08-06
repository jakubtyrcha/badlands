// Probe B follow-up: how do we get the engine's (group, binding) model in Slang?
//
// The first pass showed [[vk::binding(b, set)]] is IGNORED for the metal and
// hlsl targets -- every parameter came back space=0 with per-category sequential
// indices. This shader tests Slang's idiomatic grouping mechanism instead:
// ParameterBlock<T>, which is documented to map to a descriptor set on Vulkan,
// a descriptor table on D3D12, and an argument buffer on Metal.
//
// Layout mirrors what the engine wants:
//   group 0 = frame (shared, one per frame)
//   group 1 = per-material resources
//   group 2 = per-object uniforms

import frame_uniforms;

struct MaterialParams
{
    Texture2DArray<float4> albedo_array;
    Texture2DArray<float4> normal_array;
    Texture2DArray<float4> arm_array;
    Texture2D<float4>      biome_splat0;
    Texture2D<float4>      biome_splat1;
    SamplerState           terrain_sampler;
    SamplerState           splat_sampler;
    float4                 splat_uv;
    float4                 debug_params;
};

struct ObjectParams
{
    float4x4 modelMatrix;
};

ParameterBlock<MaterialParams> material;
ParameterBlock<ObjectParams> object;

struct VsIn
{
    float3 pos    : POSITION;
    float3 normal : NORMAL;
};

struct VsOut
{
    float4 position : SV_Position;
    float3 world    : TEXCOORD0;
};

[shader("vertex")]
VsOut vs_main(VsIn input)
{
    VsOut o;
    let world_offset_pos = mul(object.modelMatrix, float4(input.pos, 1.0)).xyz;
    o.position = worldCameraOffsetedSpaceToClipSpace(world_offset_pos);
    o.world = world_offset_pos;
    return o;
}

[shader("fragment")]
float4 fs_main(VsOut input) : SV_Target
{
    let world = input.world + frame.cameraWorldPos.xyz;
    let suv = world.xz * material.splat_uv.xy + material.splat_uv.zw;
    let w0 = material.biome_splat0.Sample(material.splat_sampler, suv);
    let w1 = material.biome_splat1.Sample(material.splat_sampler, suv);
    let a = material.albedo_array.Sample(material.terrain_sampler,
                                         float3(world.xz, 0.0));
    let n = material.normal_array.Sample(material.terrain_sampler,
                                         float3(world.xz, 0.0));
    let m = material.arm_array.Sample(material.terrain_sampler,
                                      float3(world.xz, 0.0));
    return a * w0.x + n * w1.x + m * material.debug_params.x;
}
