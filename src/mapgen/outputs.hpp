#pragma once

#include <string>
#include <string_view>

#include "mapgen/field2d.hpp"
#include "mapgen/generator.hpp"

namespace badlands::mapgen {

// Dumps the debug rasters for one generated map into out_dir: bedrock.png
// (normalized gray), biome.png (palette), heightmap.png, hillshade.png
// (relief-shaded heights — grayscale heightmaps are nearly unreadable for
// judging ridge structure by eye), water_depth.png / sediment.png (normalized
// gray), and flow.png (log2-scaled gray — drainage area spans orders of
// magnitude). `texel_m` is the horizontal sample spacing feeding the
// hillshade's slope computation.
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

// MapDebugSink that writes each dump as a numbered PNG into out_dir:
//   <NN>-<stage>.png for init/output stages (NN = sequence),
//   loop-<IIII>-<stage>.png for loop stages (IIII = iteration).
// Float fields named *-height / "cone" render as hillshade; "loop-flow"/"flow"
// render log2-scaled; other floats normalized gray. uint8 fields with stage
// "biome"/"biome-sim" use the biome palette; other masks render 0/255 gray.
class PngDebugSink final : public MapDebugSink {
 public:
  PngDebugSink(std::string out_dir, float texel_m);
  void dump(std::string_view stage, int sequence,
           const Field2D<float>& field) override;
  void dump(std::string_view stage, int sequence,
           const Field2D<uint8_t>& mask) override;

 private:
  std::string out_dir_;
  float texel_m_;
};

}  // namespace badlands::mapgen
