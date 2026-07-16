#include "mapgen/config.hpp"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <fstream>

namespace badlands::mapgen {

MapgenConfig load_config(const std::string& path) {
  MapgenConfig c;
  if (path.empty()) return c;

  std::ifstream f(path);
  if (!f) {
    std::fprintf(stderr, "mapgen: cannot open config '%s' — using defaults\n",
                 path.c_str());
    return c;
  }

  nlohmann::json j;
  try {
    f >> j;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "mapgen: invalid config '%s' (%s) — using defaults\n",
                 path.c_str(), e.what());
    return c;
  }

  c.seed = j.value("seed", c.seed);
  c.width = j.value("width", c.width);
  c.height = j.value("height", c.height);
  c.out_dir = j.value("out_dir", c.out_dir);

  c.cell_size_m = j.value("cell_size_m", c.cell_size_m);
  c.seed_jitter = j.value("seed_jitter", c.seed_jitter);

  c.elevation_freq = j.value("elevation_freq", c.elevation_freq);
  c.moisture_freq = j.value("moisture_freq", c.moisture_freq);
  c.ridged_freq = j.value("ridged_freq", c.ridged_freq);
  c.fine_freq = j.value("fine_freq", c.fine_freq);

  c.elev_lake = j.value("elev_lake", c.elev_lake);
  c.elev_high = j.value("elev_high", c.elev_high);
  c.moisture_wet = j.value("moisture_wet", c.moisture_wet);

  c.height_scale_m = j.value("height_scale_m", c.height_scale_m);
  c.cavity_depth_m = j.value("cavity_depth_m", c.cavity_depth_m);
  c.hills_ridge_m = j.value("hills_ridge_m", c.hills_ridge_m);
  c.terrace_step_m = j.value("terrace_step_m", c.terrace_step_m);

  if (j.contains("variation_amp_m") && j["variation_amp_m"].is_array()) {
    const auto& arr = j["variation_amp_m"];
    for (int i = 0; i < kBiomeCount && i < static_cast<int>(arr.size()); ++i) {
      c.variation_amp_m[i] = arr[i].get<float>();
    }
  }

  c.section_step_m = j.value("section_step_m", c.section_step_m);
  c.min_section_blocks = j.value("min_section_blocks", c.min_section_blocks);
  c.reduce_median = j.value("reduce_median", c.reduce_median);

  return c;
}

}  // namespace badlands::mapgen
