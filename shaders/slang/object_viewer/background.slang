// The environment behind the geometry, as its own depth-tested draw.
//
// THIS EXISTS FOR THE COST, not for the picture. The resolve used to branch on
// kVisEmpty having ALREADY been dispatched over every background pixel -- five
// texture fetches, an analytic gradient reconstruction and a full BRDF, thrown
// away. See "Screen-space work is a cost, not a free primitive" in
// src/engine/CLAUDE.md.
//
// The two draws are DISJOINT and cover the target exactly once between them,
// which the depth buffer decides rather than a branch:
//
//   resolve     z = 0, Less          runs where geometry was rasterized
//   background  z = 0, GreaterEqual  runs where it was not
//
// Reversed-Z clears depth to 0.0 (far), so an empty pixel fails `0 < 0` and a
// covered one fails `0 >= depth`. Getting the pair backwards renders one of
// them over the whole screen, which is visible immediately -- unlike the
// branchy version, whose waste was invisible.
module background;

import vis_common;
import output_transform;

ConstantBuffer<ResolveFrame> frame;
TextureCube<float4> prefiltered_env;
SamplerState ibl_sampler;

struct VOut {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
}

// One triangle, and at z = 0 so the depth test above means what it says.
[shader("vertex")]
VOut vs_background(uint vid : SV_VertexID)
{
    let uv = float2(float((vid << 1) & 2), float(vid & 2));
    VOut o;
    o.pos = float4(uv * 2.0 - 1.0, 0.0, 1.0);
    o.uv = float2(uv.x, 1.0 - uv.y);
    return o;
}

[shader("fragment")]
float4 fs_background(VOut i) : SV_Target
{
    // VIEW-DEPENDENT IN EXACTLY ONE DIRECTION. Under Lit this is the
    // environment; under every other view it is the flat constant it has always
    // been, because the nine debug-view oracles assert on that background and
    // sampling an environment there would silently change what nine existing
    // tests mean.
    if (uint(frame.params.x) == kViewLit && frame.ibl.z > 0.5) {
        // Mip 0: the SOURCE environment, not a roughness-convolved level. The
        // sky is what it is; only a surface reflecting it is blurred.
        let dir = normalize(frame.ray_forward.xyz +
                            frame.ray_right.xyz * (i.uv.x * 2.0 - 1.0) +
                            frame.ray_up.xyz * (1.0 - i.uv.y * 2.0));
        return float4(prefiltered_env.SampleLevel(ibl_sampler, dir, 0.0).rgb *
                          frame.ibl.y,
                      1.0);
    }
    return float4(SrgbToLinear(float3(0.05, 0.06, 0.09)), 1.0);
}
