#pragma once

// The whole offline map generation, start to finish, in one call.
//
// Both consumers need the identical sequence: the preview-image dump and the
// interactive viewer (which turns the same artifacts into terrain chunks). It
// lives here so the two cannot drift — they used to run the pipeline separately
// (main_mapgen.cpp and MapViewView::Initialize), which is exactly the kind of
// duplication that lets a viewer silently disagree with its own preview PNGs.

#include <string>

#include "mapgen/biome_assign.hpp"
#include "mapgen/config.hpp"
#include "mapgen/fields.hpp"
#include "mapgen/field2d.hpp"
#include "mapgen/sections.hpp"
#include "mapgen/voronoi.hpp"

namespace badlands::mapgen {

// Everything one map generation produces, in dependency order. Held together
// because the later stages are only meaningful next to the earlier ones (e.g.
// `graph` indexes `blocks`'s section_ids).
struct MapArtifacts {
  Fields fields;             // raw noise fields (elevation/moisture/ridged/fine)
  Voronoi voronoi;           // pre-section cells
  BiomeMap biomes;           // per-sample biome
  Field2D<float> heightmap;  // composed relief, world meters
  Field2D<Block> blocks;     // kBlockSizeM grid; section_id filled by extract_sections
  SectionGraph graph;        // sections (terraces) + ledges between them.
                             // nodes[i].id == i, so a block's section height is
                             // graph.nodes[block.section_id].mean_height.
};

// Runs fields -> voronoi -> biomes -> heightmap -> blocks -> sections.
//
// `script_path` is the noiser field script (e.g. "scripts/mapgen/fields.noiser"),
// resolved relative to the CWD -- run from the repo root.
//
// Returns false with `err` set if field evaluation fails (the only stage that
// can fail; the rest are pure CPU transforms). `out` is untouched on failure.
bool run_pipeline(const MapgenConfig& cfg, const std::string& script_path,
                  MapArtifacts& out, std::string& err);

}  // namespace badlands::mapgen
