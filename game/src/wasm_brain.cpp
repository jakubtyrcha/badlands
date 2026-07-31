#include "wasm_brain.h"

#include "brain_abi.h"
#include "components.h"
#include "entity_memory.h"
#include "game_state.h"
#include "hero_perception.h"  // observe_hero/weights_for/WorldView/ActivityWeights/kActivityCount
#include "skills.h"           // evaluate_skill_triggers -- the skills block's ready/recommended
#include "status.h"        // remaining_millis_of -- BL_ST_STUNNED / BL_ST_DISENGAGED
#include "threat_table.h"  // threat_of -- both sides of the fight-or-flee comparison
#include "combat.h"        // melee_range / ranged_range -- a threat's reach on the wire

#include <spdlog/spdlog.h>

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <limits>
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
static_assert(BL_MAX_EVENTS == kInboxCapacity,
             "BlEvent[] and EventInbox must share one capacity -- events are copied 1:1, no cap "
             "re-check per element below");
static_assert(BL_MAX_ATTACKS == kMaxAttacks,
             "BlViewAttack[] and Attacks::defs/cooldown_remaining must share one capacity -- "
             "attacks are copied 1:1, no cap re-check per element below");
static_assert(BL_MAX_SKILLS == kMaxSkills,
             "BlViewSkill[] and Skills::ids/cooldown_remaining must share one capacity -- the "
             "wire index IS the Skills index (BL_ACT_USE_SKILL's arg), so they cannot differ");

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

// bl_enqueue_action's host sink (brainhost.h's BhActionFn): appends to the
// owning WasmBrainRuntime's pending_actions, in call order. `user` is the
// WasmBrainRuntime* fixed at bh_instantiate time (see WasmBrainRuntime::create
// below) -- stable for the instance's whole lifetime, so this cast is safe
// regardless of which wake's bh_tick call is currently in progress.
// LIVE against the shipping brain: hero.nim enqueues one BL_ACT_ATTACK per
// combat wake, and tick_wasm_brain drains pending_actions through
// resolve_action (game/src/intention.h) right after the intention applies.
void forward_action(int32_t kind, uint32_t target_slot, int32_t arg, void* user) {
    auto* runtime = static_cast<WasmBrainRuntime*>(user);
    runtime->pending_actions.push_back(PendingAction{kind, target_slot, arg});
}

// The single fail-fast enforcement point (see wasm_brain.h's policy note):
// every wasm-brain failure this file can detect -- a bh_load/bh_instantiate
// failure on provided wasm bytes, a nonzero bh_spawn/bh_tick, or
// decode_suggestion rejecting a wire -- routes through here. Logs `stage`
// ("load"/"instantiate"/"spawn"/"tick"/"decode"), `slot` when the failure is
// per-entity (std::nullopt for the load-time failures, which happen before
// any slot exists), and `detail` (typically bh_last_error()'s text), then
// aborts. There is deliberately no return path: a wasm brain crash is a
// crash-and-error scenario, not a downgrade.
[[noreturn]] void brain_fatal(const char* stage, std::optional<uint32_t> slot,
                              const std::string& detail) {
    if (slot.has_value()) {
        spdlog::critical("[wasm-brain] FATAL stage={} slot={}: {}", stage, *slot, detail);
    } else {
        spdlog::critical("[wasm-brain] FATAL stage={}: {}", stage, detail);
    }
    std::abort();
}

}  // namespace

// Packs BlViewWire from (a) the WorldView observe_hero returned, field for
// field into BlViewSelf/BlViewSuggest -- deliberately 1:1 with world_view.h,
// per that header's own field-by-field documentation, plus the
// CurrentIntention summary (self.intention_kind/intention_wake_at); (b)
// game.factors.hero + this class's weights row into BlViewFactors (v2: minus
// think_min/think_max -- deliberation is gone); (c) statuses, assembled from
// the components a hero MIGHT carry right now (ChattingState/MeleeLock/
// InsideBuilding) -- advisory only this slice, per brain_abi.h's BL_ST_* doc;
// (d) the entity's Attacks component, copied 1:1 into attacks[] (v3 -- a
// brain cannot pick an attack it cannot see); (e) the entity's EventInbox,
// copied 1:1 into events[]; (f) the entity's EntityMemory chars into
// BlViewChars, slot-ascending (determinism: EntityMemory's own array order is
// not part of ITS contract, so packing must impose one).
BlViewWire pack_view_wire(const BadlandsGame& game, entt::entity e, const WorldView& view,
                          const ActivityWeights& weights) {
    BlViewWire wire{};
    wire.version = BL_ABI_VERSION;

    // --- self ---------------------------------------------------------------
    BlViewSelf& self = wire.self;
    self.world_millis = view.now_millis;
    self.think_until_millis = view.think_until_millis;
    self.roam_epoch = view.roam_epoch;
    const CurrentIntention& ci = game.registry.get<CurrentIntention>(e);
    self.intention_wake_at = ci.wake_at_millis;
    self.slot = view.slot;
    // v5: this hero's own combat potential, the other half of the comparison
    // BlThreat::threat enables (game/src/threat_table.h).
    self.threat = threat_of(game.registry, e);
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
    self.intention_kind = static_cast<int32_t>(ci.kind);

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
        // v5 standoff block: reach/speed/threat come from the hostile ITSELF,
        // looked up by slot, because perception carries only where it is --
        // and a brain cannot choose a standoff distance without knowing what
        // it is standing off from.
        BlThreat row{t.pos.x, t.pos.y, t.dist, t.slot, 0.0f, 0.0f, 0.0f, 0.0f};
        if (const entt::entity te = entity_for_slot(game, static_cast<int32_t>(t.slot));
            te != entt::null) {
            if (const auto* atk = game.registry.try_get<Attacks>(te); atk != nullptr) {
                row.reach = melee_range(*atk);
                row.ranged_reach = ranged_range(*atk);
            }
            if (const auto* st = game.registry.try_get<Stats>(te); st != nullptr) {
                row.move_speed = st->move_speed;
            }
            row.threat = threat_of(game.registry, te);
        }
        sug.threats[i] = row;
    }
    // Not part of BlViewSuggest: view.grazing (critter-only) and the
    // townfolk tax_target/deposit fields -- neither applies to a hero brain
    // (see brain_abi.h's BlViewSuggest doc comment).

    // --- factors: this hero's class weights row + the hero-decision scalars ---
    BlViewFactors& f = wire.factors;
    const HeroFactors& hf = game.factors.hero;
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
    f.entrance_radius = kEntranceRadius;  // components.h -- see BlViewFactors' own comment
    // Deliberately excluded (see brain_abi.h's BlViewFactors doc comment):
    // perception-only factors (radii, drain/fill rates, lease windows) --
    // those stay host-side, read by observe_hero above, not the brain. Also
    // excluded (v2): think_min_millis/think_max_millis -- deliberation is
    // gone.

    // --- statuses: advisory only this slice (brain_abi.h's BL_ST_* doc) -------
    int32_t status_count = 0;
    auto push_status = [&](int32_t kind, int64_t remaining_millis) {
        if (status_count < BL_MAX_STATUSES) {
            wire.statuses[status_count++] =
                BlStatus{remaining_millis, static_cast<uint32_t>(kind), 0u};
        }
    };
    if (const auto* cs = game.registry.try_get<ChattingState>(e)) {
        push_status(BL_ST_CHATTING, static_cast<int64_t>(cs->remaining * 1000.0f));
    }
    if (game.registry.all_of<MeleeLock>(e)) {
        push_status(BL_ST_MELEE_LOCKED, 0);  // indefinite -- ends when combat resolves
    }
    if (game.registry.all_of<InsideBuilding>(e)) {
        push_status(BL_ST_INSIDE_BUILDING, 0);  // indefinite -- ends when the need is filled
    }
    if (const int64_t cursed = remaining_millis_of(game.registry, e, StatusKind::Cursed);
        cursed > 0) {
        push_status(BL_ST_CURSED, cursed);
    }
    if (const int64_t dis = remaining_millis_of(game.registry, e, StatusKind::Disengaged);
        dis > 0) {
        push_status(BL_ST_DISENGAGED, dis);
    }
    if (const int64_t sneak = remaining_millis_of(game.registry, e, StatusKind::Sneaking);
        sneak > 0) {
        // A brain only ever sees this on its OWN wire: a sneaking entity is
        // absent from everyone else's threat list, which is the whole mechanic.
        push_status(BL_ST_SNEAKING, sneak);
    }
    if (const int64_t calc = remaining_millis_of(game.registry, e, StatusKind::Calcified);
        calc > 0) {
        push_status(BL_ST_CALCIFIED, calc);
    }
    if (const int64_t stun = remaining_millis_of(game.registry, e, StatusKind::Stunned);
        stun > 0) {
        // Rarely seen by the brain it belongs to -- a stunned entity is not
        // consulted at all (sim.cpp's think dispatch) -- but carried for the
        // same reason every other status is: it is this entity's own state,
        // and a brain waking the tick a stun expires should be able to tell
        // that is what just happened.
        push_status(BL_ST_STUNNED, stun);
    }
    wire.status_count = status_count;

    // --- attacks: this hero's attack loadout (the Attacks component -- every
    // spawned entity, hero or otherwise, carries one; see heroes.cpp's
    // spawn_character) -- a brain cannot pick an attack it cannot see
    // (brain_abi.h's BlViewAttack doc). category/damage_type/base_damage/
    // range come from Attacks::defs[i]; cooldown_remaining is the PER-
    // INSTANCE remaining time (Attacks::cooldown_remaining[i]), not
    // Attack::cooldown (that's the base duration, not carried on the wire --
    // a brain only needs to know "is it ready", not how long it takes).
    const Attacks& atk = game.registry.get<Attacks>(e);
    wire.attack_count = atk.count;
    for (int32_t i = 0; i < atk.count; ++i) {
        const Attack& def = atk.defs[i];
        wire.attacks[i] = BlViewAttack{static_cast<int32_t>(def.category),
                                       static_cast<int32_t>(def.damage_type),
                                       def.base_damage,
                                       def.range,
                                       atk.cooldown_remaining[i],
                                       /*_pad=*/0u};
    }

    // --- skills: this hero's learned skills, packed 1:1 with its Skills
    // component so the WIRE INDEX IS THE SKILLS INDEX -- which is exactly
    // what a brain hands back as BL_ACT_USE_SKILL's `arg` (brain_abi.h's
    // BlViewSkill doc). `ready`/`recommended` come from evaluate_skill_triggers
    // (skills.h), the host's own advice: a brain may ignore both, and the
    // engine re-validates everything at resolve time regardless.
    if (const auto* skills = game.registry.try_get<Skills>(e)) {
        SkillRecommendation recs[kMaxSkills];
        SkillContext sctx;
        sctx.health_frac = view.health_frac;
        sctx.threats = view.threats;
        sctx.threat_count = view.threat_count;
        const int32_t n = evaluate_skill_triggers(*skills, sctx, recs);
        wire.skill_count = n;
        for (int32_t i = 0; i < n && i < BL_MAX_SKILLS; ++i) {
            const SkillSpec& spec = game.skills.specs[static_cast<size_t>(skills->ids[i])];
            wire.skills[i] = BlViewSkill{static_cast<int32_t>(skills->ids[i]),
                                         skills->cooldown_remaining[i],
                                         recs[i].ready ? 1u : 0u,
                                         recs[i].recommended ? 1u : 0u,
                                         static_cast<int32_t>(spec.trigger),
                                         static_cast<int32_t>(spec.target)};
        }
    }

    // --- events: EventInbox, copied 1:1 (BL_MAX_EVENTS == kInboxCapacity) ------
    const EventInbox& inbox = game.registry.get<EventInbox>(e);
    wire.event_count = inbox.count;
    for (int32_t i = 0; i < inbox.count; ++i) {
        const InboxEvent& ev = inbox.events[i];
        wire.events[i] = BlEvent{ev.at_millis,
                                 ev.ttl_millis,
                                 static_cast<uint32_t>(ev.kind),
                                 ev.source_slot,
                                 ev.param,
                                 0u};
    }

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

// The wire trust boundary: see wasm_brain.h's doc comment on this function
// for the full policy. Two different kinds of "wrong" get two different
// responses (Fix 5 -- the trust boundary distinguishes them explicitly now):
//
//  - MALFORMED (corruption-shaped, FATAL -> nullopt, escalated by the
//    caller): a non-finite point coordinate (would propagate into
//    MoveTo/distance math, apply_intention/intention.cpp) or a duration_millis/
//    idle_hint_millis outside [0, INT32_MAX] (narrows losslessly into a
//    Command's int32_t param_b ONLY because this check bounds it first,
//    command.cpp's enqueue_set_behavior). These shapes cannot come from a
//    well-formed guest of ANY version -- they indicate a buggy/adversarial
//    module, not a vocabulary mismatch.
//  - UNKNOWN VOCABULARY (forward-compat, warn once + decode as the
//    "nothing new" value, NOT rejected): an intention_kind outside
//    [BL_INT_NONE, BL_INT_USE_SKILL], or an activity_label outside
//    [-1, kActivityCount) (-1 is activity_label's OWN "none" sentinel --
//    see Intention's doc comment -- so it is a valid value, not clamped).
//    Both are exactly the shape a newer guest talking to an OLDER host
//    would produce (a kind/label this build has not learned about yet)
//    -- treating that as fatal would make every host upgrade a breaking
//    change for every brain built against a newer vocabulary. An unknown
//    kind decodes to IntentionKind::None (apply_intention's own
//    warn+ignore, one layer up, is what a REJECTED-but-recognized kind --
//    e.g. Shoot at an unknown target -- goes through instead); an
//    out-of-range activity_label clamps to -1 (inspection-only field, no
//    downstream index risk once clamped).
//
// BL_INT_USE_SKILL is no longer a third case: it is an ordinary kind now, and
// decodes straight across like every other one (game/src/skill_focus.h). What
// it means -- a FOCUS -- is apply_intention's business, and a skill whose
// trigger is not Intention is refused there, not here.
std::optional<Intention> decode_suggestion(const BlSuggestionWire& out, uint32_t slot) {
    if (!std::isfinite(out.point_x) || !std::isfinite(out.point_z)) {
        return std::nullopt;
    }
    if (out.duration_millis < 0 ||
        out.duration_millis > static_cast<int64_t>(std::numeric_limits<int32_t>::max())) {
        return std::nullopt;
    }
    if (out.idle_hint_millis < 0 ||
        out.idle_hint_millis > static_cast<int64_t>(std::numeric_limits<int32_t>::max())) {
        return std::nullopt;
    }

    Intention intent;
    intent.point = {out.point_x, out.point_z};
    intent.target_slot = out.target_slot;
    intent.arg = out.arg;
    intent.duration_millis = out.duration_millis;
    intent.idle_hint_millis = out.idle_hint_millis;

    if (out.intention_kind < BL_INT_NONE || out.intention_kind > BL_INT_USE_SKILL) {
        spdlog::warn("[wasm-brain] slot {}: unrecognized intention_kind {}, ignored (forward-compat)",
                    slot, out.intention_kind);
        intent.kind = IntentionKind::None;
    } else {
        intent.kind = static_cast<IntentionKind>(out.intention_kind);
    }

    if (out.activity_label < -1 || out.activity_label >= kActivityCount) {
        spdlog::warn("[wasm-brain] slot {}: activity_label {} out of range, clamped to -1", slot,
                    out.activity_label);
        intent.activity_label = -1;
    } else {
        intent.activity_label = out.activity_label;
    }

    return intent;
}

std::unique_ptr<WasmBrainRuntime> WasmBrainRuntime::create(const uint8_t* wasm_bytes, size_t len) {
    BhProgram* program = bh_load(wasm_bytes, len);
    if (program == nullptr) {
        brain_fatal("load", std::nullopt,
                    std::string("bh_load failed: ") + bh_last_error() +
                        " (truncated or invalid wasm -- is git-lfs initialized?)");
    }
    // Allocated before bh_instantiate (rather than after, like `program`
    // above) so `runtime.get()` -- a stable heap address for the rest of this
    // object's life, unique_ptr moves notwithstanding -- can be handed to
    // bh_instantiate as the action callback's `user` pointer; forward_action
    // casts it straight back to WasmBrainRuntime* on every call.
    auto runtime = std::make_unique<WasmBrainRuntime>();
    runtime->program = program;
    // world_seed 0: world gen is currently seedless/static
    // (SymbolicMapGenerator is a pure function of its compile-time constants
    // -- see sim.cpp's make_world), so there is no seed to thread through yet.
    BhInstance* instance = bh_instantiate(program, BL_ABI_VERSION, /*world_seed=*/0, &forward_log,
                                          nullptr, &forward_action, runtime.get());
    if (instance == nullptr) {
        brain_fatal("instantiate", std::nullopt,
                    std::string("bh_instantiate failed: ") + bh_last_error());
    }
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

    // Cleared before every bh_tick so, by the time it returns, pending_actions
    // holds exactly this wake's bl_enqueue_action calls (forward_action
    // appends into it during the call below) -- never a stale carry-over from
    // a previous slot's wake or a previous tick's. Drained below, after the
    // suggestion is decoded/adopted (the action-resolver loop, this
    // function's tail).
    runtime.pending_actions.clear();

    BlSuggestionWire out{};
    const int32_t rc =
        bh_tick(runtime.instance, static_cast<int32_t>(slot), reinterpret_cast<const uint8_t*>(&wire),
               sizeof(wire), reinterpret_cast<uint8_t*>(&out), sizeof(out));
    if (rc != BH_OK) {
        brain_fatal("tick", slot, std::string("bh_tick failed: ") + bh_last_error());
    }

    // decode_suggestion is pure aside from its forward-compat warnings (see its
    // doc comment); a std::nullopt here is a brain bug under the fail-fast
    // policy, so this is the escalation point.
    const std::optional<Intention> intent = decode_suggestion(out, slot);
    if (!intent.has_value()) {
        brain_fatal("decode", slot,
                    "decode_suggestion rejected the wire (invalid kind/point/duration/activity)");
    }

    // apply_intention returns whether the suggestion was validated + adopted
    // -- true both for a genuine adopt/change (which logs a SetBehavior
    // command) AND for an identical restate-resume (which, per restate-log
    // dedup, logs nothing at all -- see apply_intention's own doc comment,
    // intention.cpp, for the safety argument). So `adopted` here is not a
    // "did a Command land this wake" signal; it only means the suggestion was
    // accepted, resumed or not. note_think_outcome (Fix 1, intention.h) is
    // the wake bookkeeping apply_intention itself no longer performs --
    // called unconditionally, once per think, regardless of what was
    // decided; see its own doc comment for why the two are split.
    const bool adopted = apply_intention(game, slot, *intent);
    note_think_outcome(game, slot, adopted);

    // v3 action channel: drain THIS wake's bl_enqueue_action calls, in call
    // order, through resolve_action (game/src/intention.h) -- the single
    // gateway every swing goes through (the simple monster brain,
    // monster_brain.cpp, calls the exact same function). Soft convention,
    // not enforced here: multiple actions are allowed per wake, and an
    // invalid one warns + drops without touching the suggestion just
    // adopted above (resolve_action never touches CurrentIntention) or the
    // rest of this batch.
    for (const PendingAction& action : runtime.pending_actions) {
        resolve_action(game, slot, AgentAction{action.kind, action.target_slot, action.arg});
    }
}

}  // namespace badlands
