#include "lines.h"

#include <algorithm>
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

// --- unit-local shape wireframes ---------------------------------------------
//
// WHY THESE CAN IGNORE SCALE. Every shape's evaluator works in the contracted
// frame, where the cross-section is the circle of radius r = min(hx, hz) and the
// height is hy (see sdf_contract_xz). Dividing that frame by the node's own
// half-extents -- which is exactly what world_from_local's S undoes -- sends the
// cross-section to the circle of radius 0.5 and the height to +-0.5 for ANY
// half-extents. So the shapes below are drawn once, in the unit box, and the
// node's transform stretches them into the ellipse-sectioned real thing.
//
// The two exceptions are the capsule and the vesica, whose profile CURVES bend
// differently as the box's aspect changes; those take the half-extents and use
// unit_profile below.

// One closed loop of `segments` points at height y and radius rad. Segment 0
// sits on +z, which is where sdf_sd_regular_polygon's sector fold puts a
// vertex -- so passing segments = n draws exactly the prism's cross-section,
// correctly clocked, and no separate polygon builder is needed.
void append_ring(std::vector<LineVertex>& out, const simd_float4x4& m, simd_float4 color,
                 float y, float rad, int segments) {
    auto point = [&](int i) -> simd_float3 {
        const float t = 2.0f * float(M_PI) * static_cast<float>(i % segments) /
                        static_cast<float>(segments);
        return simd_float3{rad * std::sin(t), y, rad * std::cos(t)};
    };
    for (int i = 0; i < segments; ++i) {
        out.push_back(make_vertex(point(i), m, color));
        out.push_back(make_vertex(point(i + 1), m, color));
    }
}

// A contracted-frame profile point (rho, y) in unit-local coordinates. rho
// divides by the cross-section radius and y by the half-height, which is why a
// circular arc in the contracted frame comes out elliptical here -- correctly
// so, since that is what the node's scale does to it.
simd_float2 unit_profile(float rho, float y, float r, float hy) {
    return simd_make_float2(rho / (2.0f * r), y / (2.0f * hy));
}

// Revolves a profile into `meridians` cross-sections. `profile` runs from the
// -y pole to the +y pole with rho >= 0; each meridian draws it on both sides of
// the axis, so the pair closes into one loop through both poles.
void append_lathe(std::vector<LineVertex>& out, const simd_float4x4& m, simd_float4 color,
                  const std::vector<simd_float2>& profile, int meridians) {
    for (int k = 0; k < meridians; ++k) {
        const float t = float(M_PI) * static_cast<float>(k) / static_cast<float>(meridians);
        const simd_float3 dir{std::sin(t), 0.0f, std::cos(t)};
        for (const float side : {1.0f, -1.0f}) {
            for (size_t i = 0; i + 1 < profile.size(); ++i) {
                const simd_float3 a = side * profile[i].x * dir + simd_float3{0, profile[i].y, 0};
                const simd_float3 b =
                    side * profile[i + 1].x * dir + simd_float3{0, profile[i + 1].y, 0};
                out.push_back(make_vertex(a, m, color));
                out.push_back(make_vertex(b, m, color));
            }
        }
    }
}

// Cone / capped cone: base ring, top ring once the tip is blunted, and four
// slant lines at the cardinal azimuths to carry the taper.
void append_cone_edges(std::vector<LineVertex>& out, const simd_float4x4& m, simd_float4 color,
                       float tip) {
    const float top = 0.5f * tip;
    append_ring(out, m, color, -0.5f, 0.5f, kShapeRingSegments);
    if (top > 1e-4f) {
        append_ring(out, m, color, 0.5f, top, kShapeRingSegments);
    }
    for (int i = 0; i < 4; ++i) {
        const float t = 0.5f * float(M_PI) * static_cast<float>(i);
        const simd_float3 dir{std::sin(t), 0.0f, std::cos(t)};
        out.push_back(make_vertex(0.5f * dir + simd_float3{0, -0.5f, 0}, m, color));
        out.push_back(make_vertex(top * dir + simd_float3{0, 0.5f, 0}, m, color));
    }
}

// Square frustum: base quad, top quad once blunted, four slant edges.
void append_pyramid_edges(std::vector<LineVertex>& out, const simd_float4x4& m, simd_float4 color,
                          float tip) {
    const float top = 0.5f * tip;
    const std::array<simd_float2, 4> corners = {{{1, 1}, {1, -1}, {-1, -1}, {-1, 1}}};
    for (int i = 0; i < 4; ++i) {
        const simd_float2 c = corners[i];
        const simd_float2 n = corners[(i + 1) % 4];
        const simd_float3 base_c{0.5f * c.x, -0.5f, 0.5f * c.y};
        const simd_float3 base_n{0.5f * n.x, -0.5f, 0.5f * n.y};
        const simd_float3 top_c{top * c.x, 0.5f, top * c.y};
        const simd_float3 top_n{top * n.x, 0.5f, top * n.y};
        out.push_back(make_vertex(base_c, m, color));
        out.push_back(make_vertex(base_n, m, color));
        out.push_back(make_vertex(base_c, m, color));
        out.push_back(make_vertex(top_c, m, color));
        if (top > 1e-4f) {
            out.push_back(make_vertex(top_c, m, color));
            out.push_back(make_vertex(top_n, m, color));
        }
    }
}

// n-gon prism: the two end faces plus one vertical per vertex.
void append_prism_edges(std::vector<LineVertex>& out, const simd_float4x4& m, simd_float4 color,
                        int sides) {
    append_ring(out, m, color, -0.5f, 0.5f, sides);
    append_ring(out, m, color, 0.5f, 0.5f, sides);
    for (int i = 0; i < sides; ++i) {
        const float t = 2.0f * float(M_PI) * static_cast<float>(i) / static_cast<float>(sides);
        const float x = 0.5f * std::sin(t);
        const float z = 0.5f * std::cos(t);
        out.push_back(make_vertex(simd_float3{x, -0.5f, z}, m, color));
        out.push_back(make_vertex(simd_float3{x, 0.5f, z}, m, color));
    }
}

// Octahedron: the 12 edges joining its 6 axis vertices.
void append_octahedron_edges(std::vector<LineVertex>& out, const simd_float4x4& m,
                             simd_float4 color) {
    const std::array<simd_float3, 2> poles = {{{0, 0.5f, 0}, {0, -0.5f, 0}}};
    const std::array<simd_float3, 4> belt = {{{0.5f, 0, 0}, {0, 0, 0.5f}, {-0.5f, 0, 0}, {0, 0, -0.5f}}};
    for (int i = 0; i < 4; ++i) {
        out.push_back(make_vertex(belt[i], m, color));
        out.push_back(make_vertex(belt[(i + 1) % 4], m, color));
        for (const simd_float3& pole : poles) {
            out.push_back(make_vertex(belt[i], m, color));
            out.push_back(make_vertex(pole, m, color));
        }
    }
}

// Rounded cylinder, drawn as a lathe: the straight side, the two cap arcs, and
// the flat ends that survive while the rounding is partial. `half` is needed
// because the cap radius is min(r, hy)-relative, so the profile genuinely
// changes shape as the box's aspect does.
void append_capsule_edges(std::vector<LineVertex>& out, const simd_float4x4& m, simd_float4 color,
                          float roundness, simd_float3 half) {
    const float r = std::fmin(half.x, half.z);
    const float hy = half.y;
    const float rb = roundness * std::fmin(r, hy);

    std::vector<simd_float2> profile;
    profile.push_back(unit_profile(0.0f, -hy, r, hy));           // bottom pole
    for (int i = 0; i <= kShapeProfileSegments; ++i) {           // bottom cap arc
        const float a = 0.5f * float(M_PI) * static_cast<float>(i) /
                        static_cast<float>(kShapeProfileSegments);
        profile.push_back(unit_profile(r - rb + rb * std::sin(a), -(hy - rb) - rb * std::cos(a),
                                        r, hy));
    }
    for (int i = 0; i <= kShapeProfileSegments; ++i) {           // top cap arc
        const float a = 0.5f * float(M_PI) * static_cast<float>(i) /
                        static_cast<float>(kShapeProfileSegments);
        profile.push_back(unit_profile(r - rb + rb * std::cos(a), (hy - rb) + rb * std::sin(a),
                                        r, hy));
    }
    profile.push_back(unit_profile(0.0f, hy, r, hy));            // top pole
    append_lathe(out, m, color, profile, 2);
    append_ring(out, m, color, 0.0f, 0.5f, kShapeRingSegments);  // the equator, always at full radius
}

// Vesica: the profile is a single circular arc through the two poles and the
// equator, so the lathe takes it directly. See sdf_sd_vesica for the geometry
// (and for why the arc becomes the major one once the box is wider than tall).
void append_vesica_edges(std::vector<LineVertex>& out, const simd_float4x4& m, simd_float4 color,
                         simd_float3 half) {
    const float r = std::fmin(half.x, half.z);
    const float hy = half.y;
    const float d = 0.5f * (hy * hy - r * r) / r;
    const float arc_radius = d + r;
    // Angular half-sweep from the equator to a pole, about the arc's own centre
    // at rho = -d. The pole (0, hy) sits at (d, hy) relative to that centre --
    // note the sign: it is +d, and getting it backwards sends the arc sweeping
    // past its own poles and straight out of the box. Once the box is wider
    // than it is tall d goes negative, and this angle correctly exceeds pi/2,
    // which is what draws the major arc.
    const float sweep = std::atan2(hy, d);

    std::vector<simd_float2> profile;
    const int steps = 2 * kShapeProfileSegments;
    for (int i = 0; i <= steps; ++i) {
        const float a = -sweep + 2.0f * sweep * static_cast<float>(i) / static_cast<float>(steps);
        profile.push_back(unit_profile(-d + arc_radius * std::cos(a), arc_radius * std::sin(a),
                                        r, hy));
    }
    append_lathe(out, m, color, profile, 2);
    append_ring(out, m, color, 0.0f, 0.5f, kShapeRingSegments);
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

void append_move_gizmo_grid(std::vector<LineVertex>& out, const GizmoFrame& frame, int divisions,
                            float alpha_scale) {
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
        return kGizmoGridAlpha * alpha_scale * (1.0f - smooth);
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
    // The grid spans the grid plane, NOT the u-v plane: for an attached node
    // those are the same, but for a free one the grid is world-horizontal (see
    // GizmoFrame in gizmo.h). The basis comes from the frame rather than being
    // re-derived here, so the grid, the plane patch drawn inside it, and that
    // patch's hit region all read the same two vectors.
    const simd_float3 a_dir = frame.grid_u;
    const simd_float3 b_dir = frame.grid_v;

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

    // The plane handle: the [kGizmoPatchInner, kGizmoPatchOuter]^2 square in the
    // GRID plane — the same patch pick_gizmo_handle hit-tests, from the same
    // constants and the same basis. Filled translucent quad plus a hairline
    // outline: a filled patch reads as a surface you can slide along, which is
    // what the handle actually does, and the grid it sits in says which surface.
    {
        const float a = kGizmoPatchInner * he, b = kGizmoPatchOuter * he;
        const simd_float4 c = color_for(GizmoHandle::Plane, kColorPlane);
        const simd_float3 p00 = origin + a * frame.grid_u + a * frame.grid_v;
        const simd_float3 p10 = origin + b * frame.grid_u + a * frame.grid_v;
        const simd_float3 p11 = origin + b * frame.grid_u + b * frame.grid_v;
        const simd_float3 p01 = origin + a * frame.grid_u + b * frame.grid_v;

        simd_float4 fill = c;
        fill.w = ((GizmoHandle::Plane == highlighted) ? (2.0f * kGizmoPatchFillAlpha)
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

    // Same rest/hot treatment as the placement gizmo, so hover reads
    // identically across both manipulators.
    auto color_for = [&](GizmoHandle handle, simd_float4 base) {
        if (handle == highlighted) {
            return kColorGizmoHot;
        }
        base.w = rest_alpha;
        return base;
    };

    // Boxes on the axis rays with no shaft behind them (see lines.h), well
    // outboard of both the centre box and the Placement gizmo's shafts.
    // pick_gizmo_handle clamps to the box's own extent, so drawn = hit here too.
    // Emission order (u, v, n) matches the pick tie-break order; lines_tests
    // pins the layout.
    const struct { simd_float3 dir; simd_float4 color; GizmoHandle handle; } axes[] = {
        {frame.u, kColorAxisU, GizmoHandle::AxisU},
        {frame.v, kColorAxisV, GizmoHandle::AxisV},
        {frame.n, kColorAxisN, GizmoHandle::AxisN},
    };
    for (const auto& axis : axes) {
        append_camera_facing_quad(out, origin + kScaleTipCenterFrac * he * axis.dir,
                                  kGizmoScaleTipHalfSizeFrac * he, eye,
                                  color_for(axis.handle, axis.color));
    }

    append_camera_facing_quad(out, origin, kGizmoUniformHalfSizeFrac * he, eye,
                              color_for(GizmoHandle::Uniform, kColorGizmoUniform));
}

void append_rotate_gizmo_rings(std::vector<LineVertex>& out, const GizmoFrame& frame,
                               GizmoHandle highlighted, simd_float3 eye, float rest_alpha) {
    const simd_float3 origin = frame.origin;
    const float radius = kRotateRingFrac * frame.half_extent;
    const float hw = kGizmoHandleHalfWidthFrac * frame.half_extent;

    auto color_for = [&](GizmoHandle handle, simd_float4 base) {
        if (handle == highlighted) {
            return kColorGizmoHot;
        }
        base.w = rest_alpha;
        return base;
    };

    // Each ring is spanned by the OTHER two frame vectors, taken in an order
    // that keeps the traversal right-handed about its own axis. Using the frame
    // directly rather than tangent_basis(axis) matters: these have to be the
    // same vectors the drag reads its angle in, or the drawn ring and the angle
    // it produces would disagree about which way is positive.
    //
    // Full circles, not just the camera-facing halves. Half-rings are the
    // denser-looking convention, but they need a near-side test that goes
    // unstable exactly when a ring is viewed face-on -- every point is then
    // equidistant from the eye and the visible half flickers around the
    // circumference. The whole circle costs more lines and no correctness.
    const struct { simd_float3 e1, e2, axis; simd_float4 color; GizmoHandle handle; } rings[] = {
        {frame.v, frame.n, frame.u, kColorAxisU, GizmoHandle::RingU},
        {frame.n, frame.u, frame.v, kColorAxisV, GizmoHandle::RingV},
        {frame.u, frame.v, frame.n, kColorAxisN, GizmoHandle::RingN},
    };
    for (const auto& ring : rings) {
        const simd_float4 c = color_for(ring.handle, ring.color);
        simd_float3 prev = origin + radius * ring.e1;
        for (int i = 1; i <= kRotateRingSegments; ++i) {
            const float t = 2.0f * static_cast<float>(M_PI) * static_cast<float>(i) /
                            static_cast<float>(kRotateRingSegments);
            const simd_float3 p =
                origin + radius * (std::cos(t) * ring.e1 + std::sin(t) * ring.e2);
            append_thick_segment(out, prev, p, eye, hw, c);
            prev = p;
        }
    }
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
        append_node_wireframe(out, node, color, eye_world);
    }
    return out;
}

void append_node_wireframe(std::vector<LineVertex>& out, const Node& node, simd_float4 color,
                           simd_float3 eye_world) {
    const simd_float4x4 m = node.world_from_local();
    const simd_float3 half = 0.5f * simd_abs(node.scale);
    const float param = node.shape_param;
    switch (node.shape) {
        case Shape::Cube:       append_cube_edges(out, m, color); break;
        case Shape::Sphere:     append_sphere_outline(out, m, color, eye_world); break;
        case Shape::Cone:       append_cone_edges(out, m, color, param); break;
        case Shape::Capsule:    append_capsule_edges(out, m, color, param, half); break;
        case Shape::Octahedron: append_octahedron_edges(out, m, color); break;
        case Shape::Pyramid:    append_pyramid_edges(out, m, color, param); break;
        case Shape::Prism:
            append_prism_edges(out, m, color,
                               static_cast<int>(std::lround(std::clamp(param, 3.0f, 12.0f))));
            break;
        case Shape::Vesica:     append_vesica_edges(out, m, color, half); break;
    }
}

} // namespace sq
