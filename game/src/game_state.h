// Internal game state shared between the sim (sim.cpp) and the noiser brain
// integration (brain.cpp). The public surface is game/include/badlands_sim.hpp.

#pragma once

#include "command.h"
#include "components.h"
#include "placement.h"
#include "vision.h"

#include "game/map/map_data.hpp"

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

    // Event-sourced command layer (see command.h). AI decisions are enqueued
    // during think and drained in one ordered apply pass per tick; every
    // applied command (player + AI) is appended to command_log (the trace).
    std::vector<badlands::Command> command_queue;
    std::vector<badlands::Command> command_log;

    // Transient game-event stream (see badlands::GameEvent). Notable things that
    // HAPPENED this tick-batch -- damage, downing, building destruction --
    // accumulated during tick and drained by the presentation layer each frame
    // (Sim::DrainEvents). NOT part of the determinism contract (never fed back
    // into the sim); cleared on drain.
    std::vector<badlands::GameEvent> events;

    // Replay mode. Non-null makes game_tick take this tick's decisions from the
    // log (by at_millis) instead of running the brains -- the determinism
    // contract made executable: (initial config, seed, command log) -> state.
    // The caller owns the log and must outlive the game.
    const std::vector<badlands::Command>* replay_log = nullptr;
    size_t replay_cursor = 0;

    // Day/night clock: integer milliseconds, advanced by kMillisPerTick each
    // tick (see components.h). Deterministic, no float drift.
    int64_t world_millis = 0;

    // The terrain/biome field the sim reasons about (deer roam Forest/Plains,
    // hunters seek Forest). Generated once in make_world; MapData is pure CPU
    // data with no engine/GPU dependency, so the sim owns it directly. Map-local
    // coordinates are world + size*0.5 -- use biome_at()/height_at() rather than
    // querying MapData directly so that offset lives in exactly one place.
    badlands::MapData map;

    // Behaviour tuning (see SimFactors). Defaults are compiled in; an app may
    // overwrite them from assets/creatures/factors.json before ticking.
    badlands::SimFactors factors;

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

// Reverse of entity_for_slot: the slot an entity occupies, or UINT32_MAX if it
// is not in the slot table. Linear scan (the slot table is small); used to tag
// game events with the entity-slot ids observers already speak (CharacterState.id).
uint32_t slot_for_entity(const BadlandsGame& game, entt::entity e);

// Appends one transient game event (see badlands::GameEvent) to game.events.
// The single choke point for the presentation-facing event stream.
void emit_event(BadlandsGame& game, const badlands::GameEvent& ev);

// Emits the event pair for one landed hit: always a DamageDealt, plus a lethal
// follow-up (HeroDowned / BuildingDestroyed) when `hp_after <= 0`. Callers own
// the hp mutation, cooldown, and razing; these only shape the events, so the
// event layout lives in one place. `pos`/`center` is the victim's world XZ.
void emit_char_hit(BadlandsGame& game, uint32_t actor_slot, uint32_t target_slot,
                   float amount, float hp_after, glm::vec2 pos);
void emit_building_hit(BadlandsGame& game, uint32_t actor_slot, uint32_t bid,
                       float amount, float hp_after, glm::vec2 center);

// Nearest living enemy of `self`, or entt::null.
entt::entity nearest_enemy(const BadlandsGame& game, entt::entity self);

}  // namespace badlands
