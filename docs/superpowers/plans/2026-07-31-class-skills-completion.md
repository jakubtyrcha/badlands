# Class Skills Completion — Implementation Plan

> Executed 2026-07-31. Companion to
> `docs/superpowers/specs/2026-07-31-class-skills-completion-design.md`.

## Context

The four implemented hero classes each have a skill list in the game design
doc, and only one skill per class exists. This slice ships the rest of what is
buildable, and each of the four is chosen because it forces a *different*
seam the shipped contract declared and then refused:

| Skill | Class(es) | Level | The seam |
|---|---|---|---|
| **Sneak** | Grave Robber | 3 | perception: an entity nobody can see |
| **Precision Shot** | Grave Robber 5, Hunter 2 | 5 / 2 | `SkillTrigger::Intention` — a skill that focuses |
| **Calcify** | Apprentice | 4 | none; it makes a grant that already exists stop doing nothing |
| **Teleport** | Apprentice | 8 | `SkillTargetMode::Point`, plus a navmesh window on the brain wire |

**Deferred and named:** Skin Game (Hunter 1) and Rob Grave (Grave Robber 1).
Both are economy skills; the sim has no corpses, graves, salvage or items — only
gold and tax collectors. With Skin Game deferred the Hunter would be a
one-skill class, so it gets **Precision Shot at level 2** instead. That is a
deliberate, user-approved deviation from the design doc.

Approved design: `docs/superpowers/specs/2026-07-31-class-skills-completion-design.md`.

**Two skills get their own sandbox mode**, because "watch a random duel and hope
the right pairing comes up" is not verification. Each stages one scenario and
says in one line whether the behaviour happened:

- **`--mode sneak`** — a level-8 Grave Robber and a Bandit. It should go
  unseen, close, and open with the crit.
- **`--mode teleport`** — a level-8 Apprentice beside something worth **threat
  20**. It should blink away.

Nothing is worth 20 today (Mud Golem at 6 is the ceiling, and no creature's
threat rises with level), so the teleport mode's opponent is a new
**`CreatureId::TrainingDummy`: threat anchor 20, and no attacks at all.** That
is the abstraction doing its job — the brain decides off the number on the
wire, so the opponent only has to *be* worth 20, not to prove it. It also makes
the observation clean: the dummy walks at the Apprentice forever and can never
kill it, so the mode measures the decision rather than a race.

**Goal:** a level-8 Apprentice teleports and a level-5 Grave Robber focuses a
shot, live, in a sandbox arena — with two ABI versions bumped cleanly and no
game system aware that anything unusual is happening.

**Tech Stack:** C++20/23 + EnTT + Catch2 (`game/`), Nim→wasm brain
(`scripts/brains/nim/`), Rust wasmtime host (`src/crates/brainhost`),
CMake/Ninja.

## Global Constraints

- **Append-only id spaces.** `SkillId`, `StatusKind`, `CommandKindId`,
  `GameEventKind`, `CreatureId`, and every `BL_*` wire vocabulary. New entries
  go before `Count`; nothing is renumbered.
- **A status's magnitude is a property of the status, not of what applied it.**
  `kCurseAccuracyPenalty` set the rule (compiled constants in `combat.cpp`); a
  skill's own constants tune its DURATION and its RANGE only.
- **The guest requests, the host decides.** An effect cannot read the world,
  cannot roll, and cannot apply anything; every op is re-validated against the
  context it came from (`apply_effect_batch`, `skill_cast.h`).
- **Both cast gates run the same `validate_cast`** — the cheap early one in
  `resolve_action` (`intention.h`) and the authoritative one in the `UseSkill`
  command handler (`command.cpp:141`). They must never drift.
- **Sim timers are int64 milliseconds** off `game.world_millis`, decremented by
  the compile-time `kMillisPerTick`. Never a float accumulator.
- **Tests assert mechanism, never authored numbers and never a balance
  outcome.** No test may read a shipped data file.
- **No debug controls that weren't asked for** — no ImGui panels, sliders, or
  env hooks.
- **`ctest` is canonical.** `scripts/build.sh`, `scripts/test.sh`.
- Work on a normal branch (`feat/class-skills-completion`); no git worktrees.

## Key facts established during exploration

- **`assets/creatures/creatures.json` authors `skills` for all four heroes, and
  an override REPLACES rather than merges.** Editing only
  `creature_catalog.cpp` silently loses every new grant for any app that loads
  the JSON. **Both files change together, every time.**
- `collect_threats` (`behaviours/perception.cpp:39`) is the ONE place any brain
  learns something is there, and `nearest_enemy` (`game.cpp:100`) is what
  `select_target` (`combat.cpp:193`) resolves to. Two functions cover every
  brain, wasm and engine-side alike.
- `effective_combatant` (`combat.cpp:99`) is the single hook Stunned and Cursed
  already use; both `CombatRequest` assembly sites route through it.
- `resolve_attack` (`combat.cpp:61`) is pure over `CombatRequest` and reads the
  file-scope `kCritMultiplier` (`combat.cpp:17`).
- `strike.h` is the model for a clock-derived commitment: phase is derived from
  `world_millis` vs two deadlines, never stored; `declare_strike` captures the
  attacker's stats AT DECLARATION; `cancel_strike` drops a wind-up only.
- `BL_INT_USE_SKILL` (9) is already reserved on the brain wire and rejected by
  the host — it is the channel Precision Shot needs.
- `NavMesh` (`navmesh/navmesh.h`) exposes `DebugCells` (ALL leaves) and
  `WorldToCell`/`CellCenterWorld`; there is no bounded-rect query and no
  point-passability query. Both are needed and both are pure geometry.
- `BhActionFn = extern "C" fn(i32, u32, i32, *mut c_void)`
  (`brainhost/src/lib.rs:189`), wired at `lib.rs:401`. `bh_instantiate` rejects
  any import other than `env.bl_log` and `env.bl_enqueue_action`.
- `spawn_creature_into` (`sim.cpp:488`) spawns at level 1 unconditionally;
  `grant_skills_for_level` fires rows at their EXACT level, so a spawn at 8
  must replay the ladder.
- Nim 2.2.10 is installed, so `scripts/build_brains.sh` can rebuild
  `assets/brains/hero.wasm` (LFS-committed — stage it deliberately).
- `kMaxSkills = 8`; the Grave Robber reaches 3 grants and the Apprentice 3.
  No capacity problem.
- **Every threat anchor is a single row at level 1** (`threat_table.cpp`), so no
  creature's threat rises with level. `threat_of` already reaches every brain
  through `BlThreat::threat` (ABI v5), which is the number the teleport gate
  keys on.
- `threat_table.cpp`'s `static_assert` over `kTable` forces a new `CreatureId`
  to carry an anchor — a new creature that forgets one fails the build, not a
  duel.
- `duel_pool` (`duel_mode.cpp`) already excludes anything unarmed, so a dummy
  with no attacks stays out of random duels **by the existing rule**, with no
  name-check added.
- `CharacterState` carries `level` and `skills` but **no statuses**;
  `GameEvent` carries `SkillUsed`, `StatusApplied` and `DamageDealt`. The view
  already drains the event stream each tick (`events_scratch_`) and throws it
  away — so a mode that wants to know whether a skill fired needs those events
  passed through, not a new query.

---

## File Structure

**New**
- `game/src/skill_focus.{h,cpp}` — the focus commitment, mirroring `strike.h`.
- `game/tests/skill_focus_tests.cpp`, `game/tests/sneak_tests.cpp` — new Catch2
  sources in the existing `badlands_game_tests` target.
- `src/executables/ai_sandbox/{sneak_mode,teleport_mode}.{hpp,cpp}` — two
  observation modes, view-free like `duel_mode`, tested by
  `badlands_ai_sandbox_tests`.

**Modified (principal)**
- `game/include/badlands_sim.hpp` — `StatusKind` +2, `SkillId` +3, `SkillSpec`
  +2 engine-checked fields, `Sim::SpawnCreature` gains a level.
- `game/src/combat.{h,cpp}` — `Combatant::crit_multiplier`, the Sneaking and
  Calcified branches of `effective_combatant`, the sneak gate in
  `nearest_enemy`, `CombatRequest`'s guaranteed pre-roll.
- `game/src/skill_cast.{h,cpp}` — `castable_in_melee`, `guaranteed_test`, Point
  targeting, the cast point on `CastPlan` and the context, `BL_FX_TELEPORT`.
- `game/src/skills.cpp` — three new specs, three new effects, the Calcify
  rewrite.
- `game/src/skill_abi.h` — v3. `game/src/brain_abi.h` — v6.
- `game/src/{intention.cpp,progression.{h,cpp},status.cpp,movement.cpp,sim.cpp,command.cpp,strike.cpp,wasm_brain.cpp,creature_catalog.cpp,behaviours/perception.cpp,game.cpp,navmesh/navmesh.{h,cpp},nav_world.{h,cpp}}`
- `assets/skills/skills.json`, `assets/creatures/creatures.json`
- `src/game/skill_manifest.cpp` — parse the two new fields.
- `scripts/brains/nim/{abi.nim,hero_view.nim,hero.nim}`,
  `src/crates/brainhost/{include/brainhost.h,src/lib.rs}`
- `src/executables/ai_sandbox/{duel_mode.hpp,duel_mode.cpp}`
- `CMakeLists.txt`

---

## Task 0: Branch and land the spec

- [ ] **Step 1** — `git checkout -b feat/class-skills-completion`.
- [ ] **Step 2** — copy this plan to
      `docs/superpowers/plans/2026-07-31-class-skills-completion.md`.
- [ ] **Step 3** — commit the spec and the plan together.

```bash
git add docs/superpowers/specs/2026-07-31-class-skills-completion-design.md \
        docs/superpowers/plans/2026-07-31-class-skills-completion.md
git commit -m "docs: design + plan for the remaining class skills"
```

---

## Task 1: A creature can be spawned at a level

**First, not last.** Nothing above level 3 is reachable in the sandbox today, so
without this none of the three skills below can ever be watched.

**Files:** `game/include/badlands_sim.hpp`, `game/src/{progression.h,progression.cpp,sim.cpp}`, `game/tests/progression_tests.cpp`, `src/executables/ai_sandbox/duel_mode.{hpp,cpp}`, `src/executables/ai_sandbox/tests/ai_sandbox_tests.cpp`

**Interfaces produced:**

```cpp
// progression.h -- put a freshly-spawned hero at `level` as though it had
// earned every level up to it: stats recomputed from the growth row, and EVERY
// grant row from 2..level replayed (level 1 already ran at spawn). Idempotent
// and dupe-proof, so it is safe on an entity that is already there.
//
// A no-op for anything without HeroSimulationState -- monsters carry a zeroed
// growth row and threat anchors authored at level 1, so "level" is not a thing
// they have. Clamped to [1, kMaxHeroLevel].
void set_hero_level(BadlandsGame& game, entt::entity e, int32_t level);

// badlands_sim.hpp -- Sim
// `level` > 1 puts a HERO at that level (see set_hero_level); ignored by
// anything that does not level. Initial config, not a command: a replay
// reproduces it from the same spawn call, exactly as prebuild_colony is.
uint32_t SpawnCreature(CreatureId id, int32_t team, float pos_x, float pos_z,
                       int32_t level = 1);
```

`DuelConfig` gains `min_level = 1` / `max_level = 8`; `DuelSetup` gains
`left_level` / `right_level`, drawn on splitmix axes 3 and 4 so `(seed, round)`
still determines the whole round.

- [ ] **Step 1: Write the failing tests**

```cpp
// game/tests/progression_tests.cpp
TEST_CASE("a hero spawned at a level has that level's stats", "[progression][spawn]") {
    // SpawnCreature(Mercenary, 0, 0, 0, 5) -> Health.max_hp equals
    // apply_level_stats' base + growth*4, and HeroSimulationState::level == 5.
    // Assert against arithmetic recomputed in the test from the entity's own
    // BaseStats/Growth -- never a hardcoded hp number.
}
TEST_CASE("a hero spawned at a level knows every skill up to it", "[progression][spawn]") {
    // A desc granting {A at 1, B at 3, C at 5} spawned at 5 knows all three;
    // spawned at 4 it knows A and B only. Test-local grant rows, not the catalog.
}
TEST_CASE("set_hero_level is idempotent", "[progression]") {
    // Calling it twice at the same level changes neither stats nor loadout.
}
TEST_CASE("spawning a monster at a level changes nothing", "[progression][spawn]") {
    // SpawnCreature(Rat, ..., 8) gives identical stats to SpawnCreature(Rat, ..., 1).
}

// src/executables/ai_sandbox/tests/ai_sandbox_tests.cpp
TEST_CASE("a duel samples a level in range for each side", "[duel]") {
    // Over many rounds every drawn level is within [min_level, max_level], and
    // the SAME (seed, round) gives the same pair of levels.
}
```

- [ ] **Step 2: Run, confirm failure** — `scripts/build.sh badlands_game_tests`.

- [ ] **Step 3: Implement**
  - `set_hero_level`: clamp; stamp `HeroSimulationState::{level, xp = 0,
    xp_next = xp_to_next(factors.progression, level)}`; loop
    `grant_skills_for_level(sk, grants, L)` for `L` in `2..level`; then
    `apply_level_stats(reg, e, level)` LAST, so growth reads the final level.
  - `spawn_creature_into` gains `int32_t level` and calls it after
    `spawn_entity`.
  - `DuelMode::Stage` passes each side's drawn level; `Observe`'s log line and
    `Status()` print it: `GraveRobber(lvl 6) beats Bandit on octagon in 24.3s`.

- [ ] **Step 4: Run** — `scripts/test.sh`, then watch a session:
      `perl -e 'alarm 90; exec @ARGV' ./build/badlands_ai_sandbox --mode duel`.
      Confirm levels vary between rounds and appear in the log.

- [ ] **Step 5: Commit**

```bash
git commit -am "feat(game): a creature can be spawned at a level, and duels use it"
```

---

## Task 2: Calcify and Sneak

Two statuses that share one hook. Calcify is the cheap half and lands first so
the `effective_combatant` pattern is in place before the perception work
starts.

**Files:** `game/include/badlands_sim.hpp`, `game/src/{combat.h,combat.cpp,game.cpp,behaviours/perception.cpp,skills.cpp,skill_cast.{h,cpp},strike.cpp,intention.cpp,brain_abi.h,creature_catalog.cpp}`, `src/game/skill_manifest.cpp`, `assets/skills/skills.json`, `assets/creatures/creatures.json`, `game/tests/sneak_tests.cpp`, `scripts/brains/nim/{abi.nim,hero.nim}`, `src/executables/ai_sandbox/{sandbox_mode.hpp,sneak_mode.{hpp,cpp},duel_mode.{hpp,cpp},ai_sandbox_view.cpp,main_ai_sandbox.cpp,tests/ai_sandbox_tests.cpp}`, `CMakeLists.txt`

**Interfaces produced:**

```cpp
// badlands_sim.hpp
enum class StatusKind : int32_t {
    None = 0, Stunned, Cursed, Disengaged,
    Sneaking,   // imperceptible: skipped by target selection AND by threat
                // perception, so no brain has a special case for it. Ends on
                // an aggressive act (combat.h's end_sneak_on_aggression).
    Calcified,  // hardened: flat armour up. Armour ONLY -- a calcified target
                // still misses and still gets dodged around.
};

// A sixth engine-checked SkillSpec field. Sneak is the first skill whose
// legality depends on the caster's SITUATION rather than on its target, and
// that is data the engine checks, never effect logic -- an effect cannot
// refuse a cast, it can only decline to emit ops.
bool castable_in_melee = true;   // false => validate_cast refuses under MeleeLock
```

```cpp
// combat.h
// Per-attacker crit multiplier, so a status can raise it without a new
// CombatRequest field and without touching a single call site. Defaults to the
// global kCritMultiplier; effective_combatant raises it while Sneaking.
struct Combatant { /* ... */ float crit_multiplier = kCritMultiplier; };

// An aggressive act, and the ONLY thing that breaks stealth early: declaring a
// strike, or a cast of this entity's whose batch damages someone else. Called
// from declare_strike (after the stat capture, before the status is needed
// again) and from apply_effect_batch.
void end_sneak_on_aggression(BadlandsGame& game, entt::entity e);
```

**The compiled constants**, beside `kCurseAccuracyPenalty` in `combat.cpp`:
`kSneakAccuracyBonus`, `kSneakCritMultiplier`, `kCalcifyArmourBonus`.

**Where imperceptibility is enforced — exactly two places:**

| Site | Change |
|---|---|
| `nearest_enemy` (`game.cpp:100`) | skip an entity with `Sneaking`, beside the existing `CritterState` skip |
| `collect_threats` (`behaviours/perception.cpp:39`) | skip it, beside the existing `InsideBuilding` skip |

Plus: `advance_intentions` aborts an Attack intention whose target has become
imperceptible, exactly as it aborts one whose target died.

### The sneak sandbox, and the one host change it needs

```cpp
// sandbox_mode.hpp -- Observe gains the tick's event stream. The host already
// drains it and throws it away (ai_sandbox_view.cpp's events_scratch_), so
// this hands over something it has rather than adding a query. A mode that
// wants to know whether a skill FIRED cannot infer it from CharacterState:
// statuses are not in the snapshot, and a status that comes and goes between
// two frames leaves no trace in the rows at all.
virtual bool Observe(const std::vector<CharacterState>& rows,
                     const std::vector<GameEvent>& events,
                     int64_t world_millis) = 0;
```

`DuelMode` ignores the new argument. `SneakMode` stages a level-8 Grave Robber
and a Bandit at opposite ends of the Tube (the longest arena — the approach is
the thing being watched), then reads three events in order and reports:

```
sneak 2: sneaked at 1.8s, struck at 7.4s for 31.2 (OK)
sneak 3: never sneaked in 30s (FAILED)
```

`StatusApplied` with `amount == Sneaking` is the sneak; the first `DamageDealt`
by that slot afterwards is the opening blow. It restages on a verdict either
way, so the mode loops and a flaky decision shows up as a mix of lines rather
than one lucky run.

- [ ] **Step 1: Write the failing tests** — `game/tests/sneak_tests.cpp`

```cpp
TEST_CASE("a sneaking entity is not selected as a target", "[sneak]") {
    // select_target from a hostile returns null while the status is up, and
    // returns the entity again once it expires.
}
TEST_CASE("a sneaking entity is not perceived as a threat", "[sneak]") {
    // collect_threats over a hostile's view never lists the sneaker's slot.
}
TEST_CASE("an attack intention aborts when its target vanishes", "[sneak][intention]") {
    // A hero engaging a target that then sneaks gets IntentionEnded, and stops.
}
TEST_CASE("sneak cannot be cast while melee-locked", "[sneak][skill]") {
    // validate_cast refuses; the same refusal at BOTH gates. A cast out of
    // contact succeeds, so the test proves the GATE and not the plumbing.
}
TEST_CASE("declaring a strike ends sneak, and that strike keeps the bonus", "[sneak][combat]") {
    // After declare_strike the status is gone, and the StrikeInProgress'
    // captured attacker carries the raised accuracy and crit multiplier.
}
TEST_CASE("a damaging cast ends sneak; a self-buff does not", "[sneak][skill]") {
    // Backstab out of sneak clears it; Calcify out of sneak does not.
}
TEST_CASE("sneak expires on its own timer", "[sneak]") { /* ... */ }
TEST_CASE("calcify raises armour for its duration and no other stat", "[calcify]") {
    // effective_combatant: armour up by exactly the constant; accuracy,
    // defense and evasion unchanged; back to base after expiry.
}

// src/executables/ai_sandbox/tests/ai_sandbox_tests.cpp
TEST_CASE("the sneak mode reports a success from the event stream", "[sneak][mode]") {
    // Drive Observe with SYNTHETIC events -- StatusApplied(Sneaking) then a
    // DamageDealt by the same slot -> a success verdict and a restage. No Sim,
    // no window: the mode is view-free, and this is the compile-time proof.
}
TEST_CASE("the sneak mode reports a failure on timeout", "[sneak][mode]") {
    // No events at all for the budget -> a failure verdict, still a restage.
}
```

- [ ] **Step 2: Run, confirm failure.**

- [ ] **Step 3: Implement**
  - Append the two `StatusKind` values and their `BL_ST_*` mirrors
    (`brain_abi.h`, `abi.nim`); append `SkillId::Sneak`.
  - `effective_combatant`: the Sneaking branch (accuracy + crit multiplier) and
    the Calcified branch (armour). `resolve_attack` reads
    `req.attacker.crit_multiplier` in place of the file-scope constant.
  - The two perception skips, and the intention abort.
  - `castable_in_melee` on `SkillSpec`, checked in `validate_cast` alongside its
    existing five checks, parsed by `skill_manifest.cpp` (a `ReadBool` in
    `ReadNum`'s shape).
  - `sneak_effect` (self, `BL_FX_APPLY_STATUS` with the duration constant);
    rewrite `calcify_effect` from its current no-op to the same shape.
  - `skills.json`: Sneak (`self`, `castable_in_melee: false`, cd 25,
    `duration_seconds: 20`) and Calcify's rewritten effect text +
    `duration_seconds: 30`. Fix `SkillId::Calcify`'s stale doc comment, which
    still promises an absorb charge.
  - Grants in **both** `creature_catalog.cpp` **and** `creatures.json`:
    GraveRobber gains `Sneak 3`.
  - `hero.nim`: a Sneak gate (not melee-locked, threat in view beyond melee
    reach, ready) placed ABOVE the Backstab gate, and a Calcify gate (threat in
    view, ready). Rebuild with `scripts/build_brains.sh`.
  - `SandboxMode::Observe`'s new argument, threaded from the view's existing
    `events_scratch_`; `DuelMode` takes it and ignores it.
  - `sneak_mode.{hpp,cpp}` + its `--mode sneak` entry in `MakeMode`
    (`main_ai_sandbox.cpp`), and its sources in both the app and test targets.

- [ ] **Step 4: Run** — `scripts/test.sh`, then watch the mode do the work an
      eyeball used to:

```bash
perl -e 'alarm 120; exec @ARGV' ./build/badlands_ai_sandbox --mode sneak
```

Confirm the log reports OK on most rounds, and that a run whose Grave Robber
never sneaks says so rather than passing silently.

- [ ] **Step 5: Commit** — stage `assets/brains/hero.wasm` deliberately (LFS).

```bash
git add game/ assets/skills/skills.json assets/creatures/creatures.json \
        src/game/skill_manifest.cpp scripts/brains/nim/ CMakeLists.txt
git add assets/brains/hero.wasm          # LFS: staged by path, deliberately
git commit -m "feat(game): sneak makes a hero imperceptible; calcify hardens one"

git add src/executables/ai_sandbox/
git commit -m "feat(sandbox): a sneak mode that reports whether the sneak happened"
```

---

## Task 3: Precision Shot, and the intention trigger it executes

`SkillTrigger::Intention` has been declared and refused since the skill
contract shipped. Its documented meaning — *adopted as an intention, its effect
landing after `intention_duration_seconds` of uninterrupted execution* — is
exactly a focus.

**Files:** `game/src/skill_focus.{h,cpp}` (new), `game/src/{components.h,intention.cpp,skill_cast.{h,cpp},skills.cpp,status.cpp,movement.cpp,sim.cpp,creature_catalog.cpp}`, `game/include/badlands_sim.hpp`, `src/game/skill_manifest.cpp`, `assets/skills/skills.json`, `assets/creatures/creatures.json`, `game/tests/skill_focus_tests.cpp`, `scripts/brains/nim/hero.nim`, `CMakeLists.txt`

**Interfaces produced:**

```cpp
// skill_focus.h -- a skill that takes TIME to cast. Deliberately shaped like
// strike.h: one commitment at a time, the deadline derived from the clock
// rather than a stored phase, so it resolves on the same tick live and on
// replay. A focus is DERIVED STATE -- it follows from a logged intention plus
// the clock -- so it is not itself a command.
struct SkillFocus {                       // components.h
    SkillId id = SkillId::Count;
    int32_t skill_index = -1;
    uint32_t target_slot = UINT32_MAX;
    int64_t resolve_at_millis = 0;
};

// Commits `e` to `skill_index` against `target_slot`, reading the duration off
// the skill's spec. False (and no focus) if one is already running.
bool begin_focus(BadlandsGame& game, entt::entity e, int32_t skill_index,
                 uint32_t target_slot);
// True while committed -- the single gate movement and the action channel ask.
bool focusing(const entt::registry& reg, entt::entity e);
// Drops it with no cast and no cooldown spent; emits FocusCancelled.
bool cancel_focus(BadlandsGame& game, entt::entity e);
// Per-tick sweep (tick_world): re-runs validate_cast at the deadline and, if it
// still holds, calls the ordinary run_cast. Placed beside advance_strikes.
void advance_focus(BadlandsGame& game);
```

```cpp
// badlands_sim.hpp -- the seventh engine-checked SkillSpec field
// When set, the engine's per-target pre-roll skips the defense and evasion
// gates and forces the crit, at the skill's own "crit_multiplier" constant.
// Engine-checked data, not effect logic: an effect is HANDED an outcome, it
// never decides one.
bool guaranteed_test = false;
```

**The channel split is strict, and it is what stops either gate being used to
skip the other's rules:** `BL_ACT_USE_SKILL` accepts only `Action` triggers;
`BL_INT_USE_SKILL` accepts only `Intention` ones. Both refusals are a warn +
drop, like every other refused vocabulary value.

**What ends a focus, and nothing else does:**

| | Where |
|---|---|
| resolves | `advance_focus` at the deadline, target still in range |
| a stun | `apply_status(Stunned)` — beside its existing `cancel_strike` call |
| re-deciding | `apply_intention` cancels a focus that is not an identical restatement |
| range | `advance_focus`'s re-run of `validate_cast` fails |

A focusing entity does not move (`follow_paths` skips it, as it already skips a
striking one). It is still woken — which is what makes "moving abandons it"
reachable at all: the only way to move is to abandon the focus, and that costs
the seconds already spent.

- [ ] **Step 1: Write the failing tests** — `game/tests/skill_focus_tests.cpp`

```cpp
TEST_CASE("a focus resolves at its deadline, not before", "[focus]") {
    // Tick to one tick short: no damage. One more: damage. Assert the DEADLINE,
    // computed from the spec's duration and kMillisPerTick -- not a tick count.
}
TEST_CASE("a stun mid-focus drops it with no cast", "[focus][status]") {
    // No damage ever lands, and the skill's cooldown was never stamped.
}
TEST_CASE("adopting a different intention cancels the focus", "[focus][intention]") {
    // apply_intention(MoveTo) mid-focus -> no shot.
}
TEST_CASE("restating the identical focus does not restart it", "[focus][intention]") {
    // The deadline is unchanged, matching apply_intention's resume-by-default rule.
}
TEST_CASE("a focusing entity does not move", "[focus][movement]") {
    // follow_paths leaves Position untouched while the focus is up, and the
    // NavPath survives so the route resumes afterwards.
}
TEST_CASE("a target out of range at the deadline gets no shot", "[focus]") { /* ... */ }
TEST_CASE("a guaranteed test never blocks or dodges and always crits", "[focus][combat]") {
    // Against a defender with defense 1.0 and evasion 1.0 -- which would
    // otherwise stop everything -- the pre-roll still reports HIT, and the
    // damage equals the crit multiplier times the penetrated base.
}
TEST_CASE("an action cast of an intention-triggered skill is refused", "[focus][skill]") {
    // ...and the mirror: an intention cast of an action-triggered skill.
}
```

- [ ] **Step 2: Run, confirm failure.**

- [ ] **Step 3: Implement**
  - `SkillFocus` component + `skill_focus.{h,cpp}`, added to
    `badlands_game_lib` and `badlands_game_tests` in `CMakeLists.txt`.
  - `advance_focus` in `tick_world` immediately after `advance_strikes`
    (`sim.cpp:402`) — same reasoning: resolve committed things after the
    command drain, before the movement pipeline.
  - `apply_intention`: the `UseSkill` kind runs `validate_cast`, then
    `begin_focus`; every other adopted kind calls `cancel_focus` first.
  - `guaranteed_test` in `build_cast_context`'s pre-roll, parsed by
    `skill_manifest.cpp`.
  - `precision_shot_effect`: `BL_FX_DAMAGE` with the target's `test_damage`.
  - `skills.json`: PrecisionShot (`trigger: intention`, `target: any`,
    `attack_test: ranged`, `guaranteed_test: true`, `intention_duration: 2`,
    cd 15, constants `range: 30`, `crit_multiplier: 3`).
  - **`skill_cast_range` needs a `range` constant override for a ranged-tested
    skill** — today a Ranged test forces the caster's own bow reach, and
    Precision Shot outranges every bow at 30 m. Prefer the constant when
    authored; fall back to the weapon reach when not. Update that function's
    doc comment, which currently states the opposite.
  - Grants in **both** catalog files: Hunter gains `PrecisionShot 2`,
    GraveRobber `PrecisionShot 5`.
  - `hero.nim`: suggest `BL_INT_USE_SKILL` when a threat sits between melee
    reach and 30 m and nothing is in contact. Rebuild the wasm.

- [ ] **Step 4: Run** — `scripts/test.sh`, watch a session, confirm a Hunter
      visibly stands still for two seconds and then something loses a chunk of
      health.

- [ ] **Step 5: Commit**

```bash
git add -u && git add assets/brains/hero.wasm
git commit -m "feat(game): a skill can focus -- precision shot executes the intention trigger"
```

---

## Task 4: Teleport — point targeting, and a navmesh window on the wire

The largest task, and the only one that touches both ABIs.

**Files:** `game/src/{skill_abi.h,brain_abi.h,skill_cast.{h,cpp},skills.cpp,intention.cpp,command.cpp,wasm_brain.cpp,navmesh/navmesh.{h,cpp},nav_world.{h,cpp},creature_catalog.cpp,threat_table.cpp}`, `game/include/badlands_sim.hpp`, `src/crates/brainhost/{include/brainhost.h,src/lib.rs}`, `scripts/brains/nim/{abi.nim,hero_view.nim,hero.nim}`, `assets/skills/skills.json`, `assets/creatures/creatures.json`, `game/tests/skill_cast_tests.cpp`, `src/executables/ai_sandbox/{teleport_mode.{hpp,cpp},main_ai_sandbox.cpp,tests/ai_sandbox_tests.cpp}`

**Interfaces produced:**

```c
/* skill_abi.h -- BL_SKILL_ABI_VERSION 2 -> 3 */
#define BL_FX_TELEPORT 4  /* move target_slot to the CAST'S OWN validated point */

typedef struct BlSkillCastContext {
    uint32_t version; int32_t skill_id;
    float point_x, point_z;   /* NEW: where a Point cast landed. 0 otherwise. */
    int64_t world_millis; uint64_t seed;
    /* ... unchanged ... */
} BlSkillCastContext;         /* 712 -> 720 bytes */
```

`BL_FX_TELEPORT` deliberately carries **no destination of its own**. That is
the whole safety argument in miniature: an effect may ask that someone be
moved, but only to the place the engine already validated. `BlSkillEffectOp`
and `BlSkillEffectBatch` keep their sizes.

```c
/* brain_abi.h -- BL_ABI_VERSION 5 -> 6 */
#define BL_MAX_NAV_POLYS 32
typedef struct BlNavPoly {            /* 24 bytes */
    float min_x, min_z, max_x, max_z;
    int32_t passable;
    uint32_t _pad;
} BlNavPoly;
/* in BlViewWire, after the skills block:
     int32_t nav_poly_count; uint32_t _pad7; BlNavPoly nav_polys[BL_MAX_NAV_POLYS]; */

/* env.bl_enqueue_action grows two arguments; non-point actions pass 0. */
void bl_enqueue_action(int32_t kind, uint32_t target_slot, int32_t arg,
                       float point_x, float point_z);
```

```cpp
// navmesh.h -- two pure geometric queries the mesh does not have yet
// Leaves overlapping the disc, NEAREST-FIRST by centre distance from `origin`
// and capped at `max_out`, so what survives truncation is always the closest
// ground.
void CellsNear(glm::vec2 origin, float radius, size_t max_out,
               std::vector<DebugCell>& out) const;
// Is this world point on a passable leaf? False on an empty mesh.
bool PassableAt(glm::vec2 w) const;

// skill_cast.h -- the point joins the plan and both gates
struct CastPlan { /* ... */ glm::vec2 point{}; };
bool validate_cast(const BadlandsGame& game, uint32_t caster_slot, int32_t skill_index,
                   uint32_t named_target_slot, glm::vec2 point, CastPlan& out);
```

`SkillTargetMode::Point` stops being refused: the point must be within
`skill_cast_range` (Teleport's `range` constant) **and** on a passable navmesh
cell. `CommandRecord` already carries `point_x/point_z`, so `UseSkill` records
the cast point with no change to the log format.

### The threat dummy, and what the teleport gate keys on

```cpp
// badlands_sim.hpp -- appended before Count, like every other creature
TrainingDummy,   // walks at you and never swings: NO attacks at all, and a
                 // threat anchor of 20. It exists so a brain can be shown
                 // something overwhelming without a fight breaking out --
                 // threat is an approximation of what a creature is worth, so
                 // the number is the whole of what a brain needs to react to.
```

- `threat_table.cpp`: one anchor, `{1, 20.0f}`. The `static_assert` over
  `kTable` forces the row, so forgetting it is a build failure.
- `creature_catalog.cpp`: `Archetype::Monster` (so it is hostile and
  perceivable — a Critter would be skipped by `nearest_enemy`), a `kNames` row,
  generous hp, **zero attacks**. `duel_pool` therefore drops it by the existing
  unarmed rule, with no name-check added. Its engine-side `monster_think` walks
  it at the nearest enemy and never finds a usable attack — verify it approaches
  and does nothing rather than warning every tick.

**The Apprentice's teleport gate keys on threat, not on health.** A dummy that
never hits it means health never falls, so a "flee when hurt" gate would never
fire — and keying on threat is the better test anyway: it is the first time
`BlThreat::threat` drives a decision rather than only a standoff distance, which
is exactly role 2 of the threat table. The gate: a perceived threat worth some
multiple of the hero's own threat, within some distance → blink to the passable
nav poly whose centre is farthest from it.

### The teleport sandbox

`TeleportMode` stages a level-8 Apprentice with a `TrainingDummy` a few metres
away, in the Tube (the longest arena — a 30 m blink needs somewhere to land).
It reads the events and reports:

```
teleport 1: blinked 26.4 m at 2.3s (OK)
teleport 2: never blinked in 30s (FAILED)
```

`SkillUsed` with `amount == Teleport` is the verdict; the distance comes from
the caster's row position before and after. It restages either way, so a
marginal decision shows as a mix of lines.

- [ ] **Step 1: Write the failing tests**

```cpp
// game/tests/skill_cast_tests.cpp
TEST_CASE("a point cast beyond range is refused", "[skill][point]") { /* ... */ }
TEST_CASE("a point cast onto an impassable cell is refused", "[skill][point]") {
    // Plop a Wall, aim inside its footprint -> refused at BOTH gates.
}
TEST_CASE("a legal teleport moves the caster to exactly the cast point", "[skill][point]") {
    // Position equals the validated point, not near it.
}
TEST_CASE("a teleport op cannot name a point of its own", "[skill][point]") {
    // Structural: BL_FX_TELEPORT carries no coordinates, so an effect that wants
    // an arbitrary destination has no way to express one. Assert the op applies
    // the CONTEXT's point even when the effect writes garbage into param_f.
}
TEST_CASE("nav cells near a point come back nearest-first and capped", "[nav]") {
    // CellsNear over a mesh with a plopped wall: the count respects max_out and
    // the sequence is non-decreasing in centre distance.
}

// game/tests/threat_tests.cpp
TEST_CASE("the training dummy is worth its anchor and swings at nothing", "[threat]") {
    // threat_of a spawned dummy == 20; its Attacks component is empty; and
    // duel_pool over the catalog does not contain it -- by the unarmed rule,
    // which the test asserts by checking the rule and not the name.
}

// src/executables/ai_sandbox/tests/ai_sandbox_tests.cpp
TEST_CASE("the teleport mode reports from the event stream", "[teleport][mode]") {
    // Synthetic SkillUsed(Teleport) + a moved row -> success; nothing for the
    // budget -> failure. Both restage.
}
```

```rust
// src/crates/brainhost/src/lib.rs tests
// bl_enqueue_action_forwards_calls_in_order: extend the existing wat module to
// pass f32 point args and assert they arrive intact.
```

- [ ] **Step 2: Run, confirm failure** — including
      `cargo test --manifest-path src/crates/brainhost/Cargo.toml --lib`.

- [ ] **Step 3: Implement**
  - `CellsNear` / `PassableAt` on `NavMesh`; a thin `nav_world.h` wrapper that
    rebuilds-if-stale, matching `nav_cost`'s shape.
  - Skill ABI v3: the context point, `BL_FX_TELEPORT`, the size static_asserts.
  - `validate_cast`'s Point branch, the point on `CastPlan` and in
    `build_cast_context`, `apply_effect_batch`'s teleport op (write `Position`,
    then let the ordinary movement pipeline take over — no NavPath surgery).
  - Brain ABI v6: `BlNavPoly` + the block, packed in `pack_view_wire`
    **only for a hero owning a Point-targeted skill** (count 0 otherwise);
    mirrored in `abi.nim` (with its `doAssert sizeof`) and `hero_view.nim`.
  - `bl_enqueue_action`'s two new arguments through `brainhost.h`, `BhActionFn`,
    `host_bl_enqueue_action`, `func_wrap`, and the C++ sink in
    `wasm_brain.cpp`; the point flows into the `UseSkill` command.
  - `teleport_effect`: one `BL_FX_TELEPORT` at the caster.
  - `skills.json`: Teleport (`target: point`, `attack_test: none`, cd 25,
    constant `range: 30`). Grants in **both** catalog files: Apprentice gains
    `Teleport 8`.
  - `CreatureId::TrainingDummy`: the catalog row, the `kNames` entry, and the
    `{1, 20.0f}` threat anchor.
  - `hero.nim`: the threat-ratio teleport gate above, picking the farthest
    passable nav poly within range. Rebuild the wasm.
  - `teleport_mode.{hpp,cpp}` + its `--mode teleport` entry in `MakeMode`, and
    its sources in both the app and test targets.

- [ ] **Step 4: Run** — `scripts/test.sh`, the brainhost `--lib` suite, then:

```bash
perl -e 'alarm 120; exec @ARGV' ./build/badlands_ai_sandbox --mode teleport
```

Confirm the log reports OK on most rounds, and that the Apprentice blinks
because the dummy is worth 20 rather than because it is being hurt — the dummy
never lands a blow, so a mode that only passes when health drops is measuring
the wrong thing.

- [ ] **Step 5: Commit**

```bash
git add game/ src/crates/ scripts/brains/nim/ assets/skills/skills.json \
        assets/creatures/creatures.json CMakeLists.txt
git add assets/brains/hero.wasm
git commit -m "feat(game): teleport -- point targeting, and a navmesh window on the brain wire"

git add src/executables/ai_sandbox/
git commit -m "feat(sandbox): a teleport mode, and a dummy worth threat 20 to run from"
```

---

## Verification

1. `scripts/build.sh` → `BUILD OK`.
2. `scripts/test.sh` → green. Most likely to catch regressions: `[skill]`,
   `[sneak]`, `[focus]`, `[combat]`, `[intention]`, `[movement]`,
   `[progression]`, `[determinism]`, `[nav]`.
3. `cargo test --manifest-path src/crates/brainhost/Cargo.toml --lib` → green
   (the import allowlist and the action signature both changed).
4. **Both catalogs agree.** `assets/creatures/creatures.json` REPLACES the
   compiled grants, so diff the two: every class's grant list must match
   `creature_catalog.cpp` row for row, or the JSON silently deletes the new
   skills for any app that loads it.
5. **The wire mirrors agree.** `abi.nim` and `hero_view.nim` static-assert their
   struct sizes; a build failure there is the check doing its job. Confirm
   `BL_ABI_VERSION` and `BL_SKILL_ABI_VERSION` were both bumped.
6. **The two observation modes say whether it worked** — they are the
   verification, not a substitute for looking:

```bash
perl -e 'alarm 120; exec @ARGV' ./build/badlands_ai_sandbox --mode sneak
perl -e 'alarm 120; exec @ARGV' ./build/badlands_ai_sandbox --mode teleport
```

   Mostly `(OK)` lines in both. A run of `(FAILED)` is a real finding — the
   brain gate, not the test — and the mode restaging either way is what makes
   that visible instead of a lucky single run.
7. Watch a long duel session for the two skills with no mode of their own:

```bash
perl -e 'alarm 300; exec @ARGV' ./build/badlands_ai_sandbox --mode duel
```

   A Hunter standing still for two seconds then landing a heavy hit; levels
   varying between rounds and printed in every result line; the
   `TrainingDummy` never appearing, since it is unarmed.
8. `perl -e 'alarm 30; exec @ARGV' ./build/badlands_game` → the town game is
   unaffected: heroes still recruit, route, and fight.

## Declared but not executed

- **Skin Game and Rob Grave.** Economy skills with no substrate — no corpses,
  no graves, no salvage, no items. Their own slice, ahead of the skills.
- **Precision Shot spawns no projectile.** It resolves instantly at the end of
  its focus, so a 30 m shot has no visible tracer. A projectile would need an
  effect op whose damage lands on arrival, which the batch contract cannot
  express.
- **Nobody sees through Sneak.** Imperceptible means imperceptible to
  everything; per-creature perception is the stated follow-up.
- **Teleport ignores line of sight.** A hero may blink through a wall to any
  passable cell in range — walls stop pathing, not blinks.
- **`hero_desc` still reads the compiled catalog for guild-recruited heroes**,
  so a `creatures.json` grant edit reaches an arena hero and not a recruited
  one. Inherited, unchanged.
- **Monsters do not level**, so a level-8 hero against a level-1 monster is a
  rout. That is why the level is printed rather than hidden.
- **No creature's threat rises with level.** Every anchor is still one row at
  level 1, so a level-20 Mercenary reads 2.5 exactly as a level-1 one does. The
  design doc sketches level-15/20 keyframes per class and they slot into
  `threat_table.cpp` without touching a call site — but those numbers are the
  user's to set, and the `TrainingDummy` sidesteps needing them here.
- **The dummy never fights.** It is a threat number that walks, so it proves the
  Apprentice's decision and nothing about surviving the consequence.
- **Only Sneak and Teleport get a mode.** Precision Shot and Calcify are watched
  in the duel soak; if either turns out to be as hard to catch in the wild, it
  earns its own mode the same way these two did.
- **`SkillTargetMode::Multi` and `SkillTrigger::Passive` stay refused.** Point
  and Intention were opened because a shipping skill needed each; nothing here
  needs those two, and approximating them is worse than refusing them.
