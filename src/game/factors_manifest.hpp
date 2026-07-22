#pragma once

// Loads badlands::SimFactors (behaviour tuning) from a JSON manifest so
// designers can retune without a rebuild.
//
// App-layer on purpose: badlands_game_lib links only noiser_bundle, and adding
// nlohmann to the sim would put a JSON parser inside the deterministic core for
// no reason. The sim ships compiled defaults and exposes Sim::SetFactors; apps
// parse here and push the result in. Same shape as
// src/mapview/biome_manifest.hpp.

#include <string>

#include "badlands_sim.hpp"

namespace badlands {

// Reads the factors manifest at `manifest_path` and writes it into `out`.
//
// Every key is OPTIONAL: a missing key keeps the compiled default already in
// `out`, so a partial file tunes only what it mentions. A key that is present
// but not a number is an ERROR -- that is a typo or a schema drift, and
// silently ignoring it would leave a designer wondering why their edit did
// nothing.
//
// Expected shape (values shown are the compiled defaults):
//   { "hero": { "fatigue_go_home": 0.6, "fatigue_night": 0.2,
//               "boredom_tavern": 0.5, "roam_radius": 6.0 } }
//
// Returns false (after logging) on a missing/unreadable file, unparseable JSON,
// a non-object section, or a non-numeric value. `out` is left untouched on
// failure, so a bad file falls back to defaults rather than to garbage.
bool LoadSimFactors(const std::string& manifest_path, SimFactors& out);

}  // namespace badlands
