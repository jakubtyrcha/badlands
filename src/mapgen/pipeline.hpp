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

// Per-stage wall-clock timings (milliseconds), filled when a caller passes a
// PipelineTimings* to run_pipeline. Presentation is the caller's job -- mapgen
// stays a pure library and does not log.
struct PipelineTimings {
  double fields_ms = 0.0;
  double voronoi_ms = 0.0;
  double biomes_ms = 0.0;
  double heightmap_ms = 0.0;
  double blocks_ms = 0.0;
  double sections_ms = 0.0;
  double total_ms = 0.0;
};

// Runs fields -> voronoi -> biomes -> heightmap -> blocks -> sections.
//
// `script_path` is the noiser field script (e.g. "scripts/mapgen/fields.noiser"),
// resolved relative to the CWD -- run from the repo root.
//
// Returns false with `err` set if field evaluation fails (the only stage that
// can fail; the rest are pure CPU transforms). `out` is untouched on failure.
//
// If `timings` is non-null it receives the per-stage durations (only fully
// populated on success; `fields_ms` is set even on failure).
bool run_pipeline(const MapgenConfig& cfg, const std::string& script_path,
                  MapArtifacts& out, std::string& err,
                  PipelineTimings* timings = nullptr);

// The AUTHORED alternative: read heights + biome from image assets in `map_dir`
// (see authored_map.hpp), then run the identical blocks -> sections tail.
//
// `cfg.width`/`cfg.height` are OVERWRITTEN from the asset's map_meta.json -- the
// map's extent is a property of the images, not of the CLI. Everything else in
// `cfg` (reduce_median, section_step_m, ...) still applies.
//
// `fields`, `voronoi` and `biomes.cell_biome` are left default-constructed: they
// are procedural intermediates with no consumer past the heightmap. Callers that
// dump them (write_preview_images) must skip them for an authored map.
//
// Returns false with `err` set if the metadata or either image is missing,
// malformed, or disagrees with the other. `out` is untouched on failure.
bool run_authored_pipeline(MapgenConfig& cfg, const std::string& map_dir,
                           MapArtifacts& out, std::string& err);

}  // namespace badlands::mapgen
