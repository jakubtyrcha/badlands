#include "lines.h"

#include <array>
#include <cmath>

#include "gizmo.h"
#include "scene.h"

namespace sq {

namespace {

LineVertex make_vertex(simd_float3 local, const simd_float4x4& world_from_local, simd_float4 color) {
    const simd_float4 world = simd_mul(world_from_local, (simd_float4){local.x, local.y, local.z, 1.0f});
    LineVertex v;
    v.pos = (simd_float4){world.x, world.y, world.z, 1.0f};
    v.color = color;
    return v;
}


} // namespace

void append_cube_edges(std::vector<LineVertex>& out, const simd_float4x4& world_from_local, simd_float4 color) {
    static constexpr std::array<simd_float3, 8> kCorners = {{
        {-0.5f, -0.5f, -0.5f}, // 0
        { 0.5f, -0.5f, -0.5f}, // 1
        { 0.5f,  0.5f, -0.5f}, // 2
        {-0.5f,  0.5f, -0.5f}, // 3
        {-0.5f, -0.5f,  0.5f}, // 4
        { 0.5f, -0.5f,  0.5f}, // 5
        { 0.5f,  0.5f,  0.5f}, // 6
        {-0.5f,  0.5f,  0.5f}, // 7
    }};
    static constexpr std::array<std::array<int, 2>, 12> kEdges = {{
        {0, 1}, {1, 5}, {5, 4}, {4, 0}, // bottom
        {3, 2}, {2, 6}, {6, 7}, {7, 3}, // top
        {0, 3}, {1, 2}, {5, 6}, {4, 7}, // verticals
    }};

    for (const auto& edge : kEdges) {
        out.push_back(make_vertex(kCorners[edge[0]], world_from_local, color));
        out.push_back(make_vertex(kCorners[edge[1]], world_from_local, color));
    }
}

void append_sphere_outline(std::vector<LineVertex>& out, const simd_float4x4& world_from_local,
                           simd_float4 color, simd_float3 eye_world) {
    constexpr float r = 0.5f;
    const simd_float4x4 local_from_world = simd_inverse(world_from_local);
    const simd_float4 eye_local =
        simd_mul(local_from_world, (simd_float4){eye_world.x, eye_world.y, eye_world.z, 1.0f});
    const simd_float3 o = eye_local.xyz;
    const float d2 = simd_length_squared(o);
    if (d2 <= r * r + 1e-8f) {
        return; // eye inside or on the sphere: no silhouette
    }
    const float d = std::sqrt(d2);

    // Horizon circle of the tangency points (see lines.h): center (r^2/d^2)*o,
    // radius r*sqrt(d^2 - r^2)/d, in the plane perpendicular to o.
    const simd_float3 n = o / d;
    const simd_float3 center = (r * r / d2) * o;
    const float radius = r * std::sqrt(d2 - r * r) / d;

    const simd_float3 ref = (std::fabs(n.y) < 0.99f) ? simd_float3{0.0f, 1.0f, 0.0f}
                                                     : simd_float3{1.0f, 0.0f, 0.0f};
    const simd_float3 u = simd_normalize(simd_cross(n, ref));
    const simd_float3 v = simd_cross(n, u);

    auto point = [&](int i) -> simd_float3 {
        // i % segments makes the final segment land exactly on vertex 0.
        const float t = static_cast<float>(i % kSphereOutlineSegments) /
                        static_cast<float>(kSphereOutlineSegments) * 2.0f * float(M_PI);
        return center + radius * (std::cos(t) * u + std::sin(t) * v);
    };
    for (int i = 0; i < kSphereOutlineSegments; ++i) {
        out.push_back(make_vertex(point(i), world_from_local, color));
        out.push_back(make_vertex(point(i + 1), world_from_local, color));
    }
}

void append_move_gizmo_grid(std::vector<LineVertex>& out, const GizmoFrame& frame, int divisions) {
    const simd_float3 origin = frame.origin;
    const float he = frame.half_extent;
    const float step = 2.0f * he / static_cast<float>(divisions);
    const int center = divisions / 2;

    auto push = [&](simd_float3 p) {
        LineVertex vertex;
        vertex.pos = (simd_float4){p.x, p.y, p.z, 1.0f};
        vertex.color = kColorGridLine;
        out.push_back(vertex);
    };

    // For each sample i in [0, divisions] (skipping the center, where the
    // u/v axis handles run), emit one line running along u (offset along v)
    // and one running along v (offset along u).
    for (int i = 0; i <= divisions; ++i) {
        if (i == center) {
            continue;
        }
        const float offset = -he + static_cast<float>(i) * step;
        push(origin + offset * frame.v - he * frame.u);
        push(origin + offset * frame.v + he * frame.u);
        push(origin + offset * frame.u - he * frame.v);
        push(origin + offset * frame.u + he * frame.v);
    }
}

namespace {

// Expands the segment [a, b] into a camera-facing quad (two triangles, 6
// verts) of the given half-width, endpoints extended by the half-width so
// segments meeting at a corner overlap instead of notching. A segment
// pointing straight at the eye has no on-screen extent: emit nothing.
void append_thick_segment(std::vector<LineVertex>& out, simd_float3 a, simd_float3 b,
                          simd_float3 eye, float half_width, simd_float4 color) {
    const simd_float3 ab = b - a;
    const float len = simd_length(ab);
    if (len < 1e-6f) {
        return;
    }
    const simd_float3 dir = ab / len;
    simd_float3 side = simd_cross(dir, eye - 0.5f * (a + b));
    const float side_len = simd_length(side);
    if (side_len < 1e-6f) {
        return; // edge-on to the eye
    }
    side *= half_width / side_len;

    const simd_float3 a2 = a - half_width * dir;
    const simd_float3 b2 = b + half_width * dir;

    auto push = [&](simd_float3 p) {
        LineVertex vertex;
        vertex.pos = (simd_float4){p.x, p.y, p.z, 1.0f};
        vertex.color = color;
        out.push_back(vertex);
    };
    push(a2 - side); push(b2 - side); push(b2 + side);
    push(a2 - side); push(b2 + side); push(a2 + side);
}

} // namespace

void append_move_gizmo_handles(std::vector<LineVertex>& out, const GizmoFrame& frame,
                               GizmoHandle highlighted, simd_float3 eye) {
    const simd_float3 origin = frame.origin;
    const float he = frame.half_extent;
    const float hw = kGizmoHandleHalfWidthFrac * he;

    auto color_for = [&](GizmoHandle handle, simd_float4 base) {
        return handle == highlighted ? kColorGizmoHot : base;
    };

    // Axis handles from the origin, POSITIVE half only (0..+he, R3 user
    // ruling) — pick_gizmo_handle clamps to the same segment. Emission order
    // (u, v, n) matches the pick tie-break order; lines_tests pins the layout.
    const struct { simd_float3 dir; simd_float4 color; GizmoHandle handle; } axes[] = {
        {frame.u, kColorAxisU, GizmoHandle::AxisU},
        {frame.v, kColorAxisV, GizmoHandle::AxisV},
        {frame.n, kColorAxisN, GizmoHandle::AxisN},
    };
    for (const auto& axis : axes) {
        append_thick_segment(out, origin, origin + he * axis.dir,
                             eye, hw, color_for(axis.handle, axis.color));
    }

    // Plane-handle patch outlines: the [0.3he, 0.6he]^2 square of each basis
    // pair — the same patch pick_gizmo_handle hit-tests.
    const float a = 0.3f * he, b = 0.6f * he;
    const struct { simd_float3 e1, e2; simd_float4 color; GizmoHandle handle; } patches[] = {
        {frame.u, frame.v, kColorPlaneUV, GizmoHandle::PlaneUV},
        {frame.u, frame.n, kColorPlaneUN, GizmoHandle::PlaneUN},
        {frame.v, frame.n, kColorPlaneVN, GizmoHandle::PlaneVN},
    };
    for (const auto& patch : patches) {
        const simd_float4 c = color_for(patch.handle, patch.color);
        const simd_float3 p00 = origin + a * patch.e1 + a * patch.e2;
        const simd_float3 p10 = origin + b * patch.e1 + a * patch.e2;
        const simd_float3 p11 = origin + b * patch.e1 + b * patch.e2;
        const simd_float3 p01 = origin + a * patch.e1 + b * patch.e2;
        append_thick_segment(out, p00, p10, eye, hw, c);
        append_thick_segment(out, p10, p11, eye, hw, c);
        append_thick_segment(out, p11, p01, eye, hw, c);
        append_thick_segment(out, p01, p00, eye, hw, c);
    }
}

void append_spiked_cube(std::vector<LineVertex>& out, simd_float3 center, float half_extent,
                        float half_width, simd_float3 eye, simd_float4 color) {
    const float he = half_extent;

    // 12 cube edges, world-axis-aligned corners at center ± he — reuses
    // append_cube_edges' corner/edge tables scaled inline (that function
    // bakes a transform + thin verts, this one needs thick segments).
    static constexpr std::array<simd_float3, 8> kCorners = {{
        {-1.0f, -1.0f, -1.0f}, { 1.0f, -1.0f, -1.0f},
        { 1.0f,  1.0f, -1.0f}, {-1.0f,  1.0f, -1.0f},
        {-1.0f, -1.0f,  1.0f}, { 1.0f, -1.0f,  1.0f},
        { 1.0f,  1.0f,  1.0f}, {-1.0f,  1.0f,  1.0f},
    }};
    static constexpr std::array<std::array<int, 2>, 12> kEdges = {{
        {0, 1}, {1, 5}, {5, 4}, {4, 0}, // bottom
        {3, 2}, {2, 6}, {6, 7}, {7, 3}, // top
        {0, 3}, {1, 2}, {5, 6}, {4, 7}, // verticals
    }};
    for (const auto& edge : kEdges) {
        append_thick_segment(out, center + he * kCorners[edge[0]], center + he * kCorners[edge[1]],
                             eye, half_width, color);
    }

    // 6 spikes, one per face: from the wall's center out to 2he — the marker
    // reads as a pivot from any view direction.
    static constexpr std::array<simd_float3, 6> kFaceDirs = {{
        { 1.0f, 0.0f, 0.0f}, {-1.0f, 0.0f, 0.0f},
        { 0.0f, 1.0f, 0.0f}, { 0.0f, -1.0f, 0.0f},
        { 0.0f, 0.0f, 1.0f}, { 0.0f, 0.0f, -1.0f},
    }};
    for (const simd_float3 dir : kFaceDirs) {
        append_thick_segment(out, center + he * dir, center + 2.0f * he * dir,
                             eye, half_width, color);
    }
}

std::vector<LineVertex> build_scene_lines(const SceneDocument& doc, int32_t selected_id, simd_float3 eye_world) {
    std::vector<LineVertex> out;
    for (const Node& node : doc.nodes()) {
        simd_float4 color;
        if (node.id == selected_id) {
            color = kColorSelected; // selected override always wins, either op
        } else if (node.op == Op::Subtract) {
            color = kColorSubtract; // unselected Subtract: its carve is its only visual
        } else {
            continue; // unselected Add: already visible live via the raymarch
        }
        const simd_float4x4 world_from_local = node.world_from_local();
        if (node.shape == Shape::Cube) {
            append_cube_edges(out, world_from_local, color);
        } else {
            append_sphere_outline(out, world_from_local, color, eye_world);
        }
    }
    return out;
}

} // namespace sq
