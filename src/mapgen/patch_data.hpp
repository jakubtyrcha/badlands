#pragma once

// THE PATCH CONTRACT: what a caller asks for, and what it gets back.
//
// This is the one frozen interface in the procgen split. Stage 1 (the coarse
// hydraulic erosion sim, tools/protogen/) produces a big cached world; stage 3
// (map-detailing: the cluster-LOD terrain, materials, foliage, water surfaces)
// is a sink that populates the game world. Between them sits stage 2, and
// PatchRequest -> PatchData is the whole of it.
//
// See docs/superpowers/specs/2026-08-02-procgen-stage-split-design.md.
//
// WHAT IS DELIBERATELY NOT HERE:
//
//   - No arc chains. They are a deterministic function of (rivers, tolerance),
//     and carrying both the graph and its fit creates two truths that can
//     drift. Curvature is still available to every consumer -- RiverArc stores
//     it signed and exact -- via one build_river_arcs call, measured at 10 ms.
//   - No carved channel geometry. The median cavity is 0.34 m deep and 0.52 m
//     wide, sub-texel on a 1 m lattice, so it cannot be represented on this
//     grid at all. It stays an analytic field sampled by the terrain DAG.
//   - No coarse-world density, extent or provenance beyond `origin_m`. A patch
//     is a patch; what produced it is the provider's business.
//
// COORDINATES ARE PATCH-LOCAL. Texel (0, 0) sits at world (0, 0) and the patch
// spans [0, world_size_m]. `origin_m` is echoed for provenance only -- nothing
// downstream transforms by it. This is forced from two directions: every stage-3
// consumer already assumes a zero-based lattice, and absolute coordinates at a
// 16 km offset spend float precision on an offset the patch cannot use.
//
// UNITS ARE WORLD METRES, never texels, in every field and every parameter of
// every producer. That is what makes resolution a pure config change: the same
// (origin_m, world_size_m) at two resolutions is the same patch, sampled more
// finely.

#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

#include "mapgen/cover.hpp"
#include "mapgen/field2d.hpp"
#include "mapgen/lake.hpp"
#include "mapgen/river_network.hpp"
#include "mapgen/terrain_class.hpp"

namespace badlands::mapgen {

// What a caller asks for. `resolution` and `world_size_m` are INDEPENDENT:
// neither implies the other, and neither is a constant anywhere. 128 m at 128
// texels is an iteration patch; 2048 m at 2048 texels is the game's.
struct PatchRequest {
  glm::dvec2 origin_m{0.0};      // corner, in the COARSE world's frame
  float world_size_m = 0.0f;     // extent of the region
  int resolution = 0;            // texels per side
};

// Derived, never stored on the request -- one definition, so no two providers
// can disagree about what a request means. Returns 0 for a degenerate request.
inline float patch_texel_m(const PatchRequest& r) {
  return r.resolution > 0 ? r.world_size_m / static_cast<float>(r.resolution)
                          : 0.0f;
}

// Min/max ground elevation over the patch, in metres.
//
// Load-bearing, not diagnostic: stage 3 needs it to frame a camera. The map
// view pins its focus at y = 0 with a fixed pitch, so the view ray crosses the
// terrain band at a constant multiple of the elevation away from the patch
// centre -- which means a patch narrower than roughly 0.84x its own elevation
// is never intersected and renders as an empty frame. A patch sitting at 440 m
// needs 740 m of width before it can be seen at all. Carrying the range is what
// lets the camera focus on the terrain instead of on a plane it does not touch.
struct ElevationRange {
  float min_m = 0.0f;
  float max_m = 0.0f;
};

// Over the whole raster. Empty input gives {0, 0}.
ElevationRange compute_elevation_range(const Field2D<float>& height);

// What a caller gets back. Every provider fills all of it.
struct PatchData {
  float texel_m = 0.0f;
  glm::dvec2 origin_m{0.0};  // provenance only; the lattice is zero-based

  // THE BED, never the water surface, so a basin stays a basin.
  Field2D<float> height;

  // Water rides in the LEVEL raster, not a depth field:
  //     depth = max(0, level - height)
  // and a dry texel stores `level == height`. That needs no sentinel and no
  // separate mask, and it makes a lake surface exactly flat by construction --
  // the level is the per-lake constant, so no amount of bed detail can tilt it.
  Field2D<float> level;

  // The derived form, carried rather than recomputed by each consumer. A
  // provider may derive these (see derive_water) or author them outright; the
  // contract says a patch HAS water, not where the water came from.
  Field2D<float> water_depth;
  Field2D<int32_t> lake_id;  // index into `lakes`, -1 where dry
  std::vector<LakeInfo> lakes;

  // WHAT GROWS HERE (mapgen::Cover), not what the ground is made of and not
  // what the sim's movement rules read. mapgen::Biome is a GAMEPLAY vocabulary
  // -- walkability, move cost, habitat, animal spawning, frozen across the C
  // ABI -- and deliberately does not appear in this contract.
  //
  // Ground material is NOT indexed off this. It is derived from slope,
  // curvature and soil at the heightfield's own resolution, so material edges
  // are not pinned to the cover source's much coarser lattice. That is also why
  // Cover is under no 8-value cap, unlike the biome palette it replaced.
  Field2D<uint8_t> cover;

  // Erodible cover over bedrock. Required, not optional: it is the physically
  // honest signal for how dissected a slope should look -- thin soil means bare
  // rock -- which is what a later relief pass fades on. Measured on the 16 km
  // world, bare/thin soil averages 31.6 deg of slope against 7.1 deg for deep
  // soil, a 4.4x separation that falls out of the physics rather than being
  // imposed on it.
  Field2D<float> soil;

  // Clipped to this patch and culled to what is worth drawing, in patch-local
  // world metres.
  RiverGraph rivers;

  // HOW THIS GROUND WAS CARVED, and therefore which rock and ground materials
  // to paint with and which rock props to scatter. Low-frequency by nature, and
  // per-PATCH rather than per-texel only because that is what the sources emit
  // today; a raster replaces it without changing what it means.
  //
  // Elevation-derived classes are deliberately absent from the whole contract.
  // "Hills" and "Mountain" are recoverable from `height`, so a label carrying
  // them carries nothing a consumer could not already compute.
  TerrainClass terrain_class = TerrainClass::Unknown;

  ElevationRange elevation_range;
};

}  // namespace badlands::mapgen
