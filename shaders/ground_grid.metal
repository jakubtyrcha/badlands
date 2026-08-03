#include <metal_stdlib>
#include "shared_types.h"
#include "sdf_scene.h"   // SdfRay, sdf_ray_for_pixel -- shared with the raymarch pass
#include "ground_grid.h" // ground_grid_shade + the palette/tier constants

using namespace metal;

// Ground plate on y=0: the viewport's world-space orientation layer. This
// file is deliberately plumbing only -- ray reconstruction, plane
// intersection, depth output -- with every colour decision delegated to
// ground_grid.h so it can be unit-tested without a Metal device. See
// docs/superpowers/specs/2026-08-03-viewport-orientation-aids-design.md.

// Same fullscreen-triangle trick as raymarch_vertex (see raymarch.metal for
// the full reasoning): three clip-space vertices overshooting the viewport,
// no vertex or index buffer.
constant float2 kGroundFullscreenPositions[3] = {
    float2(-1.0, -1.0),
    float2(3.0, -1.0),
    float2(-1.0, 3.0),
};

struct GroundVarying {
    float4 position [[position]];
};

vertex GroundVarying ground_grid_vertex(uint vid [[vertex_id]]) {
    GroundVarying out;
    out.position = float4(kGroundFullscreenPositions[vid], 0.0, 1.0);
    return out;
}

struct GroundOut {
    float4 color [[color(0)]];
    float depth [[depth(any)]];
};

fragment GroundOut ground_grid_fragment(GroundVarying in [[stage_in]],
                                         constant GroundGridUniforms& uniforms [[buffer(0)]]) {
    const float viewport_w = uniforms.params0.x;
    const float viewport_h = uniforms.params0.y;
    const float half_extent = uniforms.params0.z;
    const float minor_spacing = uniforms.params1.x;
    const float major_spacing = uniforms.params1.y;

    const SdfRay ray = sdf_ray_for_pixel(in.position.x, in.position.y,
                                          viewport_w, viewport_h, uniforms.inv_view_proj);

    // --- y=0 intersection, NaN/infinity-proof by construction -------------
    //
    // Rays are per-pixel, so whenever the horizon is on screen there is a row
    // of fragments with dir.y crossing zero -- every frame, not some rare
    // camera pose. A plain -origin.y/dir.y there yields +/-inf (or NaN when
    // the eye itself sits at y=0), and a `t <= 0` test does NOT catch NaN
    // (IEEE comparisons against NaN are false), so the poisoned point would
    // reach fwidth() below and corrupt the derivatives of its whole 2x2 quad
    // rather than just its own pixel.
    //
    // Dividing by a sign-preserving clamped denominator and capping the
    // result keeps `t` finite no matter what, and the rejection rides along
    // as a flag instead of a branch (see the control-flow note below).
    const float kDenEps = 1e-6f;
    const bool degenerate = fabs(ray.dir.y) < kDenEps;
    const float den = degenerate ? copysign(kDenEps, ray.dir.y) : ray.dir.y;

    const float t_raw = -ray.origin.y / den;
    const bool reject_t = !(t_raw > 0.0f); // NaN-safe by negation: plane behind, or degenerate
    const float t = clamp(t_raw, 0.0f, kGroundMaxRayT);

    const float3 p = ray.origin + ray.dir * t;
    const float2 q = float2(p.x, p.z);

    // --- derivatives, UNCONDITIONALLY ------------------------------------
    //
    // fwidth is a 2x2-quad operation: it reads the neighbouring lanes' `q`.
    // Any lane that exits before this point leaves its neighbours with
    // garbage derivatives, which shows up as sparkle along the plate edge and
    // at the horizon. So nothing above may branch out early, and every
    // rejection below goes through discard_fragment() rather than `return`.
    const float2 dq = fwidth(q);

    float4 rgba = ground_grid_shade(q, dq, half_extent, minor_spacing, major_spacing);
    rgba *= gg_grazing_fade(ray.dir.y); // premultiplied, so scaling all four channels is correct

    // Depth against the scene the raymarch/mesh passes wrote, so anything
    // resting on the floor occludes the plate behind it. A plate corner can
    // sit past the 100 far plane at the camera's 90 radius clamp, hence the
    // explicit range rejection rather than trusting clamp behaviour.
    const float4 clip = uniforms.view_proj * float4(p, 1.0);
    const float depth = clip.z / clip.w;
    const bool reject_depth = !(clip.w > 0.0) || !(depth >= 0.0) || !(depth <= 1.0);

    GroundOut out;
    out.color = rgba;
    out.depth = reject_depth ? 1.0 : depth;

    if (degenerate || reject_t || reject_depth || !(rgba.w > 0.0)) {
        discard_fragment();
    }
    return out;
}
