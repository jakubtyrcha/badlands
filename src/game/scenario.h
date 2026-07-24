#pragma once

// A sandbox scenario, loaded from JSON: a map (a walled arena, or the town map)
// plus a list of creatures to spawn on teams. This is the "configurable" surface
// for the AI sandbox -- author a fight, watch who wins.
//
// App-layer on purpose (mirrors factors_manifest.hpp / biome_manifest.hpp): the
// deterministic sim core never links a JSON parser. The app parses here, builds a
// WorldConfig + spawns each creature through the Sim API.

#include <string>
#include <utility>
#include <vector>

#include "badlands_sim.hpp"  // badlands::CreatureId, WorldConfig, CharacterState

namespace badlands {

// Per-team live-combatant summary of a set of character rows, for the arena
// winner readout. Neutral wildlife (Critter archetype) is never a combatant
// whatever its team, so a fight is over when at most one team still has a living
// combatant. Team is NOT hardcoded -- a scenario may put fighters on any team.
struct ArenaTally {
    // Teams with >= 1 living combatant, ascending by team, with their counts.
    std::vector<std::pair<int32_t, int>> teams;
    int32_t winner = -1;  // the sole surviving combatant team, or -1 (0 or >1 left)
};
ArenaTally tally_arena(const std::vector<CharacterState>& rows);

// One creature to place at scenario start.
struct ScenarioSpawn {
    CreatureId creature;
    int32_t team;
    float x, z;
};

// A fully-parsed scenario. Defaults describe an empty town-map scenario.
struct Scenario {
    std::string name = "scenario";
    // Arena interior half-extents in world units. Both > 0 => a walled arena
    // (flat, no colony, confined edges); otherwise the normal town map.
    float arena_half_x = 0.0f;
    float arena_half_z = 0.0f;
    std::vector<ScenarioSpawn> spawns;

    bool is_arena() const { return arena_half_x > 0.0f && arena_half_z > 0.0f; }
    // The WorldConfig this scenario's map implies.
    WorldConfig world_config() const {
        WorldConfig c;
        if (is_arena()) {
            c.prebuild_colony = false;
            c.terrain_blocking = false;
            c.arena_half_x = arena_half_x;
            c.arena_half_z = arena_half_z;
        }
        return c;
    }
};

// Parse the scenario JSON at `path` into `out`. Returns false (after logging) on a
// missing/unreadable file, unparseable JSON, or an unknown creature name; `out` is
// left untouched on failure. Expected shape:
//   { "name": "duel",
//     "arena": { "half_x": 12, "half_z": 8 },
//     "spawns": [ { "creature": "Mercenary", "team": 0, "x": -6, "z": 0 },
//                 { "creature": "Goblin",    "team": 1, "x":  6, "z": 0 } ] }
bool LoadScenario(const std::string& path, Scenario& out);

}  // namespace badlands
