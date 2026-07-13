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

// ---------------------------------------------------------------------------
// Building placement (MVP v0.2)
//
// The world owns a triangle-occupancy grid: tile = 1.0 world unit, grid spans
// [-H, H) on X and Z with H = GAME_GRID_HALF_EXTENT_TILES. Each tile splits
// into 4 "X-split" triangles (N=-Z, E=+X, S=+Z, W=-X, cut by both diagonals).
// Buildings stamp a footprint plus a 1-tile L1 blocking margin; nothing may be
// placed within another building's footprint or margin.
// ---------------------------------------------------------------------------

enum { GAME_GRID_HALF_EXTENT_TILES = 48 };

typedef enum GameBuildingKind {
    GAME_BUILDING_CASTLE = 0,
    GAME_BUILDING_FREE_COMPANY_QUARTERS,
    GAME_BUILDING_HUNTERS_CAMP,
    GAME_BUILDING_THIEVES_DEN,
    GAME_BUILDING_SCRIPTORIUM,
    GAME_BUILDING_TAVERN,
    GAME_BUILDING_APOTHECARY,
    GAME_BUILDING_WATCHTOWER,
    GAME_BUILDING_HOUSE,   // poppable
    GAME_BUILDING_SEWER,   // poppable
    GAME_BUILDING_KIND_COUNT
} GameBuildingKind;

// Static per-kind footprint size (tiles) and whether it auto-spawns.
typedef struct GameBuildingDef {
    int32_t width_tiles, depth_tiles;
    uint32_t poppable;  // 1 = auto-spawned (House/Sewer), never player-placed
} GameBuildingDef;
GameBuildingDef game_building_def(int32_t kind);

// The world-space box a renderer should draw for a building of this kind and
// rotation, so the drawn cuboid matches the actual grid footprint. size is the
// local (pre-rotation) X/Z extent; apply yaw_radians about Y. Diagonal
// placements snap to a lattice diamond whose spans differ from w x d, so this
// is NOT simply (width, depth) at rotation_index * 45 deg.
typedef struct GameRenderBox {
    float size_x, size_z;
    float yaw_radians;
} GameRenderBox;
GameRenderBox game_render_box(int32_t kind, int32_t rotation_index);

// Placement request: a raw (un-snapped) desired center + rotation. The sim
// snaps the center to the grid lattice for the kind's parity.
// rotation_index 0..3 -> 0/45/90/135 deg, world = R_y(idx*45)*local (glam).
typedef struct GamePlacementDesc {
    int32_t kind;
    int32_t rotation_index;
    float world_x, world_z;
} GamePlacementDesc;

// One grid triangle in a probe readout.
typedef struct GameGridTriangle {
    int32_t tile_x, tile_z;  // tile min-corner, in [-H, H)
    uint32_t corner;         // 0=N(-Z) 1=E(+X) 2=S(+Z) 3=W(-X)
    uint32_t state;          // 0=free 1=blocked 2=would-block (blocked wins)
} GameGridTriangle;

typedef struct GamePlacementProbe {
    uint32_t valid;  // 1 if the snapped footprint could be placed now
    float snapped_x, snapped_z;
} GamePlacementProbe;

// Read-only placement preview, safe to call every mouse move. Writes the
// snapped center + validity into *out, and up to `cap` triangles covering the
// snapped footprint's tile bbox expanded by 2 tiles (clipped to the grid) into
// out_triangles. Returns the TOTAL triangle count (call again with a larger
// buffer if it exceeds cap).
uint32_t game_probe_placement(const BadlandsGame* game, const GamePlacementDesc* desc,
                              GamePlacementProbe* out, GameGridTriangle* out_triangles,
                              uint32_t cap);

// Places a building. Returns the new building id, or UINT32_MAX if the snapped
// footprint is invalid (state left untouched). A successful player placement
// stamps footprint+margin, advances the urban-sprawl counter, and immediately
// spawns any owed poppables (sewers before houses).
uint32_t game_place_building(BadlandsGame* game, const GamePlacementDesc* desc);

// Per-building snapshot row (game_state pattern). ids are stable and dense.
typedef struct GameBuildingState {
    uint32_t id;
    int32_t kind;
    float center_x, center_z;
    int32_t rotation_index;
    int32_t width_tiles, depth_tiles;
} GameBuildingState;
uint32_t game_buildings(const BadlandsGame* game, GameBuildingState* out, uint32_t cap);

// World-level scalars (gold, grid size, sprawl bookkeeping).
typedef struct GameWorldState {
    uint32_t gold;
    int32_t grid_half_extent_tiles;
    uint32_t queued_poppables;  // owed but not yet placeable (crowded map)
    uint32_t urban_quarters;    // sprawl accumulator in quarter-units
} GameWorldState;
void game_world(const BadlandsGame* game, GameWorldState* out);

#ifdef __cplusplus
}  // extern "C"
#endif
