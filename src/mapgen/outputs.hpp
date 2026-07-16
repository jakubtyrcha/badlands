#pragma once

#include <string>

#include "mapgen/config.hpp"
#include "mapgen/field2d.hpp"
#include "mapgen/pipeline.hpp"
#include "mapgen/sections.hpp"

namespace badlands::mapgen {

// Dumps every debug raster + the section graph for one generated map into
// cfg.out_dir: elevation/moisture/ridged/fine (raw noise fields), voronoi,
// biome, heightmap, sections (.png) and sections.json -- i.e. the full set the
// offline tool has always produced.
//
// The caller is responsible for creating cfg.out_dir first (see
// std::filesystem::create_directories); a missing directory surfaces as
// per-file write failures.
void write_preview_images(const MapgenConfig& cfg, const MapArtifacts& a);

// Write a float field as an 8-bit grayscale PNG. If `normalize`, the field's
// [min,max] is stretched to [0,255]; otherwise values are clamped to [0,1].
void write_gray_png(const Field2D<float>& field, const std::string& path,
                    bool normalize = true);

// Write an int field as an RGBA PNG, colorizing each distinct id via a hash
// (debug view of voronoi cell ids / section ids).
void write_hashed_png(const Field2D<int>& field, const std::string& path);

// Write a per-pixel biome field (values are Biome) as an RGBA PNG using the
// fixed biome palette.
void write_biome_png(const Field2D<uint8_t>& biome, const std::string& path);

// Write the block grid as an RGBA PNG: each section a distinct hashed color,
// with section-boundary ledges drawn darker. Rendered at sample resolution
// (each block is kSamplesPerBlock px).
void write_sections_png(const Field2D<Block>& blocks, const std::string& path);

// Write the section graph (nodes + ledge edges) as JSON.
void write_section_graph_json(const SectionGraph& graph,
                              const std::string& path);

}  // namespace badlands::mapgen
