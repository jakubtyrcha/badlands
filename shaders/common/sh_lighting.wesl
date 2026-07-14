// Spherical Harmonics (L2) ambient lighting evaluation
// Matches the projection in src/core/math/spherical_harmonics.hpp
//
// Ported verbatim from sampo's src/shaders/common/sh_lighting.wesl.

// Evaluate SH L2 (L0 + L1 + L2) for diffuse irradiance
// Input: normal in world space, SH coefficients from frame uniforms
// Output: ambient irradiance color
//
// The coefficients already include diffuse convolution factors (baked in during projection),
// so we just evaluate the basis functions directly:
//   L0: Y_00 = 1 (constant)
//   L1: Y_1-1 = y, Y_10 = z, Y_11 = x
//   L2: Y_2-2 = x*y, Y_2-1 = y*z, Y_20 = 3*z*z-1, Y_21 = x*z, Y_22 = x*x-y*y
fn evaluateAmbientSHL2(normal: vec3<f32>, sh: array<vec4<f32>, 9>) -> vec3<f32> {
    let x = normal.x;
    let y = normal.y;
    let z = normal.z;

    // sh[0] = L0 (constant term)
    // sh[1-3] = L1 (y, z, x)
    // sh[4-8] = L2 (xy, yz, 3z^2-1, xz, x^2-y^2)
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

// Legacy L1 evaluation for compatibility (uses first 4 coefficients)
fn evaluateAmbientSHL1(normal: vec3<f32>, sh: array<vec4<f32>, 9>) -> vec3<f32> {
    return sh[0].xyz
         + sh[1].xyz * normal.y
         + sh[2].xyz * normal.z
         + sh[3].xyz * normal.x;
}
