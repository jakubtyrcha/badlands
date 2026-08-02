#pragma once

// The on-disk form of the coarse (protogen) artifact's manifest: a whole-world
// dump's geometry and provenance, so its 16 m cell size is not a fact that
// exists nowhere but a --res/--world flag on the command line.
//
// Copies patch_io.hpp's map.txt idiom VERBATIM in style: "key value" per
// line in `world.txt`, `#` comments skipped, unknown keys ignored for
// forward compatibility, std::optional<T> + std::string* error, no version
// field. See patch_io.hpp for the same contract over a patch.

#include <cstdint>
#include <optional>
#include <string>

namespace badlands::mapgen {

// What world.txt carries.
struct CoarseManifest {
  int resolution = 0;
  float world_size_m = 0.0f;
  // DERIVED = world_size_m / resolution. Written to disk purely for OTHER
  // readers (numpy, the protogen python tooling) so they do not have to
  // divide; load_coarse_manifest recomputes it from resolution/world_size_m
  // rather than trusting the on-disk value, so there remains exactly one
  // source of truth for it.
  float texel_m = 0.0f;
  uint32_t seed = 0;
  float runoff_m_per_yr = 0.0f;
  int steps = 0;
  // Whole-world soil quantiles (the same fractions window.cpp's
  // kMountainFrac/kHillsFrac cut with), so a patch cut later from anywhere in
  // the world classifies biomes the same way regardless of what was cut. Two
  // floats instead of a raster.
  float soil_cut_mountain_m = 0.0f;
  float soil_cut_hills_m = 0.0f;
};

// Reads <dir>/world.txt. Returns nullopt and writes the reason to `error` if
// the file is missing or `resolution`/`world_size_m` is absent or
// unparseable. Unknown keys are ignored, so a writer may add provenance
// fields without breaking older readers.
std::optional<CoarseManifest> load_coarse_manifest(const std::string& dir,
                                                    std::string* error = nullptr);

// Writes <dir>/world.txt, creating `dir` if needed.
bool write_coarse_manifest(const std::string& dir, const CoarseManifest& m,
                           std::string* error = nullptr);

}  // namespace badlands::mapgen
