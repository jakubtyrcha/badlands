// ECS components of the badlands simulation. All POD; positions live on the
// ground (XZ) plane to match the renderer's convention.

#pragma once

#include <glm/glm.hpp>

#include <entt/entt.hpp>

#include <cstdint>
#include <vector>

namespace badlands {

// --- v0.3 town/hero tunables ------------------------------------------------
constexpr int kInventoryCap = 2;                 // elixirs a hero can carry
constexpr float kInsideDurationSeconds = 3.0f;   // time hidden inside a building
constexpr float kEntranceRadius = 0.6f;          // how close to a door to enter

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

// --- v0.3 hero/town state ---------------------------------------------------

// Hero archetype (Warrior/Ranger/Rogue/Wizard). Decorative in v0.3: it only
// distinguishes spawn color; the class name is derived renderer-side from the
// home guild's kind, so this value never crosses the C API.
struct HeroClass {
    int32_t value;
};

// The hero's dedicated home = its recruiting guild; -1 once homeless.
struct Home {
    int32_t building_id;
};

// Collect-only elixir count in [0, kInventoryCap].
struct Inventory {
    int32_t count;
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
    uint16_t stuck_ticks = 0;
    bool blocked = false;
};

}  // namespace badlands
