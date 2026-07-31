#include "gizmo.h"

#include <cmath>

#include "picking.h"
#include "scene.h"

namespace sq {

void tangent_basis(simd_float3 n, simd_float3& u, simd_float3& v) {
    if (simd_length_squared(n) < 1e-8f) {
        n = simd_float3{0.0f, 1.0f, 0.0f};
    }
    const simd_float3 ref = (std::fabs(n.y) < 0.99f) ? simd_float3{0.0f, 1.0f, 0.0f}
                                                     : simd_float3{1.0f, 0.0f, 0.0f};
    u = simd_normalize(simd_cross(n, ref));
    v = simd_cross(n, u);
}

GizmoFrame gizmo_frame_for_node(const Node& node, const Camera& camera) {
    const simd_float3 forward = simd_normalize(camera.target - camera.eye);
    const DragPlane dp = drag_plane_for_node(node, forward);

    GizmoFrame f;
    f.origin = dp.point;
    f.n = dp.normal;
    tangent_basis(f.n, f.u, f.v);
    f.half_extent = kGizmoScreenFraction * simd_length(dp.point - camera.eye) *
                    2.0f * std::tan(camera.fov_y_radians * 0.5f);
    return f;
}

} // namespace sq
