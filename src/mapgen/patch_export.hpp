#pragma once

// Image ENCODINGS of the frozen PatchData contract -- another serialization
// alongside patch_io.hpp's raster dump, for looking at a stage-2 patch offline.
//
// These functions return PIXEL BUFFERS and nothing else: no PNG, no file I/O, no
// `assets` crate, no engine. That split is deliberate and load-bearing.
// src/mapgen/outputs.cpp and hillshade.cpp once encoded AND wrote PNGs, which
// made badlands_mapgen_lib link badlands_engine for CpuImage; both were deleted
// for it (see the note at CMakeLists.txt's badlands_mapgen_lib). Keeping the
// encoders pure means they unit-test with no Rust crate and no GPU, and
// src/mapgen/ stays bare-buildable for tools/protogen/. The PNG write lives in
// the one tool that wants it (src/executables/patch_export/).
//
// Buffers are tightly packed and row-major, matching Field2D: RGBA8 is
// width * height * 4 bytes, no row padding -- exactly what badlands_write_png
// expects.

#include <cstdint>
#include <vector>

#include "mapgen/field2d.hpp"
#include "mapgen/patch_data.hpp"

namespace badlands::mapgen {

// The metre<->code mapping for an export. Recorded in the tool's sidecar so a
// consumer can decode back to metres -- an autoscaled image whose range is not
// written down is a picture, not data.
struct ExportRange {
  float lo_m = 0.0f;
  float hi_m = 0.0f;
};

// Full-scale depth for the water channel and the hillshade's water tint. FIXED,
// never derived from a run's own maximum: blue has to mean the same depth in
// every image ever exported, or two runs cannot be read against each other.
// Height has no such constant available -- worlds differ by hundreds of metres,
// so its range is per-image and recorded instead.
inline constexpr float kExportWaterDepthM = 8.0f;

// Height as grayscale with the WATERMAP LAYERED ON TOP in the blue channel:
//
//   R = G = height code, linear over [range.lo_m, range.hi_m], clamped
//   B     = min(255, height code + water code), water linear over [0, water_max_m]
//   A     = 255
//
// Dry ground reads as neutral gray; water reads blue in proportion to depth. The
// water code is recovered exactly as B - R wherever their sum did not saturate
// (saturation clamps, it never wraps).
//
// 8 bits is a CHOICE, not an oversight: 0.16 m per code step over a 256 m window
// is enough for a preview, and the 16-bit writer in the assets crate is
// single-channel, so water could not share the file.
//
// A degenerate range (hi_m <= lo_m) yields flat mid-gray rather than dividing by
// zero. `water_depth` is treated as all-dry when empty or mismatched in size, so
// a patch without a derived water block still encodes.
std::vector<uint8_t> encode_height_water_rgba(const Field2D<float>& height,
                                              const Field2D<float>& water_depth,
                                              ExportRange range,
                                              float water_max_m);

// Cover ids through kCoverPalette (cover.hpp) -- the SAME palette the cluster
// terrain's per-vertex colour uses, so an exported cover map matches the 3D view
// byte for byte. Out-of-range ids clamp to opaque black rather than indexing
// past the palette.
std::vector<uint8_t> encode_cover_rgba(const Field2D<uint8_t>& cover);

// The judgement image: Lambert shading on the height gradient (fixed NW sun at
// 45 degrees, ambient floor), flat water tinted by depth, and river centrelines
// drawn from the graph's polylines. A raw heightmap does not read as terrain --
// gully and ridge quality only shows under a light.
std::vector<uint8_t> encode_hillshade_rgba(const PatchData& patch);

}  // namespace badlands::mapgen
