#pragma once

// SDF scene representation, CSG evaluation, and ray-generation -- the single
// source of truth shared by the CPU (DCSDD sampling, core/src/sdf.cpp) and
// the GPU raymarch shader (shaders/raymarch.metal).
// Dual-compile: this header is included from both plain C++ (core/) and MSL
// (shaders/*.metal). MSL has neither <simd/simd.h> nor <cfloat>/std::, so
// every type and math call below is selected via __METAL_VERSION__, the same
// idiom shared_types.h uses for its own typedefs.

#include "shared_types.h" // sq_float4, sq_float4x4

// sq_float2/sq_float3 live here rather than in shared_types.h because they are
// LOCAL MATH ONLY -- shared_types.h bans float3 (and by the same reasoning
// float2) from shared STRUCTS, where MSL's 16-byte float3 would silently
// desync from the host layout. Nothing below is ever a struct field; the 2D
// types exist because most of the shape SDFs work in a (radial, axial) plane.
#ifdef __METAL_VERSION__
typedef metal::float2 sq_float2;
typedef metal::float3 sq_float3;
#else
#include <cfloat>
#include <cmath>
typedef simd_float2 sq_float2;
typedef simd_float3 sq_float3;
#endif

// MSL has no <cfloat>. This is the standard FLT_MAX text (glibc/Darwin
// <cfloat> define FLT_MAX with this exact literal) so the two branches agree
// bit-for-bit once rounded to float32; the C++ branch uses the real FLT_MAX
// so it is exact by construction, not just "close".
#ifdef __METAL_VERSION__
#define SDF_FLT_MAX 3.402823466e+38f
#else
#define SDF_FLT_MAX FLT_MAX
#endif

// `device` is Metal's address-space qualifier for a real MTL::Buffer of
// arbitrary size (the raymarch shader's node-array binding -- `constant` is
// for small fixed-size setFragmentBytes data, not this); plain C++ has no
// address spaces, so a packed node array is just `const T*` there.
#ifdef __METAL_VERSION__
#define SDF_NODE_PTR device const SdfNode*
#else
#define SDF_NODE_PTR const SdfNode*
#endif

// ---------------------------------------------------------------------------
// Portable math toolkit: thin wrappers selecting metal:: vs simd_-prefixed
// free functions per __METAL_VERSION__, so the actual SDF/ray-gen code below
// reads the same in both languages. Kept to exactly what this file needs.

#ifdef __METAL_VERSION__
inline float sdf_abs(float v) { return metal::abs(v); }
inline sq_float3 sdf_abs(sq_float3 v) { return metal::abs(v); }
inline sq_float2 sdf_min(sq_float2 a, sq_float2 b) { return metal::min(a, b); }
inline sq_float2 sdf_max(sq_float2 a, sq_float2 b) { return metal::max(a, b); }
inline sq_float3 sdf_min(sq_float3 a, sq_float3 b) { return metal::min(a, b); }
inline sq_float3 sdf_max(sq_float3 a, sq_float3 b) { return metal::max(a, b); }
inline float sdf_min(float a, float b) { return metal::min(a, b); }
inline float sdf_max(float a, float b) { return metal::max(a, b); }
inline float sdf_length(sq_float2 v) { return metal::length(v); }
inline float sdf_length(sq_float3 v) { return metal::length(v); }
inline float sdf_dot(sq_float2 a, sq_float2 b) { return metal::dot(a, b); }
inline sq_float3 sdf_cross(sq_float3 a, sq_float3 b) { return metal::cross(a, b); }
inline sq_float3 sdf_normalize(sq_float3 v) { return metal::normalize(v); }
inline float sdf_reduce_max(sq_float3 v) { return metal::max(v.x, metal::max(v.y, v.z)); }
inline float sdf_reduce_min(sq_float3 v) { return metal::min(v.x, metal::min(v.y, v.z)); }
inline float sdf_sqrt(float v) { return metal::sqrt(v); }
inline float sdf_floor(float v) { return metal::floor(v); }
inline float sdf_cos(float v) { return metal::cos(v); }
inline float sdf_sin(float v) { return metal::sin(v); }
inline float sdf_atan2(float y, float x) { return metal::atan2(y, x); }
inline sq_float2 sdf_make2(float x, float y) { return sq_float2(x, y); }
inline sq_float3 sdf_make3(float x, float y, float z) { return sq_float3(x, y, z); }
inline sq_float4 sdf_make4(float x, float y, float z, float w) { return sq_float4(x, y, z, w); }
inline sq_float4 sdf_transform(sq_float4x4 m, sq_float4 v) { return m * v; }
#else
inline float sdf_abs(float v) { return std::fabs(v); }
inline sq_float3 sdf_abs(sq_float3 v) { return simd_abs(v); }
inline sq_float2 sdf_min(sq_float2 a, sq_float2 b) { return simd_min(a, b); }
inline sq_float2 sdf_max(sq_float2 a, sq_float2 b) { return simd_max(a, b); }
inline sq_float3 sdf_min(sq_float3 a, sq_float3 b) { return simd_min(a, b); }
inline sq_float3 sdf_max(sq_float3 a, sq_float3 b) { return simd_max(a, b); }
inline float sdf_min(float a, float b) { return std::fmin(a, b); }
inline float sdf_max(float a, float b) { return std::fmax(a, b); }
inline float sdf_length(sq_float2 v) { return simd_length(v); }
inline float sdf_length(sq_float3 v) { return simd_length(v); }
inline float sdf_dot(sq_float2 a, sq_float2 b) { return simd_dot(a, b); }
inline sq_float3 sdf_cross(sq_float3 a, sq_float3 b) { return simd_cross(a, b); }
inline sq_float3 sdf_normalize(sq_float3 v) { return simd_normalize(v); }
inline float sdf_reduce_max(sq_float3 v) { return simd_reduce_max(v); }
inline float sdf_reduce_min(sq_float3 v) { return simd_reduce_min(v); }
inline float sdf_sqrt(float v) { return std::sqrt(v); }
inline float sdf_floor(float v) { return std::floor(v); }
inline float sdf_cos(float v) { return std::cos(v); }
inline float sdf_sin(float v) { return std::sin(v); }
inline float sdf_atan2(float y, float x) { return std::atan2(y, x); }
inline sq_float2 sdf_make2(float x, float y) { return simd_make_float2(x, y); }
inline sq_float3 sdf_make3(float x, float y, float z) { return simd_make_float3(x, y, z); }
inline sq_float4 sdf_make4(float x, float y, float z, float w) { return simd_make_float4(x, y, z, w); }
inline sq_float4 sdf_transform(sq_float4x4 m, sq_float4 v) { return simd_mul(m, v); }
#endif

// Language-independent, so written once rather than dispatched.
//
// sdf_mod is GLSL's `mod`, NOT C's `fmod`: they differ in sign for a negative
// numerator, and the prism's sector fold below feeds it an atan2 result, which
// is negative for half the circle. metal::fmod is the C-style one, so wrapping
// it would have been the wrong function under the right name.
inline float sdf_clamp(float v, float lo, float hi) { return sdf_min(sdf_max(v, lo), hi); }
inline float sdf_sign(float v) { return (v < 0.0f) ? -1.0f : ((v > 0.0f) ? 1.0f : 0.0f); }
inline float sdf_mod(float x, float y) { return x - y * sdf_floor(x / y); }

// Rotates `v` by the unit quaternion `q` (xyz = imaginary, w = real -- the
// same layout simd_quatf::vector uses, so packing is a straight field copy).
//
// The standard two-cross-product form, written out by hand rather than
// dispatched per language: MSL has no quaternion type at all, so there is
// nothing to wrap on the Metal side. Non-unit `q` scales the result; every
// caller is responsible for supplying a normalized quaternion (see
// pack_scene).
inline sq_float3 sdf_rotate(sq_float4 q, sq_float3 v) {
    const sq_float3 axis = sdf_make3(q.x, q.y, q.z);
    return v + 2.0f * sdf_cross(axis, sdf_cross(axis, v) + q.w * v);
}

// ---------------------------------------------------------------------------
// SdfNode: one CSG primitive, packed for the raymarch shader's node buffer
// (bound as a `device const SdfNode*` MTL::Buffer, sized to the packed
// scene's actual byte length each frame -- see core/src/renderer.cpp -- not
// a fixed-size setFragmentBytes binding, so there is no node-count cap).
//
// pos_shape.xyz = world position; pos_shape.w = shape (the SDF_SHAPE_* ids
// below). half_extents_op.xyz = per-axis half extents (scale * 0.5);
// half_extents_op.w = op (0 = add, 1 = subtract).
// inv_rotation = the INVERSE of the node's rotation, as a unit quaternion
// (xyz imaginary, w real). Inverted at pack time rather than here because
// evaluation only ever needs world -> local, and a sphere trace evaluates
// every node at every step: conjugating once per node per frame is free,
// conjugating per step per pixel is not.
// params.x = the shape's one profile parameter (see the SHAPE PARAMETER note
// on sdf_eval_node); params.yzw are reserved, so a second parameter will not
// need another packing change.
typedef struct {
    sq_float4 pos_shape;
    sq_float4 half_extents_op;
    sq_float4 inv_rotation;
    sq_float4 params;
} SdfNode;

// Compiled under both __METAL_VERSION__ (a future raymarch .metal TU) and
// plain C++ (core/tests), via the dual-compile typedefs above -- so this one
// assert covers both sides, matching shared_types.h's MeshVertex precedent.
static_assert(sizeof(SdfNode) == 64, "SdfNode must be 64 bytes");

// Shape ids, as they ride in pos_shape.w. #defines rather than an enum because
// this header compiles as MSL, which cannot include the interop header where
// sq::Shape lives (<cstdint>, <swift/bridging>). core/src/sdf.cpp static_asserts
// each of these against its sq::Shape counterpart, so the two cannot drift.
//
// Ids 0 and 1 are load-bearing history: they were cube and sphere when
// pos_shape.w was compared directly against 0.0f, and moving them would
// silently repaint every scene.
#define SDF_SHAPE_CUBE       0
#define SDF_SHAPE_SPHERE     1
#define SDF_SHAPE_CONE       2
#define SDF_SHAPE_CAPSULE    3
#define SDF_SHAPE_OCTAHEDRON 4
#define SDF_SHAPE_PYRAMID    5
#define SDF_SHAPE_PRISM      6
#define SDF_SHAPE_VESICA     7

// Floor on a half-extent, guarding the cross-section contraction's division.
// Node::scale is clamped to kNodeScaleMin by every drag, but SceneDocument::add
// takes a Node verbatim, so the field can hold whatever a test or a future
// importer puts there -- and a zero would otherwise produce inf, then NaN, and
// a NaN propagates through the CSG fold to the whole scene.
#define SDF_MIN_HALF_EXTENT 1e-5f

// ---------------------------------------------------------------------------
// Per-shape local SDFs (see sdf_eval_node below for the world -> local
// transform, and for how half-extents reach each one).

// Exact box SDF in the box's own frame: q relative to the box center,
// half_extents = per-axis half sizes. Exact under nonuniform half_extents.
inline float sdf_sd_box(sq_float3 q, sq_float3 half_extents) {
    const sq_float3 d = sdf_abs(q) - half_extents;
    const float outside = sdf_length(sdf_max(d, sdf_make3(0.0f, 0.0f, 0.0f)));
    const float inside = sdf_min(sdf_reduce_max(d), 0.0f);
    return outside + inside;
}

// Ellipsoid SDF (iq's approximation) in the ellipsoid's own frame: q relative
// to its center, radii = per-axis radii. Reduces to the exact `length(q) - r`
// when radii are uniform. q == 0 is a singularity of the approximation
// (division by zero); guarded to return -min_component(radii), the exact
// distance to the nearest surface point from the center.
inline float sdf_sd_ellipsoid(sq_float3 q, sq_float3 radii) {
    const float k0 = sdf_length(q / radii);
    const float k1 = sdf_length(q / (radii * radii));
    if (k1 == 0.0f) {
        return -sdf_reduce_min(radii);
    }
    return k0 * (k0 - 1.0f) / k1;
}

// --- the six shapes that take their proportions from (r, hy) ---------------
//
// Every one below is written about the LOCAL Y AXIS -- axis of revolution for
// the round ones, extrusion axis for the prism, height axis for the two
// frusta -- and takes real dimensions rather than unit-normalised ones:
// `r` a radius (or base half-size), `h` a half-height. sdf_eval_node supplies
// them from the node's half-extents; see the CROSS-SECTION note there for how
// a non-circular cross-section is handled.
//
// All are iq's (https://iquilezles.org/articles/distfunctions/) specialised to
// the Y axis, which drops the general endpoint/orientation math, EXCEPT the
// square frustum, which iq's catalogue has no exact form for -- see its note.

// Capped cone: radius r1 at y = -h, radius r2 at y = +h. Exact.
// r2 = 0 is a sharp cone; r2 = r1 is a cylinder; both are reachable from the
// tip dial and neither is a special case here.
inline float sdf_sd_capped_cone(sq_float3 p, float h, float r1, float r2) {
    const sq_float2 q = sdf_make2(sdf_length(sdf_make2(p.x, p.z)), p.y);
    const sq_float2 k1 = sdf_make2(r2, h);
    const sq_float2 k2 = sdf_make2(r2 - r1, 2.0f * h);
    const sq_float2 ca =
        sdf_make2(q.x - sdf_min(q.x, (q.y < 0.0f) ? r1 : r2), sdf_abs(q.y) - h);
    // dot(k2, k2) >= 4*h*h > 0, so this division is safe for any h above the
    // half-extent floor -- no guard needed.
    const sq_float2 cb = q - k1 + k2 * sdf_clamp(sdf_dot(k1 - q, k2) / sdf_dot(k2, k2), 0.0f, 1.0f);
    const float s = (cb.x < 0.0f && ca.y < 0.0f) ? -1.0f : 1.0f;
    return s * sdf_sqrt(sdf_min(sdf_dot(ca, ca), sdf_dot(cb, cb)));
}

// Rounded cylinder: outer radius r, half-height h, edge rounding rb. Exact.
//
// This is what the CAPSULE shape actually is, and the reason it can carry a
// dial at all. rb = 0 is a flat-capped cylinder; rb = min(r, h) is a true
// capsule when h >= r, and a disc with a fully rounded rim when h < r. A plain
// capsule formula would have left that second case undefined and needing a
// clamp, and would have left the dial with nothing to say.
inline float sdf_sd_rounded_cylinder(sq_float3 p, float r, float h, float rb) {
    const sq_float2 d =
        sdf_make2(sdf_length(sdf_make2(p.x, p.z)) - (r - rb), sdf_abs(p.y) - (h - rb));
    return sdf_min(sdf_max(d.x, d.y), 0.0f) + sdf_length(sdf_max(d, sdf_make2(0.0f, 0.0f))) - rb;
}

// Octahedron with vertices at distance s along each axis. Exact -- iq's
// branching form, not the cheap `(|x|+|y|+|z|-s) * 0.57735` bound, which is a
// plane distance everywhere and badly underestimates near the vertices.
inline float sdf_sd_octahedron(sq_float3 p, float s) {
    p = sdf_abs(p);
    const float m = p.x + p.y + p.z - s;
    sq_float3 q;
    if (3.0f * p.x < m) {
        q = sdf_make3(p.x, p.y, p.z);
    } else if (3.0f * p.y < m) {
        q = sdf_make3(p.y, p.z, p.x);
    } else if (3.0f * p.z < m) {
        q = sdf_make3(p.z, p.x, p.y);
    } else {
        return m * 0.57735027f; // 1/sqrt(3): the face-plane distance, exact in this region
    }
    const float k = sdf_clamp(0.5f * (q.z - q.y + s), 0.0f, s);
    return sdf_length(sdf_make3(q.x, q.y - s + k, q.z - k));
}

// Regular n-gon in 2D, CIRCUMRADIUS r -- vertices sit on the circle of radius
// r, edges inside it, so the polygon's greatest radial extent is exactly r for
// every n. Exact.
//
// Both readings below are easy to get backwards, and getting either one wrong
// produces a polygon that looks plausible and is the wrong size or angle -- so
// sdf_tests pins them directly rather than trusting this comment:
//
//   RADIUS. `r` is the circumradius, not the apothem. After the sector fold,
//   `r * acs` IS a vertex, and the clamp walks from there along the edge to the
//   midpoint at r*cos(pi/n). Reading it as the apothem would put the vertices
//   at r/cos(pi/n) -- OUTSIDE the node's own box, by 2x for a triangle.
//
//   ORIENTATION. atan2(p.x, p.y) measures from +y and the fold is symmetric
//   about it, so a VERTEX faces +y (local +z once extruded), with the two
//   adjacent edge midpoints at +-pi/n either side.
inline float sdf_sd_regular_polygon(sq_float2 p, float r, int n) {
    const float an = 3.14159265f / float(n);
    const sq_float2 acs = sdf_make2(sdf_cos(an), sdf_sin(an));
    const float bn = sdf_mod(sdf_atan2(p.x, p.y), 2.0f * an) - an;
    p = sdf_length(p) * sdf_make2(sdf_cos(bn), sdf_abs(sdf_sin(bn)));
    p = p - r * acs;
    p.y += sdf_clamp(-p.y, 0.0f, r * acs.y);
    return sdf_length(p) * sdf_sign(p.x);
}

// Regular n-gon prism: circumradius r in the xz plane, extruded to |y| <= h.
// Exact -- the convex-extrusion combine is exact whenever the 2D profile is.
inline float sdf_sd_prism(sq_float3 p, float r, float h, int n) {
    const float d2 = sdf_sd_regular_polygon(sdf_make2(p.x, p.z), r, n);
    const sq_float2 w = sdf_make2(d2, sdf_abs(p.y) - h);
    return sdf_min(sdf_max(w.x, w.y), 0.0f) + sdf_length(sdf_max(w, sdf_make2(0.0f, 0.0f)));
}

// Vesica of revolution about y: half-length h, mid half-width w. Exact.
//
// The profile is a circular arc from (0, -h) through (w, 0) to (0, +h), revolved
// about y; the solid is the disc through those points intersected with rho >= 0.
// w < h gives the almond/spindle with a cusp at each pole, w == h gives exactly
// a sphere of radius h, and w > h gives a bi-cusped spinning-top -- a valid
// shape, not a degenerate one, which matters because w and h come straight from
// the node's box and nothing stops a user making it wider than it is tall.
//
// DEVIATION FROM iq's sdVesicaSegment, and the reason this is not just his
// formula inlined: his region test is a single angular comparison, which is only
// valid while the profile arc is the MINOR one. Once w > h the centre crosses to
// rho > 0, the arc becomes major, and the angular test wraps -- misclassifying
// points near the axis as being nearest the cusp, which returns a POSITIVE
// distance for points that are deep inside the solid. (Verified against a
// brute-force distance to the revolved arc before this was rewritten.)
//
// The test below asks the question directly instead: project onto the full
// circle, and use that projection when it lands on the arc (rho >= 0),
// otherwise fall back to the cusp. That is orientation-free, so it holds for
// the minor arc, the major arc and the semicircle alike -- and it reproduces
// his answer exactly wherever his is valid.
inline float sdf_sd_vesica(sq_float3 p, float h, float w) {
    const sq_float2 q = sdf_make2(sdf_length(sdf_make2(p.x, p.z)), sdf_abs(p.y));
    // Offset of the arc's centre from the axis, and the arc's radius. `d` goes
    // negative exactly when w > h, which is what swings the centre across the
    // axis and turns the minor arc into the major one.
    const float d = 0.5f * (h * h - w * w) / w;
    const float arc_radius = d + w;
    const sq_float2 v = q - sdf_make2(-d, 0.0f);
    const float len = sdf_length(v);

    // Distance to the circle, written as (len^2 - arc_radius^2) / (len +
    // arc_radius) rather than the obvious len - arc_radius. THE DIFFERENCE IS
    // NOT COSMETIC. A slender vesica -- which is exactly what full roundness
    // produces, since rounding shrinks the underlying spindle towards a
    // segment -- sends d towards 1e5, and subtracting two float32 values that
    // large loses every digit that mattered: the branch test below then picks
    // the wrong region and the shape reads as inside-out. Expanding the
    // difference of squares cancels d analytically instead of numerically.
    // arc_radius > 0 always (it is a radius), so the denominator cannot vanish.
    const float dist = (q.x * q.x + q.y * q.y - w * w + 2.0f * d * (q.x - w)) / (len + arc_radius);

    // Does the projection onto the full circle land on the arc (rho >= 0)?
    // Algebraically arc_radius * v.x - d * len, rearranged in terms of `dist`
    // so it inherits the same cancellation fix rather than reintroducing it.
    if (d * (q.x - dist) + w * q.x >= 0.0f) {
        return dist;
    }
    // Cusp region: nearest surface point is the pole itself. The sign still
    // comes from the disc, and cannot be zero here -- a point ON the circle
    // always projects to itself, which lands on the arc and takes the branch
    // above -- so this stays continuous across the boundary.
    return sdf_sign(dist) * sdf_length(q - sdf_make2(0.0f, h));
}

// Square frustum: base half-size a at y = -h, top half-size b at y = +h.
//
// THE ONE APPROXIMATION THIS FILE ADDS, and the only shape not taken from iq's
// catalogue (which has an exact sharp pyramid but no frustum). It is the max of
// six half-space distances -- four slant faces plus the two caps -- with UNIT
// normals, which makes it exact on the surface and throughout the interior,
// 1-Lipschitz everywhere, and conservative outside near the edges and corners
// where the true nearest feature is an edge rather than a face. The same
// bargain sdf_sd_ellipsoid already makes, and safe for sphere tracing for the
// same reason: it never overestimates.
//
// A consequence worth knowing: at b == a this is a box, and this field is
// weaker than sdf_sd_box's on that same box. Special-casing is available if it
// ever matters; it is not worth the branch up front.
inline float sdf_sd_square_frustum(sq_float3 p, float a, float b, float h) {
    // Slant-face normal, in the (horizontal, y) plane: perpendicular to the
    // face direction (b - a, 2h) and pointing outward. Non-degenerate for any
    // h above the half-extent floor, since len >= 2h.
    const float len = sdf_sqrt(4.0f * h * h + (a - b) * (a - b));
    const float nh = 2.0f * h / len;
    const float ny = (a - b) / len;
    const float offset = h * (a + b) / len;
    // |x| and |z| fold the four slant faces onto one evaluation each.
    const float slant_x = nh * sdf_abs(p.x) + ny * p.y - offset;
    const float slant_z = nh * sdf_abs(p.z) + ny * p.y - offset;
    return sdf_max(sdf_abs(p.y) - h, sdf_max(slant_x, slant_z));
}

// ---------------------------------------------------------------------------

// Half-extents, made safe to divide by: absolute value (a negative scale
// component must mirror, not invert the shape) and floored off zero.
inline sq_float3 sdf_safe_half_extents(sq_float3 h) {
    const sq_float3 floor3 =
        sdf_make3(SDF_MIN_HALF_EXTENT, SDF_MIN_HALF_EXTENT, SDF_MIN_HALF_EXTENT);
    return sdf_max(sdf_abs(h), floor3);
}

// CROSS-SECTION CONTRACTION -- see the SCALE note on sdf_eval_node for why this
// is the shape of the answer. Squeezes x and z so the elliptical cross-section
// the box asks for becomes the circle of radius r = min(hx, hz) the primitives
// above are written against. Both factors are <= 1 BY CONSTRUCTION (r is the
// min), which is the entire safety argument: a map that never expands cannot
// turn a 1-Lipschitz function into a steeper one, so no correction factor is
// needed and none must be added. When hx == hz it is the identity and the
// distance that comes back is exact.
inline sq_float3 sdf_contract_xz(sq_float3 q, sq_float3 h, float r) {
    return sdf_make3(q.x * (r / h.x), q.y, q.z * (r / h.z));
}

// Evaluates one packed node's local SDF at world-space point p: translate
// into the node's frame, rotate into its axes, then measure.
//
// SCALE IS NOT PART OF THIS TRANSFORM, and must not become part of it. It
// lives in half_extents, which sdf_sd_box takes exactly and sdf_sd_ellipsoid
// takes as radii -- so the shape is measured at its true dimensions rather
// than unit-normalized. Dividing q by half_extents to evaluate a unit
// primitive is the obvious-looking "simplification" here and it WARPS the
// field: distances would come back in a scaled space, non-uniformly so, and
// the sphere trace would overstep.
//
// The six Y-axis shapes cannot take all three half-extents the way a box does
// -- they are written about a single radius -- and the answer is a CONTRACTION,
// never that division. sdf_contract_xz squeezes the cross-section onto the
// circle of radius r = min(hx, hz) and then measures the primitive at its real
// (r, hy). Both squeeze factors are <= 1, so the composition stays 1-Lipschitz
// in world space with exactly the right zero set, and it is EXACT whenever
// hx == hz -- which includes every axially-stretched capsule and cone, i.e.
// most of what this editor is for. The crude alternative, sd_unit(q/h)*min(h),
// has the same zero set but underestimates by max(h)/min(h) EVERYWHERE, which
// starves the trace at exactly the aspect ratios this domain uses.
//
// SHAPE PARAMETER (params.x): the one profile degree of freedom the box cannot
// state. Cone and pyramid read it as a tip ratio in [0, 1] (0 = point,
// 1 = untapered), prism as a side count in [3, 12], and cube, capsule,
// octahedron and vesica as ROUNDNESS in [0, 1]. Core clamps and snaps it in
// setNodeShapeParam, but every read below is defensive anyway: this function
// also runs on GPU against a buffer, where a bad value would produce NaNs
// across the whole fold rather than one wrong node.
//
// Roundness is one idea applied four times, and it is exact and bbox-tight in
// every case by the same argument: shrink the shape by rb, then subtract rb
// from its distance. Offsetting an exact SDF by a constant is exact, and a
// convex body's extent along every direction grows by exactly rb under that
// offset -- so (extent - rb) + rb is the box you started with, and the rounded
// shape still fills it. The sphere is the only shape with no parameter at all,
// because an ellipsoid is already the roundest thing its box allows.
inline float sdf_shape_roundness(float param) { return sdf_clamp(param, 0.0f, 1.0f); }
inline float sdf_eval_node(SdfNode node, sq_float3 p) {
    const sq_float3 q = sdf_rotate(node.inv_rotation, p - node.pos_shape.xyz);
    // NOT named `half`: that's a reserved MSL type keyword (the 16-bit float
    // type), and this header must compile under __METAL_VERSION__ too.
    const sq_float3 half_extents = node.half_extents_op.xyz;
    const int shape = int(node.pos_shape.w);
    const float param = node.params.x;

    if (shape == SDF_SHAPE_CUBE) {
        // No safe half-extents and no division here, so a zero-scale cube still
        // behaves exactly as it did before rounding existed.
        const float rb = sdf_shape_roundness(param) * sdf_reduce_min(sdf_abs(half_extents));
        return sdf_sd_box(q, half_extents - sdf_make3(rb, rb, rb)) - rb;
    }
    if (shape == SDF_SHAPE_SPHERE) {
        return sdf_sd_ellipsoid(q, half_extents);
    }

    const sq_float3 h = sdf_safe_half_extents(half_extents);
    if (shape == SDF_SHAPE_OCTAHEDRON) {
        // The one shape that distinguishes no axis, so it contracts all three
        // onto s = min(h) rather than just the cross-section.
        const float s = sdf_reduce_min(h);
        const float rb = sdf_shape_roundness(param) * s;
        const sq_float3 c = sdf_make3(q.x * (s / h.x), q.y * (s / h.y), q.z * (s / h.z));
        // s - rb reaches 0 at full roundness, where sdf_sd_octahedron collapses
        // to the distance to the origin and the offset turns it into a sphere
        // of radius s. No guard needed: nothing divides by it.
        return sdf_sd_octahedron(c, s - rb) - rb;
    }
    if (shape == SDF_SHAPE_VESICA) {
        // THE VESICA CAPS ITS OWN CROSS-SECTION AT h.y, and must. Its profile
        // arc's centre crosses the axis once the mid-width passes the
        // half-length, which puts the arc's topmost point inside the revolved
        // region -- so the solid reaches (w^2 + hy^2) / 2w in y, past the box
        // that was supposed to contain it. Capping w keeps the contracted shape
        // a spindle (or, at the limit, a sphere), whose y extent IS hy.
        //
        // Contracting by the capped w rather than min(hx, hz) is what stops
        // that being a dead zone on the x handle: a box wider than it is tall
        // still fills in x and z, as a flattened lens rather than a spindle,
        // and the squeeze factors stay <= 1 either way.
        const float w = sdf_min(sdf_min(h.x, h.z), h.y);
        const sq_float3 c = sdf_contract_xz(q, h, w);
        const float rb = sdf_shape_roundness(param) * w;
        // Both floors bite only at full roundness, where the underlying spindle
        // degenerates to a segment and the offset makes it a capsule.
        return sdf_sd_vesica(c, sdf_max(h.y - rb, SDF_MIN_HALF_EXTENT),
                             sdf_max(w - rb, SDF_MIN_HALF_EXTENT)) - rb;
    }

    const float r = sdf_min(h.x, h.z);
    const sq_float3 c = sdf_contract_xz(q, h, r);
    switch (shape) {
        case SDF_SHAPE_CONE:
            return sdf_sd_capped_cone(c, h.y, r, r * sdf_clamp(param, 0.0f, 1.0f));
        case SDF_SHAPE_CAPSULE:
            // rb is capped at min(r, h.y), the largest rounding the box admits,
            // so the dial's top end is a capsule when tall and a fully rounded
            // rim when flat -- and never an out-of-range rb.
            return sdf_sd_rounded_cylinder(c, r, h.y,
                                           sdf_shape_roundness(param) * sdf_min(r, h.y));
        case SDF_SHAPE_PYRAMID:
            return sdf_sd_square_frustum(c, r, r * sdf_clamp(param, 0.0f, 1.0f), h.y);
        case SDF_SHAPE_PRISM:
            return sdf_sd_prism(c, r, h.y, int(sdf_clamp(sdf_floor(param + 0.5f), 3.0f, 12.0f)));
        default:
            // Unknown id: an empty half-space rather than a hard zero, so a
            // corrupt node cannot union a surface into the scene at the origin.
            return SDF_FLT_MAX;
    }
}

// CSG fold over a packed node array, in document order: Add -> min(d,
// d_node), Subtract -> max(d, -d_node), starting d = +FLT_MAX (SDF_FLT_MAX,
// since MSL has no <cfloat>). `count` must be <= the array's length.
//
// Known approximation: min/max CSG combine is not a true distance field near
// intersection curves (only the zero-set/surface is exact). The DCSDD paper
// (Table 1) shows the reconstruction tolerates comparable noise, so this is
// accepted rather than worked around.
inline float sdf_fold(SDF_NODE_PTR nodes, int count, sq_float3 p) {
    float d = SDF_FLT_MAX;
    for (int i = 0; i < count; ++i) {
        const SdfNode node = nodes[i];
        const float d_node = sdf_eval_node(node, p);
        d = (node.half_extents_op.w == 0.0f) ? sdf_min(d, d_node) : sdf_max(d, -d_node);
    }
    return d;
}

// ---------------------------------------------------------------------------
// Ray generation.

struct SdfRay {
    sq_float3 origin;
    sq_float3 dir; // normalized
};

// Generates the world-space camera ray through pixel (px, py) -- top-left
// origin, matching both Metal's fragment framebuffer coordinates and this
// project's ViewPoint/Camera::ray_through_view_point convention -- given the
// viewport size and the inverse view-projection matrix (compute via
// simd_inverse/metal::inverse on the caller's side; not recomputed here).
//
// direction: unprojects the pixel at Metal clip z=0 (near) and z=1 (far),
// perspective-divides both to world space, and normalizes (far - near).
//
// origin: the eye (center of projection), *not* the near-plane point --
// Camera::kNear is 0.1, so the near point sits a non-negligible distance
// from eye along the ray and would not agree with
// Camera::ray_through_view_point's origin to tight tolerance. Recovered
// algebraically from inv_view_proj alone (no separate eye/near/far input):
// for this project's perspective_matrix() (row 3 = (0,0,-1,0), so
// clip.w = -view_z), pure matrix associativity gives
//   inverse(V) * (0,0,0,1) == inv_view_proj * (P * (0,0,0,1))
// and P * (0,0,0,1) is exactly P's 4th column, (0, 0, c, 0) for some nonzero
// constant c (the only nonzero component is z) -- so the right-hand side is
// c * inv_view_proj.columns[2]. Dividing that column's xyz by its own w
// cancels the unknown c and yields eye exactly. (Verified numerically
// against this project's pinned camera literals before writing this code;
// see the R0 report.)
inline SdfRay sdf_ray_for_pixel(float px, float py, float viewport_w, float viewport_h,
                                 sq_float4x4 inv_view_proj) {
    const float ndc_x = 2.0f * px / viewport_w - 1.0f;
    const float ndc_y = 1.0f - 2.0f * py / viewport_h;

    const sq_float4 clip_near = sdf_make4(ndc_x, ndc_y, 0.0f, 1.0f);
    const sq_float4 clip_far = sdf_make4(ndc_x, ndc_y, 1.0f, 1.0f);

    const sq_float4 world_near_h = sdf_transform(inv_view_proj, clip_near);
    const sq_float4 world_far_h = sdf_transform(inv_view_proj, clip_far);

    const sq_float3 world_near = world_near_h.xyz / world_near_h.w;
    const sq_float3 world_far = world_far_h.xyz / world_far_h.w;

    SdfRay ray;
    ray.dir = sdf_normalize(world_far - world_near);

    const sq_float4 col2 = inv_view_proj.columns[2];
    ray.origin = col2.xyz / col2.w;

    return ray;
}
