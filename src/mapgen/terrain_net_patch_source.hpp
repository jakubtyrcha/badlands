#pragma once

// A patch read from a terrain-net bundle: real bare-earth LiDAR at 1 m or
// finer, plus co-registered ESA WorldCover land cover.
//
// WHY THIS EXISTS. Procgen stages 1 and 2 are set aside pending an ML upscaler,
// so nothing in the pipeline produces terrain with detail below the 16 m coarse
// cell -- and every rendering-quality question is being asked of terrain that
// cannot answer it. These bundles carry the morphology the model is expected to
// produce, measured rather than invented, so the rendering path has something
// honest to be judged against today. See
// docs/superpowers/specs/2026-08-07-detailed-patch-rendering-design.md.
//
// LIKE FilePatchSource, THIS IGNORES THE REQUEST'S GEOMETRY. A bundle is a
// finished survey of one place at one resolution; there is nothing in it from
// which a different cut could be made. `native_request()` reports what it holds.
//
// WHAT THE BUNDLE HAS, AND WHAT IS STOOD IN FOR:
//
//   height        real, and the whole point         height.r32
//   cover         real, though 10 m and blocky      landcover.r8
//   terrain_class real, one label for the area      manifest.json
//   level, lakes  DERIVED (standing_water.hpp)      from height + cover
//   soil          ESTIMATED, temporary              from slope
//   rivers        EMPTY -- deferred, see standing_water.hpp
//
// It reads the `--format raw` side of a bundle only: three flat little-endian
// arrays plus a JSON sidecar. Nothing here parses a GeoTIFF, which is what
// keeps the dependency list at "a JSON parser".

#include <memory>
#include <string>

#include "mapgen/patch_source.hpp"

namespace badlands::mapgen {

// Refuse a bundle whose survey missed more than this share of its DRY LAND.
//
// The fill (nodata_fill.hpp) extends the nearest valid ground into a void, which
// is right for the scattered metre-wide dropouts an England DTM is full of and
// wrong for a bundle that is mostly hole -- there the "terrain" would be an
// extrapolation artifact wearing a survey's name. The fetched bundles measure
// 100.0% valid, so this only ever fires on something genuinely broken.
//
// DRY LAND, not the whole raster: a hole over WATER is expected, not broken. No
// survey measures a lake bottom and several programmes leave open water as
// outright nodata, so a whole-raster limit would refuse a good lake-heavy patch
// for the offence of containing a lake.
inline constexpr float kMaxNodataFraction = 0.05f;

class TerrainNetPatchSource final : public PatchSource {
 public:
  // Ignores `req` -- see the header note. Cheap: the work happened at load.
  PatchData Fetch(const PatchRequest& req) const override;

  // The geometry this bundle actually holds.
  const PatchRequest& native_request() const { return native_; }

  // Area key and name from manifest.json, for logging. Provenance only.
  const std::string& area() const { return area_; }

  // Share of the DRY-LAND texels that were nodata and had to be filled. Water
  // is excluded -- see kMaxNodataFraction.
  float nodata_fraction() const { return nodata_fraction_; }

  // The morphology label, without going through Fetch -- which returns a whole
  // PatchData BY VALUE and so copies every raster (~25 MB at 1072^2) for what
  // is usually one log line.
  TerrainClass terrain_class() const;

 private:
  friend std::unique_ptr<TerrainNetPatchSource> LoadTerrainNetPatchSource(
      const std::string&, std::string*);
  PatchData patch_;
  PatchRequest native_;
  std::string area_;
  float nodata_fraction_ = 0.0f;
};

// True when `dir` looks like a terrain-net bundle, i.e. holds a raw.json. Used
// by the map tool to tell the three directory kinds apart without a flag.
bool IsTerrainNetBundle(const std::string& dir);

// Reads `dir` and derives everything the contract needs that the bundle does
// not carry. Returns nullptr with a reason in `error` on a missing or malformed
// sidecar, a raster whose size contradicts it, a bundle written without land
// cover, or a nodata fraction past kMaxNodataFraction.
std::unique_ptr<TerrainNetPatchSource> LoadTerrainNetPatchSource(
    const std::string& dir, std::string* error = nullptr);

}  // namespace badlands::mapgen
