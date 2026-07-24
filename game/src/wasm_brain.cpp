#include "wasm_brain.h"

#include "brain_abi.h"
#include "command.h"
#include "components.h"
#include "entity_memory.h"
#include "game_state.h"
#include "town_brain.h"

#include <spdlog/spdlog.h>

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>

namespace badlands {

namespace {

// The wire's fixed-size arrays are sized against these compile-time caps
// (brain_abi.h); the packing loops below assume they match the native ones
// they copy from without re-checking per element.
static_assert(BL_MAX_THREATS == WorldView::kMaxThreats,
             "BlViewSuggest::threats and WorldView::threats caps must match");
static_assert(BL_MAX_ACTIVITIES == kActivityCount,
             "BlViewFactors::weights and ActivityWeights must share one id space");

// bl_log's host sink: forwards to spdlog with a "[brain]" prefix so wasm
// brain diagnostics land in the same log a human already watches. Level is
// the guest's own convention (0/1/2 -> info/warn/error); anything else is
// logged at warn with the raw level attached rather than dropped, since an
// out-of-range level is itself worth seeing.
void forward_log(int32_t level, const uint8_t* msg, size_t len, void* /*user*/) {
    const std::string_view text(reinterpret_cast<const char*>(msg), len);
    switch (level) {
        case 0:
            spdlog::info("[brain] {}", text);
            break;
        case 1:
            spdlog::warn("[brain] {}", text);
            break;
        case 2:
            spdlog::error("[brain] {}", text);
            break;
        default:
            spdlog::warn("[brain] (level {}) {}", level, text);
            break;
    }
}

// The single fail-fast enforcement point (see wasm_brain.h's policy note):
// every wasm-brain failure this file can detect -- a bh_load/bh_instantiate
// failure on provided wasm bytes, a nonzero bh_spawn/bh_tick, or
// decode_decision rejecting a wire -- routes through here. Logs `stage`
// ("load"/"instantiate"/"spawn"/"tick"/"decode"), `slot` when the failure is
// per-entity (std::nullopt for the load-time failures, which happen before
// any slot exists), and `detail` (typically bh_last_error()'s text), then
// aborts. There is deliberately no return path: a wasm brain crash is a
// crash-and-error scenario, not a downgrade -- the graceful containment this
// replaces was a workaround for noiser-era bugs and does not apply to the
// wasm host.
[[noreturn]] void brain_fatal(const char* stage, std::optional<uint32_t> slot,
                              const std::string& detail) {
    if (slot.has_value()) {
        spdlog::critical("[wasm-brain] FATAL stage={} slot={}: {}", stage, *slot, detail);
    } else {
        spdlog::critical("[wasm-brain] FATAL stage={}: {}", stage, detail);
    }
    std::abort();
}

// Packs BlViewWire from (a) the WorldView observe_hero returned, field for
// field into BlViewSelf/BlViewSuggest -- deliberately 1:1 with world_view.h,
// per that header's own field-by-field documentation; (b)
// game.factors.hero + this class's weights row into BlViewFactors; (c) the
// entity's EntityMemory chars into BlViewChars, slot-ascending (determinism:
// EntityMemory's own array order is not part of ITS contract, so packing
// must impose one).
BlViewWire pack_view_wire(const BadlandsGame& game, entt::entity e, const WorldView& view,
                          const ActivityWeights& weights) {
    BlViewWire wire{};
    wire.version = BL_ABI_VERSION;

    // --- self ---------------------------------------------------------------
    BlViewSelf& self = wire.self;
    self.world_millis = view.now_millis;
    self.think_until_millis = view.think_until_millis;
    self.roam_epoch = view.roam_epoch;
    self.slot = view.slot;
    self.class_id = game.registry.get<HeroCharacter>(e).hero_class;
    self.tod = view.tod;
    self.night = view.night ? 1u : 0u;
    self.pos_x = view.pos.x;
    self.pos_z = view.pos.y;
    self.health_frac = view.health_frac;
    self.fatigue = view.fatigue;
    self.content = view.content;
    self.inventory = view.inventory;
    self.attack_range = view.self_attack_range;
    self.current_activity = view.current_activity;

    // --- suggest --------------------------------------------------------------
    BlViewSuggest& sug = wire.suggest;
    sug.roam_goal_x = view.roam_goal.x;
    sug.roam_goal_z = view.roam_goal.y;
    sug.explore_goal_x = view.explore_goal.x;
    sug.explore_goal_z = view.explore_goal.y;
    sug.has_explore_goal = view.has_explore_goal ? 1u : 0u;
    sug.move_blocked = view.move_blocked ? 1u : 0u;
    sug.blocked_x = view.blocked_point.x;
    sug.blocked_z = view.blocked_point.y;
    sug.partner_x = view.partner_pos.x;
    sug.partner_z = view.partner_pos.y;
    sug.partner_dist = view.partner_dist;
    sug.partner_slot = view.partner_slot;
    sug.has_chat_partner = view.has_chat_partner ? 1u : 0u;
    sug.chatting = view.chatting ? 1u : 0u;
    sug.prey_x = view.prey_pos.x;
    sug.prey_z = view.prey_pos.y;
    sug.prey_dist = view.prey_dist;
    sug.prey_slot = view.prey_slot;
    sug.has_prey = view.has_prey ? 1u : 0u;
    sug.home_x = view.home_door.x;
    sug.home_z = view.home_door.y;
    sug.has_home = view.has_home ? 1u : 0u;
    sug.apothecary_x = view.apothecary_door.x;
    sug.apothecary_z = view.apothecary_door.y;
    sug.has_apothecary = view.has_apothecary ? 1u : 0u;
    sug.tavern_x = view.tavern_door.x;
    sug.tavern_z = view.tavern_door.y;
    sug.has_tavern = view.has_tavern ? 1u : 0u;
    sug.threat_count = view.threat_count;
    for (int32_t i = 0; i < view.threat_count; ++i) {
        const PerceivedThreat& t = view.threats[i];
        sug.threats[i] = BlThreat{t.pos.x, t.pos.y, t.dist, t.slot};
    }
    // Not part of BlViewSuggest: view.grazing (critter-only) and the
    // townfolk tax_target/deposit fields -- neither applies to a hero brain
    // (see brain_abi.h's BlViewSuggest doc comment).

    // --- factors: this hero's class weights row + the hero-decision scalars ---
    BlViewFactors& f = wire.factors;
    const HeroFactors& hf = game.factors.hero;
    f.think_min_millis = hf.think_min_millis;
    f.think_max_millis = hf.think_max_millis;
    for (int32_t i = 0; i < BL_MAX_ACTIVITIES; ++i) {
        f.weights[i] = weights.w[i];
    }
    f.fatigue_seek = hf.fatigue_seek;
    f.fatigue_seek_night = hf.fatigue_seek_night;
    f.low_health_rest = hf.low_health_rest;
    f.content_seek = hf.content_seek;
    f.chat_content_seek = hf.chat_content_seek;
    f.chat_radius = hf.chat_radius;
    f.explore_min_fatigue = hf.explore_min_fatigue;
    // Deliberately excluded (see brain_abi.h's BlViewFactors doc comment):
    // perception-only factors (radii, drain/fill rates, lease windows) --
    // those stay host-side, read by observe_hero above, not the brain.

    // --- chars: EntityMemory, slot-ascending for determinism -------------------
    const EntityMemory& mem = game.registry.get<EntityMemory>(e);
    std::array<const MemoryChar*, BL_MAX_CHARS> ordered{};
    const int32_t n = mem.char_count;
    for (int32_t i = 0; i < n; ++i) {
        ordered[i] = &mem.chars[i];
    }
    std::sort(ordered.begin(), ordered.begin() + n,
              [](const MemoryChar* a, const MemoryChar* b) { return a->slot < b->slot; });
    wire.char_count = n;
    for (int32_t i = 0; i < n; ++i) {
        const MemoryChar& mc = *ordered[i];
        wire.chars[i] = BlViewChar{mc.last_seen_millis,
                                   mc.slot,
                                   mc.archetype,
                                   mc.team,
                                   mc.last_pos.x,
                                   mc.last_pos.y,
                                   mc.last_hp,
                                   mc.visible_now ? 1u : 0u,
                                   /*_pad=*/0u};
    }

    return wire;
}

// BL_CMD_* (brain_abi.h) -> a real Command. NONE and any id this host
// version does not recognize both decode to "no follow-up" -- an unknown
// command from a forward-newer brain must not be misinterpreted as some
// other command, only ignored.
std::optional<Command> decode_command(const BlDecisionWire& out, uint32_t slot) {
    switch (out.command_kind) {
        case BL_CMD_NONE:
        default:
            return std::nullopt;
        case BL_CMD_ATTACK:
            return Command{CommandKind::Attack, slot};
        case BL_CMD_BUY:
            return Command{CommandKind::Buy, slot};
        case BL_CMD_ENTER:
            return Command{CommandKind::EnterBuilding, slot, UINT32_MAX, {0.0f, 0.0f},
                          out.command_arg};
        case BL_CMD_ENTER_HOME:
            return Command{CommandKind::EnterHome, slot};
        case BL_CMD_SHOOT:
            // act_hunt's follow-up (Command{CommandKind::Shoot, v.slot, v.prey_slot}):
            // target_id = command_arg (the prey slot).
            return Command{CommandKind::Shoot, slot, static_cast<uint32_t>(out.command_arg)};
        case BL_CMD_CHAT:
            // act_chat's follow-up (Command{CommandKind::Chat, v.slot, v.partner_slot}):
            // target_id = command_arg (the chat partner's slot).
            return Command{CommandKind::Chat, slot, static_cast<uint32_t>(out.command_arg)};
    }
}

}  // namespace

// BlDecisionWire -> BrainDecision (town_brain.h). goal_kind == 0 ("none")
// decodes to holding the hero's current position -- the same target Idle
// commits to on the C++ path (behaviours/blocks.cpp's act_idle) -- since
// apply_brain_decision's commit branch always states an explicit goal.
//
// Wire trust boundary: `out` came back through bh_tick from the guest's own
// linear memory, so its fields are untrusted input regardless of how well
// wasm_brain.cpp trusts the rest of the pipeline -- a buggy or adversarial
// module can write anything to bl_out_buf(). Every field that feeds
// downstream math/indexing without further validation is checked here,
// before anything else touches it:
//  - a non-finite goal coordinate would otherwise propagate into
//    MoveTo/distance math (apply_brain_decision, town_brain.cpp);
//  - an out-of-range activity_id would be cast to ActivityId and used to
//    index ActivityWeights::w / ActivityHistogram::total_ (kActivityCount-
//    sized arrays) with no bounds check of their own;
//  - an out-of-range pause_kind, or a pause_duration_millis that violates
//    its kind's contract (kind==1: 0 < duration <= factors.hero.
//    think_max_millis; kind==2: duration == 0), would otherwise reach
//    command.cpp's enqueue_set_behavior, whose int64_t duration_millis
//    parameter narrows into Command::param_b (int32_t) via
//    static_cast<int32_t> -- the upper bound against think_max_millis
//    (currently 833, always far under INT32_MAX) is what keeps that
//    narrowing lossless, not merely policy-compliant.
//
// A pure function: a violation returns std::nullopt with no side effect of
// its own (no report_bug -- that went away with the graceful-containment
// machinery). Under the fail-fast policy (wasm_brain.h's policy note;
// docs/superpowers/specs/2026-07-23-wasm-brain-contract-design.md's Runtime
// section) a rejected wire is a brain bug, and it is the CALLER
// (tick_wasm_brain) that escalates a std::nullopt to brain_fatal.
std::optional<BrainDecision> decode_decision(BadlandsGame& game, const BlDecisionWire& out,
                                              uint32_t slot, glm::vec2 self_pos) {
    if (!std::isfinite(out.goal_x) || !std::isfinite(out.goal_z)) {
        return std::nullopt;
    }
    if (out.activity_id < 0 || out.activity_id >= kActivityCount) {
        return std::nullopt;
    }
    if (out.pause_kind != 0 && out.pause_kind != 1 && out.pause_kind != 2) {
        return std::nullopt;
    }
    if (out.pause_kind == 1 && (out.pause_duration_millis <= 0 ||
                                out.pause_duration_millis > game.factors.hero.think_max_millis)) {
        return std::nullopt;
    }
    if (out.pause_kind == 2 && out.pause_duration_millis != 0) {
        return std::nullopt;
    }

    BrainDecision d;
    d.activity = static_cast<ActivityId>(out.activity_id);
    d.goal = (out.goal_kind == 1) ? glm::vec2{out.goal_x, out.goal_z} : self_pos;
    d.follow_up = decode_command(out, slot);
    d.follow_up_on_arrival = out.follow_up_on_arrival != 0;
    d.pause = out.pause_kind != 0;
    d.pause_duration_millis = (out.pause_kind == 1) ? out.pause_duration_millis : 0;
    return d;
}

std::unique_ptr<WasmBrainRuntime> WasmBrainRuntime::create(const uint8_t* wasm_bytes, size_t len) {
    BhProgram* program = bh_load(wasm_bytes, len);
    if (program == nullptr) {
        brain_fatal("load", std::nullopt,
                    std::string("bh_load failed: ") + bh_last_error() +
                        " (truncated or invalid wasm -- is git-lfs initialized?)");
    }
    // world_seed 0: world gen is currently seedless/static
    // (SymbolicMapGenerator is a pure function of its compile-time constants
    // -- see sim.cpp's make_world), so there is no seed to thread through yet.
    BhInstance* instance =
        bh_instantiate(program, BL_ABI_VERSION, /*world_seed=*/0, &forward_log, nullptr);
    if (instance == nullptr) {
        brain_fatal("instantiate", std::nullopt,
                    std::string("bh_instantiate failed: ") + bh_last_error());
    }
    auto runtime = std::make_unique<WasmBrainRuntime>();
    runtime->program = program;
    runtime->instance = instance;
    return runtime;
}

WasmBrainRuntime::~WasmBrainRuntime() {
    bh_drop_instance(instance);
    bh_drop_program(program);
}

void tick_wasm_brain(BadlandsGame& game, uint32_t slot) {
    WasmBrainRuntime& runtime = *game.wasm_brains;
    entt::entity e = entity_for_slot(game, static_cast<int32_t>(slot));
    if (e == entt::null) {
        return;  // caller (sim.cpp) already validated this; defensive no-op
    }

    if (slot >= runtime.spawned.size()) {
        runtime.spawned.resize(slot + 1, false);
    }
    if (!runtime.spawned[slot]) {
        const int32_t cls = game.registry.get<HeroCharacter>(e).hero_class;
        // seed = slot + 1: distinct per hero, never 0 (bl_spawn's seed is not
        // itself required to avoid 0, but the sim's own draws treat 0 as "no
        // seed" -- see behaviours/rng.h -- so this stays consistent with that
        // convention rather than relying on the guest to guard against it).
        const int32_t rc = bh_spawn(runtime.instance, static_cast<int32_t>(slot), cls,
                                    static_cast<int32_t>(slot) + 1);
        if (rc != BH_OK) {
            brain_fatal("spawn", slot, std::string("bh_spawn failed: ") + bh_last_error());
        }
        runtime.spawned[slot] = true;
    }

    const ActivityWeights& weights = weights_for(game, e);
    const WorldView view = observe_hero(game, slot, e, weights);
    const BlViewWire wire = pack_view_wire(game, e, view, weights);

    BlDecisionWire out{};
    const int32_t rc =
        bh_tick(runtime.instance, static_cast<int32_t>(slot), reinterpret_cast<const uint8_t*>(&wire),
               sizeof(wire), reinterpret_cast<uint8_t*>(&out), sizeof(out));
    if (rc != BH_OK) {
        brain_fatal("tick", slot, std::string("bh_tick failed: ") + bh_last_error());
    }

    // decode_decision is pure (no side effect of its own -- see its doc
    // comment); a std::nullopt here is a brain bug under the fail-fast
    // policy, so this is the escalation point.
    const std::optional<BrainDecision> decision = decode_decision(game, out, slot, view.pos);
    if (!decision.has_value()) {
        brain_fatal("decode", slot,
                    "decode_decision rejected the wire (invalid goal/activity/pause fields)");
    }

    // script_intents counts intents actually DELIVERED to the sim this tick
    // (mirrors what it counted for the noiser path): apply_brain_decision
    // returns false for a pause-CONTINUE (enqueues nothing), true for a
    // commit or a pause-START.
    if (apply_brain_decision(game, slot, view.pos, *decision)) {
        ++game.script_intents;
    }
}

}  // namespace badlands
