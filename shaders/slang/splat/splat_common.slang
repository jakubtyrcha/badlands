// Shared layout for the splat chain.
//
// EVERY MEMBER IS A float4 OR A float4x4, deliberately. MSL pads float3 to 16
// bytes, so a struct that looks 12-byte-packed in Slang arrives 16-byte-strided
// on the GPU -- a mismatch this project has already been bitten by once, and one
// that shows up as wrong geometry rather than as an error. Pairs of float4 keep
// host and device layouts identical by construction.
module splat_common;

public struct SplatParams {
    public float4x4 view_proj;
    // xyz centre, w radius. A radius <= 0 disables the sphere.
    public float4 sphere_a;
    public float4 sphere_b;
    // xyz grid origin (corner of cell 0), w cell size.
    public float4 grid;
    // x cells per axis, y splat capacity, z screen width, w screen height.
    public float4 config;
}

// One point on the surface: xyz world position, w the sphere it came from,
// which the resolve turns into a colour.
public struct Splat {
    public float4 pos_source;
}
