#pragma once

// The on-disk form of a patch: headerless rasters plus a key/value manifest.
//
// Deliberately dumb, so numpy and the protogen python tooling can read and write
// it without a parser. The manifest is what makes the headerless rasters safe:
// element counts are checked against it, and a mismatch is an error rather than
// a silent misread.
//
//   patchdir/
//     map.txt        manifest, "key value" per line
//     height.f32     float32 metres, row-major, resolution^2 -- the BED
//     level.f32      float32 metres, LAKE SURFACE elevation
//     biome.u8       uint8, Biome values
//     soil.f32       float32 metres of erodible cover (see below)
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
//
// SOIL IS REQUIRED BY THE CONTRACT BUT OPTIONAL ON DISK. PatchData::soil is
// always present and correctly sized; a directory written before the two-layer
// substrate simply loads it as zeros. A present-but-malformed file is still an
// error -- only absence is tolerated, which is the same forward-compatibility
// rule the manifest applies to unknown keys.
//
// RIVERS ARE NOT IN THIS FORM YET. load_patch leaves PatchData::rivers empty and
// write_patch does not emit it; the graph gets its own serialization alongside
// the coarse artifact. A provider that needs rivers from a directory derives
// them (see file_patch_source.hpp) until then.

#include <optional>
#include <string>

#include <glm/glm.hpp>

#include "mapgen/patch_data.hpp"

namespace badlands::mapgen {

// What map.txt carries. `source` is provenance only -- nothing reads it back.
struct PatchManifest {
  int resolution = 0;
  float world_size_m = 0.0f;
  glm::dvec2 origin_m{0.0};  // optional key; absent means the origin is unknown
  std::string source;
};

// Reads <dir>/map.txt. Returns nullopt and writes the reason to `error` if the
// file is missing or a required key is absent or unparseable.
std::optional<PatchManifest> load_patch_manifest(const std::string& dir,
                                                 std::string* error = nullptr);

// Reads the manifest and the rasters into a PatchData. `water_depth`, `lake_id`
// and `lakes` are DERIVED here from height + level; `rivers` is left empty.
//
// Returns nullopt with a reason in `error` on a missing file, a size that
// contradicts the manifest, or a non-finite sample.
std::optional<PatchData> load_patch(const std::string& dir,
                                    std::string* error = nullptr);

// Writes the manifest and the four rasters. `source` is recorded verbatim as
// provenance. The derived water block is NOT written -- it is reproduced exactly
// by load_patch, and storing it would create a second truth that can drift.
bool write_patch(const std::string& dir, const PatchData& patch,
                 const std::string& source, std::string* error = nullptr);

// --- exposed for tests and for providers that author water directly ---------

// Derives water_depth / lake_id / lakes from a bed and a level raster, applying
// the depth = max(0, level - height) convention. A lake is a 4-connected run of
// wet texels; its level_m is taken from the level raster (constant across the
// component by construction, so the first member is representative).
void derive_water(const Field2D<float>& heightmap, const Field2D<float>& level,
                  float texel_m, Field2D<float>& water_depth,
                  Field2D<int32_t>& lake_id, std::vector<LakeInfo>& lakes);

}  // namespace badlands::mapgen
