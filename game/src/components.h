// ECS components of the badlands simulation. All POD; positions live on the
// ground (XZ) plane to match the renderer's convention.

#pragma once

#include "badlands_sim.hpp"  // Archetype; Attack, Combatant, DamageType, kMaxAttacks (combat primitives)

#include <glm/glm.hpp>

#include <entt/entt.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace badlands {

// --- v0.3 town/hero tunables ------------------------------------------------
constexpr int kInventoryCap = 2;                 // elixirs a hero can carry
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

// One in-game hour of sim time. Need rates are authored in in-game hours (see
// HeroFactors) and converted through here, so a factor says what it means.
constexpr int64_t kMillisPerGameHour = kMillisPerDay / 24;

// Per-tick delta that moves a 0..1 reserve the whole way in `hours` in-game
// hours. Deterministic and not dt-scaled, like the clock itself. Non-positive
// hours mean "instantly" (the whole reserve in one tick), which is what makes
// a zero in the manifest behave sensibly rather than dividing by zero.
inline float reserve_rate_per_tick(float hours) {
    if (hours <= 0.0f) {
        return 1.0f;
    }
    return static_cast<float>(kMillisPerTick) /
           (hours * static_cast<float>(kMillisPerGameHour));
}

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
// axis. Combat + movement stay on the generic components (Health/Combatant/
// Attacks/Stats/MoveTarget/NavPath/Intent/Team/InsideBuilding/MeleeLock) shared
// by all entities.

// Definition (static): the hero archetype (Mercenary/Hunter/Grave Robber/
// Apprentice). The class name is derived renderer-side from the home guild kind,
// so this value never crosses the C API.
struct HeroCharacter {
    int32_t hero_class;
};

// Simulation (dynamic): per-tick hero state.
//  - fatigue/content: need RESERVES in [0,1] -- 1 is satisfied, 0 is spent.
//    They drain on their own and are refilled by resting/being entertained.
//    (So `fatigue == 1` means well rested; see HeroFactors for why the sense
//    is uniform across needs rather than idiomatic per need.)
//  - behavior: last chosen Behaviour id (inspection only; -1 = unknown)
//  - inventory: collect-only elixir count in [0, kInventoryCap]
//  - home_building_id: dedicated home = recruiting guild; -1 once homeless
struct HeroSimulationState {
    float fatigue = 1.0f;
    float content = 1.0f;
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

// Present while a hero is hidden inside a building. Its presence excludes the
// hero from movement, combat, and enemy targeting (but not from the snapshot).
//
// There is NO duration here, deliberately. How long a hero stays is not a
// constant to be picked -- it is a consequence of why it went in and how
// depleted it was: `purpose` names the need being refilled, the fill rate comes
// from HeroFactors, and the hero leaves when that reserve is full. A hero that
// crawled home at 0.1 fatigue sleeps most of the night; one that ducked in at
// 0.8 is out again shortly.
//
// `should_leave_building` (heroes.h) owns the leave decision and is the single
// place future reasons go -- home under attack, a threat outside, dawn. It has
// to be system-side because a hidden hero is excluded from perception and so
// cannot see any of that for itself.
struct InsideBuilding {
    int32_t building_id;
    int32_t purpose;  // the ActivityId that sent them in (GoHome / VisitTavern)
};

// Present while a unit is locked in melee — movement is frozen (combat only).
struct MeleeLock {};

// A unit's attack-skill loadout with per-attack cooldown state. Fixed small array
// (kMaxAttacks) keeps it POD; `count` is how many of `defs` are live, and
// `cooldown_remaining[i]` is the seconds until `defs[i]` can be used again. The
// spawn path fills this (deriving a single melee attack from the legacy
// CharacterDesc attack_* fields when none is authored). Combatant (the tactical
// stats, badlands_sim.hpp) rides alongside it as its own component.
struct Attacks {
    Attack defs[kMaxAttacks]{};
    float cooldown_remaining[kMaxAttacks]{};
    int32_t count = 0;
};

// An in-flight ranged shot. Spawned by the ranged branch of the Attack command;
// flown and resolved by advance_projectiles. It captures the attacker's attack +
// tactical stats + the resolution seed axes AT FIRE TIME, so the hit lands
// correctly (and identically on replay) even if the shooter dies mid-flight.
struct Projectile {
    uint32_t attacker_slot = UINT32_MAX;
    uint32_t target_slot = UINT32_MAX;
    glm::vec2 pos{0.0f, 0.0f};
    float speed = 0.0f;
    Attack attack{};
    Combatant attacker{};
    int32_t attack_index = 0;
    int64_t fire_millis = 0;  // the world_millis seed axis, fixed at fire time
};

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
// arriving. Chatting refills `content` slowly and only up to a CEILING:
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

// What KIND of thing `e` is, derived from which archetype-defining state
// component it carries -- the single source of truth every consumer that
// needs to tell archetypes apart (sim.cpp's characters_of snapshot,
// entity_memory.cpp's perception scan) shares, so the mapping is defined
// exactly once: HeroSimulationState -> Hero, else CritterState -> Critter,
// else TaxCollectorState -> Townfolk, else Monster. Mirrors spawn_entity's
// own archetype switch (heroes.cpp) -- these components are what THAT recipe
// writes, so reading them back is exact, not a guess.
inline Archetype archetype_of(const entt::registry& reg, entt::entity e) {
    if (reg.all_of<HeroSimulationState>(e)) {
        return Archetype::Hero;
    }
    if (reg.all_of<CritterState>(e)) {
        return Archetype::Critter;
    }
    if (reg.all_of<TaxCollectorState>(e)) {
        return Archetype::Townfolk;
    }
    return Archetype::Monster;
}

}  // namespace badlands
