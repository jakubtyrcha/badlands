# Core Classes — Combat Mechanics, Simulation & Reporting

> **For agentic workers:** REQUIRED SUB-SKILL: use `superpowers:subagent-driven-development` (recommended) or `superpowers:executing-plans` to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax.
>
> Plan file lives here only because plan mode restricts writes; copy it to `docs/superpowers/plans/2026-07-31-core-classes-combat.md` as the first commit of implementation. The approved design is at `docs/superpowers/specs/2026-07-31-core-classes-combat-design.md` — **amend its §7 in that same commit**: threat is not a `CharacterDesc` label but a fixed calibration table with two roles (Task 0b below), with the anchors listed in Task 2.

## Context

The sim has four hero classes that are barely distinguishable in a fight. Stats are fixed at spawn (`level` only fires skill grants), attacks resolve instantly and for free, and `MeleeLock` is a hard freeze that makes "move, shoot, move" literally unreachable — a ranged unit touched once can never move again, so it can never break contact, so the lock never releases.

This builds the **mechanics** that let classes differ, plus the **simulation harness** that shows whether they do. Per the user's redirect: balance is an approximation, not the deliverable. Duel outcomes are **reported as a matrix**, not asserted into a target; stat parameters are **reported as SVG charts**. Tests assert mechanism — growth math, wind-up cancellation, disengage gating, determinism — never a balance outcome.

**Goal:** Four classes with distinct, level-scaling stats fight with attacks that cost time to throw; ranged brains keep their distance because leaving contact is ruinous; each class has one combat skill. A headless tool simulates every pairing and writes a win matrix plus parameter charts.

**Tech Stack:** C++20 + EnTT + Catch2 (`game/`), Nim → wasm32-wasi (`scripts/brains/nim/`), Rust (`src/crates/brainhost`), nlohmann/json (manifests), CMake/Ninja.

## Global Constraints

- **Every mutation is a `Command`** appended to `command_log`; `state = f(initial config, command log, N ticks)`. Pure system rules that derive from the deterministic clock (projectile arrival, status expiry) run outside the log — `advance_projectiles` is the precedent this plan follows twice.
- **Append-only id spaces.** Never renumber `CreatureId`, `SkillId`, `StatusKind`, `CommandKind`/`CommandKindId`, `GameEventKind`, `BL_ACT_*`, `BL_INT_*`, `BL_ST_*`, `BL_FX_*`.
- **Sim timers are int64 milliseconds** advanced by `kMillisPerTick` — never a float dt accumulator. Cooldowns stay float seconds to match the existing `Attacks`/`Skills` fields.
- **Every numerical value is data-driven**: compiled defaults in `game/src/creature_catalog.cpp` + `game/src/skills.cpp`, overridable by name from `assets/creatures/creatures.json` and `assets/skills/skills.json`.
- **Tests never assert on shipped data files**; manifest tests write test-local fixtures.
- **No balance assertions.** The duel matrix is printed, not gated.
- **No debug controls that weren't asked for** — no ImGui panels, sliders, env hooks.
- **`ctest` is canonical** for C++ suites; Rust crate tests need `--lib`.
- **`.wasm` artifacts are git LFS** — stage `assets/brains/*.wasm` deliberately after a rebuild.
- Work on a normal branch; no git worktrees in this repo.

---

## File Structure

**New**
- `game/src/strike.h` / `strike.cpp` — the attack-commitment mechanic: `StrikeInProgress`, `declare_strike`, `advance_strikes`, `cancel_strike`.
- `src/executables/duelsim/svg_chart.h` / `svg_chart.cpp` — a self-contained line-chart → SVG string writer. No game types, no dependencies.
- `src/executables/duelsim/duel_matrix.h` / `duel_matrix.cpp` — stage a pairing, run to resolution, tally.
- `src/executables/duelsim/main_duelsim.cpp` — CLI: run everything, write the report.
- `src/executables/duelsim/tests/duelsim_tests.cpp` — svg_chart + duel-runner determinism.
- `game/tests/growth_tests.cpp`, `game/tests/strike_tests.cpp`, `game/tests/disengage_tests.cpp`.

**Modified (principal)**
- `game/src/threat_table.{h,cpp}` — combat potential per (creature, level): FIXED authored anchors, never derived. Both the calibration target and the number brains compare (Task 0b).
- `game/include/badlands_sim.hpp` — `StatGrowth`; `CharacterDesc.growth`; `Attack.wind_up_seconds`/`.recovery_seconds`; `StatusKind::{Disengaged, Cursed}`; `CreatureId` +4; `SkillId` +3; `GameEventKind::StrikeCancelled`.
- `game/src/creature_catalog.cpp` — rebalanced level-1 rows, growth rows, four new creatures.
- `game/src/progression.{h,cpp}` — `apply_level_stats`, called at spawn and on level-up.
- `game/src/combat.cpp` — `fire_attack` declares instead of resolving.
- `game/src/movement.cpp` — `MeleeLock` no longer freezes; `StrikeInProgress` does; disengage detection.
- `game/src/status.cpp` — `Stunned` cancels a wind-up.
- `game/src/intention.h`, `game/src/command.cpp` — `Disengaged`/`StrikeInProgress` refuse actions.
- `game/src/skills.cpp`, `game/src/skill_cast.cpp`, `game/src/skill_abi.h` — `BL_FX_HEAL` + three effects.
- `game/src/brain_abi.h`, `game/src/wasm_brain.cpp`, `scripts/brains/nim/{abi,hero_view,hero}.nim`, `src/crates/brainhost/src/lib.rs` — ABI v5.
- `game/src/monster_brain.cpp` — the Bandit Archer's skirmish.
- `src/game/creature_manifest.cpp`, `src/game/skill_manifest.cpp` and their tests.
- `CMakeLists.txt` — three test sources, the `badlands_duelsim` target, `badlands_duelsim_tests`.

---

## Task 0a: Bring the design docs into the repo

Mechanical, no code. Do it first so everything after can cite it.

**Files:** copy `/Users/jakub/repos/docs/{game-design.html, theme.css, fonts/}` → `docs/design/`, joining the `intention-contract.html` already there.

- [ ] **Step 1: Copy, excluding cruft**

```bash
cp /Users/jakub/repos/docs/game-design.html /Users/jakub/repos/docs/theme.css docs/design/
rsync -a --exclude='.DS_Store' /Users/jakub/repos/docs/fonts/ docs/design/fonts/
```

`fonts/` must sit as a SIBLING of `theme.css`: the stylesheet's `@font-face` rules use relative `fonts/IM_Fell_English/...` paths, so that layout is what keeps the page rendering. 23 `.ttf` (~6.9 MB) plus their 11 licence `.txt` files.

- [ ] **Step 2: Verify it renders** — open `docs/design/game-design.html` in a browser and confirm the fonts load (the headings are Pirata One / IM Fell English, not a fallback serif).

- [ ] **Step 3: Commit — `.ttf` is git LFS, so stage deliberately**

```bash
git add docs/design/game-design.html docs/design/theme.css
git add docs/design/fonts
git lfs ls-files | grep -c 'docs/design/fonts'   # expect 23; a raw binary here means LFS missed it
git commit -m "docs: bring the game design document and its fonts into the repo"
```

**Flag at review, do not block:** `fonts/Caldareth PERSONAL USE ONLY!.ttf` is licensed for personal use only. Committing it to a repo that may be published is a licensing call for the user to make — mention it, keep it unless told otherwise.

---

## Task 0b: The role of threat

Not a task — the shared understanding Tasks 2, 5, and 9 all implement. Written into the spec's §7 in the first commit.

**Threat approximates a creature's combat potential: what it is worth in a fight.** One number, deliberately serving two roles:

1. **A source of calibration.** The anchors are FIXED targets, authored, never derived from stats. The duel matrix measures the gap between what a creature is supposed to be worth and what it actually wins. Closing that gap means moving the STATS toward the anchors — never the anchors toward the stats, which would make the whole exercise circular.
2. **A source of brain decisions.** Because role 1 keeps it a reasonable approximation, a brain can compare its own threat against a hostile's and be smart about whether to pick a fight at all. This is what turns threat from a spreadsheet column into a game mechanic.

The dependency runs one way and is worth stating plainly: **role 2 is only as trustworthy as role 1 has been done.** A brain deciding on a badly-calibrated threat number makes confident, wrong choices — which is why the calibration report exists before any fleeing behaviour does.

**In this plan:** the number is authored (Task 2), queryable per live entity (Task 2), carried on the wire for both self and every perceived hostile (Task 5), and measured (Task 9). The brain uses it for ONE decision here — how far a skirmisher wants to stand off. **Fleeing is an explicit follow-up**, named in "Declared but not executed" below; the data it needs is in place, the behaviour is not.

---

## Task 1: Stat growth and level scaling

**Files:** `game/include/badlands_sim.hpp`, `game/src/components.h`, `game/src/progression.{h,cpp}`, `game/src/heroes.cpp`, `game/src/creature_catalog.cpp`, `src/game/creature_manifest.cpp`, `assets/creatures/creatures.json`; tests `game/tests/growth_tests.cpp`, `src/game/tests/creature_manifest_tests.cpp`; `CMakeLists.txt`

**Interfaces produced:**

```cpp
// badlands_sim.hpp — per-level deltas. Authored beside the base stats and
// overridable from creatures.json; monsters leave it zeroed.
struct StatGrowth {
    float hp = 0.0f;             // flat, per level
    float accuracy = 0.0f;       // flat
    float evasion = 0.0f;        // flat
    float defense = 0.0f;        // flat
    float armour = 0.0f;         // flat
    float damage_frac = 0.0f;    // FRACTION of each attack's base_damage, per level
};
// CharacterDesc gains: StatGrowth growth; float threat = 0.0f;

// components.h — the spawn-time copy, so the level-up hook needs no desc.
// Mirrors SkillGrants exactly (game/src/heroes.cpp emplaces both together).
struct BaseStats {           // level-1 values, the growth origin
    float hp, accuracy, evasion, defense, armour;
    float attack_damage[kMaxAttacks];
    int32_t attack_count = 0;
};
struct Growth { StatGrowth rows; };

// progression.h — recompute every level-scaled stat for `level`. Idempotent:
// always base + growth*(level-1), never an accumulating += , so calling it
// twice at the same level is a no-op and a replay lands on identical floats.
// Current hp scales WITH max (a level-up is not a free heal to full) and is
// clamped to the new max.
void apply_level_stats(entt::registry& reg, entt::entity e, int32_t level);
```

- [ ] **Step 1: Write the failing tests** — `game/tests/growth_tests.cpp`

```cpp
TEST_CASE("stats are base + growth * (level-1)", "[growth]") {
    // Spawn a Mercenary; apply_level_stats at 1, 3, 15 and check hp/armour/damage
    // against the arithmetic. Assert on the FORMULA, not on authored values:
    //   expect_hp = base.hp + growth.hp * (level - 1)
}
TEST_CASE("apply_level_stats is idempotent", "[growth]") {
    // Call it twice at the same level -> identical Combatant/Attacks/Health.max.
}
TEST_CASE("a level-up is not a free heal", "[growth]") {
    // Drop hp to 50% of max, level up, assert hp/max is still ~50% and hp <= max.
}
TEST_CASE("award_xp scales stats as it levels", "[growth][progression]") {
    // Award enough XP for two levels; assert Health.max grew by 2 * growth.hp.
}
TEST_CASE("monsters have zero growth", "[growth]") {
    // A Rat at level 5 reads identical to a Rat at level 1.
}
```

Plus `src/game/tests/creature_manifest_tests.cpp` (test-local fixture, never the shipped asset): a `"growth": {...}` block parses; an unknown growth key fails the load; `"threat"` parses.

- [ ] **Step 2: Register sources, run, confirm failure**

Add `game/tests/growth_tests.cpp` beside `game/tests/progression_tests.cpp` in `CMakeLists.txt` (the `badlands_game_tests` source list, ~line 586). Run `scripts/build.sh badlands_game_tests` → FAIL.

- [ ] **Step 3: Implement**

- `badlands_sim.hpp`: `StatGrowth`, `CharacterDesc::growth`.
- `components.h`: `BaseStats`, `Growth`.
- `heroes.cpp` `spawn_entity`: emplace `BaseStats` (from the desc's authored values) and `Growth` right where `SkillGrants` is emplaced today (`heroes.cpp:190-205`), then call `apply_level_stats(reg, e, 1)`. Emplace them for EVERY archetype, not just heroes — a monster's zeroed growth makes the call a no-op, and it keeps one code path.
- `progression.cpp`: `apply_level_stats`; call it in `award_xp`'s level loop beside `grant_skills_for_level` (`progression.cpp:46`).
- `creature_catalog.cpp`: the rebalanced level-1 rows and growth rows from the design doc §2.
- `creature_manifest.cpp`: parse `"growth"` (an object of the six named floats, unknown key = fail the load, matching the file's existing posture).
- `assets/creatures/creatures.json`: state the growth rows the compiled defaults already carry.

- [ ] **Step 4: Run the tests** — `scripts/test.sh badlands_game_tests "[growth]"`, then `scripts/test.sh`.

- [ ] **Step 5: Commit**

```bash
git add game/include/badlands_sim.hpp game/src/components.h game/src/progression.h \
        game/src/progression.cpp game/src/heroes.cpp game/src/creature_catalog.cpp \
        src/game/creature_manifest.cpp src/game/tests/creature_manifest_tests.cpp \
        game/tests/growth_tests.cpp assets/creatures/creatures.json CMakeLists.txt
git commit -m "feat(game): stats scale with level from an authored growth row"
```

---

## Task 2: The enemy roster and the threat calibration table

**Files:** create `game/src/threat_table.{h,cpp}`, `game/tests/threat_table_tests.cpp`; modify `game/include/badlands_sim.hpp` (`CreatureId`), `game/src/creature_catalog.cpp`, `assets/creatures/creatures.json`, `CMakeLists.txt`; tests `game/tests/combat_tests.cpp`, `src/game/tests/scenario_tests.cpp`

Four appended creatures, each chosen to exercise something the roster currently cannot:

| Creature | hp / spd | acc / eva / def / arm | attack | xp |
|---|---|---|---|---|
| `Bandit` | 26 / 2.8 | .85 / .10 / .10 / 2 | sword slash 5, r1.4, cd1.0, crit.10 | 60 |
| `BanditArcher` | 18 / 3.0 | .85 / .15 / .05 / 1 | bow pierce 4, r7, cd1.3, crit.20 · knife slash 2, r1.2, cd0.8 | 60 |
| `BanditLeader` | 45 / 2.8 | .95 / .15 / .20 / 4 | sword slash 8, r1.5, cd0.9, crit.15 | 200 |
| `MudGolem` | 90 / 1.4 | .80 / 0 / 0 / 8 | slam **blunt** 12, r1.8, cd2.2, crit.05 | 250 |

`BanditArcher` is `CombatStance::Ranged`; the rest are Melee. The Golem's blunt slam is the only exercise anywhere of `apply_armour`'s Blunt branch and of `resolve_attack`'s "Blunt bypasses Defense" gate.

### The threat table

**Threat is a FIXED calibration target, never derived from stats.** It is what a creature is *supposed* to be worth; the duel matrix then reports how far reality sits from it. Deriving it from the stats would make it a restatement of the numbers rather than a target to measure them against, and the whole report would be circular.

```cpp
// threat_table.h — a creature's combat potential: what it is worth in a
// fight. TWO ROLES from one number (see Task 0b / the spec's §7):
//   1. CALIBRATION. These anchors are fixed TARGETS. The duel matrix measures
//      how far the stats land from them; balancing moves the stats, never
//      these -- deriving threat from stats would make the report circular.
//   2. BRAIN DECISIONS. Because role 1 keeps it a fair approximation, a brain
//      compares its own threat with a hostile's to decide whether a fight is
//      worth taking. Carried on the wire (brain_abi.h) for exactly this.
//
// COMPILED, not JSON, and deliberately so: every other number in this sim is
// a tunable, and this one is the fixed post the tunables are measured against.
// The values will change as the design settles -- but by editing this table on
// purpose, never as a side effect of tuning a stat.
namespace badlands {

struct ThreatAnchor { int32_t level; float threat; };

// The intended threat of `creature` at `level`. Monsters carry a single
// level-independent anchor; heroes are anchored per level, one point today.
// Linearly interpolated between anchors, flat outside the authored range.
float threat_target(CreatureId creature, int32_t level);

// The threat of a LIVE entity: its creature's anchor at its current level.
// The one call the wire packer and any brain-facing consumer uses, so nobody
// re-derives "which creature is this" independently. 0 for an entity with no
// CreatureKind (a test dummy, a building). Deliberately NOT scaled by current
// health or statuses -- what a wounded enemy is worth right now is a JUDGEMENT,
// and judgements belong to the brain, which already sees health on the wire.
float threat_of(const entt::registry& reg, entt::entity e);

}  // namespace badlands
```

`threat_of` needs the entity to know what it is, which nothing records today — `spawn_creature_into` takes a `CreatureId` but keeps it nowhere. So:

```cpp
// badlands_sim.hpp — CharacterDesc gains, authored by the catalog:
CreatureId creature = CreatureId::Count;   // Count = "not a catalog creature"
// components.h — the spawn-time copy, mirroring SkillGrants/Growth:
struct CreatureKind { CreatureId id = CreatureId::Count; };
```

`CreatureCatalog`'s constructor sets `d.creature` for every row (it already has the id in hand at each `at(...)` call), and `spawn_entity` emplaces `CreatureKind` beside `BaseStats`/`Growth`.

Anchors, as set by the user (these supersede the design doc's own power-curve keyframes where they differ — the doc had Hunter and Grave Robber sharing one curve at 2, these split them):

| Creature | level | threat |
|---|---|---|
| `Mercenary` | 1 | **2.5** |
| `Hunter` | 1 | **1.5** |
| `GraveRobber` | 1 | **1.0** |
| `Apprentice` | 1 | **0.75** |
| `Rat` | any | **0.25** |
| `Goblin` | any | 1.0 |
| `Bandit` | any | 2.0 |
| `BanditArcher` | any | 2.0 |
| `BanditLeader` | any | 5.0 |
| `MudGolem` | any | 6.0 |
| `Deer` | any | 0.0 |

The level-1 stat rows in Task 1 already point the right way for these targets — notably the Grave Robber's crossbow dropping to `cd 2.5`, without which it out-DPSes the Hunter's bow and inverts the 1.5-vs-1.0 ordering.

- [ ] **Step 1: Write the failing tests**

```cpp
// game/tests/threat_table_tests.cpp — the anchors ARE the contract here, so
// unlike every other test in this plan these assert the authored values.
// That is the point: a silent edit to a calibration post must fail a test.
TEST_CASE("the calibration anchors are what the design says", "[threat]") {
    CHECK(threat_target(CreatureId::Mercenary,   1) == Catch::Approx(2.5f));
    CHECK(threat_target(CreatureId::Hunter,      1) == Catch::Approx(1.5f));
    CHECK(threat_target(CreatureId::GraveRobber, 1) == Catch::Approx(1.0f));
    CHECK(threat_target(CreatureId::Apprentice,  1) == Catch::Approx(0.75f));
    CHECK(threat_target(CreatureId::Rat,         1) == Catch::Approx(0.25f));
}
TEST_CASE("a single-anchor creature is level-independent", "[threat]") {
    CHECK(threat_target(CreatureId::Rat, 20) == Catch::Approx(threat_target(CreatureId::Rat, 1)));
}
TEST_CASE("interpolation is linear between anchors and flat outside", "[threat]") {
    // Drive a locally-authored two-anchor table (not the shipped one) through
    // the interpolation helper: midpoint is the mean; beyond the last anchor
    // holds flat rather than extrapolating off a cliff.
}
TEST_CASE("every creature has an anchor", "[threat]") {
    for (int i = 0; i < kCreatureCount; ++i) {
        CHECK(threat_target(static_cast<CreatureId>(i), 1) >= 0.0f);  // no gaps
    }
}
TEST_CASE("threat_of reads a live entity's creature and level", "[threat]") {
    // Spawn a Mercenary -> threat_of == threat_target(Mercenary, 1). Level it
    // to 3 and it tracks the table, not a cached spawn-time value.
}
TEST_CASE("threat_of ignores damage", "[threat]") {
    // Drop the entity to 10% hp: threat_of is unchanged. What a wounded enemy
    // is WORTH is the brain's judgement, and the brain already sees health.
}
TEST_CASE("threat_of is 0 for an entity with no creature", "[threat]") {
    // A bare registry.create() -- no CreatureKind -- returns 0, never asserts.
}
// game/tests/combat_tests.cpp — mechanism, not authored values
TEST_CASE("blunt bypasses defense and most armour", "[combat]") {
    // Same CombatRequest twice, damage_type Blunt vs Slashing, defender
    // defense .5 / armour 10: blunt's hit_chance ignores defense, and its
    // penetrated damage is base - 0.3*armour (floored at 1) vs base - armour.
}
// src/game/tests/scenario_tests.cpp
TEST_CASE("the new creature names resolve", "[scenario]") {
    // CreatureIdFromName round-trips Bandit / BanditArcher / BanditLeader / MudGolem.
}
```

- [ ] **Step 2: Register + run, confirm failure** — add `threat_table.cpp` to `badlands_game_lib` and `threat_table_tests.cpp` to `badlands_game_tests`. `scripts/build.sh badlands_game_tests` → FAIL (no `CreatureId::Bandit`).

- [ ] **Step 3: Implement** — append the four ids AFTER `Deer` (append-only; `CreatureId`'s first `HERO_CLASS_COUNT` entries must keep lining up with `HeroClassId`), their `kNames` entries, and their catalog rows with zeroed growth; `CharacterDesc::creature` + the `CreatureKind` component + its spawn emplacement; then `threat_table.{h,cpp}` with the anchors above and `threat_of`.

- [ ] **Step 4: Run** — `scripts/test.sh badlands_game_tests "[threat]"`, then `scripts/test.sh`.

- [ ] **Step 5: Commit**

```bash
git add game/include/badlands_sim.hpp game/src/creature_catalog.cpp \
        game/src/threat_table.h game/src/threat_table.cpp game/tests/threat_table_tests.cpp \
        game/tests/combat_tests.cpp src/game/tests/scenario_tests.cpp \
        assets/creatures/creatures.json CMakeLists.txt
git commit -m "feat(game): the enemy roster, and threat as a fixed calibration table"
```

---

## Task 3: Attack commitment — wind-up and recovery

The core mechanic of this plan. An attack stops being instantaneous and free.

**Files:** create `game/src/strike.{h,cpp}`, `game/tests/strike_tests.cpp`; modify `game/include/badlands_sim.hpp`, `game/src/components.h`, `game/src/combat.cpp`, `game/src/sim.cpp`, `game/src/movement.cpp`, `game/src/status.cpp`, `game/src/intention.h`, `CMakeLists.txt`

**Interfaces produced:**

```cpp
// badlands_sim.hpp — Attack gains two timers. cooldown KEEPS its meaning
// (time between uses of this attack, measured from RESOLVE), with recovery
// running inside it -- so no third redundant number per weapon.
struct Attack {
    ...
    float wind_up_seconds = 0.0f;    // committed, CANCELLABLE, before the blow
    float recovery_seconds = 0.0f;   // committed, NOT cancellable, after it
};

// components.h — one strike at a time. Phase is derived from the clock, so
// there is no phase enum to keep in sync:
//   world_millis <  resolve_at  -> WIND-UP   (cancellable)
//   world_millis >= resolve_at  -> RECOVERY  (not cancellable)
//   world_millis >= free_at     -> component removed by advance_strikes
struct StrikeInProgress {
    int64_t resolve_at_millis = 0;
    int64_t free_at_millis = 0;
    int32_t attack_index = 0;
    uint32_t target_slot = UINT32_MAX;
    Combatant attacker;   // CAPTURED at declaration, like advance_projectiles
    Attack attack;        // captured too: a growth-driven stat change mid-swing
                          // must not retroactively change the blow in flight
};

// strike.h
// Emplaces StrikeInProgress and returns true, or false if `e` already has one.
// Deadlines are world_millis + llround(seconds * 1000) -- integer, so every
// resolve lands on the same tick live and on replay.
bool declare_strike(BadlandsGame& game, entt::entity e, int32_t attack_index,
                    uint32_t target_slot);
// Per-tick sweep (tick_world): resolve strikes whose wind-up has elapsed
// (melee damage via resolve_attack, or spawn the projectile), stamping the
// attack's cooldown AT RESOLVE; drop those whose recovery has elapsed.
void advance_strikes(BadlandsGame& game);
// Drops a strike still in WIND-UP (no damage, no cooldown), emitting
// GameEventKind::StrikeCancelled. A strike already in recovery is untouched --
// the blow was thrown. Returns whether anything was cancelled.
bool cancel_strike(BadlandsGame& game, entt::entity e);
// True while `e` is committed to a strike, either phase. The gate movement,
// think, and the action channel all ask.
bool striking(const entt::registry& reg, entt::entity e);
```

Indicative timings, authored in the catalog: sword .35/.25, bow .60/.30, crossbow .50/.40, bolt .80/.35, knife .25/.15, blades .25/.15, golem slam 1.0/.60, bandit sword .35/.25, bandit bow .55/.30, leader sword .30/.20.

- [ ] **Step 1: Write the failing tests** — `game/tests/strike_tests.cpp`

```cpp
TEST_CASE("a swing resolves at the end of its wind-up, not on declaration", "[strike]") {
    // Two adjacent combatants, accuracy 1.0 vs defense/evasion 0. Declare;
    // assert target hp UNCHANGED for every tick before the deadline, and
    // changed on the tick at/after it.
}
TEST_CASE("cooldown is stamped at resolve, not at declaration", "[strike]") {
    // cooldown_remaining stays 0 through the wind-up, becomes the attack's
    // cooldown on the resolving tick.
}
TEST_CASE("a stun mid-wind-up cancels the blow", "[strike][status]") {
    // Declare, tick partway, apply Stunned. Target hp never changes; the
    // striker has no StrikeInProgress; a StrikeCancelled event is in the log.
}
TEST_CASE("a stun during recovery does NOT undo the blow", "[strike][status]") {
    // Damage already applied; stun only prevents what comes next.
}
TEST_CASE("a committed striker does not move and does not think", "[strike]") {
    // Position unchanged across the whole wind-up+recovery window with a live
    // MoveTarget; the brain is not consulted (drive plan_paths/follow_paths
    // directly -- see movement_tests.cpp's Walker fixture idiom, and
    // status_tests.cpp which uses it for the same reason).
}
TEST_CASE("only one strike at a time", "[strike]") {
    // declare_strike returns false while one is in progress; resolve_action
    // refuses BL_ACT_ATTACK for the same reason (queue stays empty).
}
TEST_CASE("a run containing wind-ups is deterministic and replays", "[determinism]") {
    // determinism_tests.cpp's run-twice + replay-the-log shape.
}
```

- [ ] **Step 2: Register + run, confirm failure** — add `strike.cpp` to `badlands_game_lib` and `strike_tests.cpp` to `badlands_game_tests`. `scripts/build.sh badlands_game_tests` → FAIL.

- [ ] **Step 3: Implement**

- `strike.{h,cpp}`: the four functions. `advance_strikes` resolves by reusing the EXACT body `fire_attack` uses today for melee (`resolve_attack` + `emit_char_hit` + health) and for ranged (spawn the projectile) — move that code here rather than duplicating it.
- `combat.cpp` `fire_attack`: keep every validation it does today (`attack_usable`, target resolution, the `-1` auto-pick path), then end in `declare_strike` instead of resolving. This keeps the `Attack` command handler and `resolve_action` unchanged in shape — one call site changes behaviour, and the command log is untouched.
- `sim.cpp` `tick_world`: call `advance_strikes(g)` immediately after `advance_statuses(g)` (so a stun that expired this tick cannot still be cancelling) and before `advance_needs`.
- `movement.cpp` `follow_paths`: skip an entity where `striking(reg, e)`, exactly as the `Stunned` check does (`movement.cpp:258`) — and for the same reason, leaving `NavPath` intact. `separate_units` still nudges it.
- `sim.cpp` think dispatch: skip a striking entity beside the `Stunned` skip (`sim.cpp:322`).
- `status.cpp` `apply_status`: for `StatusKind::Stunned`, call `cancel_strike` beside the existing `abort_current_intention`.
- `intention.h`/`command.cpp`: refuse `BL_ACT_ATTACK`/`BL_ACT_USE_SKILL` while striking, at both the gateway and the handler — the same double-gate the skill slice established.
- `creature_catalog.cpp`: author the timings above.

- [ ] **Step 4: Run** — `scripts/test.sh badlands_game_tests "[strike]"`, then `scripts/test.sh` (watch `[combat]`, `[determinism]`, `[rat]`, `[hunter]` — every duel now takes longer in wall-clock ticks, so timeout constants may need raising, which is a legitimate fixture change, not a balance edit).

- [ ] **Step 5: Commit**

```bash
git add game/src/strike.h game/src/strike.cpp game/include/badlands_sim.hpp \
        game/src/components.h game/src/combat.cpp game/src/sim.cpp game/src/movement.cpp \
        game/src/status.cpp game/src/intention.h game/src/command.cpp \
        game/src/creature_catalog.cpp game/tests/strike_tests.cpp \
        game/tests/determinism_tests.cpp CMakeLists.txt
git commit -m "feat(game): attacks commit -- cancellable wind-up, locked recovery"
```

---

## Task 4: Contact, and the price of leaving it

**Files:** `game/include/badlands_sim.hpp` (`StatusKind::Disengaged`), `game/src/game_state.h`, `game/src/movement.cpp`, `game/src/status.cpp`, `game/src/intention.h`, `game/src/command.cpp`, `game/src/combat.cpp`; tests `game/tests/disengage_tests.cpp`; `CMakeLists.txt`

**The change in meaning:** `MeleeLock` keeps forbidding ranged attacks in contact (`attack_usable`, `combat.cpp:145` — unchanged) and stops freezing movement (`movement.cpp:248`'s view exclusion drops it). Leaving contact is then possible, and prohibitively expensive.

**Interfaces produced:**

```cpp
// badlands_sim.hpp
enum class StatusKind : int32_t { None = 0, Stunned, Disengaged };  // append-only

// game_state.h — slots moved by follow_paths THIS tick. Cleared at the top of
// follow_paths, read by update_melee_locks (which runs after it, same tick).
// Mirrors nearest_enemy_scratch's existing shape and lifetime exactly.
std::vector<uint8_t> moved_by_path_scratch;
```

`Disengaged` blocks ALL actions — no attack, no skill — enforced at `resolve_action` (the brain's channel) and re-checked in the `Attack`/`AttackBuilding`/`UseSkill` command handlers. Movement is untouched: a disengaging unit is running away, and that is the point. Duration: a compiled constant, `kDisengagePenaltySeconds = 3.0f` in `movement.cpp` beside `kMeleeHysteresis`.

**Detection**, in `update_melee_locks`' existing unlock branch (`movement.cpp:310-325`): an entity being unlocked earns `Disengaged` only if `moved_by_path_scratch[slot]` is set. If contact broke because the other side left or died, it moved nothing and pays nothing. Only path-following counts — a `separate_units` nudge never triggers it.

`apply_status` is called directly from the system rather than through a command, following `advance_projectiles`' precedent (a pure system rule over deterministic state, replayed by re-running the system). **Update `status.h`'s "always arrives through a Command handler" comment**, which this makes untrue.

- [ ] **Step 1: Write the failing tests** — `game/tests/disengage_tests.cpp`

```cpp
TEST_CASE("a melee-locked unit can now move", "[disengage]") {
    // Two units in contact, one with a MoveTarget away: it moves.
}
TEST_CASE("walking out of contact earns the penalty", "[disengage]") {
    // Retreat past reach*hysteresis -> Disengaged present, ~3000 ms.
}
TEST_CASE("being left does not", "[disengage]") {
    // The OTHER unit walks away: neither the leaver's victim nor a stationary
    // unit gets Disengaged. (The leaver itself does.)
}
TEST_CASE("a separation nudge does not", "[disengage]") {
    // Overlap two stationary locked units so separate_units moves them apart;
    // neither earns the penalty.
}
TEST_CASE("Disengaged refuses every action", "[disengage]") {
    // resolve_action returns false for BL_ACT_ATTACK and BL_ACT_USE_SKILL,
    // queue stays empty; the Attack command handler is a no-op too.
}
TEST_CASE("Disengaged does not stop movement or defense", "[disengage]") {
    // It keeps running, and effective_combatant still reports its real
    // defense/evasion -- this is a penalty on ACTING, not on being.
}
```

- [ ] **Step 2: Run, confirm failure** — `scripts/build.sh badlands_game_tests` → FAIL.

- [ ] **Step 3: Implement** the six pieces above.

- [ ] **Step 4: Run** — `scripts/test.sh badlands_game_tests "[disengage]"`, then `scripts/test.sh` (watch `[movement]`, `[rat]`, `[hunter]`, `[combat]` — melee fighters can drift now).

- [ ] **Step 5: Commit**

```bash
git add game/include/badlands_sim.hpp game/src/game_state.h game/src/movement.cpp \
        game/src/status.h game/src/status.cpp game/src/intention.h game/src/command.cpp \
        game/tests/disengage_tests.cpp CMakeLists.txt
git commit -m "feat(game): melee contact can be broken, at a ruinous price"
```

---

## Task 5: ABI v5 and the skirmish behaviour

**Files:** `game/src/brain_abi.h`, `game/src/wasm_brain.cpp`, `game/src/monster_brain.cpp`, `scripts/brains/nim/{abi,hero_view,hero}.nim`, `src/crates/brainhost/src/lib.rs`, `assets/brains/*.wasm`; tests `game/tests/brain_abi_tests.cpp`, `game/tests/wasm_brain_tests.cpp`

Standoff distance is tactics, so it lives in the brain and the engine grows no kiting policy. What the engine owes the brain is enough to decide with — and `BlThreat` is `{pos, dist, slot}` today, which says nothing about what it is standing off from or how dangerous it is.

```c
#define BL_ABI_VERSION 5
#define BL_ST_DISENGAGED 5   /* appended */

typedef struct BlThreat {
    float pos_x, pos_z;
    float dist;
    uint32_t slot;
    float reach;          /* longest MELEE range in its loadout; 0 = none */
    float ranged_reach;   /* longest RANGED range; 0 = none */
    float move_speed;     /* how fast it closes */
    float threat;         /* threat_of() -- its combat potential */
} BlThreat;               /* 16 -> 32 */

/* BlViewSelf gains, in its existing trailing pad: */
    float threat;         /* threat_of() for THIS entity -- the other half of
                             the comparison. Both sides on the wire, because
                             deciding whether a fight is worth taking is the
                             brain's call, not the engine's. */
```

`BlViewSuggest` grows by `BL_MAX_THREATS * 16 = 128`; `BlViewSelf` stays 88 (the new float lands in `_pad2`); `BlViewWire` 1760 → 1888. **If the compiler disagrees, trust the compiler** and propagate to all three mirrors: `brain_abi.h`'s `static_assert`s, `abi.nim`'s `doAssert`s, and `brainhost/src/lib.rs`'s `VIEW_WIRE_LEN` / `ABI_VERSION`.

`hero.nim` gains a skirmish block, used when the hero holds a ranged attack that outranges the threat's `reach` **and the threat is dangerous enough to be worth avoiding** — the one place this slice spends the threat number:

```nim
# Hold outside the threat's reach with a margin, shoot when the window allows,
# step back when the margin closes. NEVER breaks contact deliberately: with
# BL_ST_DISENGAGED on the table that is always the losing move, so the whole
# job is not being touched in the first place.
#
# The threat comparison is why this is not just "always kite": something well
# below this hero's own weight is not worth ceding ground to, and standing off
# from a rat costs shots for nothing.
proc wantsStandoff(v: HeroView, t: BlThreat): bool =
  rangedReach(v) > t.reach and t.ranged_reach <= 0.0'f32 and
    t.threat >= v.threat * kStandoffThreatRatio    # kStandoffThreatRatio = 0.5
```

When it wants a standoff and `t.dist < t.reach + kStandoffMargin`, suggest `BL_INT_MOVE_TO` at a point directly away from the threat; otherwise suggest `BL_INT_ATTACK` as today. Either way it still fires at most one `bl_enqueue_action(BL_ACT_ATTACK, ...)` per wake through the existing picker — the action channel is independent of the intention, which is what makes move-shoot-move expressible without a new intention kind.

**Fleeing is NOT built here.** The comparison a flee decision needs is now on the wire and exercised by the standoff gate; the decision itself is the named follow-up.

`monster_brain.cpp` gets the mirror for `BanditArcher` (host-side code, same seams).

- [ ] **Step 1: Write the failing tests** — `brain_abi_tests.cpp` (version 5, `sizeof(BlThreat) == 32`, `sizeof(BlViewSelf) == 88` unchanged, wire size, `chars` still after `threats`); `wasm_brain_tests.cpp` (a packed threat carries the enemy's real melee reach, speed, and `threat_of`; `self.threat` matches the thinking entity's own; a disengaged hero's `statuses` carries `BL_ST_DISENGAGED`).
- [ ] **Step 2: Run, confirm failure** — `scripts/test.sh badlands_game_tests "[brain_abi]"`.
- [ ] **Step 3: Implement the host side** — `brain_abi.h`, `wasm_brain.cpp`'s threat packing (fill from `melee_range`/`ranged_range`/`Stats.move_speed`), `monster_brain.cpp`.
- [ ] **Step 4: Run host tests** — `"[brain_abi]"`, `"[wasm_brain]"`.
- [ ] **Step 5: Mirror in Nim** — `abi.nim`, `hero_view.nim`, `hero.nim`'s skirmish block.
- [ ] **Step 6: Rebuild + Rust constants** — `scripts/build_brains.sh`; set `ABI_VERSION = 5` and `VIEW_WIRE_LEN` in `src/crates/brainhost/src/lib.rs`; `cargo test --manifest-path src/crates/brainhost/Cargo.toml --lib` → `real_hero_wasm_conforms` is the gate proving the committed wasm is genuinely v5.
- [ ] **Step 7: Commit (artifacts are LFS — stage explicitly)**

```bash
git add game/src/brain_abi.h game/src/wasm_brain.cpp game/src/monster_brain.cpp \
        game/tests/brain_abi_tests.cpp game/tests/wasm_brain_tests.cpp \
        scripts/brains/nim/abi.nim scripts/brains/nim/hero_view.nim \
        scripts/brains/nim/hero.nim src/crates/brainhost/src/lib.rs
git add assets/brains/hero.wasm assets/brains/idle_test.wasm assets/brains/trap_test.wasm
git commit -m "feat(brain): ABI v5 exposes threat reach and speed; heroes keep their distance"
```

---

## Task 6: The heal op, the Cursed status, and Curse

**Files:** `game/src/skill_abi.h`, `game/src/skills.{h,cpp}`, `game/src/skill_cast.cpp`, `game/src/combat.cpp`, `game/include/badlands_sim.hpp`, `game/src/creature_catalog.cpp`, `assets/skills/skills.json`; tests `game/tests/skill_effect_tests.cpp`, `game/tests/skills_tests.cpp`, `src/game/tests/skill_manifest_tests.cpp`

```c
/* skill_abi.h — append-only */
#define BL_FX_HEAL 3   /* param_f = hp to restore, clamped to the target's max */
```

```cpp
// badlands_sim.hpp
enum class StatusKind : int32_t { None = 0, Stunned, Disengaged, Cursed };
enum class SkillId : int32_t { Calcify = 0, ShieldBash, Curse, Count };  // append-only
```

`Cursed` is enforced in `effective_combatant` (`combat.cpp:95`) — the one place a status may change what `resolve_attack` sees. It subtracts its constants from `defense`/`armour` (floored at 0). Unlike `Stunned` it does NOT zero evasion: a cursed target still dodges, it just stops warding.

`Curse`: trigger `Action`, target `Any`, cooldown 15 s, attack test `None` (it always lands — the debuff is the point), constants `{duration_seconds: 8, armour_penalty: 2, defense_penalty: 0.1}`. Granted to `Apprentice` at level 1. `Calcify` moves to level 4 per the design doc; its effect stays the documented no-op.

Because `Curse` declares no attack test, its cast range comes from a `range` constant — `skill_cast_range`'s existing unranged path (`skill_cast.h:39`), where a non-positive result means UNBOUNDED. Author `range: 7` so a curse cannot outreach the Apprentice's bolt.

- [ ] **Step 1: Write the failing tests**

```cpp
// skill_effect_tests.cpp — world-free, the effect contract only
TEST_CASE("curse emits one APPLY_STATUS op per target", "[skill_effect]") { ... }
TEST_CASE("curse lands without an attack test", "[skill_effect]") {
    // attack_test BL_TEST_NOT_RUN -> still emits the op (unlike shield-bash)
}
// shield_bash_tests.cpp / a new cast test — through the world
TEST_CASE("a cursed defender has less armour and defense, but still dodges", "[skills][cast]") {
    // effective_combatant before/after; evasion unchanged.
}
TEST_CASE("BL_FX_HEAL restores hp and never exceeds max", "[skills][cast]") {
    // drive apply_effect_batch directly with a heal op on a damaged target.
}
```

- [ ] **Step 2: Run, confirm failure.**
- [ ] **Step 3: Implement** — the op kind + its `apply_effect_batch` case, `StatusKind::Cursed` + its `effective_combatant` gate, the `curse_effect` function and its catalog defaults, the grant rows, `skills.json`.
- [ ] **Step 4: Run** — `scripts/test.sh badlands_game_tests "[skill"`, then `scripts/test.sh`.
- [ ] **Step 5: Commit**

```bash
git commit -m "feat(game): heal ops, the Cursed status, and the Apprentice's Curse"
```

---

## Task 7: Dress Wounds and Backstab

**Files:** `game/src/skills.{h,cpp}`, `game/src/skill_cast.cpp`, `game/include/badlands_sim.hpp`, `game/src/creature_catalog.cpp`, `assets/skills/skills.json`; tests `game/tests/skill_effect_tests.cpp`

| Skill | Class @ level | Trigger / target / test | Constants | Effect |
|---|---|---|---|---|
| `DressWounds` | Hunter @ 2 | Action / SelfOnly / None | `heal_amount: 8`, `cooldown 30` | one `BL_FX_HEAL` on the caster |
| `Backstab` | GraveRobber @ 3 | Action / Any / Melee | `bonus_damage: 6`, `cooldown 10` | on `BL_TEST_HIT`, one `BL_FX_DAMAGE` of `test_damage + bonus` when the target is not engaging the caster, else `test_damage` |

`Backstab` is the first effect to emit `BL_FX_DAMAGE` (declared and applied since the skills slice, never yet emitted). "Not engaging the caster" is read from the context, not the world: `BlSkillCastContext` already carries per-target views, so **`BlSkillTarget` gains `int32_t engaging_caster`** — a wire field, filled by `build_cast_context` from the target's `CurrentIntention`/`MeleeLock` against the caster. Bump `BL_SKILL_ABI_VERSION` to 2 and update the `static_assert`ed sizes on both sides.

`DressWounds` gets a long wind-up once Task 3 lands — but a SKILL's wind-up is `intention_duration`, which is declared vocabulary the engine still refuses. So it is a plain instant action here, and the fact that field-dressing ought to take time is noted as deferred, not faked.

- [ ] **Step 1: Write the failing tests** — one per effect, world-free, plus one cast-level test each that the grant fires at the right level.
- [ ] **Step 2: Run, confirm failure.**
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** — `scripts/test.sh`.
- [ ] **Step 5: Commit**

```bash
git commit -m "feat(game): the Hunter dresses wounds and the Grave Robber backstabs"
```

---

## Task 8: The SVG chart writer

Pure, dependency-free, independently testable: no game types, no engine, no world.

**Files:** create `src/executables/duelsim/svg_chart.{h,cpp}`, `src/executables/duelsim/tests/duelsim_tests.cpp`; modify `CMakeLists.txt`

```cpp
// svg_chart.h -- a line chart as a self-contained <svg> string. Deliberately
// tiny and hand-rolled: SVG is text, this needs axes and polylines, and a
// charting dependency for that would be absurd. No external references of any
// kind (no fonts, no CSS files) so the file renders standalone in a browser.
namespace duelsim {

struct ChartSeries {
    std::string label;
    std::string colour;                    // "#4a7fd0"
    std::vector<std::pair<float, float>> points;
};
struct ChartSpec {
    std::string title;
    std::string x_label, y_label;
    std::vector<ChartSeries> series;
    int width = 640, height = 400;
};

// Axis ranges cover the union of every series' data, padded to round numbers.
// An empty spec renders a valid, empty chart rather than failing.
std::string RenderChartSvg(const ChartSpec& spec);
bool WriteChartSvg(const std::string& path, const ChartSpec& spec);

}  // namespace duelsim
```

- [ ] **Step 1: Write the failing tests** — `src/executables/duelsim/tests/duelsim_tests.cpp`

```cpp
TEST_CASE("a chart renders one polyline per series", "[svg]") {
    // 2 series -> exactly 2 <polyline occurrences; both labels appear.
}
TEST_CASE("axes cover the data", "[svg]") {
    // A point at x=20,y=100 -> the rendered axis labels include a bound >= those.
}
TEST_CASE("an empty chart is still valid svg", "[svg]") {
    // starts with "<svg", ends with "</svg>", no crash.
}
TEST_CASE("rendering is deterministic", "[svg]") {
    // Same spec twice -> byte-identical strings (no time, no addresses).
}
TEST_CASE("no external references", "[svg]") {
    // the output contains no "http", no "<image", no "@import".
}
```

- [ ] **Step 2: Register + run** — add `badlands_duelsim_tests` (catch2 + the two svg sources only; no game lib needed) and its `add_test`. FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** — `scripts/test.sh badlands_duelsim_tests`.
- [ ] **Step 5: Commit**

```bash
git add src/executables/duelsim/svg_chart.h src/executables/duelsim/svg_chart.cpp \
        src/executables/duelsim/tests/duelsim_tests.cpp CMakeLists.txt
git commit -m "feat(duelsim): a dependency-free svg line-chart writer"
```

---

## Task 9: The duel matrix and the parameter charts

The simulation surface. Headless, links `badlands_game_lib` only — no engine, no Dawn, no SDL, so it builds and runs fast.

**Files:** create `src/executables/duelsim/duel_matrix.{h,cpp}`, `src/executables/duelsim/main_duelsim.cpp`; modify `src/executables/duelsim/tests/duelsim_tests.cpp`, `CMakeLists.txt`

```cpp
// duel_matrix.h
struct DuelOutcome {
    int32_t winner_index = -1;   // 0 = left, 1 = right, -1 = timeout/mutual
    int32_t ticks = 0;
};
// Stage `left` vs `right` in a flat arena `separation` units apart at level
// `level`, and tick to resolution or `max_ticks`. Combat rolls are seeded off
// (attacker, target, world_millis, attack_index), so a different separation is
// a genuinely different roll stream -- that is how a single pairing yields a
// distribution rather than one repeated answer.
DuelOutcome run_duel(CreatureId left, CreatureId right, int32_t level,
                     float separation, int32_t max_ticks = 6000);

struct MatrixCell { int32_t wins = 0, losses = 0, draws = 0; int32_t median_ticks = 0; };
// Every ordered pairing over `roster`, `samples` separations each.
std::vector<std::vector<MatrixCell>> run_matrix(const std::vector<CreatureId>& roster,
                                                int32_t level, int32_t samples);
```

`main_duelsim.cpp` — `--out <dir>` (default `duelsim_out`), `--level N` (default 1), `--samples N` (default 9), `--max-level N` for the charts (default 20). It writes:

- **`duel_matrix.md`** — the roster crossed with itself, each cell `W-L-D` plus median ticks, with each creature's `threat_target` in the row and column headers. A markdown table, per the repo's "simplest thing for data presentation".
- **`calibration.md`** — the validation surface. One row per unordered pairing: both threat targets, their difference, and the observed win rate. Sorted by threat difference, so the design doc's own invariant ("threats of the same caliber win ~50% against each other") is read straight off the top of the table, and its open question ("how does a threat difference shift the expected win ratio?") is answered empirically by the rest of it.
- **`threat_calibration.svg`** — the same data as a scatter: x = threat difference, y = observed win rate, one point per pairing, with the 50%-at-zero reference line drawn. The shape of that cloud IS the answer to the doc's open question.
- **`threat_targets.svg`** — level → `threat_target` per creature, the authored calibration curve itself.
- **`stat_hp.svg`, `stat_damage.svg`, `stat_armour.svg`** — level → the raw parameter per class, straight from `apply_level_stats`. These are the "parameters as charts" the brief asks for.
- A one-line summary to stdout naming every file written.

Nothing here asserts a balance outcome, and nothing here tunes one. The threat table is the fixed post; the matrix and the calibration report measure the distance to it.

- [ ] **Step 1: Write the failing tests** — append to `duelsim_tests.cpp`

```cpp
TEST_CASE("a duel is deterministic", "[duelsim]") {
    // Same (left, right, level, separation) twice -> identical winner AND ticks.
}
TEST_CASE("separation changes the roll stream", "[duelsim]") {
    // Across 9 separations of an evenly-matched pairing, not every outcome is
    // identical -- otherwise the "samples" axis is decorative.
}
TEST_CASE("a duel always terminates", "[duelsim]") {
    // Deer vs Deer (no attacks at all) hits max_ticks and reports a draw
    // rather than hanging.
}
TEST_CASE("the matrix is square and mirror-consistent", "[duelsim]") {
    // cell[i][j].wins == cell[j][i].losses for every i != j.
}
TEST_CASE("the calibration report pairs each cell with its threat targets", "[duelsim]") {
    // Every row's delta == |threat_target(a, level) - threat_target(b, level)|,
    // and its win rate matches the matrix cell it came from. Mechanism only --
    // NOT an assertion about what the win rate ought to be.
}
```

These need the game lib, so `badlands_duelsim_tests` gains `badlands_game_lib` and `duel_matrix.cpp`.

- [ ] **Step 2: Run, confirm failure.**
- [ ] **Step 3: Implement** `duel_matrix.cpp` (build a `WorldConfig` with `prebuild_colony=false`, `terrain_blocking=false` and arena half-extents, spawn both by id via the `Sim` API, tick to resolution — reuse `tally_arena`, `src/game/scenario.h`) and `main_duelsim.cpp`.
- [ ] **Step 4: Run it and read the output**

```bash
scripts/build.sh badlands_duelsim
./build/badlands_duelsim --out duelsim_out
```

Open `duelsim_out/duel_matrix.md` and every SVG. Report the matrix as-is — do NOT tune stats to move cells. If a cell looks wrong in a way that indicates a MECHANIC is broken (a hunter that never fires, a golem that never reaches anyone), that is a bug to fix; a cell that is merely unbalanced is the expected state of an approximation.

- [ ] **Step 5: Commit** (`duelsim_out/` is a generated artifact — add it to `.gitignore`, do not commit the reports)

```bash
git add src/executables/duelsim/ CMakeLists.txt .gitignore
git commit -m "feat(duelsim): simulate every pairing, report the matrix and the curves"
```

---

## Verification

1. `scripts/build.sh` → `BUILD OK`.
2. `scripts/test.sh` → green. Most likely to catch regressions: `[determinism]`, `[strike]`, `[disengage]`, `[movement]`, `[combat]`, `[brain_abi]`, `[wasm_brain]`, `[rat]`, `[hunter]`.
3. `cargo test --manifest-path src/crates/brainhost/Cargo.toml --lib` → `real_hero_wasm_conforms` passes, proving the committed `hero.wasm` is genuinely v5.
4. `./build/badlands_duelsim --out duelsim_out` → `duel_matrix.md`, `calibration.md`, and five SVGs; open **all** of them before drawing any conclusion.
5. `perl -e 'alarm 30; exec @ARGV' ./build/badlands_ai_sandbox` → watch a live fight: swings visibly take time, a stun mid-wind-up produces no damage, a hunter backs away from a mercenary rather than standing in melee — and does NOT back away from a rat, which is the threat comparison doing its job.
6. `open docs/design/game-design.html` → renders with its own fonts from inside the repo.
7. `git lfs ls-files | grep -E 'hero.wasm|docs/design/fonts'` → the rebuilt brain and all 23 fonts are LFS objects, not raw binaries.

## Declared but not executed

- **Skill wind-up.** `DressWounds` ought to take time to apply; that is `intention_duration`, declared vocabulary the engine still refuses. Instant here, noted rather than faked.
- **Precision Shot, Sneak, Teleport, Skin Game, Rob Grave** — named in the design doc, out of this slice.
- **`Calcify`'s effect** stays the documented no-op; only its grant level moves (5 → 4, per the doc).
- **Ghost / the esoteric line** — needs ethereal damage, which does not exist.
- **Fleeing.** Threat's second role is fully plumbed — both sides of the comparison are on the wire and the standoff gate already spends it — but no brain runs away yet. That is the named follow-up: a flee intention, the disengage cost weighed against staying, and the wake cadence to re-decide as a fight turns. It is deliberately downstream of the calibration report, because a brain deciding on a badly-calibrated number makes confident, wrong choices.
- **Threat never feeds combat resolution.** It approximates the outcome; it must not become an input to it, or the approximation would be validating itself.
- **Only level-1 hero anchors exist.** `threat_target` interpolates and holds flat, so the level-15 and level-20 anchors the design doc sketches can be added later without touching a call site.
- **One-vs-many calibration stays open** — every duel in the matrix is 1v1, which is exactly the case the doc's own invariant is stated for. The level-gap question it also raises IS answered here, empirically, by `threat_calibration.svg`.
- **`hero_desc` still reads the compiled catalog** for guild-recruited heroes, so a `creatures.json` edit reaches an arena mercenary and not a recruited one — inherited from the skills slice, now covering growth rows too.
