#pragma once

// SDF scene representation, CSG evaluation, and ray-generation -- the single
// source of truth shared by the CPU (DCSDD sampling, core/src/sdf.cpp) and
// the GPU raymarch shader (shaders/raymarch.metal).
// Dual-compile: this header is included from both plain C++ (core/) and MSL
// (shaders/*.metal). MSL has neither <simd/simd.h> nor <cfloat>/std::, so
// every type and math call below is selected via __METAL_VERSION__, the same
// idiom shared_types.h uses for its own typedefs.

#include "shared_types.h" // sq_float4, sq_float4x4

#ifdef __METAL_VERSION__
typedef metal::float3 sq_float3;
#else
#include <cfloat>
#include <cmath>
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
inline sq_float3 sdf_abs(sq_float3 v) { return metal::abs(v); }
inline sq_float3 sdf_min(sq_float3 a, sq_float3 b) { return metal::min(a, b); }
inline sq_float3 sdf_max(sq_float3 a, sq_float3 b) { return metal::max(a, b); }
inline float sdf_min(float a, float b) { return metal::min(a, b); }
inline float sdf_max(float a, float b) { return metal::max(a, b); }
inline float sdf_length(sq_float3 v) { return metal::length(v); }
inline sq_float3 sdf_normalize(sq_float3 v) { return metal::normalize(v); }
inline float sdf_reduce_max(sq_float3 v) { return metal::max(v.x, metal::max(v.y, v.z)); }
inline float sdf_reduce_min(sq_float3 v) { return metal::min(v.x, metal::min(v.y, v.z)); }
inline sq_float3 sdf_make3(float x, float y, float z) { return sq_float3(x, y, z); }
inline sq_float4 sdf_make4(float x, float y, float z, float w) { return sq_float4(x, y, z, w); }
inline sq_float4 sdf_transform(sq_float4x4 m, sq_float4 v) { return m * v; }
#else
inline sq_float3 sdf_abs(sq_float3 v) { return simd_abs(v); }
inline sq_float3 sdf_min(sq_float3 a, sq_float3 b) { return simd_min(a, b); }
inline sq_float3 sdf_max(sq_float3 a, sq_float3 b) { return simd_max(a, b); }
inline float sdf_min(float a, float b) { return std::fmin(a, b); }
inline float sdf_max(float a, float b) { return std::fmax(a, b); }
inline float sdf_length(sq_float3 v) { return simd_length(v); }
inline sq_float3 sdf_normalize(sq_float3 v) { return simd_normalize(v); }
inline float sdf_reduce_max(sq_float3 v) { return simd_reduce_max(v); }
inline float sdf_reduce_min(sq_float3 v) { return simd_reduce_min(v); }
inline sq_float3 sdf_make3(float x, float y, float z) { return simd_make_float3(x, y, z); }
inline sq_float4 sdf_make4(float x, float y, float z, float w) { return simd_make_float4(x, y, z, w); }
inline sq_float4 sdf_transform(sq_float4x4 m, sq_float4 v) { return simd_mul(m, v); }
#endif

// ---------------------------------------------------------------------------
// SdfNode: one CSG primitive, packed for the raymarch shader's node buffer
// (bound as a `device const SdfNode*` MTL::Buffer, sized to the packed
// scene's actual byte length each frame -- see core/src/renderer.cpp -- not
// a fixed-size setFragmentBytes binding, so there is no node-count cap).
//
// pos_shape.xyz = world position; pos_shape.w = shape (0 = cube, 1 = sphere).
// half_extents_op.xyz = per-axis half extents (scale * 0.5);
// half_extents_op.w = op (0 = add, 1 = subtract).
typedef struct {
    sq_float4 pos_shape;
    sq_float4 half_extents_op;
} SdfNode;

// Compiled under both __METAL_VERSION__ (a future raymarch .metal TU) and
// plain C++ (core/tests), via the dual-compile typedefs above -- so this one
// assert covers both sides, matching shared_types.h's MeshVertex precedent.
static_assert(sizeof(SdfNode) == 32, "SdfNode must be 32 bytes");

// ---------------------------------------------------------------------------
// Per-shape local SDFs (both exact under identity rotation; see sdf_eval_node
// below for the world -> local transform).

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

// Evaluates one packed node's local SDF at world-space point p.
//
// Node rotation is always identity in the current MVP (see scene.h's Node),
// so this evaluates directly in the node's translation-only frame: q = p -
// node position. If/when rotation becomes editable, q must be rotated by the
// inverse rotation before this dispatch (and before sdf_sd_box/
// sdf_sd_ellipsoid above, which assume q already comes in axis-aligned with
// the shape).
inline float sdf_eval_node(SdfNode node, sq_float3 p) {
    const sq_float3 q = p - node.pos_shape.xyz;
    // NOT named `half`: that's a reserved MSL type keyword (the 16-bit float
    // type), and this header must compile under __METAL_VERSION__ too.
    const sq_float3 half_extents = node.half_extents_op.xyz;
    return (node.pos_shape.w == 0.0f) ? sdf_sd_box(q, half_extents) : sdf_sd_ellipsoid(q, half_extents);
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
