#pragma once

#include <string>

#include "mapgen/config.hpp"
#include "mapgen/field2d.hpp"

namespace badlands::mapgen {

// Biome-agnostic continuous noise fields, one value per heightmap sample.
struct Fields {
  Field2D<float> elevation;  // ~[0,1], large-scale; also the height base
  Field2D<float> moisture;   // ~[0,1], large-scale
  Field2D<float> ridged;     // ~[0,1], ridge/rocky detail
  Field2D<float> fine;       // ~[0,1], fine detail
};

// Compile the noiser field script at `script_path` and evaluate it over
// cfg.width x cfg.height in parallel tiles (one ExecutionContext per worker
// thread). On success fills `out` and returns true; on failure sets `err`.
bool evaluate_fields(const MapgenConfig& cfg, const std::string& script_path,
                     Fields& out, std::string& err);

}  // namespace badlands::mapgen
