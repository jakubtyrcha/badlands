// Slang port of shaders/common/gbuffer_encode.wesl, for the RHI/Slang probe.
//
// GBufferOutput's SV_Target semantics are the probe's fragment-output reflection
// case: the engine consumes {location, name, component_count} per output.
module gbuffer_encode;

public struct GBufferOutput
{
    public float2 normals  : SV_Target0;  // RG16F: octahedron-encoded world normal
    public float4 albedo   : SV_Target1;  // BGRA8: base color
    public float4 material : SV_Target2;  // RGBA8: R=rough G=shadow B=bakedAO A=reserved
};

public float2 encodeOctahedron(float3 n)
{
    let absN = abs(n);
    float2 oct = n.xy / (absN.x + absN.y + absN.z);
    if (n.z < 0.0)
    {
        let s = float2(oct.x >= 0.0 ? 1.0 : -1.0, oct.y >= 0.0 ? 1.0 : -1.0);
        oct = (1.0 - abs(float2(oct.y, oct.x))) * s;
    }
    return oct;
}

// Shading-model tag written to material.a; foliage transmission gates on > 0.5.
public static const float kShadingModelFoliage = 1.0;

public GBufferOutput packVoxelFoliageGBuffer(float3 worldNormal, float3 albedo,
                                             float roughness, float ao,
                                             float translucencyStrength)
{
    GBufferOutput output;
    output.normals = encodeOctahedron(normalize(worldNormal));
    output.albedo = float4(albedo, 1.0);
    output.material = float4(roughness, translucencyStrength, ao, kShadingModelFoliage);
    return output;
}

public GBufferOutput packNormalMappedGBuffer(float3 worldNormal, float3 worldTangent,
                                             float3 albedo, float3 normalMap, float4 arm)
{
    let N = normalize(worldNormal);
    let T = normalize(worldTangent - N * dot(worldTangent, N));  // Gram-Schmidt
    let B = cross(N, T);
    let TBN = float3x3(T, B, N);

    let roughness = arm.g;
    let ao = arm.r;

    let tangentNormal = normalMap * 2.0 - 1.0;
    let outNormal = normalize(mul(TBN, tangentNormal));

    GBufferOutput output;
    output.normals = encodeOctahedron(outNormal);
    output.albedo = float4(albedo, 1.0);
    output.material = float4(roughness, 1.0, ao, 0.0);
    return output;
}
