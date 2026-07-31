// C++ API of the badlands game simulation (C++/EnTT + a wasm hero brain).
//
// Data in, data out: scenarios are composed by spawning CharacterDesc rows;
// observers (renderer, tests) inspect CharacterState snapshots and SimStats.
// `badlands::Sim` owns the sim world and exposes tick/spawn/dispatch/snapshot
// as C++ methods. This C++ Sim API replaced the former extern-"C", data-only
// game_* ABI, which has been removed.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <entt/entt.hpp>  // entt::registry (the amalgamated single-include ships no fwd.hpp)

// The internal sim world, defined at global scope in game/src/game_state.h.
struct BadlandsGame;

namespace badlands {

// ---- enums (were GameBuildingKind / GameActionKind) ------------------------
enum class BuildingKind : int32_t {
    Castle = 0,
    FreeCompanyQuarters,
    HuntersCamp,
    ThievesDen,
    Scriptorium,
    Tavern,
    Apothecary,
    Watchtower,
    House,  // poppable
    Sewer,  // poppable
    // A solid 4x4 block of masonry: no door, no roof, nothing inside. Exists to
    // be an OBSTACLE -- it blocks the navmesh and the placement grid like any
    // other building and does nothing else. Plopped rather than placed (see
    // plop_building, game/src/placement.h), so a run of them abuts into a
    // continuous wall with no lane down the seam.
    Wall,
    Count
};

enum class ActionKind : int32_t {
    PlaceBuilding = 0,  // world_x/z, param_a = kind, param_b = rotation_index
    RecruitHero,        // target_id = guild building id
    DestroyBuilding,    // target_id = building id
    Count
};

// The placement/movement grid spans the WHOLE map: tile == 1 world unit == 1
// map metre, and the symbolic map is 256 m (64 blocks x 4 m), so the grid is
// [-128, 128). (It was 48 -- a 96 u window centred on the lake, which left the
// map's Forest/Plains ring unreachable; sim.cpp static_asserts this stays equal
// to the map span.)
inline constexpr int32_t kGridHalfExtentTiles = 128;  // was GAME_GRID_HALF_EXTENT_TILES

// Hero guild classes (the recruitable "class type id"). Unscoped + HERO_*
// enumerators to match the sim-internal usage this was promoted from (was
// heroes.h's HeroClassId); the numeric values are load-bearing (color table,
// registry HeroClass component .value). NB: distinct from the `HeroClass`
// EnTT *component* (game/src/components.h) -- this is the class enum.
enum HeroClassId : int32_t {
    HERO_MERCENARY = 0,
    HERO_HUNTER,
    HERO_GRAVE_ROBBER,
    HERO_APPRENTICE,
    HERO_CLASS_COUNT
};

// A building recruits at most this many distinct hero classes (usually 1). The
// per-kind recruit set lives in BuildingDef::recruits (the placement.cpp kDefs
// table is its single source of truth).
inline constexpr int32_t kMaxRecruitClasses = 3;

// Display name of a hero class ("Mercenary", ...). Empty string if out of range.
const char* HeroClassName(HeroClassId cls);

// Only characters on this team grant the player fog-of-war vision. Enemies run
// their own (future) vision and never reveal the map for the player.
inline constexpr int32_t kPlayerTeam = 0;

// Where make_world prebuilds the colony's Castle: on the plains south of the
// central lake (the map origin is water). The single source of truth for the
// colony seat -- the game's town + camera + fog-of-war centre on it.
inline constexpr float kCastleSpawnX = 0.0f;
inline constexpr float kCastleSpawnZ = 54.0f;

// The three fog-of-war knowledge levels (see the design doc). Cumulative:
// once Visible/Dormant, a texel never returns to Unknown.
enum class VisionLevel : int32_t {
    Unknown = 0,   // terra-incognita: never discovered (rendered black)
    Dormant = 1,   // discovered but not currently seen (rendered desaturated)
    Visible = 2,   // inside a player vision source right now (rendered normally)
};

// ---- POD result structs (field-for-field from badlands_game.h) -------------

// What KIND of thing is being spawned. Archetype is a spawn recipe: it decides
// which components and which brain the entity gets, and is not consulted again
// at think time (perception is relational -- friend/neutral/enemy + threat --
// never taxonomic).
enum class Archetype : int32_t {
    Hero = 0,   // needs, home, inventory, errands; the utility brain
    Townfolk,   // simple sequential goals (route, commute); no needs
    Critter,    // reactive: roam/graze, flee non-critters
    Monster,    // combat; no needs
};

// ---- activities: the AI's goal vocabulary ----------------------------------
//
// Every decision a brain can take is an ActivityId. One id space, shared by the
// sim, the command log (SetBehavior.param_a), the snapshot
// (CharacterState.behavior), and the statistics histogram -- so it is
// APPEND-ONLY: never renumber, never reuse.
//
// (game/src/behaviours/world_view.h aliases this as `badlands::Behavior`, the
// name the sim internals have always used.)
enum class ActivityId : int32_t {
    Idle = 0,
    Roam,
    Buy,
    GoHome,
    VisitTavern,
    Combat,
    Graze,
    VisitTax,
    Deposit,
    Hunt,
    Flee,     // bolting from a threat (was reported as Roam)
    Think,    // deliberating: an idle pause between goals
    Explore,  // walking toward terra incognita
    Chat,     // socializing with another hero (partial entertainment)
    Count
};
inline constexpr int32_t kActivityCount = static_cast<int32_t>(ActivityId::Count);

// TWO tiers, and deliberately only two.
//
// Danger is immediate danger -- a threat that pre-empts whatever you were
// doing. Normal is everything else. There is no "productive vs filler"
// classification and there must not be one: sorting activities into worthiness
// categories makes the category, rather than the character's actual state,
// decide what it does. Rest does not outrank a hunt because resting is a nobler
// class of act; it outranks it because the hero is tired, and stops outranking
// it once the hero is not.
//
// So within Normal, ordering comes from NEED: `weight x score`, where score is
// an urgency curve over the character's reserves (see HeroFactors). Danger
// exists purely so safety is structural -- no weight, however large, can keep a
// character standing about while something bears down on it.
enum class ActivityBand : int32_t {
    Danger = 0,  // immediate danger: fight, flee, defend
    Normal,      // everything else, ordered by need
    Count
};

// One catalog row: an activity's stable identity, for inspection.
struct ActivityInfo {
    ActivityId id;
    const char* name;  // stable inspection-facing label ("GoHome")
    ActivityBand band;
};

// Every activity, indexed by id (ActivityCatalog()[i].id == ActivityId(i)).
// The single source of truth for names + bands: UIs and statistics read this
// instead of hardcoding a switch that silently rots when an activity is added.
std::span<const ActivityInfo> ActivityCatalog();
// Catalog lookup; out-of-range ids resolve to the Idle row.
const ActivityInfo& ActivityInfoOf(int32_t id);
// Convenience name lookup; "-" for an out-of-range id (e.g. a -1 "no decision
// yet" from a snapshot row).
const char* ActivityName(int32_t id);

// Per-(class, activity) preference, indexed by ActivityId. Utility is
// `weight * considerations`, compared only WITHIN a band -- so a weight
// expresses "hunters explore constantly, apprentices almost never" without ever
// letting exploration outrank danger. A weight of 0 removes the activity from
// that class entirely, which is how classes get unique activity sets without
// separate code paths (perception may skip its cost too).
struct ActivityWeights {
    float w[kActivityCount] = {};

    float of(ActivityId id) const {
        const int32_t i = static_cast<int32_t>(id);
        return (i >= 0 && i < kActivityCount) ? w[i] : 0.0f;
    }
    void set(ActivityId id, float value) {
        const int32_t i = static_cast<int32_t>(id);
        if (i >= 0 && i < kActivityCount) {
            w[i] = value;
        }
    }
};

// ---- statuses --------------------------------------------------------------
// A timed condition on a character. Append-only id space, same discipline as
// ActivityId/SkillId: never renumber, never reuse a value (the wire's BL_ST_*
// mirrors these, and a status can outlive the tick it was applied on).
//
// A status is DATA + A TIMER here and nothing else: what a status DOES is
// enforced by the systems that care about it (the think dispatch, movement,
// the combat defender assembly), never by this vocabulary. That is what keeps
// "stunned" from becoming a special case threaded through the sim -- each
// system asks `has_status(...)` for itself, at the one place it already makes
// its own decision.
enum class StatusKind : int32_t {
    None = 0,
    Stunned,     // skips the think, freezes movement, zeroes the active defense
    Cursed,      // accuracy/armour sapped for a duration (Apprentice's Curse).
                 // Unlike Stunned it does NOT touch evasion: a cursed target
                 // still dodges, it has just stopped warding.
    Disengaged,  // walked out of melee contact: no actions at all for a few
                 // seconds. Movement and defense are untouched -- this is a
                 // penalty on ACTING, not on being, and it is meant to be
                 // steep enough that a brain that can count never chooses it.
    Sneaking,    // IMPERCEPTIBLE. Skipped by target selection (nearest_enemy)
                 // and by threat perception (collect_threats) alike, so no
                 // brain -- wasm or engine-side -- has a special case for it.
                 // Ends on an aggressive act, not only on its timer
                 // (combat.h's end_sneak_on_aggression), and carries an
                 // accuracy and crit bonus into the blow that ends it.
    Calcified,   // hardened: flat armour up. ARMOUR ONLY -- a calcified target
                 // still misses and still gets dodged around, which is what
                 // makes it a ward rather than a general buff.
};
inline constexpr int32_t kStatusKindCount = static_cast<int32_t>(StatusKind::Calcified) + 1;
// Fixed component capacity; matches BL_MAX_STATUSES (game/src/brain_abi.h).
inline constexpr int32_t kMaxStatuses = 8;
// Stable inspection name ("Stunned"); "-" for an out-of-range kind.
const char* StatusName(int32_t kind);

// ---- skills (identity only; defs/triggers live in game/src/skills.h) -------
// Append-only id space, same discipline as ActivityId.
enum class SkillId : int32_t {
    Calcify = 0,   // Apprentice: hardens its own hide -- flat armour, for a while
    ShieldBash,    // Mercenary: a shield slam that stuns what it lands on
    Curse,         // Apprentice: saps a target's accuracy and armour
    DressWounds,   // Hunter: field-dresses its own wounds
    Backstab,      // Grave Robber: heavy bonus damage on someone not facing it
    Sneak,         // Grave Robber: goes unseen until it strikes
    PrecisionShot, // Hunter / Grave Robber: a focused shot that cannot miss
    Count,
};
inline constexpr int32_t kSkillCount = static_cast<int32_t>(SkillId::Count);
// Fixed component/snapshot capacity (kMaxAttacks precedent).
inline constexpr int32_t kMaxSkills = 8;
// Stable inspection name ("Calcify"); "-" for an out-of-range id.
const char* SkillName(int32_t id);

// ---- skill templates --------------------------------------------------------
// A skill is DATA THE ENGINE CHECKS plus an EFFECT. The data below is the
// engine's whole vocabulary -- everything it validates before an effect ever
// runs -- and the effect is a pure function of a flat context
// (game/src/skill_abi.h), so it can be C++ today and a wasm script later
// without this template changing. The AI advice vocabulary (which condition
// recommends a skill) stays internal in game/src/skills.h.
//
// Unimplemented values are REFUSED, never approximated: an engine that cannot
// yet execute a trigger or a targeting mode warns and drops the cast rather
// than silently treating it as its nearest implemented neighbour.

// How a skill is initiated.
//   Action    -- instant, fired through the action channel (BL_ACT_USE_SKILL).
//   Passive   -- no cast at all; applies at some engine hook. DECLARED ONLY.
//   Intention -- a "focus": adopted as an intention, its effect landing after
//                intention_duration_seconds of uninterrupted execution.
//                DECLARED ONLY.
enum class SkillTrigger : int32_t { Action = 0, Passive, Intention };

// Who a cast may name.
//   None     -- targets nobody (a pure self-contained effect).
//   SelfOnly -- the caster, and ONLY the caster: naming anyone else is
//               refused by the engine, never remapped back onto the caster.
//   Any      -- one named entity, friend or foe (the effect decides what that
//               means; `relation` reaches it through the cast context).
//   Multi    -- up to target_limit entities. DECLARED ONLY.
//   Point    -- an area centred on a point. DECLARED ONLY.
enum class SkillTargetMode : int32_t { None = 0, SelfOnly, Any, Multi, Point };

// Whether the engine rolls a combat test per target before the effect runs,
// and off which of the caster's attacks. The declared test also SUPPLIES THE
// CAST RANGE (one source of truth, no second range field to disagree with the
// weapon): Melee -> the caster's melee reach, Ranged -> its ranged reach,
// None -> the optional "range" constant, whose absence (0) means no range
// check at all -- which is what a SelfOnly skill wants.
enum class SkillAttackTest : int32_t { None = 0, Melee, Ranged };

// One authored tuning value. Skill-specific by design: the manifest carries
// CONSTANTS, never logic, and each skill's own code knows the names it reads.
inline constexpr int32_t kMaxSkillConstants = 8;
// Most entities one cast may affect; mirrors BL_SKILL_MAX_TARGETS
// (game/src/skill_abi.h), the capacity of the context an effect receives.
inline constexpr int32_t kMaxSkillTargets = 8;
struct SkillConstant {
    std::string name;
    float value = 0.0f;
};

// One skill's template: the five engine-checked fields plus its constants.
// Initial config in the determinism contract -- a replay must use the same
// catalog, since cooldowns, target legality, and effect tuning all read from
// here.
struct SkillSpec {
    SkillTrigger trigger = SkillTrigger::Action;
    SkillTargetMode target = SkillTargetMode::Any;
    int32_t target_limit = 1;                  // Multi only; >= 1
    float cooldown_seconds = 0.0f;             // <= 0 => none
    float intention_duration_seconds = 0.0f;   // <= 0 => none; Intention only
    SkillAttackTest attack_test = SkillAttackTest::None;
    // May this be cast while the caster is locked in melee contact? The first
    // skill whose legality depends on the caster's SITUATION rather than on its
    // target -- and situation is data the engine checks, never effect logic:
    // an effect cannot refuse a cast, it can only decline to emit ops.
    bool castable_in_melee = true;
    // Does the declared attack test SKIP its gates? When set, the engine's
    // per-target pre-roll cannot block or dodge and always crits, at the
    // skill's own "crit_multiplier" constant. Engine-checked data, not effect
    // logic: an effect is HANDED an outcome, it never decides one -- which is
    // exactly why "guaranteed" has to live on this side of the contract.
    // Meaningless (and ignored) when attack_test is None.
    bool guaranteed_test = false;
    std::string effect;                        // brief descriptive string
    SkillConstant constants[kMaxSkillConstants];
    int32_t constant_count = 0;

    // Named lookup, `fallback` when absent. The ONE way skill code reads
    // tuning -- host-side today, and the same lookup a guest reimplements
    // over the cast context's own copy of these (game/src/skill_abi.h).
    float constant(const char* name, float fallback = 0.0f) const;
};

// A SkillSpec per skill (specs[i] belongs to SkillId(i)). Compiled defaults
// live in skills.cpp; an app may override fields by NAME from
// assets/skills/skills.json and push the result through Sim::SetSkillCatalog.
// Held per-Sim, so it is initial config (a replay must use the same catalog).
struct SkillCatalog {
    SkillCatalog();  // fills the compiled defaults
    SkillSpec specs[kSkillCount];
};

// Parse a skill name ("Calcify"); returns SkillId::Count if unknown.
SkillId SkillIdFromName(const char* name);

// One "this creature learns skill X on reaching level L" row. Authored per
// creature in the catalog (game/src/creature_catalog.cpp), so which class
// learns what is DATA rather than an engine table. Carried on the spawn desc
// rather than looked up by class, so nothing has to re-derive a hero's class
// after the fact to know what it should learn.
//
// NB which catalog a spawn reads is a PRE-EXISTING split (heroes.cpp's
// hero_desc): a directly-spawned creature reads the per-Sim catalog, so
// assets/creatures/creatures.json overrides reach it, while a RECRUITED hero
// reads the compiled defaults and does not. Grants inherit that split exactly
// as stats do -- editing a grant level in creatures.json changes a spawned
// mercenary and not a guild-recruited one.
struct SkillGrantRow {
    int32_t skill = -1;  // SkillId; -1 = empty row
    int32_t level = 1;
};

// ---- tuning factors (data, not code) ---------------------------------------
// Per-archetype behaviour tuning. The sim ships the defaults below, so it is
// fully usable -- and unit-testable -- with no file present; an app may load
// assets/creatures/factors.json over them (src/game/factors_manifest.hpp) so
// designers can tune without a rebuild.
//
// Factors are INITIAL CONFIG in the determinism contract
// (state = f(seed, initial config, command log, N ticks)): a replay must use
// the same factors, and the command log does not carry them.
struct HeroFactors {
    // --- needs: RESERVES in [0,1], where 1 is satisfied ---------------------
    // Both drain on their own and are refilled by doing something about it.
    // NB the sense of `fatigue`: it is a reserve like any other, so 1 means
    // well rested and 0 means spent -- resting RAISES it. That reads backwards
    // against the everyday word, but one consistent direction for every need is
    // worth more than each one reading nicely on its own.
    //
    // Rates are in IN-GAME HOURS so the numbers say what they mean; needs.h
    // converts to a per-tick delta in exactly one place. Every one of these is
    // live: Sim::SetFactors takes effect on the next tick, mid-run.
    float fatigue_drain_hours = 24.0f;  // 1 -> 0 with no sleep
    float content_drain_hours = 12.0f;  // 1 -> 0 with no diversion
    float rest_fill_hours = 4.0f;       // 0 -> 1 sleeping at home
    float tavern_fill_hours = 8.0f;     // 0 -> 1 at the tavern

    // Below this reserve a hero starts wanting to do something about it, and
    // urgency ramps linearly from 0 at the threshold to 1 at empty. That
    // urgency IS the activity's score, which is what makes what a hero does
    // next fall out of how depleted it is rather than out of what KIND of
    // activity it is.
    float fatigue_seek = 0.55f;        // by day
    float fatigue_seek_night = 0.90f;  // far readier to turn in after dark
    float content_seek = 0.60f;
    // A hurt hero wants to lie down whatever its reserves say.
    float low_health_rest = 0.5f;  // hp fraction below which rest urges

    // --- chatting -----------------------------------------------------------
    // Two under-entertained heroes who meet keep each other company. Weaker
    // than the tavern by construction: slower to fill, and it cannot fill you
    // past a ceiling -- so company takes the edge off and a night out still
    // pulls.
    float chat_content_seek = 0.5f;     // low enough to settle for company
    float chat_fill_hours = 20.0f;      // slower than a night out
    float chat_content_ceiling = 0.6f;  // company can never fully satisfy
    float chat_sight = 18.0f;           // how far a hero looks for a companion
    float chat_radius = 2.0f;           // close enough to actually strike it up
    float chat_duration = 6.0f;         // seconds a conversation lasts
    // --- exploration --------------------------------------------------------
    // Walking into terra incognita. Competes on need like everything else; it
    // stands down when the hero has no reserve to spare, when the world already
    // refused it once this window, or when there is prey right there.
    float explore_min_fatigue = 0.5f;     // not enough left in the tank to strike out
    float explore_min_distance = 6.0f;    // how far past the frontier to aim
    float explore_max_distance = 18.0f;
    float explore_search_radius = 90.0f;  // how far afield to look for a frontier
    int64_t explore_lease_millis = 8000;  // how long one target is committed to
    // Per-class appetite, drawn once per lease window: the probability a hero of
    // that class feels like exploring at all. A FREQUENCY, which a weight cannot
    // express -- a weight decides which activity wins when both apply, so a low
    // one means "always loses", i.e. never, not "rarely". Filled by SimFactors().
    float explore_chance[HERO_CLASS_COUNT];
    float roam_radius = 6.0f;      // world units around the roam anchor
    float hunt_sight_radius = 22.0f;  // how far a Hunter spots prey (deer)
    // How far a hero notices hostiles. Feeds WorldView's threat list, which
    // gates deliberation (you do not stand and think with a rat closing in).
    float threat_radius = 14.0f;
    // How long EntityMemory (game/src/entity_memory.h) keeps a character
    // sighting after it was last actually seen; once world_millis advances
    // past last_seen_millis by more than this, the entry is forgotten
    // (evicted) on the next tick's update pass. Buildings never expire this
    // way, so this only bounds char entries.
    int64_t memory_ttl_millis = 10000;
    // Vestigial: was the deliberation pause between goal changes, drawn
    // uniformly from this range (the prototype day is 120 s, so an in-game
    // minute was ~83 ms of sim time and the default range was roughly 0-10
    // in-game minutes). Deliberation itself is deleted -- the intention
    // contract (game/src/intention.h) replaced it, and no consumer downstream
    // of factors reads either field anymore (sim.cpp's sanitize_factors is
    // the sole remaining touch, keeping the min<=max pair-invariant rather
    // than acting on the values). Kept only pending a wire/factors
    // retirement decision -- removing the fields outright would ripple into
    // the JSON manifest schema, not just this struct.
    int64_t think_min_millis = 0;
    int64_t think_max_millis = 833;
    // Per-class preference table (see ActivityWeights). Filled with the
    // compiled defaults by SimFactors' constructor; factors.json may override
    // any single entry. This is the primary dial for class personality.
    ActivityWeights weights[HERO_CLASS_COUNT];
};

// Critter (deer) tuning. Deer graze/roam in Forest/Plains and bolt from any
// non-critter that comes within sight.
struct CritterFactors {
    float sight_radius = 12.0f;    // notices non-critters within this range
    float flee_radius = 8.0f;      // bolts when one is this close
    float flee_distance = 12.0f;   // how far it runs from the threat
    float roam_radius = 14.0f;     // wander range around the home anchor
    float graze_fraction = 0.4f;   // fraction of each roam cycle spent grazing
    // Deer run the SAME banded selector and the same shared blocks as heroes --
    // only this table and their activity list differ. That is the shareability
    // of the core, made executable rather than asserted.
    ActivityWeights weights;
};

// Townfolk (tax collector) tuning + the town economy.
struct TownfolkFactors {
    int64_t spawn_interval_millis = 60000;  // a collector leaves the castle this often
    int32_t max_alive = 2;                  // cap on live collectors
    float move_speed = 2.2f;                // a plodding taxman
    uint32_t house_income_per_day = 50;     // each House accrues this each midnight
};

// Monster (rat) tuning. Rats spawn from the Sewer and attack the nearest hostile
// unit, falling back to gnawing the nearest targettable building.
struct MonsterFactors {
    int64_t spawn_interval_millis = 20000;  // a rat crawls out this often
    int32_t max_alive = 4;                  // cap on live rats
};

// Hero progression: XP accrual + the leveling curve. XP amounts are INTEGERS.
struct ProgressionFactors {
    // XP per newly-discovered fog texel, credited to the discovering hero.
    int32_t xp_per_texel = 1;
    // A killed creature's xp_reward splits evenly (round up) over heroes
    // within this radius of the corpse (euclidean; obstacles ignored).
    float kill_xp_radius = 10.0f;
    // Cost to advance FROM level L: floor(level_base_xp * L^level_exponent).
    int32_t level_base_xp = 100;
    float level_exponent = 1.6f;
};

struct SimFactors {
    // Fills the activity weight tables with the compiled per-class defaults
    // (the scalar members above carry their own in-class defaults). Declared
    // rather than defaulted because the weight defaults are a table, not a
    // constant -- see game/src/activity_catalog.cpp.
    SimFactors();

    HeroFactors hero;
    CritterFactors critter;
    TownfolkFactors townfolk;
    MonsterFactors monster;
    ProgressionFactors progression;
};

// ---- combat primitives -----------------------------------------------------
// Typed attack-skills + tactical stats. resolve_attack (game/src/combat.h) runs
// the seeded probabilistic pipeline over these; the ECS carries a Combatant plus
// an Attacks component (game/src/components.h) that wraps Attack. Physical only
// for now -- Soul / willpower / resolve are reserved for the deferred psychology
// layer and are not read anywhere yet.
enum class DamageType : int32_t { Blunt = 0, Piercing, Slashing };  // Soul reserved
enum class AttackCategory : int32_t { Melee = 0, Ranged };
enum class CombatStance : int32_t { Melee = 0, Ranged };

// At most this many attacks per entity ("most characters have 1-2").
inline constexpr int kMaxAttacks = 3;

// One attack-skill. crit_chance is PER-ATTACK (a piercing thrust is authored with
// a higher crit than a slash), so the damage type's crit affinity lives in the
// data rather than in a pipeline multiplier.
struct Attack {
    AttackCategory category = AttackCategory::Melee;
    DamageType damage_type = DamageType::Slashing;
    float base_damage = 0.0f;
    float range = 0.0f;
    float cooldown = 0.0f;    // seconds between uses of THIS attack, from RESOLVE
    float crit_chance = 0.0f;
    // Commitment (game/src/strike.h). The attacker neither moves nor thinks
    // through either window, which is what makes standing still to shoot cost
    // ground. wind_up is CANCELLABLE (a stun drops the blow); recovery is not
    // (the blow was already thrown).
    //
    // `cooldown` keeps its meaning and is measured from resolve, with recovery
    // running INSIDE it -- so a weapon needs no third redundant number, and an
    // un-authored attack (both 0) resolves instantly, exactly as before.
    float wind_up_seconds = 0.0f;
    float recovery_seconds = 0.0f;
};

// Tactical stats (the resolve_attack inputs) + the class engagement preference.
// accuracy: the attacker's chance to beat the target's parry/shield (gate 1).
// evasion:  the defender's chance to dodge an on-target blow (gate 2).
// defense:  the defender's parry/block (contested by accuracy in gate 1).
// armour:   flat damage reduction (gate 3).
// What a critical hit multiplies penetrated damage by, before any status has
// its say. Lives here rather than beside combat.cpp's other tuning constants
// because it is the DEFAULT of the field below, and a default initializer needs
// it visible; everything else about crits stays in resolve_attack.
inline constexpr float kBaseCritMultiplier = 2.0f;

struct Combatant {
    float accuracy = 0.0f;
    float evasion = 0.0f;
    float defense = 0.0f;
    float armour = 0.0f;
    // Attacker-side, and per-entity so a STATUS can raise it: effective_combatant
    // returns a copy, so a bonus rides through to resolve_attack without a new
    // CombatRequest field and without touching a single assembly site.
    float crit_multiplier = kBaseCritMultiplier;
    CombatStance stance = CombatStance::Melee;
    // reserved (deferred psychology): float willpower, resolve;
};

// Per-level stat deltas. A creature's stats are ALWAYS
//   stat = base + growth * (level - 1)
// recomputed from scratch (game/src/progression.h's apply_level_stats), never
// accumulated -- so a replay that recomputes lands on identical floats instead
// of drifting, and calling it twice at one level is a no-op.
//
// The rates come from the design doc's level-15 rating table (docs/design/
// game-design.html §5.2) times a per-stat step, which is why linear stat
// growth produces a CONVEX power curve: power is roughly dps x ehp, a product
// of two rising terms. Armour's flat reduction self-plateaus against rising
// damage, reproducing the doc's "armour scaling hits diminishing returns"
// without any special case.
//
// Monsters leave this zeroed -- they do not level.
struct StatGrowth {
    float hp = 0.0f;           // flat, per level
    float accuracy = 0.0f;     // flat
    float evasion = 0.0f;      // flat
    float defense = 0.0f;      // flat
    float armour = 0.0f;       // flat
    // FRACTION of each attack's own base_damage, per level -- so a big weapon
    // gains more per level than a small one at the same design rating.
    float damage_frac = 0.0f;
};

// The creatures the sim knows by name. Append-only: JSON overrides key by
// name, and SpawnCreature spawns by id. The first
// HERO_CLASS_COUNT ids line up with HeroClassId, so a hero class maps straight
// to its creature.
//
// Declared HERE, above CharacterDesc, because a desc names its own creature
// (CharacterDesc::creature) -- the catalog struct that uses these ids still
// lives further down, with the rest of the catalog.
enum class CreatureId : int32_t {
    Mercenary = 0,
    Hunter,
    GraveRobber,
    Apprentice,
    Rat,
    Goblin,
    Deer,
    Bandit,
    BanditArcher,
    BanditLeader,
    MudGolem,
    Count,
};
inline constexpr int kCreatureCount = static_cast<int>(CreatureId::Count);

// Spawn input. pos is on the ground (XZ) plane, matching the renderer.
struct CharacterDesc {
    Archetype archetype = Archetype::Hero;
    // Which catalog creature this desc IS. Authored by CreatureCatalog for
    // every row; Count for a hand-built desc that names no creature. Copied
    // onto the entity at spawn (components.h's CreatureKind) because the
    // threat table is keyed by creature, and nothing else recorded what a
    // spawned entity actually is.
    CreatureId creature = CreatureId::Count;
    // Explicit HeroClassId for hero descs; -1 = derive from the recruiting
    // guild at spawn (heroes.cpp's spawn_entity). The hero creature-catalog
    // defs (creature_catalog.cpp) author this so a directly-spawned/recruited
    // hero and a catalog-spawned one agree on class before spawn-time grants
    // (e.g. grant_skills_for_level) run.
    int32_t hero_class = -1;
    float pos_x, pos_z;
    int32_t team;
    float hp;
    float move_speed;       // units/sec
    float attack_range;
    float attack_damage;
    float attack_cooldown;  // seconds between swings
    float size_x, size_y, size_z;
    float color_r, color_g, color_b;
    // Fog-of-war vision (only kPlayerTeam entities grant the player vision).
    // radius 0 => grants no vision. cone_half_angle_deg is the half-angle of
    // the forward vision cone (>= 180 => full circle). facing is the initial
    // XZ look direction; {0,0} => the model-forward default (kCharacterForward).
    float vision_radius = 0.0f;
    float vision_cone_half_angle_deg = 180.0f;
    float facing_x = 0.0f, facing_z = 0.0f;
    // --- combat loadout (Stage-3) -------------------------------------------
    // Tactical stats (default = "reduces to the old deterministic melee": full
    // accuracy, no defense/evasion/armour) plus up to kMaxAttacks attack-skills.
    // When attack_count == 0 the spawn path derives a single melee attack from
    // the legacy attack_* fields above, so an un-authored desc still fights.
    float accuracy = 1.0f;
    float evasion = 0.0f;
    float defense = 0.0f;
    float armour = 0.0f;
    CombatStance stance = CombatStance::Melee;
    Attack attacks[kMaxAttacks]{};
    int32_t attack_count = 0;
    // Per-level deltas applied to the stats above (see StatGrowth). The values
    // here are the LEVEL-1 row; growth carries the creature from there.
    StatGrowth growth{};
    // Level-gated skill acquisition (see SkillGrantRow). Rows fire at their
    // exact level: spawn applies the level-1 ones, and the level-up hook
    // (game/src/progression.h) applies the rest as the hero grows.
    SkillGrantRow skill_grants[kMaxSkills]{};
    int32_t skill_grant_count = 0;
    // XP paid out on this creature's death, split over nearby heroes (see
    // ProgressionFactors.kill_xp_radius). 0 = no reward (deer, heroes, dummies).
    int32_t xp_reward = 0;
};

// ---- named-creature catalog ------------------------------------------------

// A CharacterDesc template per creature (pos/team filled in at spawn). Compiled
// defaults live in creature_catalog.cpp; an app may override fields by name from
// JSON and push the result through Sim::SetCreatureCatalog. Held per-Sim, so it is
// initial config in the determinism contract (a replay must use the same catalog).
struct CreatureCatalog {
    CreatureCatalog();  // fills the compiled defaults
    CharacterDesc defs[kCreatureCount];
};

// Stable inspection/JSON name for a creature id ("Mercenary"), or "" if invalid.
const char* CreatureName(CreatureId id);
// Parse a creature name; returns CreatureId::Count if unknown.
CreatureId CreatureIdFromName(const char* name);

// The one shared compiled default catalog. MercenaryDesc/GoblinDesc/hero_desc read
// from this rather than each constructing their own copy of the defaults.
const CreatureCatalog& DefaultCreatureCatalog();

// Default in-game day length, in milliseconds of sim world time (a fast day, for
// prototyping). Only the default: WorldConfig::millis_per_day below is what a
// world actually runs on.
inline constexpr int64_t kDefaultMillisPerDay = 120 * 1000;

// Day length in sim world-millis for a day that should take `sim_seconds` of
// presentation time at 1x speed (i.e. SimClock::real_seconds_per_day) -- use
// this to keep a rendered day/night cycle and the sim's own day in lockstep.
//
// Converts through TICKS, not sim_seconds * 1000: the sim advances by a whole
// 33 ms per tick at 30 Hz, so a sim-second is 990 ms of world time, not 1000.
// Multiplying by 1000 instead would leave the sim clock running ~1% fast against
// the sky, permanently and cumulatively.
//
// Non-positive or absurdly large inputs clamp to a valid period (the sim divides
// by this, so it must stay >= 1 ms).
int64_t MillisPerDayForSimSeconds(float sim_seconds);

// Placement request: a raw (un-snapped) desired center + rotation. The sim
// snaps the center to the grid lattice for the kind's parity.
//
// Declared up here rather than beside the rest of the placement surface below
// because WorldConfig carries a list of them, and a world's initial config has
// to be describable before the placement API is.
struct PlacementDesc {
    int32_t kind;
    int32_t rotation_index;
    float world_x, world_z;
};

// Which terrain a world stands on. The town's hand-authored map, or nothing at
// all -- a world that wants no terrain says so rather than inheriting the
// town's and quietly standing on its central lake.
enum class MapKind : int32_t {
    Symbolic = 0,  // the hand-authored greybox map (SymbolicMapGenerator)
    FlatPlains,    // featureless flat plain everywhere (FlatMapGenerator)
};

// How to build the world (initial config).
struct WorldConfig {
    bool prebuild_colony = true;   // seed the colony Castle
    bool terrain_blocking = true;  // false = terrain stops nobody, and no navmesh is built
    MapKind map = MapKind::Symbolic;
    // Structures that exist the moment the world does, plopped in order through
    // plop_building -- so they carry no player margin and may abut into a
    // continuous run. Initial config, exactly like prebuild_colony: part of the
    // `state = f(config, log, ticks)` input rather than a command.
    //
    // The sim neither knows nor cares what shape they form. Whoever builds the
    // config does.
    std::vector<PlacementDesc> plops;
    // Length of one in-game day, in milliseconds of sim world time. Initial
    // config in the determinism contract: a replay must use the same value.
    // This sets what an in-game HOUR means (day/24), so it scales every
    // HeroFactors rate authored in hours -- a longer day drains needs
    // proportionally slower. Use MillisPerDayForSimSeconds above to derive it
    // from a presentation day length. Clamped to >= 1 ms at world construction.
    int64_t millis_per_day = kDefaultMillisPerDay;
};

// One in-flight projectile, for the debug-line overlay (Sim::Projectiles()).
struct ProjectileState {
    float x, z;                // current position, world XZ
    float target_x, target_z;  // where it is headed
};

// Per-living-entity snapshot row: the renderer reads pos/size/color, tests
// read team/hp.
struct CharacterState {
    uint32_t id;
    int32_t team;
    float pos_x, pos_z;
    float hp, max_hp;
    float size_x, size_y, size_z;
    float color_r, color_g, color_b;
    int32_t home_building_id;    // recruiting guild; -1 = homeless / not a hero
    int32_t inside_building_id;  // -1 = outside; >=0 => hidden (don't draw; list in panel)
    // Hero simulation/display state, for the inspector. Zeroed for non-heroes.
    // Need RESERVES in 0..1, where 1 is satisfied and 0 is spent (see
    // HeroFactors). Zeroed for non-heroes.
    float fatigue, content;
    int32_t behavior;        // last decided badlands::Behavior; -1 = none yet
    char name[24];           // NUL-terminated display name; "" for non-heroes
    // Current goal + pathfinding state: what this entity is walking toward now
    // and how far along the route it is.
    int32_t goal_kind;       // MoveTarget::Kind: 0 None, 1 Point, 2 Entity, 3 Building
    float goal_x, goal_z;    // goal position in world XZ (0,0 when goal_kind == 0)
    int32_t path_waypoints;  // waypoints remaining on the planned route
    int32_t archetype;       // Archetype (Hero/Townfolk/Critter/Monster)
    int32_t hero_class;      // HeroClassId; -1 for non-heroes. Lets an observer
                             // attribute behaviour to a class without a lookup.
    // Unit XZ look direction (the character Transform's rotation applied to the
    // model-forward axis, projected to XZ). Drives the vision cone and the
    // render pose. Always normalized (defaults to kCharacterForward).
    float facing_x, facing_z;
    // Fog-of-war vision this entity grants (0 radius => none). The renderer draws
    // the cone debug overlay from these + facing.
    float vision_radius;
    float vision_cone_half_angle_deg;  // >= 180 => full circle
    // --- hero progression (zeroed for non-heroes; level >= 1 marks a hero) --
    int32_t level;
    int32_t xp;          // progress toward the next level
    int32_t xp_next;     // cost of the next level at current factors
    int32_t skill_count;
    int32_t skills[kMaxSkills];  // SkillId values; only [0, skill_count) valid
};

// Run counters. NB: NOT `Stats` — badlands::Stats already exists (a sim
// component, game/src/components.h:24). Use SimStats for the run counters.
struct SimStats {
    uint64_t ticks;
};

// ---------------------------------------------------------------------------
// Goal statistics: how many entity-ticks each activity was active for, overall
// and per hero class. The point is to make a large run legible -- "apprentices
// never explore", "everybody is asleep by noon", "Flee is 40% of deer ticks" --
// so that something being off is visible rather than something you have to
// happen to be watching at the right moment.
//
// It is a FOLD OVER SNAPSHOTS, deliberately outside the sim core: neither
// tick_world nor any brain knows it exists. Two reasons, and both matter:
//
//   * A counter threaded through decision code drifts from reality the moment
//     one path forgets to bump it, and a wrong histogram is worse than none --
//     you would go looking for a bug in the AI that is really a bug in the
//     accounting.
//   * Folding the same rows an observer reads means the histogram cannot
//     disagree with the inspector next to it.
//
// Accumulate one snapshot per tick. Sim::Tick() does this for you; a caller
// driving the internal tick_world directly is measuring nothing and gets zeros,
// which is the honest answer.
// ---------------------------------------------------------------------------
class ActivityHistogram {
   public:
    // Folds one tick's rows in. Rows whose behavior is -1 ("has not decided
    // anything yet") are counted as samples but attributed to no activity.
    void Accumulate(std::span<const CharacterState> rows);
    void Reset();

    // Entity-ticks of this activity across every entity.
    uint64_t Total(ActivityId id) const;
    // Entity-ticks of this activity by heroes of one class.
    uint64_t ForClass(HeroClassId cls, ActivityId id) const;
    // Entity-ticks folded in altogether (the denominator for a share).
    uint64_t Samples() const { return samples_; }

   private:
    uint64_t total_[kActivityCount] = {};
    uint64_t per_class_[HERO_CLASS_COUNT][kActivityCount] = {};
    uint64_t samples_ = 0;
};

// Static per-kind footprint size (tiles), behavior flags, and recruit set.
struct BuildingDef {
    int32_t width_tiles, depth_tiles;
    bool poppable;            // auto-spawned (House/Sewer), never player-placed
    bool user_destructible;   // player may DESTROY (the 7 buildable kinds)
    bool enemy_targettable;   // future monster attacks may target (Castle/House)
    // Fog-of-war vision radius (world units) measured from the footprint EDGES
    // (a euclidean expansion of the footprint). 0 => grants no vision.
    float vision_radius;
    // Hero classes this kind recruits. recruit_count is 0 for non-guilds and 1
    // for today's guilds; the array has room for 2-3 (guilds are the only
    // kinds with recruit_count > 0). Declared per kind in placement.cpp kDefs.
    int32_t recruit_count;
    HeroClassId recruits[kMaxRecruitClasses];
};

// The world-space box a renderer should draw for a building of this kind and
// rotation. size is the local (pre-rotation) X/Z extent; apply yaw_radians
// about Y.
struct RenderBox {
    float size_x, size_z;
    float yaw_radians;
};

// One grid triangle in a probe readout.
struct GridTriangle {
    int32_t tile_x, tile_z;  // tile min-corner, in [-H, H)
    uint32_t corner;         // 0=N(-Z) 1=E(+X) 2=S(+Z) 3=W(-X)
    uint32_t state;          // 0=free 1=blocked 2=would-block (blocked wins)
};

struct PlacementProbe {
    bool valid;  // the snapped footprint could be placed now
    float snapped_x, snapped_z;
};

// Generic player->world action trigger. Every player action crosses this one
// entry point (Sim::Dispatch).
struct Action {
    ActionKind kind;
    uint32_t target_id;  // building/entity id for id-addressed actions
    float world_x, world_z;
    int32_t param_a, param_b;
};

// Per-building snapshot row (Characters() pattern). ids are stable and dense.
struct BuildingState {
    uint32_t id;
    BuildingKind kind;
    float center_x, center_z;
    int32_t rotation_index;
    int32_t width_tiles, depth_tiles;
    uint32_t taxable_income;  // uncollected tax owed (Houses accrue at midnight)
    float hp, max_hp;         // structure health (rats chew it down)
};

// World-level scalars (gold, grid size, sprawl bookkeeping).
struct WorldState {
    uint32_t gold;
    int32_t grid_half_extent_tiles;
    uint32_t queued_poppables;  // owed but not yet placeable (crowded map)
    uint32_t urban_quarters;    // sprawl accumulator in quarter-units
    uint32_t guild_roster_cap;  // kGuildRosterCap (heroes per guild); UI mirrors it
    // The sim clock. world_millis is the authoritative integer time (advanced by
    // a compile-time constant per tick at a fixed 30 Hz); the rest are derived.
    int64_t world_millis;
    float time_of_day;  // 0..1 within the current day
    uint32_t day;       // whole days elapsed
    int32_t is_night;   // 0/1
};

// ---------------------------------------------------------------------------
// Command log (the trace of record)
//
// Every mutation of the sim -- player action and AI decision alike -- is a
// Command applied at a single point, in deterministic order, and appended here.
// (initial config, seed, command log) reproduces a run exactly, so this is both
// the replay input and what a debug panel shows.
// ---------------------------------------------------------------------------
enum class CommandKindId : int32_t {
    PlaceBuilding = 0,
    RecruitHero,
    DestroyBuilding,
    MoveTo,
    EnterBuilding,
    EnterHome,
    Buy,
    Attack,
    SetBehavior,
    CollectTax,
    Deposit,
    AttackBuilding,
    Chat,
    Engage,  // hold at range of a live entity target (single-gateway combat's engagement executor)
    UseSkill,  // cast skill param_a (an index into the actor's OWN Skills) at target_id
    FocusSkill,  // begin a long cast of skill param_a at target_id (skill_focus.h)
};

struct CommandRecord {
    CommandKindId kind;
    uint32_t actor;  // entity id; UINT32_MAX = player/global
    uint32_t target_id;
    float point_x, point_z;
    int32_t param_a, param_b;
    int64_t at_millis;  // sim time the command took effect
};

// ---------------------------------------------------------------------------
// Game event stream (transient, presentation-facing)
//
// A generic, per-tick stream of notable things that HAPPENED (as opposed to
// CommandRecord, which is what was DECIDED). Damage is one kind; downing and
// building destruction are others. Consumers (the renderer's floating combat
// text + the HUD combat log) drain it each frame via Sim::DrainEvents.
//
// Unlike the command log, events are NOT part of the determinism contract:
// they are a pure function of the tick, so a replay reproduces them, but they
// are transient and never fed back into the sim. The buffer is cleared on drain.
// ---------------------------------------------------------------------------
enum class GameEventKind : int32_t {
    DamageDealt = 0,     // amount = hp removed; actor = attacker, target = victim
    BuildingDestroyed,   // target = building id; actor = attacker (or NONE)
    HeroDowned,          // a character's HP reached 0; actor = attacker (or NONE)
    HeroDied,            // reserved: true removal, distinct from downing. Not emitted yet.
    HeroLeveledUp,       // a hero crossed a level threshold; actor = hero slot, amount = new level
    StatusApplied,       // a status was applied/refreshed; target = afflicted slot,
                         // actor = who inflicted it, amount = the StatusKind
    SkillUsed,           // a skill was cast; actor = caster slot, target = its primary
                         // target (the caster itself for a self/untargeted cast),
                         // amount = the SkillId
    StrikeCancelled,     // a committed attack was interrupted during its wind-up and
                         // never landed; actor = the attacker whose swing was dropped,
                         // target = who it was aimed at, amount = the attack index
    FocusCancelled,      // a long cast was abandoned before it resolved -- stunned,
                         // re-decided, or no longer legal at its deadline; actor =
                         // the caster, target = what it was aimed at, amount = the
                         // SkillId. The strike's counterpart on the other channel
                         // (game/src/skill_focus.h).
};

// One event. Field meaning is per `kind` (see GameEventKind). `actor_id` and
// (for a Character target) `target_id` are entity SLOTS -- the same ids
// CharacterState.id carries; a Building target's id is its BuildingState.id.
// `x`/`z` is the victim's world position when the event fired, so a lethal hit
// still floats a number where the victim died (it is destroyed the same tick).
struct GameEvent {
    GameEventKind kind;
    uint32_t actor_id;    // attacker slot; UINT32_MAX = none/unknown
    uint32_t target_id;   // victim: character slot OR building id (see target_kind)
    int32_t target_kind;  // 0 = Character, 1 = Building
    float amount;         // DamageDealt: hp removed; else 0
    float x, z;           // victim world position at event time
    int64_t at_millis;    // sim time the event fired
};

// The two target_kind values, named for readability at call sites.
inline constexpr int32_t kEventTargetCharacter = 0;
inline constexpr int32_t kEventTargetBuilding = 1;

// Read-only snapshot of the published (front) fog-of-war field. The grid lives
// in the SIM coordinate frame; texel (i,j) covers the world square whose min
// corner is (world_min_x + i*texel_m, world_min_z + j*texel_m). `rg` is
// nx*nz*2 bytes, interleaved per texel: [2*k+0] = discovered (0 or 255),
// [2*k+1] = visible (0 or 255), k = j*nx + i. Pointer valid until the next
// Tick(); empty (rg==nullptr) until ConfigureVision() has been called.
struct VisionField {
    int32_t nx = 0, nz = 0;
    float world_min_x = 0.0f, world_min_z = 0.0f;
    float texel_m = 1.0f;
    const uint8_t* rg = nullptr;
};

// One navmesh cell for the debug overlay: an axis-aligned world-XZ rectangle, a
// terrain-cost multiplier, and whether a unit can stand on it. impassable cells
// (buildings, water, mountain) have passable == false.
struct NavDebugCell {
    float min_x = 0.0f, min_z = 0.0f, max_x = 0.0f, max_z = 0.0f;
    float cost = 0.0f;
    bool passable = false;
};

// A debug path query result: waypoints as flat world-XZ pairs (x0,z0,x1,z1,...),
// the total cost-weighted length, and whether the goal was reachable.
struct NavPathResult {
    std::vector<float> waypoints_xz;
    float cost = 0.0f;
    bool reachable = false;
};

// Which brain drives spawned heroes. wasm_bytes/wasm_len: a compiled brain
// wasm module (game/src/wasm_brain.h), hero-only -- see the think loop's
// dispatch order in sim.cpp: when loaded, it takes the no-enemy tick for
// BrainKind::Town entities outright (combat is a host pre-empt); every other
// archetype is always mock-driven. wasm_bytes == nullptr means every hero
// simply idles absent an enemy (no C++ decision layer to fall back to).
struct BrainDesc {
    const uint8_t* wasm_bytes = nullptr;
    size_t wasm_len = 0;
};

// ---- the sim ---------------------------------------------------------------
class Sim {
   public:
    // BrainDesc: which brain drives spawned heroes (see BrainDesc above).
    // wasm_bytes provided but failing to compile/instantiate is FATAL (crash-
    // and-error, not a fallback -- see WasmBrainRuntime::create's doc comment,
    // game/src/wasm_brain.h); wasm_bytes null is not a failure at all (every
    // hero simply idles absent an enemy).
    explicit Sim(const BrainDesc& brain_desc);
    // The composing form: an explicit world config (which map, what is
    // already built, how long a day is) x which brain. The other ctor forwards
    // here conceptually (single make_world implementation, sim.cpp).
    Sim(const WorldConfig& config, const BrainDesc& brain_desc);
    ~Sim();
    Sim(Sim&&) noexcept;
    Sim& operator=(Sim&&) noexcept;
    Sim(const Sim&) = delete;
    Sim& operator=(const Sim&) = delete;

    // Returns the entity id used in CharacterState rows. `level` > 1 puts a
    // HERO at that level (see SpawnCreature).
    uint32_t Spawn(const CharacterDesc& desc, int32_t level = 1);
    // Spawns a named creature from the catalog at (pos_x, pos_z) on `team`;
    // returns its entity id. Hero creatures also get their hero class set.
    //
    // `level` > 1 starts a HERO partway up the ladder: its stats are recomputed
    // from its growth row and every skill grant up to that level is applied, so
    // it is indistinguishable from one that earned its way there. Ignored by
    // anything that does not level -- monsters have a threat anchor authored at
    // level 1 and no growth row at all. INITIAL CONFIG, not a command: a replay
    // reproduces it from the same spawn call, exactly as prebuild_colony is.
    uint32_t SpawnCreature(CreatureId id, int32_t team, float pos_x, float pos_z,
                           int32_t level = 1);
    // Replaces the creature catalog (see CreatureCatalog). Call before spawning;
    // a replay must use the same catalog the recorded run used.
    void SetCreatureCatalog(const CreatureCatalog& catalog);
    const CreatureCatalog& Creatures() const;
    // Replaces the skill template catalog (see SkillCatalog). Durations and
    // cooldowns are clamped non-negative at this boundary. Initial config:
    // call before ticking; a replay must use the same catalog.
    void SetSkillCatalog(const SkillCatalog& catalog);
    const SkillCatalog& Skills() const;
    void Tick(float dt);
    // Executes a player action. Returns >= 0 on success (a new building/hero
    // id, or 0 for id-less actions) and < 0 on error.
    int64_t Dispatch(const Action& action);

    // --- Fog-of-war (vision) ----------------------------------------------
    // Sizes/anchors the vision grid (SIM frame). Must be called before the
    // field is meaningful; Tick() resolves vision only once configured. A
    // grid of ceil(world_size / texel_m) texels per axis is allocated.
    // Idempotent for the same params; re-sizing resets the discovered history.
    void ConfigureVision(float world_min_x, float world_min_z, float world_size_x,
                         float world_size_z, float texel_m);
    // Resolve + publish the vision field immediately, WITHOUT advancing the sim
    // (no brains/movement/combat, no tick-counter bump). Tick() also resolves at
    // its end; this is for populating the field before the first render when no
    // Tick has run yet (e.g. a headless single-frame --screenshot). No-op until
    // ConfigureVision().
    // NB: priming discovers texels CREDITLESSLY (no exploration XP) -- it is a
    // presentation affordance outside the tick/determinism contract.
    void ResolveVision();
    // The published (double-buffered) field. Read by the renderer to upload the
    // vision texture. Empty until ConfigureVision().
    VisionField GetVisionField() const;
    // Object-bounds query: the highest VisionLevel over the grid texels within
    // `radius` of (cx, cz) in the SIM frame (Visible wins over Dormant over
    // Unknown). Drives per-entity render decisions (e.g. hide units). Returns
    // Unknown when the vision grid is unconfigured or the bounds miss it.
    VisionLevel QueryVision(float cx, float cz, float radius) const;

    // --- Navmesh debug (Dear ImGui overlay) -------------------------------
    // Both ensure the navmesh reflects the current world (rebuild-if-stale)
    // regardless of terrain_blocking, so the overlay always shows a live mesh.
    // One entry per navmesh cell: rectangles + cost + passability.
    std::vector<NavDebugCell> NavDebugCells();
    // Shortest cost-respecting path from (sx,sz) to (gx,gz) for the overlay.
    NavPathResult NavQuery(float sx, float sz, float gx, float gz);

    // Snapshot accessors — identical semantics to the old ABI, POD vectors.
    std::vector<CharacterState> Characters() const;  // was game_state
    std::vector<BuildingState> Buildings() const;    // was game_buildings
    // Out-param overloads: reuse the caller's buffer (out.clear() then fill),
    // avoiding a per-frame allocation on the render path. These are the
    // primitives; the value-returning versions delegate to them.
    void Characters(std::vector<CharacterState>& out) const;
    void Buildings(std::vector<BuildingState>& out) const;
    // In-flight projectiles, for the debug-line overlay.
    std::vector<ProjectileState> Projectiles() const;
    WorldState World() const;
    // Replaces the tuning factors (see SimFactors). Call before ticking; a
    // replay must use the same factors the recorded run used.
    void SetFactors(const SimFactors& factors);
    const SimFactors& Factors() const;
    // The applied-command trace, oldest-first (see CommandRecord).
    std::vector<CommandRecord> CommandLog() const;                         // was game_world
    // Drains this tick-batch's game events into `out` (out.clear() then fill),
    // emptying the sim's internal buffer -- call once per frame. Reuses the
    // caller's buffer (no per-frame allocation), mirroring Characters(out).
    void DrainEvents(std::vector<GameEvent>& out);
    // Dominant biome at a world XZ, as a mapgen::Biome index (0 Lake, 1 Swamp,
    // 2 Forest, 3 Plains, 4 Hills, 5 Mountain). The sim owns the terrain field;
    // callers (deer placement, later biome-weighted nav) query it here rather
    // than re-deriving the world<->map offset. Out of bounds clamps.
    int32_t BiomeAt(float world_x, float world_z) const;
    SimStats GetStats() const;                        // was game_stats
    // Goal statistics accumulated across Tick() calls (see ActivityHistogram).
    // Folded in the wrapper from the same snapshot rows Characters() returns --
    // the sim core does no counting, so this cannot drift from what the brains
    // actually did.
    const ActivityHistogram& ActivityStats() const { return activity_stats_; }
    void ResetActivityStats() { activity_stats_.Reset(); }
    // Placement preview; returns validity, fills out_triangles (was
    // game_probe_placement).
    PlacementProbe ProbePlacement(const PlacementDesc& desc,
                                  std::vector<GridTriangle>& out_triangles) const;

    // The shared world. Later increments read render/sim components off this.
    entt::registry& registry();
    const entt::registry& registry() const;

   private:
    std::unique_ptr<::BadlandsGame> world_;  // the EXISTING internal world, unchanged
    ActivityHistogram activity_stats_;
    // Reused across ticks so the per-tick fold costs no allocation.
    std::vector<CharacterState> stats_scratch_;
};

// ---- handle-less helpers (were game_*; pure computations) ------------------
BuildingDef BuildingDefOf(BuildingKind kind);                     // was game_building_def
RenderBox RenderBoxOf(BuildingKind kind, int32_t rotation_index);  // was game_render_box
CharacterDesc MercenaryDesc(float pos_x, float pos_z);             // was game_desc_mercenary
CharacterDesc GoblinDesc(float pos_x, float pos_z);               // was game_desc_goblin

}  // namespace badlands
