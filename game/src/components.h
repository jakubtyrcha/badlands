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

// --- Fog-of-war vision ------------------------------------------------------

// Model-space forward axis (XZ) a character faces at zero rotation. Its
// Transform's rotation is applied to this to get the world look direction; for
// the sim we track that world direction directly in Facing.
inline constexpr glm::vec2 kCharacterForward{0.0f, 1.0f};

// A character's world XZ look direction (unit length). Equivalent to the
// character Transform's rotation applied to kCharacterForward, projected to XZ;
// a minimal stand-in until characters carry a full Transform, at which point
// this becomes a derived view of it. The movement pipeline turns it toward the
// direction of travel; an idle character keeps its last facing.
struct Facing {
    glm::vec2 dir{kCharacterForward};
};

// A character's vision, granted to the player only for team == kPlayerTeam.
// cone_half_cos = cos(half-angle): a texel at unit direction d from the
// character is seen when dot(d, Facing.dir) >= cone_half_cos (and within
// radius). A half-angle >= 180deg yields cone_half_cos = -1 (full circle).
struct Vision {
    float radius = 0.0f;
    float cone_half_cos = -1.0f;
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
    // Deliberation: while world_millis is below this the hero is standing and
    // thinking. Set by the SetBehavior handler from the command's duration (so
    // the pause is IN the log and a replay reproduces it), and cleared by
    // committing to any other activity.
    int64_t think_until_millis = 0;
};

// Display: renderer/panel-facing.
struct HeroDisplayState {
    std::string name;
};

// Critter (deer) dynamic state. Deer wander around a fixed home range and cycle
// walk -> graze -> walk; the anchor keeps them near where they spawned (their
// forest) instead of drifting off the map.
struct CritterState {
    glm::vec2 roam_anchor{0.0f, 0.0f};  // centre of the wander range (spawn spot)
    int32_t behavior = -1;              // last chosen Behavior, for inspection
};

// Tax collector (townfolk) dynamic state. It makes a round of the buildings
// owing tax, banking each into `carried_gold`, then returns to a Castle/
// Watchtower to deposit the carry into the player's gold and despawn. `visited`
// stops it collecting the same building twice in one round; dying with a carry
// (e.g. to a rat) loses that gold -- the vulnerability the round creates.
struct TaxCollectorState {
    std::vector<uint32_t> visited;  // building ids already collected this round
    uint32_t carried_gold = 0;
    int32_t home_building_id = -1;  // the Castle it set out from (deposit fallback)
    int32_t behavior = -1;          // last chosen Behavior, for inspection
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

// The world refused a step: the character tried to walk into terrain it cannot
// cross. Written by the movement system (systems may write the registry; brains
// may not), read by perception, and acted on by whichever activity cares.
//
// This is the seam that decouples "where the AI decided to go" from "where the
// character can actually get to". A brain is allowed to want the impossible --
// it plans through unexplored ground it has no information about — and finding
// out is an EVENT it reacts to, not a precondition it must check. It is never
// cleared: `at_millis` says when it happened, and perception decides whether
// that is still relevant (which keeps a stale blockage from vetoing forever).
struct MoveBlocked {
    glm::vec2 point{0.0f, 0.0f};  // where the step was refused
    int64_t at_millis = 0;
};

// Present on BOTH heroes of a conversation. Created only by the Chat command
// (so the pairing is in the trace and replays) and dissolved by advance_chats
// as a system rule — expiry, the partner dying or wandering off, or a threat
// arriving. Chatting decays boredom toward a FLOOR rather than clearing it:
// company is a weaker entertainment than the tavern, which is what makes the
// tavern still worth walking to.
struct ChattingState {
    uint32_t partner_slot = UINT32_MAX;
    float remaining = 0.0f;  // seconds left in the conversation
};

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
