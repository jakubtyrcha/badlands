#include "mapgen/pipeline.hpp"

#include "mapgen/heightmap.hpp"

namespace badlands::mapgen {

bool run_pipeline(const MapgenConfig& cfg, const std::string& script_path,
                  MapArtifacts& out, std::string& err) {
  MapArtifacts a;

  // The only fallible stage: noiser has to compile + evaluate the script.
  if (!evaluate_fields(cfg, script_path, a.fields, err)) return false;

  a.voronoi = build_voronoi(cfg);
  a.biomes = assign_biomes(cfg, a.voronoi, a.fields);
  a.heightmap = compose_heightmap(cfg, a.fields, a.biomes);
  a.blocks = reduce_to_blocks(a.heightmap, a.biomes.pixel, cfg.reduce_median);
  // Mutates a.blocks (fills section_id) and returns the graph indexing them.
  a.graph = extract_sections(a.blocks, cfg.section_step_m,
                            cfg.min_section_blocks);

  out = std::move(a);
  return true;
}

}  // namespace badlands::mapgen
