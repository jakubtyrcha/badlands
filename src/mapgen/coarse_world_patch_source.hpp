#pragma once

// CoarseWorldPatchSource -- the REAL provider: cuts an arbitrary patch out of
// a whole coarse (protogen) world sitting on disk (world.txt + the `<tag>-`
// snapshot rasters + rivers.bin, all written by tools/protogen/).
//
// Unlike FilePatchSource, which merely echoes a directory that already IS one
// patch, this source answers ANY PatchRequest inside the coarse world's
// extent: a different origin, a different resolution, a different world_size_m
// than whatever the sim happened to run at. That is the whole point of
// splitting stage 1 (the coarse hydraulic sim) from stage 2 (this) from stage
// 3 (map-detailing) -- the coarse artifact is simulated once, and every patch
// after that is a cheap resample.
//
// RESOLUTION- AND DENSITY-INDEPENDENCE. Fetch must give the same answer (up to
// resample error) for the same (origin_m, world_size_m) regardless of the
// request's resolution, and regardless of how densely the coarse world itself
// was simulated -- so nothing here may hardcode the coarse cell size. It comes
// from the manifest (CoarseManifest::texel_m), every time.
//
// Construction is where failure is reported (a missing/malformed manifest, a
// raster whose size contradicts it, a malformed rivers.bin). By the time a
// CoarseWorldPatchSource exists, it works.
//
// FETCH NEVER FAILS, AND THIS IS A DELIBERATE NARROWING of what PatchSource
// allows. That interface says a source unable to honour a request may return a
// default-constructed PatchData, which `empty()` detects. This source does not
// use that latitude: a request reaching past the world's edge resamples off the
// clamped rasters rather than returning nothing, because a partly-outside
// request is a caller bug and a visibly edge-smeared patch says so louder than
// an empty one. `empty()` therefore only ever reports a DEGENERATE request here
// (resolution <= 0, or a non-positive extent).
//
// The consequence worth knowing: a badly mistyped --patch-origin renders a
// plausible-looking patch rather than erroring. Range validation belongs to the
// caller; this source has no opinion about where a world "should" be sampled.

#include <memory>
#include <string>

#include "mapgen/coarse_io.hpp"
#include "mapgen/patch_source.hpp"

namespace badlands::mapgen {

class CoarseWorldPatchSource final : public PatchSource {
 public:
  PatchData Fetch(const PatchRequest& req) const override;

  // The manifest this world loaded from -- resolution, world_size_m, texel_m,
  // and the whole-world soil quantiles Fetch classifies biomes against.
  const CoarseManifest& manifest() const { return manifest_; }

 private:
  friend std::unique_ptr<CoarseWorldPatchSource> LoadCoarseWorldPatchSource(
      const std::string&, const std::string&, std::string*);

  CoarseManifest manifest_;
  // Whole-world rasters at manifest_.resolution^2, world metres, texel (0,0)
  // at world (0,0) -- the coarse world's own frame, NOT patch-local (that
  // rebasing happens per request, in Fetch).
  Field2D<float> height_;       // the BED
  Field2D<float> water_depth_;  // standing water DEPTH (protogen's `water`
                                // raster; see tools/protogen/protogen.cpp's
                                // Dump -- it is a depth, not a surface)
  Field2D<float> soil_;         // erodible cover
  RiverGraph rivers_;           // whole-world graph, world coordinates
};

// Loads `dir`: world.txt (mapgen/coarse_io.hpp), the `<tag>-{height,water,
// soil}.f32` snapshot rasters, and rivers.bin (mapgen/river_io.hpp). An empty
// `tag` selects the lexicographically LAST `*-height.f32` in the directory --
// tags are zero-padded step counts ("0060-step"), so lexicographic order is
// numeric order and the last one is the final step. Returns nullptr with a
// reason in `error` on any of that failing.
std::unique_ptr<CoarseWorldPatchSource> LoadCoarseWorldPatchSource(
    const std::string& dir, const std::string& tag = {},
    std::string* error = nullptr);

}  // namespace badlands::mapgen
