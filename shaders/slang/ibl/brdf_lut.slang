// The split-sum BRDF lookup table.
//
// PORTED from shaders/ibl/brdf_lut_render.wesl, which is itself sampo's. The
// integral is untouched; only the language changed. Output is RG16Float:
//   R = scale (multiply with F0), G = bias (add).
//   final Fresnel = F0 * R + G
//
// NO BINDINGS AT ALL. Every input is derived from the fragment's UV, which is
// why this is generated once at startup and never again.
module brdf_lut;

import ibl_sampling;

static const uint kSampleCount = 1024u;

struct VOut {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
}

// One triangle, not two: a quad re-shades every 2x2 rasterizer quad along the
// diagonal. See "Screen-space work is a cost" in src/engine/CLAUDE.md.
[shader("vertex")]
VOut vs_brdf_lut(uint vid : SV_VertexID)
{
    let uv = float2(float((vid << 1) & 2), float(vid & 2));
    VOut o;
    o.pos = float4(uv * 2.0 - 1.0, 0.0, 1.0);
    o.uv = uv;
    return o;
}

// Smith geometry, IBL parameterization (k = a^2/2, not the direct-light form).
float GeometrySchlickGGX(float NdotV, float roughness)
{
    let a = roughness;
    let k = (a * a) / 2.0;
    return NdotV / (NdotV * (1.0 - k) + k);
}

float GeometrySmith(float3 N, float3 V, float3 L, float roughness)
{
    let NdotV = max(dot(N, V), 0.0);
    let NdotL = max(dot(N, L), 0.0);
    return GeometrySchlickGGX(NdotV, roughness) * GeometrySchlickGGX(NdotL, roughness);
}

float2 IntegrateBrdf(float NdotV, float roughness)
{
    // The normal is +Z, so the view sits in the XZ plane at the given NdotV.
    let V = float3(sqrt(1.0 - NdotV * NdotV), 0.0, NdotV);
    let N = float3(0.0, 0.0, 1.0);

    float A = 0.0;
    float B = 0.0;
    for (uint i = 0u; i < kSampleCount; ++i) {
        let Xi = Hammersley(i, kSampleCount);
        let H = ImportanceSampleGGX(Xi, N, roughness);
        let L = normalize(2.0 * dot(V, H) * H - V);

        let NdotL = max(L.z, 0.0);
        let NdotH = max(H.z, 0.0);
        let VdotH = max(dot(V, H), 0.0);

        if (NdotL > 0.0) {
            let G = GeometrySmith(N, V, L, roughness);
            let G_Vis = (G * VdotH) / (NdotH * NdotV);
            let Fc = pow(1.0 - VdotH, 5.0);
            A += (1.0 - Fc) * G_Vis;
            B += Fc * G_Vis;
        }
    }
    return float2(A, B) / float(kSampleCount);
}

[shader("fragment")]
float2 fs_brdf_lut(VOut i) : SV_Target
{
    // X = NdotV, Y = roughness. Y is flipped because the target's texel (0,0)
    // is top-left while the triangle's uv starts at the bottom-left corner of
    // clip space -- the same flip the WESL original carries.
    let NdotV = max(i.uv.x, 0.001);
    let roughness = max(1.0 - i.uv.y, 0.001);
    return IntegrateBrdf(NdotV, roughness);
}
