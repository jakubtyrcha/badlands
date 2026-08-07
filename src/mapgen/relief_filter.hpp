#pragma once

// The stage-2 relief DETAIL filter -- the deferred §3.4 pass of the procgen
// stage split (docs/superpowers/specs/2026-08-06-stage2-relief-filter-design.md).
//
// A stateless per-point function implementing stacked faded gullies, after
// Rune Skovbo Johansen's "Fast and Gorgeous Erosion Filter" (source note and
// license in relief_filter.cpp -- the implementation is a PROTOTYPE port and
// will be replaced): phasor-blended oriented stripe octaves, orientation
// accumulated per octave (branching), fade-toward-target with a stacked
// ridge/crease-protecting mask, modulated by the physical signals (soil,
// biome) and masked off standing water. It returns a DELTA to add onto the
// Catmull-Rom resample -- the raster path stays the single source of the base
// surface, and filter-off output is bit-identical to the plain resample.
//
// Everything is world METRES and a pure function of (seed, world position):
// two patches evaluate identical detail at identical world positions, so
// borders agree by construction; resolution only changes which octaves are
// audible (an octave is active only while its wavelength clears the OUTPUT
// Nyquist), never where the detail sits.

#include <cstdint>

#include <glm/glm.hpp>

#include "mapgen/field2d.hpp"

namespace badlands::mapgen {

// Const views into the COARSE world (the provider's whole-world rasters, all
// at src_texel_m). Pointers rather than references so the struct stays
// aggregate-initializable; every field is required.
struct ReliefContext {
  const Field2D<float>* bed = nullptr;          // the coarse BED, metres
  const Field2D<float>* soil = nullptr;         // erodible cover, metres
  const Field2D<uint8_t>* biome = nullptr;      // mapgen::Biome values
  const Field2D<float>* water_depth = nullptr;  // standing water depth, metres
  float src_texel_m = 0.0f;
  uint32_t seed = 0;
};

struct ReliefSample {
  float delta_m = 0.0f;   // detail height to ADD onto the resampled bed
  // APPROXIMATE d(delta)/d(world metres): carries the faded wave terms only,
  // not the mask/target spatial derivatives -- the same approximation the
  // reference makes. Good enough for shading-like consumers; nothing
  // downstream needs better today.
  glm::vec2 grad{0.0f};
};

// The per-point filter. `out_texel_m` is the requested output density -- it
// gates which octaves are active (wavelength >= 2*out_texel_m) and nothing
// else, so it never moves detail, only reveals it.
ReliefSample sample_relief_delta(const ReliefContext& ctx,
                                 glm::dvec2 world_pos_m, float out_texel_m);

// Raster driver: adds the delta to every texel of `height_inout` (texel j at
// world origin_m + j*out_texel_m, node registration). Tiled across threads;
// results are independent of thread count.
void apply_relief(const ReliefContext& ctx, glm::dvec2 origin_m,
                  float out_texel_m, Field2D<float>& height_inout);

}  // namespace badlands::mapgen
