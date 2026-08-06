// Screen-space material resolve + lighting.
//
// Everything material-related happens here, which is why the raster pass could
// bind nothing. The resource set is fixed and small -- three texture arrays,
// two splat textures, two samplers -- so it is a perfectly ordinary graph node
// despite driving every material in the scene.
//
// Terrain blending needs NO bindless because of a decision already baked into
// the engine: biome weights come from a splat texture sampled in world XZ, not
// from vertex data (chosen so boundaries survive LOD decimation). That means
// world position is the only per-pixel input, and world position reconstructs
// from depth -- so the resolve needs no barycentric attribute fetch either.

import lab_common;

ConstantBuffer<LabFrame> frame;

Texture2D<uint>   visbuffer;
Texture2D<float>  depthbuffer;
Texture2DArray<float4> albedo_array;
Texture2DArray<float4> arm_array;     // R=AO G=roughness B=metal A=displacement
Texture2D<float4> biome_splat0;       // weights, layers 0..3
Texture2D<float4> biome_splat1;       // weights, layers 4..7
SamplerState terrain_sampler;
SamplerState splat_sampler;

struct VOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };

[shader("vertex")]
VOut vs_fullscreen(uint vid : SV_VertexID)
{
    let uv = float2(float((vid << 1) & 2), float(vid & 2));
    VOut o;
    o.pos = float4(uv * 2.0 - 1.0, 0.0, 1.0);
    o.uv = float2(uv.x, 1.0 - uv.y);
    return o;
}

static const float kTerrainTileMetres = 2.5;
static const float kMinLayerWeight = 0.00392;  // 1/255

struct Surface { float3 albedo; float ao; float roughness; };

// Height-lerped blend over eight layers, weights from the splat. Zero-weight
// layers are skipped: a real fragment blends one or two, not eight.
Surface BlendTerrain(float2 world_xz, float4 w0, float4 w1)
{
    let uv = world_xz / kTerrainTileMetres;

    float heights[8];
    float weights[8];
    float tallest = -1e9;
    for (uint i = 0u; i < 8u; ++i)
    {
        let w = (i < 4u) ? w0[i] : w1[i - 4u];
        weights[i] = w;
        if (w <= kMinLayerWeight) { heights[i] = 0.0; continue; }
        heights[i] = arm_array.Sample(terrain_sampler, float3(uv, float(i))).a;
        tallest = max(tallest, heights[i] + w);
    }

    // Only layers within the transition band of the tallest contribute, which
    // makes the seam interlock rather than cross-fade.
    tallest -= 0.2;
    float3 albedo = float3(0.0);
    float2 arm = float2(0.0);
    float total = 0.0;
    for (uint i = 0u; i < 8u; ++i)
    {
        if (weights[i] <= kMinLayerWeight) { continue; }
        let b = max(heights[i] + weights[i] - tallest, 0.0);
        if (b <= 0.0) { continue; }
        albedo += b * albedo_array.Sample(terrain_sampler, float3(uv, float(i))).rgb;
        let a = arm_array.Sample(terrain_sampler, float3(uv, float(i)));
        arm += b * float2(a.r, a.g);
        total += b;
    }

    let inv = 1.0 / max(total, 1e-4);
    Surface s;
    s.albedo = albedo * inv;
    s.ao = arm.x * inv;
    s.roughness = arm.y * inv;
    return s;
}

// Reversed-Z: depth 1 is near, 0 is far.
float3 WorldFromDepth(float2 uv, float depth)
{
    let ndc = float4(uv.x * 2.0 - 1.0, (1.0 - uv.y) * 2.0 - 1.0, depth, 1.0);
    let world = mul(frame.invViewProj, ndc);
    return world.xyz / world.w;
}

[shader("fragment")]
float4 fs_resolve(VOut i) : SV_Target
{
    uint w, h;
    visbuffer.GetDimensions(w, h);
    let px = int2(int(i.uv.x * float(w)), int(i.uv.y * float(h)));

    let packed = visbuffer.Load(int3(px, 0));
    if (packed == kVisEmpty)
    {
        return float4(0.05, 0.07, 0.10, 1.0);  // sky
    }

    let depth = depthbuffer.Load(int3(px, 0));
    let world = WorldFromDepth(i.uv, depth);

    // Geometric normal from depth derivatives. The vertex normal is not
    // available without a barycentric fetch, and for a material-and-blend view
    // this is adequate -- noted as a known MVP limitation.
    let n = normalize(cross(ddx(world), ddy(world)));

    let slot = VisSlot(packed);
    float3 albedo;
    float ao = 1.0;
    float roughness = 0.8;

    if (slot >= 0x40000u)
    {
        // Trees: one flat material, no splat lookup.
        albedo = float3(0.18, 0.32, 0.12);
        roughness = 0.9;
    }
    else
    {
        let suv = world.xz * frame.splatUv.xy + frame.splatUv.zw;
        let w0 = biome_splat0.Sample(splat_sampler, suv);
        let w1 = biome_splat1.Sample(splat_sampler, suv);
        let s = BlendTerrain(world.xz, w0, w1);
        albedo = s.albedo;
        ao = s.ao;
        roughness = s.roughness;
    }

    // Sun N.L plus SH ambient. No shadows, no GTAO, no IBL -- the MVP proves
    // the pass shape, not the V0 lighting stack.
    let ndotl = max(dot(n, normalize(frame.sunDirection.xyz)), 0.0);
    let direct = frame.sunColor.rgb * ndotl;
    let ambient = EvaluateSH(frame.ambientSH, n) * ao;
    let lit = albedo * (direct + ambient);

    // Reinhard, then sRGB encode. Enough to look at; not the engine's grade.
    let mapped = lit / (lit + 1.0);
    return float4(pow(mapped, 1.0 / 2.2), 1.0);
}

// Debug view: the visibility buffer itself, hashed to colour. Proves the
// raster before the resolve is trusted.
[shader("fragment")]
float4 fs_visbuffer_debug(VOut i) : SV_Target
{
    uint w, h;
    visbuffer.GetDimensions(w, h);
    let px = int2(int(i.uv.x * float(w)), int(i.uv.y * float(h)));
    let packed = visbuffer.Load(int3(px, 0));
    if (packed == kVisEmpty) { return float4(0.0, 0.0, 0.0, 1.0); }
    let slot = float(VisSlot(packed));
    return float4(0.5 + 0.5 * cos(6.2831853 * (slot * 0.113 + float3(0.0, 0.33, 0.67))), 1.0);
}
