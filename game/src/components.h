// ECS components of the badlands simulation. All POD; positions live on the
// ground (XZ) plane to match the renderer's convention.

#pragma once

#include <glm/glm.hpp>

#include <entt/entt.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace badlands {

// --- v0.3 town/hero tunables ------------------------------------------------
constexpr int kInventoryCap = 2;                 // elixirs a hero can carry
constexpr float kInsideDurationSeconds = 3.0f;   // time hidden inside a building
constexpr float kEntranceRadius = 0.6f;          // how close to a door to enter

// --- day/night clock (integer milliseconds, fixed 30 Hz) --------------------
// Sim time is an int64 millisecond count advanced by a compile-time constant
// each tick; no float accumulator (deterministic, no drift, no dt coupling).
constexpr int64_t kSimHz = 30;                        // fixed sim rate
constexpr int64_t kMillisPerTick = 1000 / kSimHz;     // 33 ms/tick (~30 Hz, ~1% round)
constexpr int64_t kSecondsPerDay = 120;               // fast day for prototyping
constexpr int64_t kMillisPerDay = kSecondsPerDay * 1000;  // day length in ms
constexpr float kNightStart = 0.75f;                  // ~18:00
constexpr float kNightEnd = 0.25f;                    // ~06:00

// Fraction of the day in [0,1): 0 = midnight, 0.5 = midday.
inline float time_of_day(int64_t world_millis) {
    int64_t t = world_millis % kMillisPerDay;
    if (t < 0) {
        t += kMillisPerDay;
    }
    return static_cast<float>(t) / static_cast<float>(kMillisPerDay);
}
inline uint32_t day_count(int64_t world_millis) {
    return static_cast<uint32_t>(world_millis / kMillisPerDay);
}
inline bool is_night(float tod) { return tod >= kNightStart || tod < kNightEnd; }

// --- needs growth (policy placeholders) -------------------------------------
// Per-tick deltas for fatigue/boredom. Deterministic (not dt-scaled). Defaults
// sized so an unmet need saturates over roughly one day; boredom rises faster
// so heroes seek the tavern before nightfall. These are tunable policy, not the
// architecture.
constexpr float kFatiguePerTick =
    static_cast<float>(kMillisPerTick) / static_cast<float>(kMillisPerDay);
constexpr float kBoredomPerTick = kFatiguePerTick * 2.0f;

struct Position {
    glm::vec2 pos;  // XZ
};

struct Stats {
    float move_speed;
    float attack_range;
    float attack_damage;
    float attack_cooldown;
};

struct Health {
    float hp;
    float max_hp;
};

struct Team {
    int32_t id;
};

struct CooldownTimer {
    float remaining;
};

struct RenderShape {
    glm::vec3 size;
    glm::vec3 color;
};

// Transient per-tick output of a brain (script host calls or the mock brain).
// kind: 0 = idle, 1 = move along dir, 2 = attack nearest enemy.
struct Intent {
    int kind;
    glm::vec2 dir;
};

// --- hero state: definition / simulation / display --------------------------
// Hero-specific data grouped on the static-config / dynamic-state / display
// axis. Combat + movement stay on the generic components (Health/Stats/
// CooldownTimer/MoveTarget/NavPath/Intent/Team/InsideBuilding/MeleeLock) shared
// by all entities.

// Definition (static): the hero archetype (Mercenary/Hunter/Grave Robber/
// Apprentice). The class name is derived renderer-side from the home guild kind,
// so this value never crosses the C API.
struct HeroCharacter {
    int32_t hero_class;
};

// Simulation (dynamic): per-tick hero state.
//  - fatigue/boredom: day/night needs in [0,1] (0 = satisfied, rising over time)
//  - behavior: last chosen Behaviour id (inspection only; -1 = unknown)
//  - inventory: collect-only elixir count in [0, kInventoryCap]
//  - home_building_id: dedicated home = recruiting guild; -1 once homeless
struct HeroSimulationState {
    float fatigue = 0.0f;
    float boredom = 0.0f;
    int32_t behavior = -1;
    int32_t inventory = 0;
    int32_t home_building_id = -1;
};

// Display: renderer/panel-facing.
struct HeroDisplayState {
    std::string name;
};

// Present while a hero is hidden inside a building; `timer` counts down to the
// reappear at the approach tile. Its presence excludes the hero from movement,
// combat, and enemy targeting (but not from the game_state snapshot).
struct InsideBuilding {
    int32_t building_id;
    float timer;
};

// Present while a unit is locked in melee — movement is frozen (combat only).
struct MeleeLock {};

// --- v0.3 movement / pathfinding --------------------------------------------

// A sub-tile capsule agent; radius drives clearance in the navmesh path query.
struct Agent {
    float radius;
};

// Durable movement goal (set by host calls / mock brains). Distinct from the
// transient per-tick Intent: MoveTarget is "where to walk", Intent is "act now".
struct MoveTarget {
    enum class Kind { None, Point, Entity, Building } kind = Kind::None;
    glm::vec2 point{};
    entt::entity entity = entt::null;
    uint32_t building = UINT32_MAX;
    float stop_distance = 0.0f;
};

// The planned route toward a MoveTarget plus repath bookkeeping.
struct NavPath {
    std::vector<glm::vec2> waypoints;
    uint32_t cursor = 0;
    uint32_t epoch = 0;            // nav_epoch the path was planned against
    float repath_cooldown = 0.0f;
};

}  // namespace badlands
