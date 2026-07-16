#pragma once

// Resolves the biome -> PBR material pack mapping that feeds the terrain-blend
// texture arrays. Game-side on purpose: the engine's LoadTerrainArrays only
// takes "N pack dirs" and has no notion of biomes.

#include <string>
#include <vector>

namespace badlands {

// Reads the biome manifest at `manifest_path` (JSON: one string pack dir per
// mapgen::biome_name(), e.g. {"lake": "assets/materials/...", ...}) and writes
// the pack dirs into `out_pack_dirs` in Biome ENUM ORDER -- index i is the pack
// for biome i, which is exactly the terrain array's layer index.
//
// Keyed by name rather than positionally so a reordered or renamed entry fails
// loudly instead of silently mis-mapping a biome to the wrong texture. Every
// biome must be present and map to a string; anything else is an error.
//
// Returns false (after logging) on a missing/unreadable file, unparseable JSON,
// or a missing/non-string biome entry. `out_pack_dirs` is cleared first and is
// left empty on failure.
bool ResolveBiomePacks(const std::string& manifest_path,
                       std::vector<std::string>& out_pack_dirs);

}  // namespace badlands
