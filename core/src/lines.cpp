#include "lines.h"

#include <array>
#include <cmath>

#include <ground_grid.h> // kGroundAxisY -- shared with the ground plate's shader

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

    // Radial fade in the frame's own (a, b) coordinates, so the grid dissolves
    // into a soft disc instead of ending at a hard square edge (user ruling).
    const auto alpha_at = [&](float a, float b) {
        const float r = std::sqrt(a * a + b * b);
        const float t = std::clamp((r - kGizmoGridFadeBegin * he) /
                                       ((kGizmoGridFadeEnd - kGizmoGridFadeBegin) * he),
                                   0.0f, 1.0f);
        const float smooth = t * t * (3.0f - 2.0f * t);
        return kGizmoGridAlpha * (1.0f - smooth);
    };

    auto push = [&](float a, float b, simd_float3 e1, simd_float3 e2) {
        const simd_float3 p = origin + a * e1 + b * e2;
        LineVertex vertex;
        vertex.pos = (simd_float4){p.x, p.y, p.z, 1.0f};
        vertex.color = kColorGridLine;
        vertex.color.w = alpha_at(a, b);
        out.push_back(vertex);
    };

    // For each sample i in [0, divisions], emit one line running along u
    // (offset along v) and one running along v (offset along u). The center
    // lines are drawn like any other: the axis handles only cover their
    // positive halves (R3), so skipping the center would leave the -he..0
    // halves gapped; the positive halves just sit under the thick handles.
    //
    // Each line is emitted as kGizmoGridSegmentsPerLine disjoint segments
    // rather than one long one: the fade is radial, so a 2-vertex line would
    // interpolate it linearly and wash out the falloff (see lines.h).
    // The grid spans frame.grid_normal's plane, NOT the u-v drag plane: for an
    // attached node those are the same plane, but for a free one the grid is a
    // world-horizontal reference (see GizmoFrame in gizmo.h). Derived through
    // tangent_basis so the attached case reproduces (u, v) exactly -- it is the
    // same call gizmo_frame_for_node made to build them.
    simd_float3 a_dir, b_dir;
    tangent_basis(frame.grid_normal, a_dir, b_dir);

    const int segs = kGizmoGridSegmentsPerLine;
    for (int i = 0; i <= divisions; ++i) {
        const float offset = -he + static_cast<float>(i) * step;
        for (int s = 0; s < segs; ++s) {
            const float t0 = -he + 2.0f * he * static_cast<float>(s) / static_cast<float>(segs);
            const float t1 = -he + 2.0f * he * static_cast<float>(s + 1) / static_cast<float>(segs);
            // Along a (offset along b): coordinates are (t, offset) in (a, b).
            push(t0, offset, a_dir, b_dir);
            push(t1, offset, a_dir, b_dir);
            // Along b (offset along a): coordinates are (offset, t).
            push(offset, t0, a_dir, b_dir);
            push(offset, t1, a_dir, b_dir);
        }
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

// A square facing the eye, centered at `center` with the given half-size (two
// triangles, 6 verts). Used for the point-like bits of chrome -- the origin
// pip and the gizmo's axis terminator dots -- where a screen-space dot is
// wanted but the pass draws world geometry. The in-plane basis comes from
// tangent_basis (gizmo.h), the same routine the gizmo frame uses, so the
// square's orientation is derived exactly once in the codebase.
void append_camera_facing_quad(std::vector<LineVertex>& out, simd_float3 center, float half_size,
                               simd_float3 eye, simd_float4 color) {
    simd_float3 n = eye - center;
    const float len = simd_length(n);
    if (len < 1e-6f) {
        return; // eye exactly at the center: no facing direction to build from
    }
    n /= len;

    simd_float3 u, v;
    tangent_basis(n, u, v);
    const simd_float3 su = half_size * u;
    const simd_float3 sv = half_size * v;

    auto push = [&](simd_float3 p) {
        LineVertex vertex;
        vertex.pos = (simd_float4){p.x, p.y, p.z, 1.0f};
        vertex.color = color;
        out.push_back(vertex);
    };
    push(center - su - sv); push(center + su - sv); push(center + su + sv);
    push(center - su - sv); push(center + su + sv); push(center - su + sv);
}

} // namespace

void append_move_gizmo_handles(std::vector<LineVertex>& out, const GizmoFrame& frame,
                               GizmoHandle highlighted, simd_float3 eye, float rest_alpha) {
    const simd_float3 origin = frame.origin;
    const float he = frame.half_extent;
    const float hw = kGizmoHandleHalfWidthFrac * he;
    const float border_hw = kGizmoPatchBorderHalfWidthFrac * he;
    // Plane fills track the handles' own opacity, so a dimmed gizmo dims as a
    // whole rather than keeping its largest, loudest element at full strength.
    const float fill_scale = rest_alpha / kGizmoHandleRestAlpha;

    // A resting handle is its own color at `rest_alpha`; the highlighted one
    // goes opaque white. Hover therefore reads as both a brightness and a color
    // change, which is what lets it stay legible on every handle regardless of
    // that handle's base color.
    auto color_for = [&](GizmoHandle handle, simd_float4 base) {
        if (handle == highlighted) {
            return kColorGizmoHot;
        }
        base.w = rest_alpha;
        return base;
    };

    // Axis shafts from the origin, POSITIVE half only (R3 user ruling), over
    // the Placement band. The shaft stops at kMoveAxisShaftFrac*he and a
    // camera-facing dot caps it; the pick clamps to kMoveAxisOuterFrac, so the
    // dot is grabbable rather than being dead space past the end of the target.
    // Emission order (u, v, n) matches the pick tie-break order; lines_tests
    // pins the layout.
    const struct { simd_float3 dir; simd_float4 color; GizmoHandle handle; } axes[] = {
        {frame.u, kColorAxisU, GizmoHandle::AxisU},
        {frame.v, kColorAxisV, GizmoHandle::AxisV},
        {frame.n, kColorAxisN, GizmoHandle::AxisN},
    };
    for (const auto& axis : axes) {
        append_thick_segment(out, origin, origin + kMoveAxisShaftFrac * he * axis.dir,
                             eye, hw, color_for(axis.handle, axis.color));
    }
    for (const auto& axis : axes) {
        append_camera_facing_quad(out, origin + kMoveAxisShaftFrac * he * axis.dir,
                                  kGizmoAxisTipHalfSizeFrac * he, eye,
                                  color_for(axis.handle, axis.color));
    }

    // Plane handles: the [kGizmoPatchInner, kGizmoPatchOuter]^2 square of each
    // basis pair — the same patch pick_gizmo_handle hit-tests, from the same
    // constants. Filled translucent quad plus a hairline outline, rather than
    // the previous bare outline of full-weight bars: a filled patch reads as
    // a surface you can slide along, which is what the handle actually does.
    const float a = kGizmoPatchInner * he, b = kGizmoPatchOuter * he;
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

        simd_float4 fill = c;
        fill.w = ((patch.handle == highlighted) ? (2.0f * kGizmoPatchFillAlpha)
                                                : kGizmoPatchFillAlpha) * fill_scale;
        auto push_fill = [&](simd_float3 p) {
            LineVertex vertex;
            vertex.pos = (simd_float4){p.x, p.y, p.z, 1.0f};
            vertex.color = fill;
            out.push_back(vertex);
        };
        push_fill(p00); push_fill(p10); push_fill(p11);
        push_fill(p00); push_fill(p11); push_fill(p01);

        append_thick_segment(out, p00, p10, eye, border_hw, c);
        append_thick_segment(out, p10, p11, eye, border_hw, c);
        append_thick_segment(out, p11, p01, eye, border_hw, c);
        append_thick_segment(out, p01, p00, eye, border_hw, c);
    }

    // Origin pip: anchors the three shafts at a single visible point, which
    // matters more now that they are thin. Dims with the rest of the gizmo --
    // when the pair is coalesced this sits inside the Shape gizmo's uniform
    // box, and a full-strength white dot there would read as a handle of its
    // own rather than as the Placement gizmo's centre.
    simd_float4 pip = kColorOriginPip;
    pip.w *= rest_alpha / kGizmoHandleRestAlpha;
    append_camera_facing_quad(out, origin, kGizmoAxisTipHalfSizeFrac * he, eye, pip);
}

void append_scale_gizmo_handles(std::vector<LineVertex>& out, const GizmoFrame& frame,
                                GizmoHandle highlighted, simd_float3 eye, float rest_alpha) {
    const simd_float3 origin = frame.origin;
    const float he = frame.half_extent;
    const float hw = kGizmoHandleHalfWidthFrac * he;

    // Same rest/hot treatment as the placement gizmo, so hover reads
    // identically across both manipulators.
    auto color_for = [&](GizmoHandle handle, simd_float4 base) {
        if (handle == highlighted) {
            return kColorGizmoHot;
        }
        base.w = rest_alpha;
        return base;
    };

    // Shafts run the Shape band, outboard of both the centre box and the
    // Placement gizmo's own shafts; pick_gizmo_handle clamps to the same band,
    // so drawn geometry = hit geometry here too. Emission order (u, v, n)
    // matches the pick tie-break order; lines_tests pins the layout.
    const struct { simd_float3 dir; simd_float4 color; GizmoHandle handle; } axes[] = {
        {frame.u, kColorAxisU, GizmoHandle::AxisU},
        {frame.v, kColorAxisV, GizmoHandle::AxisV},
        {frame.n, kColorAxisN, GizmoHandle::AxisN},
    };
    for (const auto& axis : axes) {
        const simd_float4 c = color_for(axis.handle, axis.color);
        append_thick_segment(out, origin + kScaleAxisInnerFrac * he * axis.dir,
                             origin + kScaleAxisShaftFrac * he * axis.dir, eye, hw, c);
    }
    for (const auto& axis : axes) {
        append_camera_facing_quad(out, origin + kScaleAxisShaftFrac * he * axis.dir,
                                  kGizmoScaleTipHalfSizeFrac * he, eye,
                                  color_for(axis.handle, axis.color));
    }

    append_camera_facing_quad(out, origin, kGizmoUniformHalfSizeFrac * he, eye,
                              color_for(GizmoHandle::Uniform, kColorGizmoUniform));
}

void append_anchor_tether(std::vector<LineVertex>& out, simd_float3 anchor, simd_float3 centre,
                          float half_width, simd_float3 eye) {
    append_thick_segment(out, anchor, centre, eye, half_width, kColorAnchorTether);
}

void append_focus_dot(std::vector<LineVertex>& out, simd_float3 center, float half_size,
                      simd_float3 eye, simd_float4 color) {
    append_camera_facing_quad(out, center, half_size, eye, color);
}

void append_origin_marker(std::vector<LineVertex>& out, float height, float half_width,
                          float pip_half_size, simd_float3 eye) {
    const simd_float3 origin = {0.0f, 0.0f, 0.0f};
    // kGroundAxisY is the shader's own constant (ground_grid.h), not a copy:
    // the +Y shaft has to match the X/Z lines the plate draws, and those live
    // on the other side of the language boundary.
    append_thick_segment(out, origin, (simd_float3){0.0f, height, 0.0f}, eye, half_width,
                         kGroundAxisY);
    append_camera_facing_quad(out, origin, pip_half_size, eye, kColorOriginPip);
}

void append_pivot_crosshair(std::vector<LineVertex>& out, simd_float3 center, float radius,
                            float half_width, simd_float3 eye, simd_float4 color) {
    simd_float3 n = eye - center;
    const float len = simd_length(n);
    if (len < 1e-6f) {
        return; // eye exactly at the pivot: no facing plane to draw in
    }
    n /= len;

    // Ring and ticks both live in the eye-facing plane, so the marker is a
    // flat disc-shaped annotation from every angle -- the point of replacing
    // the spiked cube, which changed silhouette as the camera moved and so
    // read as an object in the scene.
    simd_float3 u, v;
    tangent_basis(n, u, v);

    const auto ring_point = [&](int i) {
        // i % segments makes the final segment land exactly on vertex 0.
        const float t = static_cast<float>(i % kPivotRingSegments) /
                        static_cast<float>(kPivotRingSegments) * 2.0f * float(M_PI);
        return center + radius * (std::cos(t) * u + std::sin(t) * v);
    };
    for (int i = 0; i < kPivotRingSegments; ++i) {
        append_thick_segment(out, ring_point(i), ring_point(i + 1), eye, half_width, color);
    }

    const simd_float3 tick_dirs[] = {u, -u, v, -v};
    for (const simd_float3 dir : tick_dirs) {
        append_thick_segment(out, center + kPivotTickInnerFrac * radius * dir,
                             center + kPivotTickOuterFrac * radius * dir,
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
