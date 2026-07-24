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
// module can write anything to bl_out_buf(). Two fields feed straight into
// downstream math/indexing without further validation, so they are checked
// here, before anything else touches them: a non-finite goal coordinate
// would otherwise propagate into MoveTo/distance math (apply_brain_decision,
// town_brain.cpp), and an out-of-range activity_id would be cast to
// ActivityId and used to index ActivityWeights::w /
// ActivityHistogram::total_ (kActivityCount-sized arrays) with no bounds
// check of their own. Both reject the same way a script/trap error does:
// report_bug once, no decoded decision (the caller applies no commands this
// tick; the entity idles and the wire is re-read fresh next tick).
std::optional<BrainDecision> decode_decision(BadlandsGame& game, const BlDecisionWire& out,
                                              uint32_t slot, glm::vec2 self_pos) {
    if (!std::isfinite(out.goal_x) || !std::isfinite(out.goal_z)) {
        report_bug(game, "wasm_decode", "non-finite goal_x/goal_z in wasm decision wire");
        return std::nullopt;
    }
    if (out.activity_id < 0 || out.activity_id >= kActivityCount) {
        report_bug(game, "wasm_decode",
                   "activity_id " + std::to_string(out.activity_id) +
                       " out of range in wasm decision wire");
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

std::unique_ptr<WasmBrainRuntime> WasmBrainRuntime::create(const uint8_t* wasm_bytes, size_t len,
                                                            std::string& out_error) {
    BhProgram* program = bh_load(wasm_bytes, len);
    if (program == nullptr) {
        out_error = bh_last_error();
        return nullptr;
    }
    // world_seed 0: world gen is currently seedless/static
    // (SymbolicMapGenerator is a pure function of its compile-time constants
    // -- see sim.cpp's make_world), so there is no seed to thread through yet.
    BhInstance* instance =
        bh_instantiate(program, BL_ABI_VERSION, /*world_seed=*/0, &forward_log, nullptr);
    if (instance == nullptr) {
        out_error = bh_last_error();
        bh_drop_program(program);
        return nullptr;
    }
    auto runtime = std::make_unique<WasmBrainRuntime>();
    runtime->program = program;
    runtime->instance = instance;
    runtime->instantiation_count = 1;
    return runtime;
}

WasmBrainRuntime::~WasmBrainRuntime() {
    bh_drop_instance(instance);
    bh_drop_program(program);
}

namespace {

// brainhost.h: "a BhInstance that has trapped (BH_ERR_TRAP/BH_ERR_FUEL) is
// not reused -- drop it ... and bh_instantiate a fresh one from the same
// BhProgram". Called after `rc` has already been report_bug'd by the
// caller; no-ops for every other error code (BH_ERR_SCRIPT/BH_ERR_ARGS/
// BH_ERR_PANIC do not invalidate the instance).
//
// On success, `runtime.instance`/`spawned` are updated in place. On a
// re-instantiation failure there is no usable wasm runtime left, so
// `game.wasm_brains` itself is reset to null -- after this call returns,
// `runtime` may be a dangling reference to the just-destroyed object, so
// the caller must not touch it again (tick_wasm_brain always returns
// immediately after calling this, which is what makes that safe).
void reinstantiate_if_trapped(BadlandsGame& game, WasmBrainRuntime& runtime, int32_t rc) {
    if (rc != BH_ERR_TRAP && rc != BH_ERR_FUEL) {
        return;
    }
    bh_drop_instance(runtime.instance);
    runtime.instance = nullptr;
    BhInstance* fresh =
        bh_instantiate(runtime.program, BL_ABI_VERSION, /*world_seed=*/0, &forward_log, nullptr);
    if (fresh == nullptr) {
        report_bug(game, "wasm_reinit", std::string("bh_instantiate failed: ") + bh_last_error());
        game.wasm_brains.reset();  // dead runtime: the think loop falls back to mock
        return;
    }
    runtime.instance = fresh;
    ++runtime.instantiation_count;
    std::fill(runtime.spawned.begin(), runtime.spawned.end(), false);
}

}  // namespace

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
            report_bug(game, "wasm_spawn",
                       std::string("bh_spawn failed: ") + bh_last_error());
            reinstantiate_if_trapped(game, runtime, rc);
            return;  // not marked spawned: retried next tick
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
        report_bug(game, "wasm_tick", std::string("bh_tick failed: ") + bh_last_error());
        reinstantiate_if_trapped(game, runtime, rc);
        return;  // no commands this tick: the hero idles, retried next tick
    }

    // decode_decision report_bug's ("wasm_decode") and returns nullopt itself
    // on a wire trust-boundary violation (non-finite goal, out-of-range
    // activity_id) -- same containment as the bh_spawn/bh_tick failures
    // above: no commands this tick, retried next tick against a fresh wire.
    const std::optional<BrainDecision> decision = decode_decision(game, out, slot, view.pos);
    if (!decision.has_value()) {
        return;
    }

    // One applied decision, successful or not a no-op (mirrors what
    // script_intents counted for the noiser path: an intent actually
    // delivered to the sim this tick).
    ++game.script_intents;
    apply_brain_decision(game, slot, view.pos, *decision);
}

}  // namespace badlands
