#pragma once

// Structs shared between Slang and C++. Layout rule: only float, float4
// and float4x4 fields allowed. float3 is banned: MSL gives it 16-byte
// size/alignment, which silently desyncs from the host layout. Slang agrees
// with MSL here, so the rule outlived the MSL arm that motivated it.
//
// The Slang arm is why the shaders need no mirrored struct declarations: a
// .slang file includes this header and gets the same types the host packs,
// rather than a hand-copy that agrees until someone edits one side.

#if defined(SDF_SLANG)
typedef float4          sq_float4;
typedef float4x4        sq_float4x4;
#else
#include <simd/simd.h>
typedef simd_float4     sq_float4;
typedef simd_float4x4   sq_float4x4;
#endif

// Slang has no `sizeof` and cannot parse a static_assert built on one. What
// these asserts pin is a CPU<->GPU layout contract, so the CPU asserting it is
// what matters -- the shader cannot disagree about a struct it takes from this
// same header. Empty on the Slang arm.
#if defined(SDF_SLANG)
#define SDF_STATIC_ASSERT(cond, msg)
#else
#define SDF_STATIC_ASSERT(cond, msg) static_assert(cond, msg)
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

// Compiled under both SDF_SLANG (mesh.slang) and plain C++ (core/tests) via the
// typedefs above -- so this one assert covers both sides.
SDF_STATIC_ASSERT(sizeof(MeshVertex) == 32, "MeshVertex must be 32 bytes");

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

// Compiled under both SDF_SLANG (raymarch.slang) and plain C++ (core/tests) --
// same precedent as MeshVertex above.
SDF_STATIC_ASSERT(sizeof(RaymarchUniforms) == 160, "RaymarchUniforms must be 160 bytes");

// Per-frame uniforms for the ground-plate pass (shaders/ground_grid.metal),
// uploaded via setFragmentBytes. Same shape and same reasoning as
// RaymarchUniforms above: inv_view_proj drives sdf_ray_for_pixel (no separate
// eye field needed -- it is recoverable from column 2), view_proj drives the
// fragment's depth output, and the scalars ride in float4s per this file's
// float4-only convention.
typedef struct {
    sq_float4x4 view_proj;      // for fragment depth output
    sq_float4x4 inv_view_proj;  // for ray generation
    sq_float4   params0;        // (drawable_width_px, drawable_height_px, half_extent, 0)
    sq_float4   params1;        // (minor_spacing, major_spacing, 0, 0)
} GroundGridUniforms;

SDF_STATIC_ASSERT(sizeof(GroundGridUniforms) == 160, "GroundGridUniforms must be 160 bytes");
