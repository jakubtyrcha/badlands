#pragma once

// Output-resolution low-pass over the final heightmap.
//
// The wrinkle it removes is overwhelmingly gully_detail_delta's — 4 octaves
// from 60 m down to 7.5 m at 2 m amplitude, covering every sloped texel. The
// eroded sim surface underneath is comparatively smooth. Smoothing the final
// result rather than reducing the detail filter's own parameters was a
// deliberate choice: it softens river valleys and shorelines too, and that was
// accepted.
//
// See docs/superpowers/specs/2026-07-29-mapgen-heightmap-smoothing-design.md.

#include "mapgen/field2d.hpp"

namespace badlands::mapgen {

// Separable Gaussian, then `mix(h, blurred, strength)`.
//
// `sigma_m` is in WORLD METRES, not texels, so the same parameters give the
// same terrain at any --resolution — the resolution-independence invariant the
// generator's units-guard test pins for kSlopeMPerM. The kernel is truncated
// at 3 sigma and edges clamp-extend (Field2D has no wrap semantics anywhere in
// mapgen).
//
// Returns `h` bit-identically when sigma_m <= 0 or strength <= 0.
Field2D<float> smooth_heightmap(const Field2D<float>& h, float texel_m,
                                float sigma_m, float strength);

}  // namespace badlands::mapgen
