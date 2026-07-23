#pragma once

// WorldView -- an entity's read-only idea of the world for one tick, and the
// ONLY perception path a behaviour block sees. No block reads the registry;
// they read a WorldView. Two reasons that boundary is worth enforcing:
//
//   1. Fog of war (roadmap) inserts a visibility filter at exactly this seam --
//      if blocks reached around the view, every one would need revisiting.
//   2. Blocks stay unit-testable against a synthetic view with no sim.
//
// Modelled on scripts/brains/hero.noiser's flat WorldView: a superset struct
// whose fields are simply absent (has_* = false / zeroed) for archetypes that
// do not perceive them. A hero populates buildings/needs; a deer (later phase)
// populates threat/biome; the shared Roam/Flee blocks read the shared fields.

#include <cstdint>
#include <optional>

#include <glm/glm.hpp>

#include "badlands_sim.hpp"  // badlands::ActivityId (the shared id space)
#include "command.h"         // badlands::Command

namespace badlands {

// The name the sim internals have always used for the shared goal id space.
// There is exactly ONE such space -- the command log (SetBehavior.param_a), the
// snapshot (CharacterState.behavior), the statistics histogram, and any future
// noiser brain all speak it. Names + bands live in ActivityCatalog().
using Behavior = ActivityId;

struct WorldView {
    uint32_t slot = UINT32_MAX;
    glm::vec2 pos{0.0f, 0.0f};

    // --- needs (heroes) -----------------------------------------------------
    float fatigue = 0.0f;
    float boredom = 0.0f;
    int32_t inventory = 0;

    // --- clock --------------------------------------------------------------
    float tod = 0.0f;      // time of day in [0,1)
    bool night = false;
    // Quantized time window for a stable roam goal (world_millis / lease): the
    // roam target is re-drawn only when this changes, so a wanderer does not
    // jitter to a fresh point every tick (and plan_paths does not repath).
    int64_t roam_epoch = 0;
    // The wander goal, chosen in perception (observe_*): heroes draw it from an
    // rng ring, deer biome-filter it to Forest/Plains. The shared Roam block
    // just walks to it -- so map access stays in observe and the block stays
    // registry-free and testable.
    glm::vec2 roam_goal{0.0f, 0.0f};
    // True during the graze half of a critter's walk->graze cycle.
    bool grazing = false;

    // --- buildings this entity cares about (door tile + existence) ----------
    glm::vec2 home_door{0.0f, 0.0f};        bool has_home = false;
    glm::vec2 apothecary_door{0.0f, 0.0f};  bool has_apothecary = false;
    glm::vec2 tavern_door{0.0f, 0.0f};      bool has_tavern = false;

    // --- nearest perceived threat (Flee); populated from a later phase ------
    glm::vec2 threat_pos{0.0f, 0.0f};       bool has_threat = false;
    float threat_dist = 0.0f;

    // --- hunter: nearest prey (a critter within hunt sight) ----------------
    glm::vec2 prey_pos{0.0f, 0.0f};         bool has_prey = false;
    uint32_t prey_slot = UINT32_MAX;
    float prey_dist = 0.0f;
    float self_attack_range = 0.0f;         // the observer's own reach, to gate a shot

    // --- townfolk (tax collector) ------------------------------------------
    // Nearest unvisited building that still owes tax (the next stop on the
    // round); and the nearest place to bank the carry (Castle/Watchtower).
    glm::vec2 tax_target_door{0.0f, 0.0f};  bool has_tax_target = false;
    uint32_t tax_target_id = UINT32_MAX;
    glm::vec2 deposit_door{0.0f, 0.0f};     bool has_deposit = false;
};

// One tick's decision: which behaviour, where to walk, and an optional follow-up
// command (enter/buy/attack) the caller gates on arrival.
struct BehaviourResult {
    ActivityId id = ActivityId::Idle;
    glm::vec2 target{0.0f, 0.0f};
    std::optional<Command> follow_up;
    // If true, the caller enqueues follow_up only once the entity is within an
    // entrance radius of `target` (the door handlers re-validate authoritatively,
    // so emitting once on arrival keeps the whole walk out of the command log).
    bool follow_up_on_arrival = true;
};

}  // namespace badlands
