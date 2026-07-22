// Internal game state shared between the sim (game.cpp) and the noiser brain
// integration (brain.cpp). The public surface is game/include/badlands_game.h.

#pragma once

#include "command.h"
#include "components.h"
#include "placement.h"

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

    // Pluggable path-geometry provider (Rust nav service); zero-initialized
    // means "no provider" -> straight-line fallback in the movement pipeline.
    GamePathfinder pathfinder{};

    // Event-sourced command layer (see command.h). AI decisions are enqueued
    // during think and drained in one ordered apply pass per tick; every
    // applied command (player + AI) is appended to command_log (the trace).
    std::vector<badlands::Command> command_queue;
    std::vector<badlands::Command> command_log;

    // Replay mode. Non-null makes game_tick take this tick's decisions from the
    // log (by at_millis) instead of running the brains -- the determinism
    // contract made executable: (initial config, seed, command log) -> state.
    // The caller owns the log and must outlive the game.
    const std::vector<badlands::Command>* replay_log = nullptr;
    size_t replay_cursor = 0;

    // Day/night clock: integer milliseconds, advanced by kMillisPerTick each
    // tick (see components.h). Deterministic, no float drift.
    int64_t world_millis = 0;

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
