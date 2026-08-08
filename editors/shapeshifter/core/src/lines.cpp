#include "lines.h"

#include <algorithm>
#include <array>
#include <cmath>

#include <ground_grid.h> // kGroundAxisY -- shared with the ground plate's shader

#include <sdf_scene.h>   // SDF_MIN_HALF_EXTENT -- the same floor the evaluator uses

#include "gizmo.h"
#include "math_util.h"      // trs_matrix
#include "scene.h"
#include "sdf.h"        // local_sdf_node -- profiles are measured off the shape's own field
#include "sdf_scene.h"  // sdf_eval_node

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
// The capsule and the vesica are the exceptions, because their profile CURVES
// bend as both the box's aspect and the dial change. Rather than re-deriving
// those curves, they are measured off the node's own SDF -- see sample_profile.

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

// Samples a surface-of-revolution's half-profile from the node's OWN SDF, by
// bisecting outward from the centre over a fan of polar angles running pole to
// pole. Returns unit-local (rho, y), which is what append_lathe consumes.
//
// Sampled rather than derived, for two reasons. The profile has to track the
// dial -- a capsule's caps and a vesica's tips both change shape as roundness
// turns -- and hand-deriving each offset curve is exactly how this file first
// shipped a vesica arc that swept past its own poles and out of its box. And
// this way the drawn outline cannot disagree with the rendered surface: it is
// measured from it.
//
// Sound because every shape here is convex, hence star-shaped about its centre,
// so each ray meets the surface exactly once.
std::vector<simd_float2> sample_profile(const Node& node, simd_float3 half_extents) {
    const SdfNode sn = local_sdf_node(node, half_extents);
    // The full extent, since the profile is sampled in UNIT-local coordinates
    // and this maps them into the node's rigid frame. Already absolute and
    // already carrying any inherited uniform scale, because it came from a
    // resolved placement.
    const simd_float3 scale = 2.0f * half_extents;
    // Every shape is inscribed in the unit box, so no surface sits beyond its
    // half-diagonal; 0.9 brackets that with room to spare.
    constexpr float kOutside = 0.9f;
    constexpr int kBisectionSteps = 24;

    std::vector<simd_float2> profile;
    const int steps = 2 * kShapeProfileSegments;
    for (int i = 0; i <= steps; ++i) {
        // -pi/2 is the bottom pole, 0 the equator, +pi/2 the top pole.
        const float angle = float(M_PI) * (static_cast<float>(i) / static_cast<float>(steps) - 0.5f);
        const simd_float2 dir = simd_make_float2(std::cos(angle), std::sin(angle));
        float inside = 0.0f;
        float outside = kOutside;
        for (int k = 0; k < kBisectionSteps; ++k) {
            const float mid = 0.5f * (inside + outside);
            const simd_float3 unit{mid * dir.x, mid * dir.y, 0.0f};
            if (sdf_eval_node(sn, unit * scale) < 0.0f) {
                inside = mid;
            } else {
                outside = mid;
            }
        }
        profile.push_back(0.5f * (inside + outside) * dir);
    }
    return profile;
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

// Rounded box: the six FLAT FACES, each a rectangle bounded by where that face
// meets the rounded edge around it. Every corner of every rectangle lies on the
// surface, and at full roundness each rectangle collapses to a point at its
// face's centre -- which is exactly where the resulting ball touches its box.
//
// Not used at zero roundness: the six rectangles degenerate to the box's own
// edges there, drawn twice over, so append_cube_edges' single pass is both
// cheaper and what the existing vertex-count expectations pin.
void append_rounded_box_edges(std::vector<LineVertex>& out, const simd_float4x4& m,
                              simd_float4 color, simd_float3 half, float roundness) {
    const float rb = roundness * simd_reduce_min(half);
    // Unit-local half-width of each face's flat portion. Anisotropic, because
    // the rounding radius is a single world length while unit-local space
    // divides each axis by its own half-extent.
    // FLOORED, and by the evaluator's own constant. rb is zero whenever any
    // extent is, so an unfloored divisor makes this 0/0 -- NaN positions
    // straight into the vertex buffer, for a node the shader's cube branch
    // deliberately keeps benign. Reachable through SceneDocument::add, which is
    // the caller the surrounding comments already cite as the reason to defend.
    const simd_float3 safe_half =
        simd_max(simd_abs(half), simd_float3{SDF_MIN_HALF_EXTENT, SDF_MIN_HALF_EXTENT,
                                             SDF_MIN_HALF_EXTENT});
    const simd_float3 flat = 0.5f * (simd_float3{1.0f, 1.0f, 1.0f} - rb / safe_half);
    for (int axis = 0; axis < 3; ++axis) {
        const int b = (axis + 1) % 3;
        const int c = (axis + 2) % 3;
        for (const float side : {0.5f, -0.5f}) {
            std::array<simd_float3, 4> corner{};
            for (int k = 0; k < 4; ++k) {
                const float sb = (k == 0 || k == 3) ? 1.0f : -1.0f;
                const float sc = (k < 2) ? 1.0f : -1.0f;
                corner[k] = simd_float3{0.0f, 0.0f, 0.0f};
                corner[k][axis] = side;
                corner[k][b] = sb * flat[b];
                corner[k][c] = sc * flat[c];
            }
            for (int k = 0; k < 4; ++k) {
                out.push_back(make_vertex(corner[k], m, color));
                out.push_back(make_vertex(corner[(k + 1) % 4], m, color));
            }
        }
    }
}

// Rounded octahedron: the eight flat triangular faces. In unit-local space the
// evaluator reduces to an octahedron of vertex distance (1-c)/2 offset by c/2 --
// isotropic, whatever the node's half-extents, because that shape contracts all
// three axes onto one. So the core shrinks and the offset grows as the dial
// turns, and at full roundness all eight triangles collapse onto the sphere the
// shape has become. Sharp case excluded for the same reason as the box above.
void append_rounded_octahedron_edges(std::vector<LineVertex>& out, const simd_float4x4& m,
                                     simd_float4 color, float roundness) {
    const float core = 0.5f * (1.0f - roundness);          // vertex distance of the core
    const float offset = 0.5f * roundness / std::sqrt(3.0f); // face offset, per component
    for (int sx = -1; sx <= 1; sx += 2) {
        for (int sy = -1; sy <= 1; sy += 2) {
            for (int sz = -1; sz <= 1; sz += 2) {
                const simd_float3 n{static_cast<float>(sx), static_cast<float>(sy),
                                    static_cast<float>(sz)};
                const std::array<simd_float3, 3> tri = {{
                    simd_float3{n.x * core, 0.0f, 0.0f} + offset * n,
                    simd_float3{0.0f, n.y * core, 0.0f} + offset * n,
                    simd_float3{0.0f, 0.0f, n.z * core} + offset * n,
                }};
                for (int k = 0; k < 3; ++k) {
                    out.push_back(make_vertex(tri[k], m, color));
                    out.push_back(make_vertex(tri[(k + 1) % 3], m, color));
                }
            }
        }
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

// Both surfaces of revolution draw the same way: their profile measured off
// the SDF, revolved, plus the equator ring at full radius. What differs between
// a capsule and a vesica is entirely in that profile, so nothing here has to
// know which it is drawing.
void append_lathe_shape(std::vector<LineVertex>& out, const simd_float4x4& m, simd_float4 color,
                        const Node& node, simd_float3 half_extents) {
    append_lathe(out, m, color, sample_profile(node, half_extents), 2);
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
        if (node.kind != NodeKind::Shape) {
            continue; // a Group has no box to outline
        }
        simd_float4 color;
        if (node.id == selected_id) {
            color = kColorSelected; // selected override always wins, either op
        } else if (node.op == Op::Subtract) {
            color = kColorSubtract; // unselected Subtract: its carve is its only visual
        } else {
            continue; // unselected Add: already visible live via the raymarch
        }
        append_node_wireframe(out, node, doc.placement(node.id), color, eye_world);
    }
    return out;
}

void append_node_wireframe(std::vector<LineVertex>& out, const Node& node,
                           const NodePlacement& placement, simd_float4 color,
                           simd_float3 eye_world) {
    // half_extents rather than the node's raw scale, and the difference is
    // load-bearing twice over. The evaluator measures against
    // abs(half_extents) (sdf_safe_half_extents), so a negative scale component
    // mirrors the solid; drawing the raw scale would flip the outline instead
    // and put an asymmetric shape -- a cone, a pyramid -- tip-down against a
    // tip-up surface. And the box is the node's own scale times whatever
    // uniform scale it inherits, which only the document can resolve.
    const simd_float3 half = placement.half_extents;
    const simd_float4x4 m =
        trs_matrix(placement.frame.position, placement.frame.rotation, 2.0f * half);
    const float param = node.shape_param;
    // Zero-roundness threshold: below it the rounded builders would draw every
    // edge twice for no visible gain, so the sharp forms take over.
    const bool rounded = param > 1e-4f;
    switch (node.shape) {
        case Shape::Cube:
            if (rounded) { append_rounded_box_edges(out, m, color, half, param); }
            else { append_cube_edges(out, m, color); }
            break;
        case Shape::Sphere:     append_sphere_outline(out, m, color, eye_world); break;
        case Shape::Cone:       append_cone_edges(out, m, color, param); break;
        case Shape::Capsule:    append_lathe_shape(out, m, color, node, half); break;
        case Shape::Octahedron:
            if (rounded) { append_rounded_octahedron_edges(out, m, color, param); }
            else { append_octahedron_edges(out, m, color); }
            break;
        case Shape::Pyramid:    append_pyramid_edges(out, m, color, param); break;
        case Shape::Prism:
            append_prism_edges(out, m, color,
                               static_cast<int>(std::lround(std::clamp(param, 3.0f, 12.0f))));
            break;
        case Shape::Vesica:     append_lathe_shape(out, m, color, node, half); break;
    }
}

} // namespace sq
