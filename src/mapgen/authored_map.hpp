#pragma once

// Load an AUTHORED map — heights + biome labels supplied as image assets — instead of
// generating one.
//
// The procedural pipeline (pipeline.hpp) derives its heightmap from noise fields. An
// authored map skips all of that: the terrain IS the asset. Everything downstream of
// heights+biome is unchanged, because `reduce_to_blocks`/`extract_sections` never needed
// the noise intermediates (see sections.hpp) — so the two paths share one tail and
// cannot drift.
//
// The assets are produced by scripts/mapgen/map_from_render.py; map_meta.json is the
// contract between that script and this loader. Nothing here knows how the images were
// authored, only how to read them.

#include <string>

#include "mapgen/field2d.hpp"

namespace badlands::mapgen {

// map_meta.json. The map's dimensions and elevation span are properties of the ASSET,
// not of the CLI — a `--resolution` alongside `--map` would be a contradiction.
struct AuthoredMapMeta {
  int width = 0;
  int height = 0;
  float meters_per_sample = 1.0f;
  float height_min_m = 0.0f;  // the affine that decodes heights_*.png back to meters:
  float height_max_m = 0.0f;  //   h_m = height_min_m + v/65535*(height_max_m-height_min_m)
  float water_level_m = 0.0f;
  std::string heights_png;  // resolved paths, derived from `width`
  std::string biome_png;
};

// Read + validate `<dir>/map_meta.json`. Returns false with `err` set if the file is
// missing/unparseable, if a required key is absent, or if the declared geometry can't
// work: non-positive dims, dims not a multiple of kSamplesPerBlock (the remainder would
// silently fall off the block grid and out of the mesh), an inverted height range, or a
// meters_per_sample that disagrees with the engine's compile-time kMetersPerSample.
bool read_authored_meta(const std::string& dir, AuthoredMapMeta& out, std::string& err);

// Decode `<dir>/heights_<W>.png` into world meters via the meta's affine.
//
// Reads the PNG at 16 BITS (badlands_decode_image16), not through the usual RGBA8 path:
// 8 bits would quantize this map's ~279 m span to ~1.1 m steps and terrace the terrain.
// Fails if the image is missing, or if its dimensions disagree with the meta.
bool load_authored_heights(const AuthoredMapMeta& meta, Field2D<float>& out,
                           std::string& err);

// Decode `<dir>/biome_<W>.png` into raw Biome enum values.
//
// 8-bit greyscale holding label indices, NOT the debug palette that outputs.cpp writes —
// a label is data here, not a colour. Fails if the image is missing, if its dimensions
// disagree with the meta, or if any sample is >= kBiomeCount (which would index past the
// terrain texture array).
bool load_authored_biome(const AuthoredMapMeta& meta, Field2D<uint8_t>& out,
                         std::string& err);

}  // namespace badlands::mapgen
