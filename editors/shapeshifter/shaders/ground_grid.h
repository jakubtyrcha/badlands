#pragma once

// Ground-plate grid shading -- the single source of truth for the y=0
// orientation grid, shared by the GPU pass (shaders/ground_grid.metal) and
// the CPU tests (tests/core/ground_grid_tests.cpp).
//
// Dual-compile: included from both plain C++ and MSL, the same
// __METAL_VERSION__ idiom sdf_scene.h uses. Every decision the fragment
// shader makes about colour lives here; the .metal file only reconstructs
// the ray, intersects y=0, and writes depth. That split is deliberate -- it
// means the tiering, anti-aliasing and palette are all testable headlessly,
// with the screen-space derivative supplied as a plain argument instead of
// coming from fwidth().
//
// See docs/superpowers/specs/2026-08-03-viewport-orientation-aids-design.md.

#include "shared_types.h" // sq_float4

#ifdef __METAL_VERSION__
typedef metal::float2 sq_float2;
#define GG_CONST constant
#else
#include <cmath>
#include <simd/simd.h>
typedef simd_float2 sq_float2;
#define GG_CONST inline constexpr
#endif

// ---------------------------------------------------------------------------
// Palette. THIS IS THE ONLY DEFINITION of the world axis colours: the shader
// reads X/Z from here for the in-plane axis lines, and core/src/lines.cpp
// reads Y from here for the +Y origin marker (which is above the plane, so it
// has to be geometry rather than part of this pass). Desaturated on purpose
// -- full-saturation primaries at world scale read as a toy and fight the
// move gizmo for attention (user ruling).

GG_CONST sq_float4 kGroundAxisX = {0.878f, 0.337f, 0.384f, 1.0f};
GG_CONST sq_float4 kGroundAxisY = {0.486f, 0.808f, 0.478f, 1.0f};
GG_CONST sq_float4 kGroundAxisZ = {0.322f, 0.518f, 0.910f, 1.0f};
GG_CONST sq_float4 kGroundLineColor = {1.0f, 1.0f, 1.0f, 1.0f};

// Tier alphas. Each is the alpha that tier shows when it is the topmost layer
// at a pixel -- the front-to-back accumulation below guarantees that, so
// these numbers mean literally what they say (a major line is 0.36, not 0.36
// stacked on top of a minor line's 0.15).
GG_CONST float kGroundMinorAlpha    = 0.15f;
GG_CONST float kGroundMajorAlpha    = 0.36f;
GG_CONST float kGroundBorderAlpha   = 0.45f;
GG_CONST float kGroundAxisAlphaPos  = 0.60f;
GG_CONST float kGroundAxisAlphaNeg  = 0.24f;

// Plate geometry (user ruling: finite plate, fixed spacing, no LOD).
GG_CONST float kGroundHalfExtent   = 30.0f;
GG_CONST float kGroundMinorSpacing = 1.0f;
GG_CONST float kGroundMajorSpacing = 5.0f;

// Density fade thresholds, in grid cells per pixel. See gg_density_fade.
GG_CONST float kGroundFadeBegin = 0.20f; // lines ~5 px apart: still crisp
GG_CONST float kGroundFadeEnd   = 0.70f; // lines ~1.4 px apart: gone

// |ray.dir.y| below which the plate is faded out entirely. See gg_grazing_fade.
GG_CONST float kGroundGrazingLimit = 0.06f;

// Largest ray parameter the plate intersection may report. The plate is
// +/-30 and the camera radius clamps at 90, so anything past this is
// certainly off-plate; the point of the cap is that `t` stays finite for
// horizon rays rather than reaching fwidth() as an infinity.
GG_CONST float kGroundMaxRayT = 1.0e4f;

// ---------------------------------------------------------------------------
// Portable math toolkit, same pattern as sdf_scene.h's. Kept to exactly what
// this file needs. Vector/scalar arithmetic (+ - * /) is spelled directly:
// both simd_float2 and metal::float2 broadcast scalars, so no wrapper needed.

#ifdef __METAL_VERSION__
inline float     gg_abs(float v)                { return metal::abs(v); }
inline sq_float2 gg_abs2(sq_float2 v)           { return metal::abs(v); }
inline float     gg_min(float a, float b)       { return metal::min(a, b); }
inline float     gg_max(float a, float b)       { return metal::max(a, b); }
inline sq_float2 gg_max2(sq_float2 a, float b)  { return metal::max(a, sq_float2(b)); }
inline sq_float2 gg_fract2(sq_float2 v)         { return metal::fract(v); }
inline sq_float2 gg_make2(float x, float y)     { return sq_float2(x, y); }
inline sq_float4 gg_make4(float x, float y, float z, float w) { return sq_float4(x, y, z, w); }
#else
inline float     gg_abs(float v)                { return std::fabs(v); }
inline sq_float2 gg_abs2(sq_float2 v)           { return simd_abs(v); }
inline float     gg_min(float a, float b)       { return std::fmin(a, b); }
inline float     gg_max(float a, float b)       { return std::fmax(a, b); }
inline sq_float2 gg_max2(sq_float2 a, float b)  { return simd_max(a, simd_make_float2(b, b)); }
inline sq_float2 gg_fract2(sq_float2 v) {
    return simd_make_float2(v.x - std::floor(v.x), v.y - std::floor(v.y));
}
inline sq_float2 gg_make2(float x, float y)     { return simd_make_float2(x, y); }
inline sq_float4 gg_make4(float x, float y, float z, float w) { return simd_make_float4(x, y, z, w); }
#endif

inline float gg_saturate(float v) { return gg_min(gg_max(v, 0.0f), 1.0f); }

// ---------------------------------------------------------------------------

// Periodic tiers are anti-aliased by dividing the distance-to-line by the
// screen-space derivative, which gives a clean ~1px line at any spacing --
// but ONLY while the lines are further apart than a pixel. Once they pack
// tighter than that, the naive form saturates to coverage 1 everywhere and
// the tier reads as a solid white smear (the classic failure of a
// derivative-AA grid zoomed out). Fading the tier out by its own on-screen
// density is what lets the plate keep fixed 1-unit spacing across the whole
// 0.5..90 camera radius range with no LOD.
//
// Note this is anti-aliasing driven by pixel density, NOT the authored
// zoom-aware tier fading the user rejected: nothing appears or disappears as
// a function of camera distance per se, a tier just stops being drawn once it
// is no longer resolvable. In practice minors dissolve first and majors
// persist, because dg scales with 1/spacing.
inline float gg_density_fade(float dg) {
    return 1.0f - gg_saturate((dg - kGroundFadeBegin) / (kGroundFadeEnd - kGroundFadeBegin));
}

// Anti-aliased coverage, in [0,1], of the nearest line of a periodic grid of
// `spacing`, faded out once that grid is too dense to resolve. Working in
// grid space (q/spacing) makes the AA width one pixel regardless of spacing.
inline float gg_line_coverage(sq_float2 q, sq_float2 dq, float spacing) {
    const sq_float2 g  = q / spacing;
    const sq_float2 dg = gg_max2(dq / spacing, 1e-8f);
    const sq_float2 f  = gg_abs2(gg_fract2(g - 0.5f) - 0.5f); // distance to line, grid units
    const sq_float2 e  = f / dg;                              // ... in pixels
    const float cov = 1.0f - gg_min(gg_min(e.x, e.y), 1.0f);
    return cov * gg_density_fade(gg_max(dg.x, dg.y));
}

// Anti-aliased coverage of a single (non-periodic) line at distance `d`.
inline float gg_single_line_coverage(float d, float dp) {
    return 1.0f - gg_min(d / gg_max(dp, 1e-8f), 1.0f);
}

// Belt-and-braces fade for view rays nearly parallel to the plate. The
// density fade above already handles the honest case (the plate compresses
// toward the horizon, dg explodes, tiers dissolve), but derivatives get
// numerically nasty in exactly that region, so this damps the last sliver
// where a stray fwidth could still produce a bright band. `dir_y` is the
// view ray's Y component.
inline float gg_grazing_fade(float dir_y) {
    const float a = gg_saturate(gg_abs(dir_y) / kGroundGrazingLimit);
    return a * a * (3.0f - 2.0f * a); // smoothstep(0, kGroundGrazingLimit, |dir_y|)
}

// Front-to-back premultiplied accumulation: `acc += layer * (1 - acc.a)`.
// Compositing in this direction is what makes each tier's constant mean its
// literal alpha -- whatever is added first wins the pixel, and everything
// below is attenuated by exactly what has already been laid down. That is
// where "minor suppressed under major" comes from; there is no separate
// suppression term anywhere.
inline sq_float4 gg_accum(sq_float4 acc, sq_float4 color, float alpha) {
    const float a = gg_saturate(alpha) * (1.0f - acc.w);
    return gg_make4(acc.x + color.x * a, acc.y + color.y * a, acc.z + color.z * a, acc.w + a);
}

// Shades one point on the ground plate.
//
//   q  = the ground hit point's .xz (q.x is world X, q.y is world Z)
//   dq = fwidth(q) in the shader; a hand-supplied derivative in tests
//
// Returns PREMULTIPLIED rgba -- ground_pso_ blends One / OneMinusSourceAlpha
// to match. Alpha 0 means "nothing here", which the shader turns into a
// discard.
//
// Layers, composited front-to-back: +/-X axis, +/-Z axis, plate border,
// major tier, minor tier.
inline sq_float4 ground_grid_shade(sq_float2 q, sq_float2 dq, float half_extent,
                                    float minor_spacing, float major_spacing) {
    sq_float4 acc = gg_make4(0.0f, 0.0f, 0.0f, 0.0f);

    // Chebyshev radius -> a square plate rather than a disc.
    const float r  = gg_max(gg_abs(q.x), gg_abs(q.y));
    const float px = gg_max(dq.x, dq.y);

    // One pixel of slack past the edge so the border line itself is not
    // clipped in half by its own extent test.
    if (r > half_extent + px) {
        return acc;
    }

    const float inside = (r <= half_extent) ? 1.0f : 0.0f;

    // X axis is the line z == 0; its positive half is q.x > 0. Z axis is the
    // line x == 0; its positive half is q.y > 0. Drawing the negative halves
    // dimmer is what makes "which way is +X" answerable without a legend
    // (user ruling).
    const float cov_axis_x = inside * gg_single_line_coverage(gg_abs(q.y), dq.y);
    const float cov_axis_z = inside * gg_single_line_coverage(gg_abs(q.x), dq.x);
    const float cov_axis   = gg_max(cov_axis_x, cov_axis_z);

    const float cov_border = gg_single_line_coverage(gg_abs(r - half_extent), px);

    // Tiers coincide by construction: every major line sits on a minor line,
    // and both axes sit on both. Masking each tier by the RAW coverage of
    // everything above it is what keeps each alpha constant meaning literally
    // what it says -- without this a major line renders at
    // 0.36 + 0.15*(1-0.36) = 0.456 rather than 0.36, and the origin stacks
    // four tiers deep. The masks use raw coverage, not post-mask coverage: a
    // major line hides the minor beneath it whether or not the major is
    // itself hidden under an axis.
    const float raw_major = inside * gg_line_coverage(q, dq, major_spacing);
    const float raw_minor = inside * gg_line_coverage(q, dq, minor_spacing);
    const float mask_top  = (1.0f - cov_axis) * (1.0f - cov_border);

    const float cov_major = raw_major * mask_top;
    const float cov_minor = raw_minor * mask_top * (1.0f - raw_major);

    acc = gg_accum(acc, kGroundAxisX,
                   (q.x >= 0.0f ? kGroundAxisAlphaPos : kGroundAxisAlphaNeg) * cov_axis_x);
    acc = gg_accum(acc, kGroundAxisZ,
                   (q.y >= 0.0f ? kGroundAxisAlphaPos : kGroundAxisAlphaNeg) * cov_axis_z);
    acc = gg_accum(acc, kGroundLineColor, kGroundBorderAlpha * cov_border);
    acc = gg_accum(acc, kGroundLineColor, kGroundMajorAlpha * cov_major);
    acc = gg_accum(acc, kGroundLineColor, kGroundMinorAlpha * cov_minor);

    return acc;
}
