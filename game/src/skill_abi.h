// THE SKILL EXECUTION CONTRACT: what a skill's effect is given, and what it
// may ask for in return.
//
// Written as a C header, in the same style and for the same reason as
// game/src/brain_abi.h: this IS the wire a Nim/wasm skill script will
// implement later. Flat POD, fixed capacities, explicit padding, no C++
// types, no engine handles, no pointers into the world. An effect is a PURE
// FUNCTION of a BlSkillCastContext:
//
//   * it cannot read the world      -- everything it may know is in the context
//   * it cannot mutate the world    -- it fills a BlSkillEffectBatch of OPS
//   * it cannot roll                -- the engine pre-rolls the declared attack
//                                      test per target (seeded, replayable) and
//                                      hands the outcome in
//
// The engine validates and applies the batch (game/src/skill_cast.h), which is
// the same discipline the brain's action channel uses: a guest REQUESTS, the
// host decides. That is what makes an untrusted, scripted effect safe to run
// later without changing anything on this side of the boundary.
//
// LAYOUT RULES (brain_abi.h's, verbatim): every gap is an explicit `_pad`
// field, every struct's size is a documented static_assert, and no member is
// reordered without bumping BL_SKILL_ABI_VERSION.

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Bumped on any incompatible layout change. A future host will reject a skill
// module whose reported version disagrees, exactly as bh_instantiate does for
// the brain wire.
#define BL_SKILL_ABI_VERSION 3

// Capacities baked into the fixed-size arrays below.
#define BL_SKILL_MAX_TARGETS 8     // == badlands::kMaxSkillTargets
#define BL_SKILL_MAX_CONSTANTS 8   // == badlands::kMaxSkillConstants
#define BL_SKILL_MAX_OPS 8
#define BL_SKILL_NAME_LEN 24       // constant-name capacity, NUL-terminated

// Per-target outcome of the attack test the skill's SkillAttackTest declared,
// rolled ENGINE-SIDE before the effect runs (append-only).
#define BL_TEST_NOT_RUN 0   // the skill declares attack_test = none
#define BL_TEST_BLOCKED 1   // stopped at the attack-vs-defense gate
#define BL_TEST_DODGED 2    // evaded
#define BL_TEST_HIT 3       // landed; test_damage carries what it would deal

// Effect op kinds (append-only): everything an effect may ask the engine to
// do. A host that meets an op kind it does not know warns and skips it, so a
// newer script degrades rather than crashes.
#define BL_FX_NONE 0
#define BL_FX_APPLY_STATUS 1  // param_i = badlands::StatusKind, param_f = duration ms
#define BL_FX_DAMAGE 2        // param_f = hp to remove
#define BL_FX_HEAL 3          // param_f = hp to restore, clamped to the target's max
// Move target_slot to the CAST'S OWN validated point (the context's point_x/
// point_z). The op carries NO destination of its own, deliberately: an effect
// may ask that somebody be moved, but only to the place the engine already
// checked was in range and stand-on-able. That is the whole safety argument of
// this contract in miniature -- a guest cannot reach anywhere it was not shown,
// and here it cannot even name anywhere at all.
#define BL_FX_TELEPORT 4

// How a target relates to the caster (append-only). Lets one effect serve
// friend and foe without the script re-deriving teams.
#define BL_REL_SELF 0
#define BL_REL_FRIEND 1
#define BL_REL_ENEMY 2

// --- BlSkillCaster -----------------------------------------------------------
// Who is casting. Reaches are included because a skill's own geometry (how far
// its effect spills, say) is usually expressed against them, and a guest
// cannot look them up.
typedef struct BlSkillCaster {
    uint32_t slot;
    float pos_x, pos_z;
    float accuracy;      // the attacker-side gate the test already used
    float health_frac;   // hp / max_hp
    float melee_range;
    float ranged_range;
    uint32_t _pad;
} BlSkillCaster;

// --- BlSkillTarget -----------------------------------------------------------
// One entity this cast legally affects, in the order the engine resolved them.
// Defensive stats are the EFFECTIVE ones (combat.h's effective_combatant), so
// a script sees the same numbers the test was rolled against.
typedef struct BlSkillTarget {
    uint32_t slot;
    float pos_x, pos_z;
    float dist;          // from the caster, at cast time
    float health_frac;
    float defense;
    float evasion;
    float armour;
    int32_t attack_test; // BL_TEST_*
    float test_damage;   // what the roll produced; 0 unless BL_TEST_HIT
    int32_t relation;    // BL_REL_*
    // v2 (was _pad, so the struct size is unchanged): is this target currently
    // engaging the CASTER -- fighting it, or locked with it? An effect that
    // rewards catching someone unaware needs to know, and must not have to ask
    // the world: a guest script never can. Non-zero = engaged with the caster.
    int32_t engaging_caster;
} BlSkillTarget;

// --- BlSkillConstant ---------------------------------------------------------
// One authored tuning value, addressed BY NAME rather than by index: a skill's
// code and its manifest entry must agree on meaning, and a name says what an
// index cannot. Names are truncated to BL_SKILL_NAME_LEN - 1 characters.
typedef struct BlSkillConstant {
    char name[BL_SKILL_NAME_LEN];
    float value;
    uint32_t _pad;
} BlSkillConstant;

// --- BlSkillCastContext ------------------------------------------------------
// The whole input to one cast. `seed` is a deterministic, engine-derived
// stream an effect may draw from when it needs a choice of its own -- the
// attack test itself is already rolled, so nothing NEEDS it yet, but an effect
// that rolls its own must not reach for a global RNG (there isn't one; the sim
// seeds everything off replay-reproducible identity axes).
typedef struct BlSkillCastContext {
    uint32_t version;    // == BL_SKILL_ABI_VERSION
    int32_t skill_id;    // badlands::SkillId
    // v3: where a Point cast landed, already validated by the engine (in range,
    // on passable ground). Zero for every other targeting mode. An effect that
    // wants to move something has to spend THIS point -- see BL_FX_TELEPORT.
    float point_x, point_z;
    int64_t world_millis;
    uint64_t seed;
    BlSkillCaster caster;
    int32_t target_count;
    uint32_t _pad;
    BlSkillTarget targets[BL_SKILL_MAX_TARGETS];
    int32_t constant_count;
    uint32_t _pad2;
    BlSkillConstant constants[BL_SKILL_MAX_CONSTANTS];
} BlSkillCastContext;

// --- BlSkillEffectOp ---------------------------------------------------------
// One thing the effect asks the engine to do. `target_slot` MUST name one of
// the context's own targets -- an op reaching anything else is dropped, which
// is what bounds a scripted effect to the entities the engine chose to show it.
typedef struct BlSkillEffectOp {
    int32_t kind;         // BL_FX_*
    uint32_t target_slot;
    int32_t param_i;
    float param_f;
} BlSkillEffectOp;

// --- BlSkillEffectBatch ------------------------------------------------------
// The whole output of one cast, applied in order.
typedef struct BlSkillEffectBatch {
    int32_t count;
    uint32_t _pad;
    BlSkillEffectOp ops[BL_SKILL_MAX_OPS];
} BlSkillEffectBatch;

#ifdef __cplusplus
}  // extern "C"

// --- static size/shape checks ------------------------------------------------
// In the header (not only in the test) so any TU including it fails the
// instant the layout drifts from what is documented above.
static_assert(sizeof(BlSkillCaster) == 32, "BlSkillCaster size drifted");
static_assert(sizeof(BlSkillTarget) == 48, "BlSkillTarget size drifted");
static_assert(sizeof(BlSkillConstant) == 32, "BlSkillConstant size drifted");
static_assert(sizeof(BlSkillCastContext) == 720, "BlSkillCastContext size drifted");
static_assert(sizeof(BlSkillEffectOp) == 16, "BlSkillEffectOp size drifted");
static_assert(sizeof(BlSkillEffectBatch) == 136, "BlSkillEffectBatch size drifted");
#endif
