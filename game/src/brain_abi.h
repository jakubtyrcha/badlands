#pragma once

// The wasm-brain wire format: the single source of truth for the byte layout
// a "brain" wasm module (authored in Nim, later possibly other languages)
// exchanges with the C++ game through the brainhost crate's opaque byte
// buffers (src/crates/brainhost/include/brainhost.h moves bytes only -- it
// knows nothing about these structs).
//
// v2 (the intention contract, docs/design/intention-contract.html): the brain
// suggests an INTENTION now, not a command -- the engine (game/src/
// intention.h) validates/executes/tracks it and decides when to wake the
// brain again. v1's command/decision/deliberation vocabulary (BL_CMD_*,
// BlDecisionWire, the think_min/think_max pause) is gone; see the design doc
// for the full rationale. This is a breaking wire bump (BL_ABI_VERSION 1 ->
// 2) -- a v1 guest cannot run against this host and vice versa.
//
// v3 (contract-v3-alignment, docs/superpowers/specs/
// 2026-07-25-contract-v3-alignment-design.md): adds a second, write-only
// channel alongside the one suggestion a wake still returns -- a brain may
// call the new bl_enqueue_action(kind, target_slot, arg) host import
// (src/crates/brainhost/include/brainhost.h) any number of times per wake to
// fire instant actions (BL_ACT_*; only BL_ACT_ATTACK is live), each
// validated by the engine independently at resolve time, in enqueue order.
// This is a breaking wire bump (BL_ABI_VERSION 2 -> 3): BlViewWire gains the
// attack-loadout block below (a brain cannot pick an attack it cannot see)
// and the import allowlist grows by one entry. Shipped, not neutral: single-
// gateway combat (the same slice) deleted the old host-level combat_preempt
// pass outright, so tick_wasm_brain draining every wake's calls through
// resolve_action (game/src/intention.h -- wasm_brain.cpp's
// WasmBrainRuntime::pending_actions is the sink) is now THE way a wasm
// hero's every swing happens -- hero.nim calls bl_enqueue_action(BL_ACT_
// ATTACK, ...) once per combat wake (scripts/brains/nim/hero.nim); the
// simple monster brain (monster_brain.cpp) reaches the exact same
// resolve_action entry point host-side, never a privileged path.
//
// Plain C, includable from both C++ (game/) and generated bindings. There is
// a hand-mirrored copy of every struct below in scripts/brains/nim/abi.nim --
// keep the two in sync; both sides static-assert their sizes so a mismatch is
// a build failure, not a silent wire corruption.
//
// LAYOUT RULES (why the field order below is not just "whatever reads
// naturally"): every struct lists its int64_t fields FIRST, then its 4-byte
// (int32_t/uint32_t/float) fields, with an explicit `_pad*` inserted wherever
// the compiler would otherwise insert an IMPLICIT one (an 8-byte field pulls
// a struct's alignof up to 8, which pads sizeof up to a multiple of 8; a
// struct embedded before another one that starts with an 8-byte field needs
// its own size rounded to a multiple of 8 for the same reason -- true even
// for a struct like v2's BlViewFactors, which has NO int64_t members of its
// own: its alignof is only 4, but the array embedded right after it in
// BlViewWire, BlStatus[], starts with an int64_t and needs 8-byte alignment,
// so BlViewFactors' OWN sizeof must still land on a multiple of 8, via an
// explicit trailing pad, not because BlViewFactors itself needs the
// alignment). Making every pad explicit means sizeof/offsetof are
// load-bearing, checkable facts, not "however this compiler happens to lay it
// out". Bools are `uint32_t` 0/1 -- wasm32 has no fixed `bool` wire
// representation worth relying on.
//
// This header does NOT include badlands_sim.hpp (kept dependency-free so it
// can be handed to a bindings generator); the ActivityId::Count relationship
// for BL_MAX_ACTIVITIES is asserted in game/tests/brain_abi_tests.cpp, which
// does include it.

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Wire format version. Bumped on any incompatible layout change; the host
// (bh_instantiate) rejects a module whose bl_abi_version() disagrees.
#define BL_ABI_VERSION 5

// Capacities baked into the wire structs below (fixed-size arrays -- no
// dynamic length on the wasm side of this boundary).
#define BL_MAX_THREATS 8
#define BL_MAX_CHARS 16
#define BL_MAX_EVENTS 8
#define BL_MAX_STATUSES 8
// Must equal (int32_t)badlands::ActivityId::Count (game/include/badlands_sim.hpp).
// Checked at test time (brain_abi_tests.cpp), not compile time, because this
// header deliberately does not include badlands_sim.hpp.
#define BL_MAX_ACTIVITIES 14
// Must equal badlands::kMaxAttacks (game/include/badlands_sim.hpp). Checked
// at compile time in wasm_brain.cpp (which already includes badlands_sim.hpp
// via components.h), same reasoning as BL_MAX_ACTIVITIES above.
#define BL_MAX_ATTACKS 3
// Must equal badlands::kMaxSkills (game/include/badlands_sim.hpp), checked the
// same way in wasm_brain.cpp.
#define BL_MAX_SKILLS 8

// Action kinds (append-only): the vocabulary for bl_enqueue_action's `kind`
// argument (src/crates/brainhost/include/brainhost.h's BhActionFn). Unlike
// BL_INT_* (one suggestion per wake, validated/tracked by apply_intention),
// an action is fire-and-forget -- write-only, no return, resolved by the
// engine's action resolver at THIS tick's resolve point, independently of
// the wake's suggestion. Calling bl_enqueue_action is not permission: an
// invalid action (bad index, on cooldown, wrong category for the current
// lock, out of range) is warned and dropped without affecting the rest of
// the batch or the yielded suggestion. Same discipline as every other
// append-only vocabulary here: never renumber or reuse a shipped value.
#define BL_ACT_NONE 0
// live: arg = index into BlViewWire.skills (the actor's OWN skill slots, the
// same convention BL_ACT_ATTACK's arg uses for its attacks), target_slot =
// the victim (UINT32_MAX = the current Attack-intention's target, or the
// caster itself for a self-targeted skill). Everything else -- cooldown,
// whether the skill is castable as an action at all, targeting-mode legality,
// reach -- is checked host-side (game/src/skill_cast.h's validate_cast).
#define BL_ACT_USE_SKILL 1
#define BL_ACT_USE_POTION 2  // reserved
// live: arg = index into BlViewWire.attacks (BL_ACT_ATTACK's attack index),
// target_slot = victim slot (UINT32_MAX = the current Attack-intention's
// target -- BL_INT_ATTACK's own target_slot).
#define BL_ACT_ATTACK 3

// Intention kinds (append-only): the suggestion's `kind`. 0 = no suggestion
// this wake. Mirrors badlands::IntentionKind (game/src/components.h) 1:1 for
// values 0..8 -- BL_INT_USE_SKILL(9) has no IntentionKind counterpart yet
// (reserved). Same discipline as v1's BL_CMD_* list: never renumber or reuse
// a shipped value.
#define BL_INT_NONE 0
#define BL_INT_MOVE_TO 1
#define BL_INT_ATTACK 2
#define BL_INT_SHOOT 3
#define BL_INT_ENTER 4
#define BL_INT_ENTER_HOME 5
#define BL_INT_BUY 6
#define BL_INT_CHAT 7
#define BL_INT_IDLE 8
#define BL_INT_USE_SKILL 9  // reserved: rejected (warn+ignore) by the host until the skills slice

// Event kinds (append-only), mirroring badlands::InboxEventKind
// (game/src/components.h) 1:1.
#define BL_EV_NONE 0
#define BL_EV_DAMAGE_TAKEN 1
#define BL_EV_THREAT_SIGHTED 2
#define BL_EV_MOVE_BLOCKED 3
#define BL_EV_INTENTION_ENDED 4

// Status kinds (append-only). All three are advisory only this slice (no
// status bypasses the think) -- see docs/design/intention-contract.html §2.
#define BL_ST_NONE 0
#define BL_ST_CHATTING 1
#define BL_ST_MELEE_LOCKED 2
#define BL_ST_INSIDE_BUILDING 3
// The first status that is NOT merely advisory: a stunned entity is not
// consulted at all (sim.cpp's think dispatch skips it), so a brain only ever
// sees this on the wire for an entity whose stun ended before its wake. It is
// carried anyway, with its remaining time, because the vocabulary is the
// entity's own state and a future partial-incapacitation status will want the
// same shape.
#define BL_ST_STUNNED 4
// Walked out of melee contact: can take NO action for a few seconds. Movement
// and defense are untouched -- see game/src/movement.h's disengage penalty.
#define BL_ST_DISENGAGED 5
// Accuracy and armour sapped for a duration. Advisory like the rest: a brain
// that can see it may choose to disengage-and-wait or press anyway. Adding a
// STATUS VALUE changes no struct layout, so this is not an ABI break.
#define BL_ST_CURSED 6

// --- BlViewSelf --------------------------------------------------------------
// This entity's own state: clock, identity, needs, and a summary of what the
// engine is CURRENTLY executing for it (the CurrentIntention component,
// game/src/components.h) -- not what the brain itself decided last (a brain
// is stateless across wakes; this is how it learns "what am I already doing"
// on a fresh wake). One per BlViewWire (the thinking entity), as opposed to
// BlViewChar (every OTHER entity it can currently see).
typedef struct BlViewSelf {
    int64_t world_millis;         // sim clock right now
    int64_t think_until_millis;   // carried from v1 (HeroSimulationState's own
                                   // deliberation pause, unrelated to the
                                   // intention contract) -- always 0 for a
                                   // hero driven by this wire, since nothing
                                   // on this path ever requests ActivityId::
                                   // Think anymore; kept only because BlViewSelf
                                   // otherwise stays 1:1 with WorldView.
    int64_t roam_epoch;           // world_millis / roam lease window (stable roam goal)
    int64_t intention_wake_at;    // CurrentIntention.wake_at_millis; 0 only for a
                                   // hero that has never been adopted/restated
                                   // yet -- v3's default wake cadence
                                   // (intention-contract.html §2) means an
                                   // adopted hero's deadline is never 0
    uint32_t slot;                // this entity's slot index
    int32_t class_id;             // HeroClassId
    float tod;                    // time of day in [0,1)
    uint32_t night;               // bool: is it night
    float pos_x;
    float pos_z;
    float health_frac;            // hp / max_hp
    float fatigue;                // reserve in [0,1]; 1 = well rested
    float content;                // reserve in [0,1]; 1 = fully entertained
    int32_t inventory;
    float attack_range;           // this entity's own attack reach
    int32_t current_activity;     // ActivityId this entity is doing now; -1 = none yet
    int32_t intention_kind;       // CurrentIntention.kind (BL_INT_*); BL_INT_NONE = nothing running
    // v5 (was _pad2, so the struct size is unchanged): this entity's OWN
    // combat potential (threat_of, game/src/threat_table.h). The other half of
    // the comparison BlThreat::threat enables -- both sides ride the wire
    // because deciding whether a fight is worth taking is the brain's call,
    // not the engine's.
    float threat;
} BlViewSelf;

// --- BlThreat ------------------------------------------------------------------
// One perceived hostile, nearest-first (see WorldView::threats).
typedef struct BlThreat {
    float pos_x;
    float pos_z;
    float dist;
    uint32_t slot;
    /* v5, the standoff block: what a brain needs to decide how close it is
       willing to be, and whether the fight is worth taking at all. Standoff
       distance is TACTICS, so it lives in the brain -- the engine grows no
       kiting policy, it just stops making the decision unmakeable. */
    float reach;         /* longest MELEE range in its loadout; 0 = none */
    float ranged_reach;  /* longest RANGED range; 0 = none */
    float move_speed;    /* how fast it closes */
    /* Its combat potential (threat_of, game/src/threat_table.h). Compare
       against BlViewSelf::threat. This is threat's SECOND role: a fixed
       calibration target that, because it is kept a fair approximation, also
       tells a brain whether to pick this fight. */
    float threat;
} BlThreat;

// --- BlViewSuggest -------------------------------------------------------------
// Perception's pre-computed suggestions: goals, candidates, and existence
// flags a brain would otherwise have to derive itself (mirrors WorldView,
// game/src/behaviours/world_view.h). Unchanged from v1. A brain reads these
// rather than re-deriving them from raw world state, which stays entirely
// host-side.
typedef struct BlViewSuggest {
    float roam_goal_x;
    float roam_goal_z;
    float explore_goal_x;
    float explore_goal_z;
    uint32_t has_explore_goal;
    uint32_t move_blocked;        // a step was refused recently enough to matter
    float blocked_x;
    float blocked_z;
    float partner_x;              // nearest chat companion
    float partner_z;
    float partner_dist;
    uint32_t partner_slot;
    uint32_t has_chat_partner;
    uint32_t chatting;            // already mid-conversation
    float prey_x;                 // nearest huntable prey (Hunter only)
    float prey_z;
    float prey_dist;
    uint32_t prey_slot;
    uint32_t has_prey;
    float home_x;
    float home_z;
    uint32_t has_home;
    float apothecary_x;
    float apothecary_z;
    uint32_t has_apothecary;
    float tavern_x;
    float tavern_z;
    uint32_t has_tavern;
    int32_t threat_count;
    // Explicit: without this, `threats` would sit at offset 116 (not a
    // multiple of 8), leaving this struct's total size (244) not a multiple
    // of 8 either -- which would force an IMPLICIT 4-byte pad into
    // BlViewWire between `suggest` and `factors` (the next field). Padding
    // here instead keeps that padding explicit and keeps sizeof(BlViewSuggest)
    // itself a documented, asserted constant.
    uint32_t _pad;
    BlThreat threats[BL_MAX_THREATS];
} BlViewSuggest;

// --- BlViewFactors -------------------------------------------------------------
// EXACTLY the factors.hero.* tuning scalars read by the hero decision layer:
// every score_*/act_* implementation used by the hero decision table
// (scripts/brains/nim/hero.nim's kHeroActivities -- the sole implementation
// now; game/src/behaviours/blocks.cpp keeps only the shared/non-hero blocks).
// Unlike v1, this does NOT carry
// think_min_millis/think_max_millis -- deliberation is gone; the idle hint
// (BlSuggestionWire::idle_hint_millis, drawn guest-side against a compiled
// constant) replaces it. Perception-only factors (radii used by observe_hero,
// explore_chance, drain/fill rates, lease windows) stay excluded, same as v1.
typedef struct BlViewFactors {
    // Per-class activity preference (ActivityWeights), for THIS entity's class
    // only -- selection still happens the same way a C++ brain would do it.
    float weights[BL_MAX_ACTIVITIES];
    float fatigue_seek;            // read by scoreGoHome (day bar)
    float fatigue_seek_night;      // read by scoreGoHome (night bar)
    float low_health_rest;         // read by scoreGoHome (injury override)
    float content_seek;            // read by scoreVisitTavern
    float chat_content_seek;       // read by scoreChat
    float chat_radius;             // read by actChat (strike-up-conversation range)
    float explore_min_fatigue;     // read by scoreExplore
    // Cleanup (was the trailing pad below -- same size/offset, so this is a
    // same-size field swap, not a layout change): mirrors game/src/
    // components.h's kEntranceRadius, the "close enough to the door to fire
    // the actual action instead of just walking toward it" threshold the
    // door-activities (GoHome/Buy/VisitTavern, scripts/brains/nim/blocks.nim)
    // gate their act_* arrival check on. Was a hand-copied guest-side
    // constant kept in sync by comment only; this field is its single
    // source of truth now.
    float entrance_radius;
} BlViewFactors;

// --- BlViewChar ----------------------------------------------------------------
// A snapshot of one OTHER entity currently known to the thinking entity
// (mirrors WorldView-adjacent perception, not WorldView itself -- this is the
// raw character list a brain's own logic might scan, e.g. to pick a
// companion or a target). Unchanged from v1.
typedef struct BlViewChar {
    int64_t last_seen_millis;
    uint32_t slot;
    int32_t archetype;    // badlands::Archetype
    int32_t team;
    float last_x;
    float last_z;
    float last_hp;
    uint32_t visible_now; // bool: currently visible vs. remembered from last sighting
    uint32_t _pad;
} BlViewChar;

// --- BlStatus --------------------------------------------------------------
// One advisory status affecting this entity right now: {kind, remaining}.
// kind in BL_ST_*; remaining_millis == 0 means indefinite (ends on some
// condition other than a timer -- e.g. BL_ST_INSIDE_BUILDING ends when the
// need that sent the hero in is filled, not on a clock). All three v1 kinds
// are advisory only this slice -- none bypasses the think (see brain_abi.h's
// BL_ST_* doc above and docs/design/intention-contract.html §2).
typedef struct BlStatus {
    int64_t remaining_millis;    // 0 = indefinite
    uint32_t kind;
    uint32_t _pad;
} BlStatus;

// --- BlViewAttack ------------------------------------------------------------
// One entry of this entity's own attack loadout (mirrors badlands::Attack +
// the matching slot of Attacks::cooldown_remaining, game/src/components.h,
// 1:1) -- new in v3: a brain cannot pick an attack it cannot see. `category`/
// `damage_type` are badlands::AttackCategory/DamageType; `cooldown_remaining`
// is seconds until this attack is usable again (0 = ready now). No int64_t
// member, so alignof is only 4 -- the explicit trailing `_pad` is the same
// discipline as BlViewFactors' own trailing pad (brain_abi.h's LAYOUT RULES
// comment): it keeps sizeof(BlViewAttack) a documented multiple of 8 so the
// fixed-size array embedded after it in BlViewWire needs no compiler-
// inserted gap either.
typedef struct BlViewAttack {
    int32_t category;      // badlands::AttackCategory
    int32_t damage_type;   // badlands::DamageType
    float base_damage;
    float range;
    float cooldown_remaining;  // seconds; 0 = ready
    uint32_t _pad;
} BlViewAttack;

// --- BlViewSkill -------------------------------------------------------------
// One of this entity's learned skills. Its INDEX in this array is exactly what
// BL_ACT_USE_SKILL's `arg` names -- the array is packed 1:1 with the entity's
// own Skills component (game/src/components.h), the same way `attacks` is
// packed with Attacks. A brain cannot use a skill it cannot see, and cannot
// name one by id: it names the slot it was shown.
//
// `trigger`/`target_mode` are badlands::SkillTrigger/SkillTargetMode, so a
// brain can tell an action it may fire from a passive it may not; `ready` is
// the cooldown gate pre-computed, and `recommended` is the host's advice
// (evaluate_skill_triggers, game/src/skills.h) -- ADVICE, never a command.
// No int64_t member, so alignof is 4; the array still needs no compiler gap
// because 24 is a multiple of 8.
typedef struct BlViewSkill {
    int32_t skill_id;           /* badlands::SkillId */
    float cooldown_remaining;   /* seconds; 0 = ready */
    uint32_t ready;             /* bool: off cooldown */
    uint32_t recommended;       /* bool: the host's trigger advice */
    int32_t trigger;            /* badlands::SkillTrigger */
    int32_t target_mode;        /* badlands::SkillTargetMode */
} BlViewSkill;

// --- BlEvent -----------------------------------------------------------------
// One timed inbox entry (mirrors badlands::InboxEvent, game/src/components.h
// 1:1): {kind, source_slot, param}, plus the two clocks a sticky, TTL-expiring
// entry needs (when it happened, how much longer it survives unread).
typedef struct BlEvent {
    int64_t at_millis;
    int64_t ttl_millis;
    uint32_t kind;
    uint32_t source_slot;
    float param;
    uint32_t _pad;
} BlEvent;

// --- BlViewWire ------------------------------------------------------------
// The whole per-wake view buffer: bl_view_buf() points at one of these.
// Buffer addresses are fixed for the instance's lifetime (src/crates/
// brainhost/include/brainhost.h): the host queries bl_view_buf() ONCE, at
// instantiation, and reuses that address for every later wake -- so this
// struct must live at a fixed address for as long as the instance does (an
// ordinary global variable, as every shipping brain in scripts/brains/nim
// does, satisfies this trivially).
//
// Block order is binding: self / suggest / factors / statuses / attacks /
// skills / events / chars. `attacks` (v3) sits right after `statuses`, and
// `skills` (v4) right after `attacks` -- the entity's own loadout blocks stay
// adjacent.
typedef struct BlViewWire {
    uint32_t version;    // must equal BL_ABI_VERSION
    uint32_t _pad;       // explicit: keeps `self` (starts with int64_t) 8-aligned
    BlViewSelf self;
    BlViewSuggest suggest;
    BlViewFactors factors;
    int32_t status_count;  // number of valid entries in `statuses` (0..BL_MAX_STATUSES)
    uint32_t _pad2;         // explicit: keeps `statuses` (BlStatus starts with int64_t) 8-aligned
    BlStatus statuses[BL_MAX_STATUSES];
    int32_t attack_count;  // number of valid entries in `attacks` (0..BL_MAX_ATTACKS)
    uint32_t _pad3;         // explicit: pairs with attack_count the same way every other
                            // count field in this struct pairs with its own pad; BlViewAttack
                            // has no int64_t member (alignof 4) so this one isn't load-bearing
                            // for alignment the way _pad2/_pad4/_pad5 are, but it's kept for
                            // the same "every gap is explicit" discipline (see BlViewAttack's
                            // own doc comment, which mirrors BlViewFactors' identical case).
    BlViewAttack attacks[BL_MAX_ATTACKS];
    int32_t skill_count;   // number of valid entries in `skills` (0..BL_MAX_SKILLS)
    uint32_t _pad6;         // explicit: same "every gap is named" discipline as _pad3
    BlViewSkill skills[BL_MAX_SKILLS];
    int32_t event_count;   // number of valid entries in `events` (0..BL_MAX_EVENTS)
    uint32_t _pad4;         // explicit: keeps `events` (BlEvent starts with int64_t) 8-aligned
    BlEvent events[BL_MAX_EVENTS];
    int32_t char_count;    // number of valid entries in `chars` (0..BL_MAX_CHARS)
    uint32_t _pad5;         // explicit: keeps `chars` (BlViewChar starts with int64_t) 8-aligned
    BlViewChar chars[BL_MAX_CHARS];
} BlViewWire;

// --- BlSuggestionWire --------------------------------------------------------
// The whole per-wake suggestion buffer: bl_out_buf() points at one of these.
// Same "queried once, at instantiation" contract as BlViewWire above -- this
// struct must live at a fixed address for the instance's whole lifetime too.
//
// Exactly ONE suggestion per wake: {intention_kind, activity_label, point,
// target_slot, arg} plus a duration (BL_INT_IDLE only) and an idle hint
// (scheduling advice for every other kind -- "you don't need me for X ms",
// not a promise in either direction). activity_label is inspection/histogram
// only; it carries no semantics the engine reads.
typedef struct BlSuggestionWire {
    int64_t idle_hint_millis;    // 0 = no preference (v3: the engine defaults this
                                  // to a ~1s cadence rather than "forever" --
                                  // intention-contract.html §2)
    int64_t duration_millis;     // BL_INT_IDLE only
    int32_t intention_kind;      // BL_INT_*
    int32_t activity_label;      // ActivityId, inspection only
    float point_x, point_z;
    uint32_t target_slot;
    int32_t arg;                 // building kind for ENTER, etc.
} BlSuggestionWire;

#ifdef __cplusplus
}  // extern "C"

// --- static size/shape checks ------------------------------------------------
// Kept in this header (rather than only in the Catch2 test) so any TU that
// includes brain_abi.h fails to compile the instant the layout drifts from
// what's documented above -- the Catch2 test in
// game/tests/brain_abi_tests.cpp additionally exercises offsetof for a few
// sentinel fields and the BL_MAX_ACTIVITIES/ActivityId::Count relationship
// (which needs badlands_sim.hpp, deliberately not included here).
static_assert(sizeof(BlViewSelf) == 88, "BlViewSelf size drifted");
static_assert(sizeof(BlThreat) == 32, "BlThreat size drifted");
static_assert(sizeof(BlViewSuggest) == 376, "BlViewSuggest size drifted");
static_assert(sizeof(BlViewFactors) == 88, "BlViewFactors size drifted");
static_assert(sizeof(BlViewChar) == 40, "BlViewChar size drifted");
static_assert(sizeof(BlStatus) == 16, "BlStatus size drifted");
static_assert(sizeof(BlViewAttack) == 24, "BlViewAttack size drifted");
static_assert(sizeof(BlViewSkill) == 24, "BlViewSkill size drifted");
static_assert(sizeof(BlEvent) == 32, "BlEvent size drifted");
static_assert(sizeof(BlViewWire) == 1888, "BlViewWire size drifted");
static_assert(sizeof(BlSuggestionWire) == 40, "BlSuggestionWire size drifted");
#endif
