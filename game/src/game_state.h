// Internal game state shared between the sim (sim.cpp) and the noiser brain
// integration (brain.cpp). The public surface is game/include/badlands_sim.hpp.

#pragma once

#include "components.h"
#include "placement.h"
#include "vision.h"

#include <entt/entt.hpp>

#include <cstdint>
#include <memory>
#include <vector>

namespace badlands {
struct BrainRuntime;
}

struct BadlandsGame {
    entt::registry registry;
    // Entity id (as seen by the C API and by scripts) -> entity. Slots are
    // never reused; dead entities leave entt::null behind.
    std::vector<entt::entity> slots;
    // Compiled brain program + host bindings; null -> mock brains only.
    std::unique_ptr<badlands::BrainRuntime> brains;

    // World state (the sim owns gold and the building/placement grid).
    badlands::PlacementState placement;
    uint32_t gold = 1000;

    // Fog-of-war visibility field (double-buffered; resolved each tick from the
    // player's vision sources). Unconfigured until Sim::ConfigureVision.
    badlands::VisionGrid vision;

    // Pluggable path-geometry provider (Rust nav service); zero-initialized
    // means "no provider" -> straight-line fallback in the movement pipeline.
    badlands::Pathfinder pathfinder{};

    uint64_t ticks = 0;
    uint64_t script_intents = 0;
    uint32_t noiser_bugs = 0;

    ~BadlandsGame();
};

namespace badlands {

// Loud, grep-able failure record; every call permanently costs the entity its
// script brain (the caller downgrades) and bumps the counter tests assert on.
void report_bug(BadlandsGame& game, const char* stage, const std::string& message);

entt::entity entity_for_slot(const BadlandsGame& game, int32_t slot);

// Nearest living enemy of `self`, or entt::null.
entt::entity nearest_enemy(const BadlandsGame& game, entt::entity self);

}  // namespace badlands
