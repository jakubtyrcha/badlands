// C++ API of the badlands game simulation (C++/EnTT + noiser brains).
//
// Data in, data out: scenarios are composed by spawning CharacterDesc rows;
// observers (renderer, tests) inspect CharacterState snapshots and SimStats.
// `badlands::Sim` owns the sim world and exposes tick/spawn/dispatch/snapshot
// as C++ methods. This C++ Sim API replaced the former extern-"C", data-only
// game_* ABI, which has been removed.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <entt/entt.hpp>  // entt::registry (the amalgamated single-include ships no fwd.hpp)

// The internal sim world, defined at global scope in game/src/game_state.h.
struct BadlandsGame;

namespace badlands {

// ---- enums (were GameBuildingKind / GameActionKind) ------------------------
enum class BuildingKind : int32_t {
    Castle = 0,
    FreeCompanyQuarters,
    HuntersCamp,
    ThievesDen,
    Scriptorium,
    Tavern,
    Apothecary,
    Watchtower,
    House,  // poppable
    Sewer,  // poppable
    Count
};

enum class ActionKind : int32_t {
    PlaceBuilding = 0,  // world_x/z, param_a = kind, param_b = rotation_index
    RecruitHero,        // target_id = guild building id
    DestroyBuilding,    // target_id = building id
    Count
};

inline constexpr int32_t kGridHalfExtentTiles = 48;  // was GAME_GRID_HALF_EXTENT_TILES

// Hero guild classes (the recruitable "class type id"). Unscoped + HERO_*
// enumerators to match the sim-internal usage this was promoted from (was
// heroes.h's HeroClassId); the numeric values are load-bearing (color table,
// registry HeroClass component .value). NB: distinct from the `HeroClass`
// EnTT *component* (game/src/components.h) -- this is the class enum.
enum HeroClassId : int32_t {
    HERO_MERCENARY = 0,
    HERO_HUNTER,
    HERO_GRAVE_ROBBER,
    HERO_APPRENTICE,
    HERO_CLASS_COUNT
};

// A building recruits at most this many distinct hero classes (usually 1). The
// per-kind recruit set lives in BuildingDef::recruits (the placement.cpp kDefs
// table is its single source of truth).
inline constexpr int32_t kMaxRecruitClasses = 3;

// Display name of a hero class ("Mercenary", ...). Empty string if out of range.
const char* HeroClassName(HeroClassId cls);

// ---- POD result structs (field-for-field from badlands_game.h) -------------

// Spawn input. pos is on the ground (XZ) plane, matching the renderer.
struct CharacterDesc {
    float pos_x, pos_z;
    int32_t team;
    float hp;
    float move_speed;       // units/sec
    float attack_range;
    float attack_damage;
    float attack_cooldown;  // seconds between swings
    float size_x, size_y, size_z;
    float color_r, color_g, color_b;
};

// Per-living-entity snapshot row: the renderer reads pos/size/color, tests
// read team/hp.
struct CharacterState {
    uint32_t id;
    int32_t team;
    float pos_x, pos_z;
    float hp, max_hp;
    float size_x, size_y, size_z;
    float color_r, color_g, color_b;
    int32_t home_building_id;    // recruiting guild; -1 = homeless / not a hero
    int32_t inside_building_id;  // -1 = outside; >=0 => hidden (don't draw; list in panel)
};

// Run counters. NB: NOT `Stats` — badlands::Stats already exists (a sim
// component, game/src/components.h:24). Use SimStats for the run counters.
struct SimStats {
    uint64_t ticks;
    uint64_t script_intents;  // intents delivered by noiser brains (0 when mocked)
    uint32_t noiser_bugs;     // failures that downgraded an entity to the mock brain
};

// Static per-kind footprint size (tiles), behavior flags, and recruit set.
struct BuildingDef {
    int32_t width_tiles, depth_tiles;
    bool poppable;            // auto-spawned (House/Sewer), never player-placed
    bool user_destructible;   // player may DESTROY (the 7 buildable kinds)
    bool enemy_targettable;   // future monster attacks may target (Castle/House)
    // Hero classes this kind recruits. recruit_count is 0 for non-guilds and 1
    // for today's guilds; the array has room for 2-3 (guilds are the only
    // kinds with recruit_count > 0). Declared per kind in placement.cpp kDefs.
    int32_t recruit_count;
    HeroClassId recruits[kMaxRecruitClasses];
};

// The world-space box a renderer should draw for a building of this kind and
// rotation. size is the local (pre-rotation) X/Z extent; apply yaw_radians
// about Y.
struct RenderBox {
    float size_x, size_z;
    float yaw_radians;
};

// Placement request: a raw (un-snapped) desired center + rotation. The sim
// snaps the center to the grid lattice for the kind's parity.
struct PlacementDesc {
    int32_t kind;
    int32_t rotation_index;
    float world_x, world_z;
};

// One grid triangle in a probe readout.
struct GridTriangle {
    int32_t tile_x, tile_z;  // tile min-corner, in [-H, H)
    uint32_t corner;         // 0=N(-Z) 1=E(+X) 2=S(+Z) 3=W(-X)
    uint32_t state;          // 0=free 1=blocked 2=would-block (blocked wins)
};

struct PlacementProbe {
    bool valid;  // the snapped footprint could be placed now
    float snapped_x, snapped_z;
};

// Generic player->world action trigger. Every player action crosses this one
// entry point (Sim::Dispatch).
struct Action {
    ActionKind kind;
    uint32_t target_id;  // building/entity id for id-addressed actions
    float world_x, world_z;
    int32_t param_a, param_b;
};

// Per-building snapshot row (Characters() pattern). ids are stable and dense.
struct BuildingState {
    uint32_t id;
    BuildingKind kind;
    float center_x, center_z;
    int32_t rotation_index;
    int32_t width_tiles, depth_tiles;
};

// World-level scalars (gold, grid size, sprawl bookkeeping).
struct WorldState {
    uint32_t gold;
    int32_t grid_half_extent_tiles;
    uint32_t queued_poppables;  // owed but not yet placeable (crowded map)
    uint32_t urban_quarters;    // sprawl accumulator in quarter-units
    uint32_t guild_roster_cap;  // kGuildRosterCap (heroes per guild); UI mirrors it
};

// Injected Rust nav provider (was GamePathfinder) — kept as-is, by value. The
// engine delegates path *geometry* to this provider; obstacles mutate one
// building at a time so the provider maintains its graph incrementally.
struct Pathfinder {
    void* ctx = nullptr;
    void (*add_obstacle)(void* ctx, uint32_t building_id, const float* poly_xz,
                         int32_t n_verts) = nullptr;
    void (*remove_obstacle)(void* ctx, uint32_t building_id) = nullptr;
    int32_t (*find_path)(void* ctx, float sx, float sz, float gx, float gz, float radius,
                         uint32_t exempt_building, float* out_xz, int32_t cap) = nullptr;
};

// ---- the sim ---------------------------------------------------------------
class Sim {
   public:
    // brain_script_source: noiser source driving spawned entities' brains, or
    // nullptr for mock-brains-only. A script that fails to compile is recorded
    // as a noiser bug and the sim falls back to mock brains.
    explicit Sim(const char* brain_script_source = nullptr);
    ~Sim();
    Sim(Sim&&) noexcept;
    Sim& operator=(Sim&&) noexcept;
    Sim(const Sim&) = delete;
    Sim& operator=(const Sim&) = delete;

    // Returns the entity id used in CharacterState rows.
    uint32_t Spawn(const CharacterDesc& desc);
    void Tick(float dt);
    // Recompiles the brain script; on failure the previous program is kept
    // (returns false). On success all brains restart on the new program.
    bool ReloadScript(const std::string& source);
    // Executes a player action. Returns >= 0 on success (a new building/hero
    // id, or 0 for id-less actions) and < 0 on error.
    int64_t Dispatch(const Action& action);
    // Registers the nav provider (copied by value) and back-fills every alive
    // building. Pass a default-constructed Pathfinder to clear.
    void SetPathfinder(const Pathfinder& pf);

    // Snapshot accessors — identical semantics to the old ABI, POD vectors.
    std::vector<CharacterState> Characters() const;  // was game_state
    std::vector<BuildingState> Buildings() const;    // was game_buildings
    // Out-param overload: reuses the caller's buffer (out.clear() then fill),
    // avoiding a per-frame allocation on the render path. The primitive; the
    // value-returning Buildings() delegates here.
    void Buildings(std::vector<BuildingState>& out) const;
    WorldState World() const;                         // was game_world
    SimStats GetStats() const;                        // was game_stats
    // Placement preview; returns validity, fills out_triangles (was
    // game_probe_placement).
    PlacementProbe ProbePlacement(const PlacementDesc& desc,
                                  std::vector<GridTriangle>& out_triangles) const;

    // The shared world. Later increments read render/sim components off this.
    entt::registry& registry();
    const entt::registry& registry() const;

   private:
    std::unique_ptr<::BadlandsGame> world_;  // the EXISTING internal world, unchanged
};

// ---- handle-less helpers (were game_*; pure computations) ------------------
BuildingDef BuildingDefOf(BuildingKind kind);                     // was game_building_def
RenderBox RenderBoxOf(BuildingKind kind, int32_t rotation_index);  // was game_render_box
CharacterDesc MercenaryDesc(float pos_x, float pos_z);             // was game_desc_mercenary
CharacterDesc GoblinDesc(float pos_x, float pos_z);               // was game_desc_goblin

}  // namespace badlands
