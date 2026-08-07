// Spherical Harmonics (L2) ambient lighting.
//
// PORTED VERBATIM from shaders/common/sh_lighting.wesl.
//
// THE CONVENTION TRAVELS WITH THE EVALUATOR, and this is the trap. This
// function evaluates the RAW basis, because the engine bakes the diffuse
// convolution into the coefficients when it projects them
// (src/core/math/spherical_harmonics.hpp). The lab's EvaluateSH in
// shaders/slang/rhi_lab/lab_common.slang does the opposite: it applies the
// c1..c5 convolution constants inline and expects UNCONVOLVED coefficients.
//
// Mixing one's coefficients with the other's evaluator silently double-applies
// or drops the convolution. Both results look like plausible ambient light,
// which is why this is a comment and an oracle rather than a comment alone.
module sh_lighting;

// Evaluate SH L2 (L0 + L1 + L2) for diffuse irradiance.
//   sh[0]   L0 (constant)
//   sh[1-3] L1 (y, z, x)
//   sh[4-8] L2 (xy, yz, 3z^2-1, xz, x^2-y^2)
public float3 EvaluateAmbientSHL2(float3 normal, float4 sh[9])
{
    let x = normal.x;
    let y = normal.y;
    let z = normal.z;

    return sh[0].xyz
         + sh[1].xyz * y
         + sh[2].xyz * z
         + sh[3].xyz * x
         + sh[4].xyz * (x * y)
         + sh[5].xyz * (y * z)
         + sh[6].xyz * (3.0 * z * z - 1.0)
         + sh[7].xyz * (x * z)
         + sh[8].xyz * (x * x - y * y);
}
