#include "mapgen/pipeline.hpp"

#include <chrono>

#include "mapgen/authored_map.hpp"
#include "mapgen/heightmap.hpp"

namespace badlands::mapgen {

namespace {

// Everything from heights+biome onward. Shared by both entry points on purpose: it is
// the whole reason an authored map needs no bespoke downstream — blocks depend on
// nothing but these two fields.
void reduce_to_block_grid(const MapgenConfig& cfg, MapArtifacts& a) {
  a.blocks = reduce_to_blocks(a.heightmap, a.biomes.pixel, cfg.reduce_median);
}

}  // namespace

bool run_pipeline(const MapgenConfig& cfg, const std::string& script_path,
                  MapArtifacts& out, std::string& err,
                  PipelineTimings* timings) {
  using clock = std::chrono::steady_clock;
  auto since = [](clock::time_point t) {
    return std::chrono::duration<double, std::milli>(clock::now() - t).count();
  };
  const auto t_all = clock::now();
  auto t = t_all;

  MapArtifacts a;

  // The only fallible stage: noiser has to compile + evaluate the script.
  const bool ok = evaluate_fields(cfg, script_path, a.fields, err);
  if (timings) timings->fields_ms = since(t);
  if (!ok) return false;

  t = clock::now();
  a.voronoi = build_voronoi(cfg);
  if (timings) timings->voronoi_ms = since(t);

  t = clock::now();
  a.biomes = assign_biomes(cfg, a.voronoi, a.fields);
  if (timings) timings->biomes_ms = since(t);

  t = clock::now();
  a.heightmap = compose_heightmap(cfg, a.fields, a.biomes);
  if (timings) timings->heightmap_ms = since(t);

  t = clock::now();
  reduce_to_block_grid(cfg, a);
  if (timings) timings->blocks_ms = since(t);

  t = clock::now();
  // Mutates a.blocks (fills section_id) and returns the graph indexing them.
  a.graph = extract_sections(a.blocks, cfg.section_step_m, cfg.min_section_blocks);
  if (timings) timings->sections_ms = since(t);

  if (timings) timings->total_ms = since(t_all);
  out = std::move(a);
  return true;
}

bool run_authored_pipeline(MapgenConfig& cfg, const std::string& map_dir,
                           MapArtifacts& out, std::string& err) {
  AuthoredMapMeta meta;
  if (!read_authored_meta(map_dir, meta, err)) return false;

  // The asset dictates the map's extent -- see the header.
  cfg.width = meta.width;
  cfg.height = meta.height;

  MapArtifacts a;
  if (!load_authored_heights(meta, a.heightmap, err)) return false;
  if (!load_authored_biome(meta, a.biomes.pixel, err)) return false;

  // Optional focus: keep only a sub-region of the loaded map (cfg.map_crop_*). The
  // images are untouched; this windows the decoded fields and shrinks the map to the
  // crop, at the same world scale.
  if (cfg.map_crop_w > 0 && cfg.map_crop_h > 0) {
    if (!crop_authored_map(a.heightmap, a.biomes.pixel, cfg.map_crop_x, cfg.map_crop_y,
                           cfg.map_crop_w, cfg.map_crop_h, err)) {
      return false;
    }
    cfg.width = cfg.map_crop_w;
    cfg.height = cfg.map_crop_h;
  }

  // fields/voronoi/biomes.cell_biome stay empty: nothing downstream reads them. The
  // preview writer knows to skip them (outputs.cpp), and that is the only consumer that
  // ever looked.
  reduce_to_block_grid(cfg, a);

  // NO extract_sections, on purpose. A section is a TERRACE, and terraces only exist
  // because compose_heightmap quantizes procedural heights into flat plateaus for the
  // flood fill to find. An authored map is a continuous surface: at this map's ~25 deg
  // median slope, adjacent 4 m blocks differ by ~1.8 m -- far more than
  // section_step_m -- so nearly every block becomes its own "terrace". Measured, that
  // is 62k sections from 262k blocks, which the O(sections x blocks) merge loop turns
  // into ~16G block-visits. It is meaningless AND ruinous, so it does not run: `graph`
  // stays empty and `blocks[i].section_id` stays -1. Consumers must handle that.
  out = std::move(a);
  return true;
}

}  // namespace badlands::mapgen
