// Slang port of shaders/common/terrain_layers.wesl, for the RHI/Slang probe.
//
// Height-lerped 8-layer terrain blend over three texture_2d_arrays. This is the
// heaviest function in the probe set (two passes, per-layer early-outs,
// explicit-gradient sampling), so it dominates terrain_cluster's compile time.
module terrain_layers;

import frame_uniforms;

// Structural constants -- compile-time by design, not runtime knobs.
public static const float TERRAIN_MATERIAL_SCALE = 2.5;      // meters per texture tile
public static const bool  TERRAIN_BREAK_TILING = true;
public static const float TILING_VARIATION_TILES = 1.5;
public static const float TERRAIN_HEIGHT_BLEND_DEPTH = 0.2;
public static const float TERRAIN_MIN_LAYER_WEIGHT = 0.00392;  // 1/255

float hash21(float2 p)
{
    float2 q = frac(p * float2(123.34, 345.45));
    q = q + float2(dot(q, q + 34.345));
    return frac(q.x * q.y);
}

// Smooth value noise in [0,1]; procedural to avoid another binding + upload.
float variation_noise(float2 p)
{
    let i = floor(p);
    let f = frac(p);
    let u = f * f * (3.0 - 2.0 * f);
    let a = hash21(i);
    let b = hash21(i + float2(1.0, 0.0));
    let c = hash21(i + float2(0.0, 1.0));
    let d = hash21(i + float2(1.0, 1.0));
    return lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y);
}

// The two virtual-copy offsets + blend weight, computed once per fragment and
// shared by all three arrays so they never disagree on which copy they sampled.
public struct TileVariation
{
    public float2 offa;
    public float2 offb;
    public float  blend;
};

public TileVariation tile_variation(float2 uv)
{
    TileVariation v;
    let k = variation_noise(uv / TILING_VARIATION_TILES);
    let index = k * 8.0;
    let i = floor(index);
    v.offa = sin(float2(3.0, 7.0) * (i + 0.0));
    v.offb = sin(float2(3.0, 7.0) * (i + 1.0));
    v.blend = frac(index);
    return v;
}

// One layer fetch, with or without de-tiling. Explicit gradients are mandatory:
// the offsets introduce discontinuities that would break implicit mip selection.
public float4 sample_layer(Texture2DArray<float4> t, SamplerState samp, float2 uv,
                           uint layer, float2 dx, float2 dy, TileVariation v, float blend)
{
    if (!TERRAIN_BREAK_TILING)
    {
        return t.SampleGrad(samp, float3(uv, float(layer)), dx, dy);
    }
    let cola = t.SampleGrad(samp, float3(uv + v.offa, float(layer)), dx, dy);
    let colb = t.SampleGrad(samp, float3(uv + v.offb, float(layer)), dx, dy);
    return lerp(cola, colb, blend);
}

public struct BlendedSurface
{
    public float3 albedo;
    public float3 tangent_normal;
    public float  ao;
    public float  roughness;
};

// Planar UV from world XZ (re-add cameraWorldPos so tiling is world-stable).
public float2 terrainPlanarUv(float3 world_offset_pos)
{
    let world = world_offset_pos + frame.cameraWorldPos.xyz;
    return world.xz / TERRAIN_MATERIAL_SCALE;
}

// Tangent frame for the planar XZ projection: dP/du = +X, dP/dv = +Z.
public float3x3 terrainPlanarTbn(float3 n)
{
    let N = normalize(n);
    float3 seed = float3(1.0, 0.0, 0.0);
    if (abs(N.x) > 0.99)
    {
        seed = float3(0.0, 0.0, 1.0);
    }
    let T = normalize(seed - N * dot(seed, N));
    let B = cross(T, N);
    return float3x3(T, B, N);
}

uint layerCount(Texture2DArray<float4> t)
{
    uint w, h, elements, levels;
    t.GetDimensions(0, w, h, elements, levels);
    return elements;
}

// Blend the array layers by their weights, height-lerped and normalized.
// Zero-weight layers are SKIPPED -- a real fragment usually blends <=2 layers
// (~6 samples) rather than the naive 24.
public BlendedSurface sampleTerrainLayers(Texture2DArray<float4> albedo_array,
                                          Texture2DArray<float4> normal_array,
                                          Texture2DArray<float4> arm_array,
                                          SamplerState samp,
                                          float2 uv, float4 w0, float4 w1,
                                          float2 dx, float2 dy)
{
    let max_albedo = layerCount(albedo_array) - 1u;
    let max_normal = layerCount(normal_array) - 1u;
    let max_arm = layerCount(arm_array) - 1u;
    let var_ = tile_variation(uv);

    // Pass 1: resolve each active layer's de-tiling crossfade from ALBEDO and
    // read its height out of ARM alpha.
    float  w_in[8];
    float  blend_c[8];
    float3 albedo_c[8];
    float2 arm_c[8];
    float  h_c[8];
    float ma = -1e9;
    for (uint i = 0u; i < 8u; ++i)
    {
        float w = (i < 4u) ? w0[i] : w1[i - 4u];
        if (w <= TERRAIN_MIN_LAYER_WEIGHT)
        {
            w_in[i] = 0.0;
            continue;
        }
        w_in[i] = w;

        let li_a = min(i, max_albedo);
        float blend = 0.0;
        float3 col = float3(0.0);
        if (TERRAIN_BREAK_TILING)
        {
            let ca = albedo_array.SampleGrad(samp, float3(uv + var_.offa, float(li_a)), dx, dy).rgb;
            let cb = albedo_array.SampleGrad(samp, float3(uv + var_.offb, float(li_a)), dx, dy).rgb;
            let d = ca - cb;
            blend = smoothstep(0.2, 0.8, var_.blend - 0.1 * (d.x + d.y + d.z));
            col = lerp(ca, cb, blend);
        }
        else
        {
            col = albedo_array.SampleGrad(samp, float3(uv, float(li_a)), dx, dy).rgb;
        }
        blend_c[i] = blend;
        albedo_c[i] = col;

        let a = sample_layer(arm_array, samp, uv, min(i, max_arm), dx, dy, var_, blend);
        arm_c[i] = float2(a.r, a.g);
        h_c[i] = a.a;
        ma = max(ma, a.a + w);
    }

    // Pass 2: height-lerped weights, then the one remaining fetch (normal).
    ma = ma - TERRAIN_HEIGHT_BLEND_DEPTH;
    float3 albedo = float3(0.0);
    float3 tn = float3(0.0);
    float2 arm = float2(0.0);
    float wsum = 0.0;
    for (uint i = 0u; i < 8u; ++i)
    {
        if (w_in[i] <= 0.0) { continue; }
        let b = max(h_c[i] + w_in[i] - ma, 0.0);
        if (b <= 0.0) { continue; }
        albedo = albedo + b * albedo_c[i];
        arm = arm + b * arm_c[i];
        let n = sample_layer(normal_array, samp, uv, min(i, max_normal), dx, dy,
                             var_, blend_c[i]).rgb * 2.0 - 1.0;
        tn = tn + b * n;
        wsum = wsum + b;
    }

    let inv = 1.0 / max(wsum, 1e-4);
    BlendedSurface output;
    output.albedo = albedo * inv;
    output.ao = arm.x * inv;
    output.roughness = arm.y * inv;
    let tn_avg = tn * inv;
    if (dot(tn_avg, tn_avg) < 1e-8)
    {
        output.tangent_normal = float3(0.0, 0.0, 1.0);
    }
    else
    {
        output.tangent_normal = normalize(tn_avg);
    }
    return output;
}
