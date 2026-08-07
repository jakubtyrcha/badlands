// Ground plate on y=0: the viewport's world-space orientation layer.
//
// Ported from shaders/ground_grid.metal, and deliberately plumbing only --
// ray reconstruction, plane intersection, depth output. Every colour decision
// stays in ground_grid.h (shared with the CPU, so ground_grid_tests can check
// it without a Metal device) and the ray generation in sdf_scene.h, shared with
// the raymarch pass.

#define SDF_SLANG 1
#include "shared_types.h"
#include "sdf_scene.h"
#include "ground_grid.h"

ConstantBuffer<GroundGridUniforms> uniforms;

// Same fullscreen-triangle trick as raymarch.slang.
static const float2 kGroundFullscreen[3] = {
    float2(-1.0, -1.0),
    float2(3.0, -1.0),
    float2(-1.0, 3.0),
};

struct GroundVarying {
    float4 position : SV_Position;
};

struct GroundOut {
    float4 color : SV_Target;
    float depth : SV_Depth;
};

[shader("vertex")]
GroundVarying vs_main(uint vid : SV_VertexID) {
    GroundVarying out;
    out.position = float4(kGroundFullscreen[vid], 0.0, 1.0);
    return out;
}

[shader("fragment")]
GroundOut fs_main(GroundVarying in) {
    const float viewport_w = uniforms.params0.x;
    const float viewport_h = uniforms.params0.y;
    const float half_extent = uniforms.params0.z;
    const float minor_spacing = uniforms.params1.x;
    const float major_spacing = uniforms.params1.y;

    const SdfRay ray = sdf_ray_for_pixel(in.position.x, in.position.y,
                                         viewport_w, viewport_h, uniforms.inv_view_proj);

    // --- y=0 intersection, NaN/infinity-proof by construction --------------
    //
    // Rays are per-pixel, so whenever the horizon is on screen there is a row
    // of fragments with dir.y crossing zero -- every frame, not some rare
    // camera pose. A plain -origin.y/dir.y yields +/-inf there (or NaN when the
    // eye sits at y=0), and a `t <= 0` test does NOT catch NaN, so the poisoned
    // point would reach fwidth() below and corrupt its whole 2x2 quad's
    // derivatives rather than just its own pixel.
    //
    // Dividing by a sign-preserving clamped denominator and capping the result
    // keeps t finite regardless, with the rejection riding along as a flag
    // rather than a branch.
    const float kDenEps = 1e-6f;
    const bool degenerate = abs(ray.dir.y) < kDenEps;
    // Slang has no copysign; the sign() form is the same thing, and sign(0)
    // cannot occur here because that path is the `degenerate` one either way.
    const float den = degenerate ? (ray.dir.y < 0.0f ? -kDenEps : kDenEps) : ray.dir.y;

    const float t_raw = -ray.origin.y / den;
    const bool reject_t = !(t_raw > 0.0f);   // NaN-safe by negation
    const float t = clamp(t_raw, 0.0f, kGroundMaxRayT);

    const float3 p = ray.origin + ray.dir * t;
    const float2 q = float2(p.x, p.z);

    // --- derivatives, UNCONDITIONALLY -------------------------------------
    //
    // fwidth is a 2x2-quad operation: it reads neighbouring lanes' q. Any lane
    // exiting before this point leaves its neighbours with garbage derivatives,
    // which shows up as sparkle along the plate edge and at the horizon. So
    // nothing above may branch out early, and every rejection below goes
    // through discard rather than an early return.
    const float2 dq = fwidth(q);

    float4 rgba = ground_grid_shade(q, dq, half_extent, minor_spacing, major_spacing);
    rgba *= gg_grazing_fade(ray.dir.y);   // premultiplied: scaling all four channels is correct

    // Depth against what the raymarch/mesh passes wrote, so anything resting on
    // the floor occludes the plate behind it. A plate corner can sit past the
    // far plane at the camera's radius clamp, hence the explicit range
    // rejection rather than trusting clamp behaviour.
    const float4 clip = mul(uniforms.view_proj, float4(p, 1.0));
    const float depth = clip.z / clip.w;
    const bool reject_depth = !(clip.w > 0.0) || !(depth >= 0.0) || !(depth <= 1.0);

    GroundOut out;
    out.color = rgba;
    // Reversed-Z: the far value is 0, not 1. Moot in practice -- a rejected
    // fragment is discarded below and never writes depth -- but a value that
    // says "far" while meaning "nearest possible" is a trap for the next reader.
    out.depth = reject_depth ? 0.0 : depth;

    if (degenerate || reject_t || reject_depth || !(rgba.w > 0.0)) {
        discard;
    }
    return out;
}
