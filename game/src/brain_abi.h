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
#define BL_ABI_VERSION 2

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
    int64_t intention_wake_at;    // CurrentIntention.wake_at_millis; 0 = no deadline
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
    uint32_t _pad2;
} BlViewSelf;

// --- BlThreat ------------------------------------------------------------------
// One perceived hostile, nearest-first (see WorldView::threats).
typedef struct BlThreat {
    float pos_x;
    float pos_z;
    float dist;
    uint32_t slot;
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
// Block order is binding: self / suggest / factors / statuses / events /
// chars.
typedef struct BlViewWire {
    uint32_t version;    // must equal BL_ABI_VERSION
    uint32_t _pad;       // explicit: keeps `self` (starts with int64_t) 8-aligned
    BlViewSelf self;
    BlViewSuggest suggest;
    BlViewFactors factors;
    int32_t status_count;  // number of valid entries in `statuses` (0..BL_MAX_STATUSES)
    uint32_t _pad2;         // explicit: keeps `statuses` (BlStatus starts with int64_t) 8-aligned
    BlStatus statuses[BL_MAX_STATUSES];
    int32_t event_count;   // number of valid entries in `events` (0..BL_MAX_EVENTS)
    uint32_t _pad3;         // explicit: keeps `events` (BlEvent starts with int64_t) 8-aligned
    BlEvent events[BL_MAX_EVENTS];
    int32_t char_count;    // number of valid entries in `chars` (0..BL_MAX_CHARS)
    uint32_t _pad4;         // explicit: keeps `chars` (BlViewChar starts with int64_t) 8-aligned
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
    int64_t idle_hint_millis;    // 0 = none
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
static_assert(sizeof(BlThreat) == 16, "BlThreat size drifted");
static_assert(sizeof(BlViewSuggest) == 248, "BlViewSuggest size drifted");
static_assert(sizeof(BlViewFactors) == 88, "BlViewFactors size drifted");
static_assert(sizeof(BlViewChar) == 40, "BlViewChar size drifted");
static_assert(sizeof(BlStatus) == 16, "BlStatus size drifted");
static_assert(sizeof(BlEvent) == 32, "BlEvent size drifted");
static_assert(sizeof(BlViewWire) == 1480, "BlViewWire size drifted");
static_assert(sizeof(BlSuggestionWire) == 40, "BlSuggestionWire size drifted");
#endif
