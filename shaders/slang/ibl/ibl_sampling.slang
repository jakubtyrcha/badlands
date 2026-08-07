// GGX importance sampling and the cube face convention, shared by the
// prefilter and the BRDF LUT.
//
// SHARED RATHER THAN COPIED, and that matters more here than usual: the
// prefilter bakes a roughness into each mip and the LUT integrates the same
// distribution for the other half of the split sum. Two copies that drift give
// a result that is smooth, plausible, and wrong by a factor nothing reports.
module ibl_sampling;

public static const float kPi = 3.14159265359;

// Van der Corput radical inverse, base 2.
public float RadicalInverseVdC(uint bits_in)
{
    uint bits = bits_in;
    bits = (bits << 16) | (bits >> 16);
    bits = ((bits & 0x55555555u) << 1) | ((bits & 0xAAAAAAAAu) >> 1);
    bits = ((bits & 0x33333333u) << 2) | ((bits & 0xCCCCCCCCu) >> 2);
    bits = ((bits & 0x0F0F0F0Fu) << 4) | ((bits & 0xF0F0F0F0u) >> 4);
    bits = ((bits & 0x00FF00FFu) << 8) | ((bits & 0xFF00FF00u) >> 8);
    return float(bits) * 2.3283064365386963e-10;
}

public float2 Hammersley(uint i, uint n)
{
    return float2(float(i) / float(n), RadicalInverseVdC(i));
}

// GGX half-vector from a Hammersley point.
//
// ALPHA IS roughness^2 HERE, INSIDE the distribution -- that is where the
// squaring belongs. The mip parameterization outside stays linear, and the
// prefilter and the resolve must agree on it; see PrefilteredCube.
public float3 ImportanceSampleGGX(float2 Xi, float3 N, float roughness)
{
    let a = roughness * roughness;

    let phi = 2.0 * kPi * Xi.x;
    let cosTheta = sqrt((1.0 - Xi.y) / (1.0 + (a * a - 1.0) * Xi.y));
    let sinTheta = sqrt(1.0 - cosTheta * cosTheta);

    let H = float3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);

    let up = abs(N.z) < 0.999 ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0);
    let tangent = normalize(cross(up, N));
    let bitangent = cross(N, tangent);
    return normalize(tangent * H.x + bitangent * H.y + N * H.z);
}

// Face index + UV ([0,1], v = 0 at the TOP) -> unit direction.
//
// MUST match badlands::ibl::FaceUVToDirection and
// CubemapBuilder::FaceUVToDirection exactly. A disagreement rotates the
// prefiltered cube relative to the source it was convolved from, which reads
// as a lighting choice rather than as a bug.
public float3 FaceUVToDirection(uint face, float2 uv)
{
    let s = uv.x * 2.0 - 1.0;
    let t = uv.y * 2.0 - 1.0;
    switch (face) {
        case 0u: return normalize(float3( 1.0, -t, -s));  // +X
        case 1u: return normalize(float3(-1.0, -t,  s));  // -X
        case 2u: return normalize(float3(  s, 1.0,  t));  // +Y
        case 3u: return normalize(float3(  s, -1.0, -t)); // -Y
        case 4u: return normalize(float3(  s, -t,  1.0)); // +Z
        default: return normalize(float3( -s, -t, -1.0)); // -Z
    }
}
