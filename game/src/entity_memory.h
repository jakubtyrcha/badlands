// EntityMemory: each character's bounded, host-updated knowledge of who it
// can see or saw recently, plus the buildings it knows. Pure derived state --
// update_entity_memory only reads the world and writes EntityMemory
// components; it never enqueues a Command (game systems are event-sourced,
// but this is not a mutation of the simulated world, just an observation of
// it, same spirit as ActivityHistogram -- see sim.cpp's Sim::Tick comment).
//
// Nothing consumes this component yet: it packs into the wasm-brain wire
// format in a later task (see brain_abi.h's BlViewChar, which MemoryChar
// semantically mirrors -- native types here, wire packing is that task's
// job). This header is deliberately free of any wire-format dependency
// beyond the BL_MAX_CHARS capacity constant.

#pragma once

#include "brain_abi.h"  // BL_MAX_CHARS

#include <glm/glm.hpp>

#include <cstdint>

struct BadlandsGame;

namespace badlands {

// Buildings don't cross the wire in v1 (brain_abi.h has no building view
// yet), so this cap is host-side only -- not tied to any wire constant.
inline constexpr int32_t kMemoryMaxBuildings = 24;

// Semantic mirror of BlViewChar (native glm/bool types; wire packing is a
// later task). One entry per OTHER character this entity currently sees or
// remembers seeing.
struct MemoryChar {
    uint32_t slot = UINT32_MAX;
    int32_t archetype = -1;
    int32_t team = -1;
    glm::vec2 last_pos{0.0f, 0.0f};
    float last_hp = 0.0f;
    bool visible_now = false;
    int64_t last_seen_millis = 0;
};

// One building this entity currently sees or remembers seeing.
struct MemoryBuilding {
    uint32_t id = UINT32_MAX;
    int32_t kind = -1;
    glm::vec2 door{0.0f, 0.0f};
    bool alive = false;  // as last seen
    bool is_home = false;
    int64_t last_seen_millis = 0;
};

// ECS component: `chars`/`buildings` are dense (count + swap-remove on
// evict/expire) -- array ORDER is not part of the contract, only the SET of
// entries after N deterministic ticks is.
struct EntityMemory {
    int32_t char_count = 0;
    MemoryChar chars[BL_MAX_CHARS];
    int32_t building_count = 0;
    MemoryBuilding buildings[kMemoryMaxBuildings];
};

// Tick sub-pass (sim.cpp's tick_world calls this once per tick, before
// think, unconditionally -- including during replay, since this is pure
// derived state that must stay consistent for inspection either way).
//
// For every entity carrying EntityMemory + Position (an "observer", by slot
// index ascending): refreshes which OTHER characters (by slot index
// ascending) and buildings (by building id ascending) are within its
// Vision radius right now, ages out character sightings older than
// HeroFactors::memory_ttl_millis, and evicts on overflow. See
// entity_memory.cpp for the exact per-field rules.
void update_entity_memory(BadlandsGame& game);

// Spawn-time seeding ("residents know their town"): pre-fills `mem.buildings`
// with `home_building_id` (is_home = true) followed by every other
// currently-alive building, oldest-first by id, capped at
// kMemoryMaxBuildings. Called by heroes.cpp's spawn_entity for any character
// spawned with a home (home_building_id >= 0); homeless spawns (goblins,
// deer) never call this and start with an empty EntityMemory.
void seed_home_town_memory(BadlandsGame& game, EntityMemory& mem, uint32_t home_building_id);

}  // namespace badlands
