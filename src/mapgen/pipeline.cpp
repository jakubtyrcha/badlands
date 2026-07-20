#include "mapgen/pipeline.hpp"

#include <chrono>

#include "mapgen/heightmap.hpp"

namespace badlands::mapgen {

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
  a.blocks = reduce_to_blocks(a.heightmap, a.biomes.pixel, cfg.reduce_median);
  if (timings) timings->blocks_ms = since(t);

  t = clock::now();
  // Mutates a.blocks (fills section_id) and returns the graph indexing them.
  a.graph = extract_sections(a.blocks, cfg.section_step_m,
                            cfg.min_section_blocks);
  if (timings) timings->sections_ms = since(t);

  if (timings) timings->total_ms = since(t_all);
  out = std::move(a);
  return true;
}

}  // namespace badlands::mapgen
