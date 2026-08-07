// The primary viewport: a per-pixel sphere trace of the CSG scene.
//
// Ported from shaders/raymarch.metal. Everything that matters -- the SDF
// library, the CSG fold, ray generation -- comes from sdf_scene.h, the SAME
// header the CPU compiles for picking and DCSDD sampling. This file is the
// entry-point plumbing around it and nothing else, which is why porting it did
// not mean porting the maths.

#define SDF_SLANG 1
#include "shared_types.h"
#include "sdf_scene.h"

ConstantBuffer<RaymarchUniforms> uniforms;
StructuredBuffer<SdfNode> nodes;

// Fullscreen-triangle trick: three clip-space vertices that overshoot the
// viewport on two sides, so the single triangle covers the screen after
// rasterization -- no vertex buffer, no index buffer, cheaper than a quad.
// z plays no role (the fragment stage writes real depth) as long as it is
// inside the clip volume.
static const float2 kFullscreenTriangle[3] = {
    float2(-1.0, -1.0),
    float2(3.0, -1.0),
    float2(-1.0, 3.0),
};

// Sphere-trace hit epsilon: distance-scaled, with a fixed floor so it does not
// collapse to zero near the camera.
float hit_epsilon(float t) { return max(1e-4f, 5e-4f * t); }

// Step budget. Unrelated to the node buffer, which is sized to the scene's
// actual node count each frame -- there is no node cap.
static const int kMaxTraceSteps = 128;

struct RaymarchVarying {
    float4 position : SV_Position;
};

struct RaymarchOut {
    float4 color : SV_Target;
    float depth : SV_Depth;
};

[shader("vertex")]
RaymarchVarying vs_main(uint vid : SV_VertexID) {
    RaymarchVarying out;
    out.position = float4(kFullscreenTriangle[vid], 0.0, 1.0);
    return out;
}

[shader("fragment")]
RaymarchOut fs_main(float4 frag_coord : SV_Position) {
    const float viewport_w = uniforms.params0.x;
    const float viewport_h = uniforms.params0.y;
    const int node_count = int(uniforms.params0.z);
    const float near_plane = uniforms.params1.x;
    const float far_plane = uniforms.params1.y;

    const SdfRay ray = sdf_ray_for_pixel(frag_coord.x, frag_coord.y,
                                         viewport_w, viewport_h, uniforms.inv_view_proj);

    // Sphere trace from `near`. Hit when the SDF drops below the
    // distance-scaled epsilon; miss when t runs past `far` or the budget is
    // exhausted -- either way the fragment is discarded and the clear colour
    // shows through. Budget exhaustion as a miss is a known sphere-tracing
    // artifact at grazing silhouettes, accepted rather than worked around.
    float t = near_plane;
    float3 p = float3(0.0);   // always overwritten before use
    bool did_hit = false;
    for (int i = 0; i < kMaxTraceSteps; ++i) {
        p = ray.origin + t * ray.dir;
        const float d = sdf_fold(nodes, node_count, p);
        if (d < hit_epsilon(t)) {
            did_hit = true;
            break;
        }
        t += d;
        if (t > far_plane) {
            break;
        }
    }

    RaymarchOut out;
    if (!did_hit) {
        discard;
        // Unreachable, but every path must still yield a value.
        out.color = float4(0.0);
        out.depth = 0.0;
        return out;
    }

    // Normal via the tetrahedron-offset gradient of the same sdf_fold (iq's
    // technique): 4 fold evaluations instead of the 6 a naive central
    // difference needs. Offset scaled by the hit epsilon at this t, matching
    // the sphere-trace step that found the hit.
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

    const float4 clip = mul(uniforms.view_proj, float4(p, 1.0));

    out.color = float4(0.5 * (n + 1.0), 1.0);
    // Reversed-Z, so the near plane is 1.0: clip.z/clip.w can land fractionally
    // ABOVE 1.0 for an oblique hit found at t ~= near (the hit epsilon lets p
    // sit just short of the near plane). The hardware clamps depth writes to
    // [0,1], so this is silently harmless -- only reachable with the camera
    // nearly touching a surface.
    out.depth = clip.z / clip.w;
    return out;
}
