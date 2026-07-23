#pragma once

// Loads creature-stat OVERRIDES from JSON over the compiled CreatureCatalog, so
// designers can rebalance without a rebuild (the arena's whole point). App-layer,
// exactly like factors_manifest.hpp: the sim ships compiled defaults and exposes
// Sim::SetCreatureCatalog; the app parses here and pushes the result in.
//
// Overridable per creature (keyed by CreatureName): the scalar stats
//   hp, move_speed, accuracy, evasion, defense, armour
// plus the PRIMARY attack's damage / cooldown / crit_chance
//   attack_damage, attack_cooldown, attack_crit
// Every key is optional; a missing key keeps the compiled default. Attack
// category/type and secondary attacks stay compiled for now.

#include <string>

#include "badlands_sim.hpp"  // badlands::CreatureCatalog

namespace badlands {

// Reads the manifest at `path` and applies its overrides onto `out`. Returns
// false (after logging) on a missing/unreadable file, unparseable JSON, an unknown
// creature name, or a non-numeric value; `out` is left untouched on failure.
//
// Expected shape (values are examples):
//   { "Mercenary": { "hp": 35, "armour": 4 },
//     "Hunter":    { "attack_damage": 6, "attack_crit": 0.3 } }
bool LoadCreatureCatalog(const std::string& path, CreatureCatalog& out);

}  // namespace badlands
