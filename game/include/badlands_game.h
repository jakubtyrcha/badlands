// C API of the badlands game simulation (C++/EnTT + noiser brains).
//
// Data in, data out: scenarios are composed by spawning GameCharacterDesc
// rows; observers (renderer, tests) inspect GameCharacterState snapshots and
// GameStats. No scenario- or query-shaped RPCs.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct BadlandsGame BadlandsGame;

// Spawn input. pos is on the ground (XZ) plane, matching the renderer.
typedef struct GameCharacterDesc {
    float pos_x, pos_z;
    int32_t team;
    float hp;
    float move_speed;       // units/sec
    float attack_range;
    float attack_damage;
    float attack_cooldown;  // seconds between swings
    float size_x, size_y, size_z;
    float color_r, color_g, color_b;
} GameCharacterDesc;

// Per-living-entity snapshot row: the renderer reads pos/size/color, tests
// read team/hp.
typedef struct GameCharacterState {
    uint32_t id;
    int32_t team;
    float pos_x, pos_z;
    float hp, max_hp;
    float size_x, size_y, size_z;
    float color_r, color_g, color_b;
} GameCharacterState;

typedef struct GameStats {
    uint64_t ticks;
    uint64_t script_intents;  // intents delivered by noiser brains (0 when mocked)
    uint32_t noiser_bugs;     // failures that downgraded an entity to the mock brain
} GameStats;

// brain_script_source: noiser source driving spawned entities' brains, or
// NULL for mock-brains-only. A script that fails to compile is recorded as a
// noiser bug and the game falls back to mock brains.
BadlandsGame* game_create(const char* brain_script_source);
void game_destroy(BadlandsGame* game);

// Returns the entity id used in GameCharacterState rows.
uint32_t game_spawn(BadlandsGame* game, const GameCharacterDesc* desc);

// Canonical Stage-2 duelist descriptors — the single source of truth shared
// by the C++ tests and the Rust app.
GameCharacterDesc game_desc_mercenary(float pos_x, float pos_z);
GameCharacterDesc game_desc_goblin(float pos_x, float pos_z);

void game_tick(BadlandsGame* game, float dt);

// Returns the TOTAL number of living entities and writes min(cap, total)
// rows. A return value larger than `cap` means the snapshot was truncated:
// call again with a larger buffer.
uint32_t game_state(const BadlandsGame* game, GameCharacterState* out, uint32_t cap);

void game_stats(const BadlandsGame* game, GameStats* out);

// Recompiles the brain script; on failure the previous program is kept
// (returns false). On success all brains restart on the new program.
bool game_reload_script(BadlandsGame* game, const char* source);

#ifdef __cplusplus
}  // extern "C"
#endif
