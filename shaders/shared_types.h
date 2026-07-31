#pragma once

// Structs shared between MSL and C++. Layout rule: only float, float4 and
// float4x4 fields allowed. float3 is banned: MSL gives it 16-byte
// size/alignment, which silently desyncs from the host layout.

#ifdef __METAL_VERSION__
#include <metal_stdlib>
typedef metal::float4   sq_float4;
typedef metal::float4x4 sq_float4x4;
#else
#include <simd/simd.h>
typedef simd_float4     sq_float4;
typedef simd_float4x4   sq_float4x4;
#endif

typedef struct {
    sq_float4 pos;   // xyz used
    sq_float4 color;
} LineVertex;

typedef struct {
    sq_float4x4 view_proj;
} LineUniforms;

typedef struct {
    sq_float4 pos;    // xyz used, w=1
    sq_float4 normal; // xyz used, w=0
} MeshVertex;

// Compiled under both __METAL_VERSION__ (mesh.metal) and plain C++ (core/tests),
// via the dual-compile typedefs above -- so this one assert covers both sides.
static_assert(sizeof(MeshVertex) == 32, "MeshVertex must be 32 bytes");

// Per-frame uniforms for the raymarch pass (shaders/raymarch.metal), uploaded
// via setFragmentBytes alongside the packed SdfNode array.
//
// DEVIATION FROM THE DESIGN SPEC (docs/superpowers/specs/2026-07-31-raymarch-
// rendering-layer-design.md §2, amended by this commit): the spec's original
// layout had a separate `float4 eye` field. R0 proved the ray origin is
// recoverable exactly from inv_view_proj alone (see sdf_ray_for_pixel's doc
// comment in sdf_scene.h -- column-2 of inv_view_proj, divided by its own w,
// yields eye with no separate input), so a stored eye field would be dead
// weight. params0/params1 replace it; counts/sizes ride as floats (this
// project's float4-only struct convention -- node_count <= 128 is
// float-exact).
//
// Byte count note: the task brief that specified this layout stated 192 B,
// but the four fields below (2 * float4x4 = 128 B, 2 * float4 = 32 B) sum to
// 160 B -- confirmed by the compiler and consistent with LineUniforms'
// pinned 64-byte single-float4x4 size elsewhere in this file. Asserting the
// actual computed size (160) rather than forcing 192 with an undocumented
// padding field.
typedef struct {
    sq_float4x4 view_proj;      // for fragment depth output
    sq_float4x4 inv_view_proj;  // for ray generation
    sq_float4   params0;        // (drawable_width_px, drawable_height_px, node_count, 0)
    sq_float4   params1;        // (near, far, 0, 0)
} RaymarchUniforms;

// Compiled under both __METAL_VERSION__ (raymarch.metal) and plain C++
// (core/tests) -- same precedent as MeshVertex above.
static_assert(sizeof(RaymarchUniforms) == 160, "RaymarchUniforms must be 160 bytes");
