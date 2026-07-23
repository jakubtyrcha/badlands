#pragma once

// The wasm-brain wire format: the single source of truth for the byte layout
// a "brain" wasm module (authored in Nim, later possibly other languages)
// exchanges with the C++ game through the brainhost crate's opaque byte
// buffers (src/crates/brainhost/include/brainhost.h moves bytes only -- it
// knows nothing about these structs). This header owns the shapes; the
// per-tick view/decision plumbing that fills and reads them is a later task.
//
// Plain C, includable from both C++ (game/) and generated bindings. There is
// a hand-mirrored copy of every struct below in scripts/brains/nim/abi.nim --
// keep the two in sync; both sides static-assert their sizes so a mismatch is
// a build failure, not a silent wire corruption.
//
// LAYOUT RULES (why the field order below is not just "whatever reads
// naturally"): every struct lists its int64_t fields FIRST, then its 4-byte
// (int32_t/uint32_t/float) fields, with an explicit `uint32_t _pad*` inserted
// wherever the compiler would otherwise insert an IMPLICIT one (an 8-byte
// field pulls a struct's alignof up to 8, which pads sizeof up to a multiple
// of 8; a struct embedded before another one that starts with an 8-byte field
// needs its own size rounded to a multiple of 8 for the same reason). Making
// every pad explicit means sizeof/offsetof are load-bearing, checkable facts,
// not "however this compiler happens to lay it out". Bools are `uint32_t`
// 0/1 -- wasm32 has no fixed `bool` wire representation worth relying on.
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
#define BL_ABI_VERSION 1

// Capacities baked into the wire structs below (fixed-size arrays -- no
// dynamic length on the wasm side of this boundary).
#define BL_MAX_THREATS 8
#define BL_MAX_CHARS 16
// Must equal (int32_t)badlands::ActivityId::Count (game/include/badlands_sim.hpp).
// Checked at test time (brain_abi_tests.cpp), not compile time, because this
// header deliberately does not include badlands_sim.hpp.
#define BL_MAX_ACTIVITIES 14

// Command kinds a brain's decision can request. Deliberately NOT the game's
// CommandKindId values (badlands_sim.hpp): the host (C++ game side, a later
// task) maps these onto real Commands, so the wire ABI stays stable even if
// the game's internal enum reorders or grows. ABI version 1 (BL_ABI_VERSION
// above): this list is kept in sync BY HAND with scripts/brains/nim/abi.nim's
// own `BL_CMD_*` const block. It is the wire's command-kind VOCABULARY, not
// its struct layout (that is what sizeof/offsetof below actually pin) -- so
// it may grow, append-only, same discipline as badlands::ActivityId; never
// renumber or reuse a value once shipped.
#define BL_CMD_NONE 0
#define BL_CMD_ATTACK 1
#define BL_CMD_BUY 2
#define BL_CMD_ENTER 3  // command_arg = BuildingKind
#define BL_CMD_ENTER_HOME 4
// Task 5 (wasm hero decision layer): act_hunt/act_chat's follow-ups
// (CommandKind::Shoot/Chat, game/src/behaviours/blocks.cpp) target a specific
// OTHER entity, which none of the four kinds above carry -- BL_CMD_ATTACK is
// actor-only, and ENTER/ENTER_HOME/BUY carry a building kind or nothing.
// command_arg = target slot. Adjudicated as compatible with the brief's
// "abi.nim unchanged (wire is frozen)": that line is about the wire STRUCT
// layout (BlViewWire/BlDecisionWire, unchanged here, still exactly
// sizeof/offsetof-asserted below), not this command-kind list -- and
// decode_command's matching extension (wasm_brain.cpp) was explicitly
// authorized by the brief itself.
#define BL_CMD_SHOOT 5   // command_arg = prey slot (hunter's shot)
#define BL_CMD_CHAT 6    // command_arg = chat partner slot

// --- BlViewSelf --------------------------------------------------------------
// This entity's own state: clock, identity, needs, and its own current
// decision. One per BlViewWire (the thinking entity), as opposed to
// BlViewChar (every OTHER entity it can currently see).
typedef struct BlViewSelf {
    int64_t world_millis;         // sim clock right now
    int64_t think_until_millis;   // a deliberation pause in progress ends at this time
    int64_t roam_epoch;           // world_millis / roam lease window (stable roam goal)
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
// game/src/behaviours/world_view.h). A brain reads these rather than
// re-deriving them from raw world state, which stays entirely host-side.
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
    // BlViewWire between `suggest` and `factors` (the next field, which
    // starts with an int64_t and so needs 8-byte alignment). Padding here
    // instead keeps that padding explicit and keeps sizeof(BlViewSuggest)
    // itself a documented, asserted constant.
    uint32_t _pad;
    BlThreat threats[BL_MAX_THREATS];
} BlViewSuggest;

// --- BlViewFactors -------------------------------------------------------------
// EXACTLY the factors.hero.* tuning scalars read by the hero decision layer:
// every score_*/act_* implementation used by town_brain.cpp's kHeroActivities
// (game/src/behaviours/blocks.cpp) plus deliberation.cpp's deliberate().
// Perception-only factors (radii used by observe_hero, explore_chance,
// drain/fill rates, lease windows) are deliberately excluded -- perception
// stays host-side; a brain sees only what feeds its scoring.
typedef struct BlViewFactors {
    // i64 fields first (layout rule): both read by deliberate() to draw the
    // pause length.
    int64_t think_min_millis;      // read by deliberate
    int64_t think_max_millis;      // read by deliberate
    // Per-class activity preference (ActivityWeights), for THIS entity's class
    // only -- selection still happens the same way a C++ brain would do it.
    float weights[BL_MAX_ACTIVITIES];
    float fatigue_seek;            // read by score_go_home (day bar)
    float fatigue_seek_night;      // read by score_go_home (night bar)
    float low_health_rest;         // read by score_go_home (injury override)
    float content_seek;            // read by score_visit_tavern
    float chat_content_seek;       // read by score_chat
    float chat_radius;             // read by act_chat (strike-up-conversation range)
    float explore_min_fatigue;     // read by score_explore
    // Explicit: 2 int64_t (16B) + 14 weights (56B) + 7 scalars (28B) = 100B,
    // not a multiple of 8 -- this struct's own alignof is 8 (it has int64_t
    // members), so the compiler would silently round sizeof up to 104 on its
    // own. Declaring the 4 bytes ourselves keeps that rounding explicit.
    uint32_t _pad;
} BlViewFactors;

// --- BlViewChar ----------------------------------------------------------------
// A snapshot of one OTHER entity currently known to the thinking entity
// (mirrors WorldView-adjacent perception, not WorldView itself -- this is the
// raw character list a brain's own logic might scan, e.g. to pick a
// companion or a target).
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

// --- BlViewWire ------------------------------------------------------------
// The whole per-tick view buffer: bl_view_buf() points at one of these.
typedef struct BlViewWire {
    uint32_t version;   // must equal BL_ABI_VERSION
    uint32_t _pad;      // explicit: keeps `self` (starts with int64_t) 8-aligned
    BlViewSelf self;
    BlViewSuggest suggest;
    BlViewFactors factors;
    int32_t char_count;  // number of valid entries in `chars` (0..BL_MAX_CHARS)
    uint32_t _pad2;      // explicit: keeps `chars` (BlViewChar starts with int64_t) 8-aligned
    BlViewChar chars[BL_MAX_CHARS];
} BlViewWire;

// --- BlDecisionWire --------------------------------------------------------
// The whole per-tick decision buffer: bl_out_buf() points at one of these.
typedef struct BlDecisionWire {
    int64_t pause_duration_millis;  // valid only when pause_kind != 0
    int32_t activity_id;            // ActivityId to report/apply
    int32_t goal_kind;              // 0 = none, 1 = point (goal_x/goal_z valid)
    float goal_x;
    float goal_z;
    int32_t command_kind;           // BL_CMD_* (0 = none)
    int32_t command_arg;            // e.g. BuildingKind for BL_CMD_ENTER
    uint32_t follow_up_on_arrival;  // bool: gate command_kind on reaching goal
    uint32_t pause_kind;            // 0 = none, 1 = start, 2 = continue
} BlDecisionWire;

#ifdef __cplusplus
}  // extern "C"

// --- static size/shape checks ------------------------------------------------
// Kept in this header (rather than only in the Catch2 test) so any TU that
// includes brain_abi.h fails to compile the instant the layout drifts from
// what's documented above -- the Catch2 test in
// game/tests/brain_abi_tests.cpp additionally exercises offsetof for a few
// sentinel fields and the BL_MAX_ACTIVITIES/ActivityId::Count relationship
// (which needs badlands_sim.hpp, deliberately not included here).
static_assert(sizeof(BlViewSelf) == 72, "BlViewSelf size drifted");
static_assert(sizeof(BlThreat) == 16, "BlThreat size drifted");
static_assert(sizeof(BlViewSuggest) == 248, "BlViewSuggest size drifted");
static_assert(sizeof(BlViewFactors) == 104, "BlViewFactors size drifted");
static_assert(sizeof(BlViewChar) == 40, "BlViewChar size drifted");
static_assert(sizeof(BlViewWire) == 1080, "BlViewWire size drifted");
static_assert(sizeof(BlDecisionWire) == 40, "BlDecisionWire size drifted");
#endif
