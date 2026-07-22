// Event-sourced command layer: the single, deterministic mutation point for
// game mechanics. Player actions and AI decisions are both Commands. Player
// commands apply synchronously (game_dispatch) and AI commands are enqueued
// during the brain-think pass and drained in one ordered apply pass per tick;
// every applied command is appended to BadlandsGame::command_log (the trace /
// replay input). Handlers reuse the placement/heroes bodies behind one switch.

#pragma once

#include "badlands_sim.hpp"

#include <glm/glm.hpp>

#include <cstdint>

struct BadlandsGame;

namespace badlands {

enum class CommandKind : int32_t {
    // Player (mirrors the legacy GameActionKind values).
    PlaceBuilding,
    RecruitHero,
    DestroyBuilding,
    // AI / unit (was the intent_* host-call side effects).
    MoveTo,
    EnterBuilding,
    EnterHome,
    Buy,
    Attack,
    SetBehavior,
    CollectTax,  // tax collector banks a building's taxable_income into its carry
    Deposit,     // tax collector banks its carry into player gold, then despawns
    AttackBuilding,  // monster swings at a building (target_id); razes it at 0 hp
};

// The log is exposed verbatim through Sim::CommandLog(), so the two enums are
// one id space.
static_assert(static_cast<int32_t>(CommandKind::PlaceBuilding) ==
              static_cast<int32_t>(CommandKindId::PlaceBuilding));
static_assert(static_cast<int32_t>(CommandKind::RecruitHero) ==
              static_cast<int32_t>(CommandKindId::RecruitHero));
static_assert(static_cast<int32_t>(CommandKind::DestroyBuilding) ==
              static_cast<int32_t>(CommandKindId::DestroyBuilding));
static_assert(static_cast<int32_t>(CommandKind::MoveTo) ==
              static_cast<int32_t>(CommandKindId::MoveTo));
static_assert(static_cast<int32_t>(CommandKind::EnterBuilding) ==
              static_cast<int32_t>(CommandKindId::EnterBuilding));
static_assert(static_cast<int32_t>(CommandKind::EnterHome) ==
              static_cast<int32_t>(CommandKindId::EnterHome));
static_assert(static_cast<int32_t>(CommandKind::Buy) ==
              static_cast<int32_t>(CommandKindId::Buy));
static_assert(static_cast<int32_t>(CommandKind::Attack) ==
              static_cast<int32_t>(CommandKindId::Attack));
static_assert(static_cast<int32_t>(CommandKind::SetBehavior) ==
              static_cast<int32_t>(CommandKindId::SetBehavior));
static_assert(static_cast<int32_t>(CommandKind::CollectTax) ==
              static_cast<int32_t>(CommandKindId::CollectTax));
static_assert(static_cast<int32_t>(CommandKind::Deposit) ==
              static_cast<int32_t>(CommandKindId::Deposit));
static_assert(static_cast<int32_t>(CommandKind::AttackBuilding) ==
              static_cast<int32_t>(CommandKindId::AttackBuilding));

// One command. `actor` is the acting entity slot (UINT32_MAX = player/global);
// `target_id` is a building/entity id; `point` is world XZ for positional
// commands; `param_a`/`param_b` carry kind-specific scalars (e.g. building kind
// + rotation for PlaceBuilding, building kind for EnterBuilding).
struct Command {
    CommandKind kind;
    uint32_t actor = UINT32_MAX;
    uint32_t target_id = UINT32_MAX;
    glm::vec2 point{0.0f, 0.0f};
    int32_t param_a = 0;
    int32_t param_b = 0;
    // Stamped by apply_command from game.world_millis. Producers leave it 0; it
    // is what makes the log self-describing (and replayable at tick boundaries).
    int64_t at_millis = 0;
};

// Applies one command (the single mutation point) and appends it to
// game.command_log. Return value is kind-specific: PlaceBuilding/RecruitHero
// return the new id or -1; DestroyBuilding returns 0 or <0; the unit commands
// return 0 (applied) — they never fail the caller, they just no-op if invalid.
int64_t apply_command(BadlandsGame& game, const Command& cmd);

// Drains game.command_queue in FIFO order through apply_command (the AI pass).
void apply_commands(BadlandsGame& game);

// --- edge-triggered producers (shared by the C++ and noiser brain paths) ----
// Brains re-decide every tick, but re-stating an unchanged decision is not a
// decision: it bloats the log (the debug trace of what was DECIDED) without
// changing state. These enqueue only when the request differs from what the
// entity already has. Both read components that replay reproduces exactly, so a
// live run and its replay emit identical command streams.
void enqueue_move_to(BadlandsGame& game, uint32_t slot, glm::vec2 target);
void enqueue_set_behavior(BadlandsGame& game, uint32_t slot, int32_t behavior);

// Replay: enqueues + applies every command in game.replay_log stamped at or
// before the current game.world_millis, advancing game.replay_cursor. game_tick
// calls this INSTEAD of the brain-think pass when a replay log is set, which is
// what makes (initial config, seed, command log) -> state reproducible: no
// decision is re-derived, they are all replayed at the tick they were made.
void apply_replay_commands(BadlandsGame& game);

}  // namespace badlands
