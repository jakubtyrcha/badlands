#include <metal_stdlib>
#include "shared_types.h"
#include "sdf_scene.h" // SdfNode, sdf_fold, sdf_ray_for_pixel -- the dual-compile SDF core (R0)

using namespace metal;

// Fullscreen-triangle trick: three clip-space vertices that overshoot the
// viewport on two sides, so the single triangle they form covers the whole
// screen after rasterization -- no vertex buffer, no index buffer, cheaper
// than a two-triangle quad. z=0 for every vertex; it plays no role (the
// fragment stage writes the real depth via [[depth(any)]]) as long as it's
// inside the clip volume.
constant float2 kFullscreenTrianglePositions[3] = {
    float2(-1.0, -1.0),
    float2(3.0, -1.0),
    float2(-1.0, 3.0),
};

struct RaymarchVarying {
    float4 position [[position]];
};

vertex RaymarchVarying raymarch_vertex(uint vid [[vertex_id]]) {
    RaymarchVarying out;
    out.position = float4(kFullscreenTrianglePositions[vid], 0.0, 1.0);
    return out;
}

struct RaymarchOut {
    float4 color [[color(0)]];
    float depth [[depth(any)]];
};

namespace {

// Sphere-trace hit epsilon: distance-scaled per the brief, with a fixed
// floor so it doesn't collapse to zero near the camera.
inline float hit_epsilon(float t) {
    return max(1e-4f, 5e-4f * t);
}

} // namespace

fragment RaymarchOut raymarch_fragment(
    float4 frag_coord [[position]],
    constant RaymarchUniforms& uniforms [[buffer(0)]],
    constant SdfNode* nodes [[buffer(1)]])
{
    const float viewport_w = uniforms.params0.x;
    const float viewport_h = uniforms.params0.y;
    const int node_count = int(uniforms.params0.z);
    const float near = uniforms.params1.x;
    const float far = uniforms.params1.y;

    const SdfRay ray = sdf_ray_for_pixel(frag_coord.x, frag_coord.y, viewport_w, viewport_h, uniforms.inv_view_proj);

    // Sphere trace: up to 128 steps, starting at `near`. Hit when the SDF
    // value drops below the distance-scaled epsilon; miss when `t` runs past
    // `far` or the step budget is exhausted -- either way `did_hit` stays
    // false and the fragment is discarded (clear color shows through).
    float t = near;
    float3 p = float3(0.0); // always overwritten before use -- see the loop's first statement
    bool did_hit = false;
    for (int i = 0; i < 128; ++i) {
        p = ray.origin + t * ray.dir;
        const float d = sdf_fold(nodes, node_count, p);
        if (d < hit_epsilon(t)) {
            did_hit = true;
            break;
        }
        t += d;
        if (t > far) {
            break;
        }
    }

    if (!did_hit) {
        discard_fragment();
        // Unreachable at runtime (the fragment is discarded above), but MSL
        // still requires every path to return a value.
        RaymarchOut discarded;
        discarded.color = float4(0.0);
        discarded.depth = 0.0;
        return discarded;
    }

    // Normal via the tetrahedron-offset gradient of the same sdf_fold (iq's
    // technique): 4 fold evaluations instead of 6 for a naive central-
    // difference gradient. Offset scaled by the hit epsilon at this t, same
    // as the sphere-trace step that found the hit.
    const float h = hit_epsilon(t);
    const float3 e1 = float3(1.0, -1.0, -1.0);
    const float3 e2 = float3(-1.0, -1.0, 1.0);
    const float3 e3 = float3(-1.0, 1.0, -1.0);
    const float3 e4 = float3(1.0, 1.0, 1.0);
    const float3 n = normalize(
        e1 * sdf_fold(nodes, node_count, p + e1 * h) +
        e2 * sdf_fold(nodes, node_count, p + e2 * h) +
        e3 * sdf_fold(nodes, node_count, p + e3 * h) +
        e4 * sdf_fold(nodes, node_count, p + e4 * h));

    const float4 clip = uniforms.view_proj * float4(p, 1.0);

    RaymarchOut out;
    out.color = float4(0.5 * (n + 1.0), 1.0);
    out.depth = clip.z / clip.w;
    return out;
}
