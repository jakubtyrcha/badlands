#pragma once

#include <string>

#include "mapgen/field2d.hpp"
#include "mapgen/generator.hpp"

namespace badlands::mapgen {

// Dumps the debug rasters for one generated map into out_dir: bedrock.png
// (normalized gray), biome.png (palette), heightmap.png, and hillshade.png
// (relief-shaded heights — grayscale heightmaps are nearly unreadable for
// judging ridge structure by eye). `texel_m` is the horizontal sample
// spacing feeding the hillshade's slope computation.
//
// The caller is responsible for creating out_dir first (see
// std::filesystem::create_directories); a missing directory surfaces as
// per-file write failures.
void write_preview_images(const std::string& out_dir, const MapArtifacts& a,
                          float texel_m);

// Write a float field as an 8-bit grayscale PNG. If `normalize`, the field's
// [min,max] is stretched to [0,255]; otherwise values are clamped to [0,1].
//
// NOTE: `normalize = true` autoscales PER IMAGE, so two images written this way
// are NOT comparable to each other — the same grey means a different value in
// each. Use the explicit-range overload below when images are meant to be
// compared.
void write_gray_png(const Field2D<float>& field, const std::string& path,
                    bool normalize = true);

// Write a float field as grayscale with an EXPLICIT value range: `lo` maps to
// black, `hi` to white, out-of-range clamps. Use this to render several fields
// against one shared range so their greys mean the same thing.
void write_gray_png_range(const Field2D<float>& field, const std::string& path,
                          float lo, float hi);

// Write a per-pixel biome field (values are Biome) as an RGBA PNG using the
// fixed biome palette.
void write_biome_png(const Field2D<uint8_t>& biome, const std::string& path);

}  // namespace badlands::mapgen
