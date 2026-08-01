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
    Chat,            // two heroes start a conversation (target_id = partner slot)
    Engage,      // hold at range of a live entity target (target_id; point.x = stop_distance)
    UseSkill,    // cast skill param_a (an index into the ACTOR's own Skills, not a
                 // SkillId -- the same convention Attack's param_a uses for attacks)
                 // at target_id; the handler re-validates everything and runs the
                 // effect through game/src/skill_cast.h
    CancelFocus, // ABANDON a long cast in progress. Logged for exactly the same
                 // reason FocusSkill is: a replay never thinks, so a cancel
                 // performed only inside apply_intention would happen live and
                 // never on replay -- and the shot the live run did NOT fire
                 // would land in the replay, which is the sharpest divergence
                 // this contract can produce.
    FocusSkill,  // BEGIN a long cast of skill param_a at target_id: the same index
                 // convention, on the other channel (game/src/skill_focus.h).
                 //
                 // A Command rather than a direct begin_focus call inside
                 // apply_intention, and that is load-bearing: a REPLAY never
                 // thinks (apply_replay_commands runs INSTEAD of the think pass),
                 // so a focus created only at adoption would exist live and never
                 // on replay, and its shot would silently go missing. Logging the
                 // start is what makes the whole two seconds reproducible -- the
                 // RESOLUTION needs no command of its own, being a timer over
                 // derived state that advance_focus re-runs identically either way.
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
static_assert(static_cast<int32_t>(CommandKind::Chat) ==
              static_cast<int32_t>(CommandKindId::Chat));
static_assert(static_cast<int32_t>(CommandKind::Engage) ==
              static_cast<int32_t>(CommandKindId::Engage));
static_assert(static_cast<int32_t>(CommandKind::UseSkill) ==
              static_cast<int32_t>(CommandKindId::UseSkill));

// One command. `actor` is the acting entity slot (UINT32_MAX = player/global);
// `target_id` is a building/entity id; `point` is world XZ for positional
// commands; `param_a`/`param_b` carry kind-specific scalars (e.g. building kind
// + rotation for PlaceBuilding, building kind for EnterBuilding; for Attack,
// param_a is the attack index -- -1 = auto-pick (select_attack's usual
// tie-break), any other value names that index exactly, re-validated
// authoritatively by fire_attack). param_a's own default is -1 (Finding 2026-
// 07-29 review fix): a bare `{CommandKind::Attack, actor, target}` with no
// param_a named therefore auto-picks, the same as an explicit -1 -- restoring
// the pre-command-log raw idiom's meaning. Kinds that do not read param_a at
// all (MoveTo, EnterHome, Buy, Chat, CollectTax, Deposit, AttackBuilding,
// Engage) are unaffected either way.
struct Command {
    CommandKind kind;
    uint32_t actor = UINT32_MAX;
    uint32_t target_id = UINT32_MAX;
    glm::vec2 point{0.0f, 0.0f};
    int32_t param_a = -1;
    int32_t param_b = 0;
    // Stamped by apply_command from game.world_ticks. Producers leave it 0; it
    // is what makes the log self-describing (and replayable at tick boundaries).
    int64_t at_ticks = 0;
};

// Applies one command (the single mutation point) and appends it to
// game.command_log. Return value is kind-specific: PlaceBuilding/RecruitHero
// return the new id or -1; DestroyBuilding returns 0 or <0; the unit commands
// return 0 (applied) — they never fail the caller, they just no-op if invalid.
int64_t apply_command(BadlandsGame& game, const Command& cmd);

// Drains game.command_queue in FIFO order through apply_command (the AI pass).
void apply_commands(BadlandsGame& game);

// --- edge-triggered producers ----
// Brains re-decide every tick, but re-stating an unchanged decision is not a
// decision: it bloats the log (the debug trace of what was DECIDED) without
// changing state. These enqueue only when the request differs from what the
// entity already has. Both read components that replay reproduces exactly, so a
// live run and its replay emit identical command streams.
void enqueue_move_to(BadlandsGame& game, uint32_t slot, glm::vec2 target);
// `duration_ticks` rides along on SetBehavior: for the intention contract
// (game/src/intention.h's apply_intention) it is the wake schedule (Idle's
// own duration, or the idle hint for any other kind). Putting it IN the
// command is what makes it replayable -- a replay does not re-draw/re-derive
// it, it reads the one the live run logged (the SetBehavior command handler,
// command.cpp, derives CurrentIntention::wake_at_ticks from it too, so a
// replay reconstructs the schedule from the log alone). Unused (0) for
// critter/townfolk callers below, which never carry a CurrentIntention to
// schedule a wake for; formerly also carried the C++ hero decision layer's
// own deliberation pause (SetBehavior(Think, duration)), but that layer --
// and every producer of ActivityId::Think -- is gone (command.cpp's
// SetBehavior handler has the full account).
//
// `force`: skip the edge-trigger (re-stating an unchanged `behavior` is
// normally not a decision, so no command is enqueued) -- set true when a
// wake's schedule must reach the log even though the activity label happens
// to repeat (apply_intention's call: every ADOPTED intention is a real
// decision, sparse by construction via should_wake, so there is no per-tick
// log-bloat concern to trade against here the way there is for a per-tick
// re-decision).
void enqueue_set_behavior(BadlandsGame& game, uint32_t slot, int32_t behavior,
                          int64_t duration_ticks = 0, bool force = false);

// Sets/maintains MoveTarget as Kind::Entity toward `target_slot`, holding at
// `stop_distance` (the caller's engagement_range) -- unlike enqueue_move_to's
// Kind::Point, this tracks the target's LIVE position every plan_paths pass
// (movement.cpp) rather than a one-shot snapshot, and plan_paths' own
// `distance(pos, live_target_pos) <= stop_distance` gate stops the approach
// AT that live distance -- no arrival-radius slop the way walking onto a
// precomputed offset point would carry (kArriveRadius, movement.h). This is
// the single-gateway combat engagement executor's mutation point
// (apply_intention's Attack case, game/src/intention.h) -- a LOGGED,
// replay-safe successor to the deleted combat_preempt's direct MoveTarget
// write (docs/superpowers/specs/2026-07-25-contract-v3-alignment-design.md
// §2). No-op if `slot` or `target_slot` names no live entity. Edge-triggered
// like enqueue_move_to above: re-stating the same target at the same range
// is not a new decision.
void enqueue_engage(BadlandsGame& game, uint32_t slot, uint32_t target_slot,
                    float stop_distance);

// Replay: enqueues + applies every command in game.replay_log stamped at or
// before the current game.world_ticks, advancing game.replay_cursor. game_tick
// calls this INSTEAD of the brain-think pass when a replay log is set, which is
// what makes (initial config, seed, command log) -> state reproducible: no
// decision is re-derived, they are all replayed at the tick they were made.
void apply_replay_commands(BadlandsGame& game);

}  // namespace badlands
