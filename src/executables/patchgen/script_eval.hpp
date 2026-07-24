#pragma once

// Evaluate an arbitrary noiser script over a rectangular patch of the world.
//
// Deliberately NOT evaluate_fields (fields.hpp): that one hardwires the map's five
// uniforms, a 4-tuple result, and a domain that is always the whole map. This takes an
// arbitrary uniform list, expects a scalar result, and lets the caller place the patch
// anywhere in the world at any density — which is what makes a preview of a terrain
// script (and a seam check between two adjacent patches) possible.

#include <string>
#include <vector>

#include "mapgen/field2d.hpp"

namespace badlands::mapgen {

struct ScriptUniform {
  std::string name;
  float value = 0.0f;
};

// Where a patch sits in the world, in METERS. The evaluator passes these to the script
// as the `origin_x`/`origin_z`/`meters_per_sample` uniforms so the script can work in
// world meters — see scripts/mapgen/biomes/README for the contract.
//
// Sample (px, pz) of the patch is world (origin_x + (px+0.5)*meters_per_sample, ...),
// so two patches with different origins agree exactly where they overlap.
struct PatchDomain {
  int size = 512;                  // samples per side
  float origin_x = 0.0f;           // world meters
  float origin_z = 0.0f;
  float meters_per_sample = 1.0f;  // the VIEW's density, not the map's grid
};

// Compiles `source` and evaluates it once per sample of `domain`, writing the scalar
// result into `out` (sized domain.size x domain.size).
//
// `uniforms` are set once per worker thread (Reset preserves them). A uniform the script
// does not declare is skipped with a warning — the script's own default wins, which is
// noiser's behaviour, not something this can override.
//
// Returns false with `err` set on: unreadable file, compile error, a non-scalar result,
// or any per-sample runtime error.
bool evaluate_patch_script(const std::string& source,
                           const PatchDomain& domain,
                           const std::vector<ScriptUniform>& uniforms,
                           Field2D<float>& out, std::string& err);

// Convenience: reads the file, then evaluate_patch_script.
bool evaluate_patch_script_file(const std::string& script_path,
                                const PatchDomain& domain,
                                const std::vector<ScriptUniform>& uniforms,
                                Field2D<float>& out, std::string& err);

}  // namespace badlands::mapgen
