// The GGX prefilter: one roughness per mip, one draw per (face, mip).
//
// PORTED from shaders/ibl/prefilter_render.wesl. Render path rather than
// compute for the same reason it is there: TextureUsage::Storage is not in the
// RHI's MVP, and RGBA16Float is renderable regardless.
//
// EVERY MIP IS RE-CONVOLVED FROM THE SOURCE, never downsampled from the mip
// above. Per-face downsampling is what actually produces cross-face seams --
// the hardware filters a cube seamlessly on its own, so a seam in the output
// means something built the chain the wrong way. A headless assertion checks
// exactly that.
module prefilter;

import ibl_sampling;

static const uint kMaxSamples = 1024u;

struct PrefilterParams {
    // x = roughness for this mip, y = face index, z = output size in texels.
    // A float4 rather than three scalars because a constant buffer of one
    // scalar is a 4-byte object every backend pads differently.
    float4 params;
}

ConstantBuffer<PrefilterParams> frame;
TextureCube<float4> source;
SamplerState source_sampler;

struct VOut {
    float4 pos : SV_Position;
}

[shader("vertex")]
VOut vs_prefilter(uint vid : SV_VertexID)
{
    let uv = float2(float((vid << 1) & 2), float(vid & 2));
    VOut o;
    o.pos = float4(uv * 2.0 - 1.0, 0.0, 1.0);
    return o;
}

float3 PrefilterEnvMap(float3 N, float roughness)
{
    // Mirror: the convolution is the identity, so skip 1024 samples that would
    // reproduce the source texel with quadrature noise on top.
    if (roughness < 0.001) {
        return source.SampleLevel(source_sampler, N, 0.0).rgb;
    }

    // V = N, the standard isotropic assumption: the prefiltered chain cannot
    // carry a view direction, which is precisely the error the BRDF LUT's other
    // half of the split sum compensates for.
    let V = N;

    float3 prefiltered = float3(0.0);
    float totalWeight = 0.0;

    // Fewer samples where the lobe is tight and the integrand nearly constant.
    let sampleCount = roughness > 0.5 ? kMaxSamples : kMaxSamples / 4u;

    for (uint i = 0u; i < sampleCount; ++i) {
        let Xi = Hammersley(i, sampleCount);
        let H = ImportanceSampleGGX(Xi, N, roughness);
        let L = normalize(2.0 * dot(V, H) * H - V);

        let NdotL = max(dot(N, L), 0.0);
        if (NdotL > 0.0) {
            prefiltered += source.SampleLevel(source_sampler, L, 0.0).rgb * NdotL;
            totalWeight += NdotL;
        }
    }

    return totalWeight > 0.0 ? prefiltered / totalWeight : float3(0.0);
}

[shader("fragment")]
float4 fs_prefilter(VOut i) : SV_Target
{
    // SV_Position is the pixel CENTRE, and the target IS this (face, mip), so
    // dividing by its size gives the texel-centre UV directly.
    let uv = i.pos.xy / frame.params.z;
    let N = FaceUVToDirection(uint(frame.params.y), uv);
    return float4(PrefilterEnvMap(N, frame.params.x), 1.0);
}
