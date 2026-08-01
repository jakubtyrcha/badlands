#pragma once

// Loads a map from raw rasters on disk into the same MapArtifacts that
// generate_map produces, so a map simulated OUTSIDE this process renders through
// the identical path.
//
// The on-disk form is deliberately dumb -- three headerless rasters plus a
// key/value manifest -- so numpy and the protogen python tooling can read and
// write it without a parser. The manifest is what makes the headerless rasters
// safe: element counts are checked against it, and a mismatch is an error rather
// than a silent misread.
//
//   mapdir/
//     map.txt        manifest, "key value" per line
//     height.f32     float32 metres, row-major, resolution^2 -- the BED
//     biome.u8       uint8, Biome values
//     level.f32      float32 metres, LAKE SURFACE elevation
//     soil.f32       float32 metres of erodible cover over bedrock (OPTIONAL --
//                    it postdates the two-layer substrate; absent is fine, and
//                    loads with MapArtifacts::sediment left empty)
//
// THE LEVEL RASTER CARRIES THE WATER, not a depth field:
//
//     depth = max(0, level - height)
//
// and a dry texel simply stores `level == height`. That needs no sentinel and no
// separate mask, and it makes a lake surface exactly flat by construction --
// the level is the per-lake constant, so no amount of bed detail can tilt it.
// LakeInfo::level_m then loads directly instead of being averaged back out of a
// depth field.

#include <optional>
#include <string>

#include "mapgen/generator.hpp"

namespace badlands::mapgen {

// What map.txt carries. `source` is provenance only -- nothing reads it back.
struct MapManifest {
  int resolution = 0;
  float world_size_m = 0.0f;
  std::string source;
};

// Reads <dir>/map.txt. Returns nullopt and writes the reason to `error` if the
// file is missing or a required key is absent or unparseable.
std::optional<MapManifest> load_manifest(const std::string& dir,
                                         std::string* error = nullptr);

// Reads the manifest and all three rasters. `lake_id` and `lakes` are DERIVED
// here by connected components over the wet mask -- BuildLakeSurfaceTriangles
// needs both, and deriving them keeps the on-disk form to three rasters.
// `bedrock` is set to the heightmap: mapview reads it only for its dimensions.
//
// Returns nullopt with a reason in `error` on a missing file, a size that
// contradicts the manifest, or a non-finite sample.
std::optional<MapArtifacts> load_map(const std::string& dir,
                                     std::string* error = nullptr);

// --- exposed for unit tests ---

// Derives water_depth / lake_id / lakes from a bed and a level raster, applying
// the depth = max(0, level - height) convention. A lake is a 4-connected run of
// wet texels; its level_m is taken from the level raster (constant across the
// component by construction, so the first member is representative).
void derive_water(const Field2D<float>& heightmap, const Field2D<float>& level,
                  float texel_m, Field2D<float>& water_depth,
                  Field2D<int32_t>& lake_id, std::vector<LakeInfo>& lakes);

}  // namespace badlands::mapgen
