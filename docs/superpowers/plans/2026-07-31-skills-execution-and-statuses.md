# Skills Execution + Status Effects (Shield-Bash / Stun) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: use `superpowers:subagent-driven-development` (recommended) or `superpowers:executing-plans` to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.
>
> **Executed 2026-07-31**, all ten tasks. Deviations from the plan as written, each for a reason the plan could not have known:
> - The cast machinery lives in a new `game/src/skill_cast.{h,cpp}` rather than in `skills.{h,cpp}`: `skills.cpp` is linked STANDALONE by `badlands_skill_manifest_tests` and `badlands_scenario_tests`, so it has to stay free of world dependencies.
> - `GameEventKind::StatusApplied` landed with Task 1 (`apply_status` emits it) instead of Task 5, so the status subsystem was complete on its own.
> - A skill card shows its declared `SkillTargetMode` verbatim (`any`), not `enemy`: `any` means friend or foe, and the effect decides which.
> - `separate_units` deliberately still nudges a stunned body — otherwise units stack on top of one.
> - Task 9 gained a layout-level test (the skills region emits a hit rect) because the wheel routing depends on it and no headless screenshot can stage a selected level-3 hero.

## Context

Skills shipped as **slice A** (identity + JSON template + HUD inspection) and were deliberately left inert: `Skills::cooldown_remaining` is never ticked, no skill is ever used, `BL_ACT_USE_SKILL`/`BL_INT_USE_SKILL` are reserved-and-rejected in the ABI, and the only grant table is compiled (`kGrants`, `game/src/skills.cpp`). There is no status/timer mechanic in the sim at all — the closest precedents are the ad-hoc `MeleeLock{}`, `ChattingState{remaining}`, and `InsideBuilding{purpose}` components.

This is slice B. It ships one skill (**shield-bash**, mercenary, level 3) and one status (**stunned**), but the deliverable is the contract underneath them: a skill is *data the engine checks* plus *an effect that is a pure function of a flat context*. Effects are hardcoded C++ today and are expected to become Nim→wasm scripts later, so the effect boundary is designed now as a wire, not as a C++ call.

Decisions locked in brainstorming (2026-07-31):

- **A skill's data contract is five engine-checked fields** — trigger (`action | passive | intention`), target (`none | self | any | multi(limit) | point`), cooldown (`none | time`), intention-duration (`none | time`), effect.
- **JSON carries constants only, never logic.** Behaviour is code reading named, skill-specific constants.
- **Effects are `(context) → op batch`.** The engine resolves targets, pre-rolls the declared attack test, hands the effect a flat POD context, and applies the flat ops it returns. No registry, no `BadlandsGame`, no callbacks — the exact shape a wasm skill can implement, mirroring the brain's `BlViewWire` → `BlSuggestionWire`.
- **This slice executes only what shield-bash exercises:** trigger `action`; targets `none`/`self`/`any`. `passive`, `intention`, `multi`, `point` parse, validate, and are **refused at cast with an explicit warn** — never silently ignored.
- **The seam is the existing action gateway:** `BL_ACT_USE_SKILL` with `arg` = index into the caster's own `Skills`, like `BL_ACT_ATTACK`'s attack index.
- **Stun = full incapacitation:** think skipped, `defense`/`evasion` forced to 0, movement frozen, running `CurrentIntention` aborted (`IntentionEnded`) so the brain re-decides on expiry.
- **Bash is pure control:** the engine's melee attack test runs, the effect discards the damage and applies `Stunned`.
- **Own cooldown only:** a bash and a swing may both land in one wake.
- **Grants live on the creature config** (`CharacterDesc.skill_grants`, overridable in `assets/creatures/creatures.json`).
- **Hero view:** windowed skill cards + wheel scroll, mirroring the combat log (the `ui` crate cannot clip or scroll).

**Goal:** A level-3 mercenary shield-bashes through the brain's action channel; a landed bash stuns the target, which stands defenceless and brainless until the timer expires — replayable from the command log, inspectable in the hero panel, and with the effect written against a contract a wasm script could implement unchanged.

**Tech Stack:** C++20 + EnTT + Catch2 (`game/`), Nim → wasm32-wasi (`scripts/brains/nim/`), Rust (`src/crates/brainhost`, `src/crates/ui`), nlohmann/json (manifests), CMake/Ninja.

## Global Constraints

- **Every mutation is a `Command`** appended to `command_log`; `state = f(initial config, command log, N ticks)`. A brain never writes the registry, and neither does a skill effect — it returns ops the engine applies.
- **Append-only id spaces.** Never renumber or reuse a shipped value in `SkillId`, `CommandKind`/`CommandKindId`, `GameEventKind`, `StatusKind`, `BL_ACT_*`, `BL_INT_*`, `BL_ST_*`, `BL_FX_*`.
- **Sim timers are int64 milliseconds** advanced by the compile-time `kMillisPerTick` — never a float dt accumulator. (Cooldowns stay float seconds to match the existing `Attacks` field; status durations are int64 ms.)
- **Unimplemented vocabulary is refused loudly**, never silently treated as its nearest implemented neighbour.
- **No debug controls that weren't asked for** — no ImGui panels, sliders, toggles, env hooks. Fixed constants.
- **Game UI (`ui` crate) and debug UI (ImGui) stay separate surfaces.** Skill cards are game UI.
- **No game types in `src/engine/` or `src/core/`.** Untouched here.
- **Tests never assert on shipped data files** (`assets/skills/skills.json`, `assets/creatures/creatures.json`); manifest tests write temp fixtures.
- **`ctest` is canonical** for C++ suites; Rust crate tests need `--lib`.
- **`.wasm` artifacts are git LFS** — stage `assets/brains/*.wasm` deliberately after a rebuild.
- Work on the current branch; no git worktrees in this repo.

---

## File Structure

**New**
- `game/src/skill_abi.h` — the skill execution contract, written as a C header in the style of `game/src/brain_abi.h`: fixed capacities, explicit padding, `static_assert`ed sizes, no C++ dependencies. `BlSkillCastContext` in, `BlSkillEffectBatch` out. This file *is* the future wasm wire.
- `game/src/status.h` / `game/src/status.cpp` — status timers: `apply_status`, `has_status`, `remaining_millis_of`, `advance_statuses`.
- `game/tests/status_tests.cpp`, `game/tests/skill_effect_tests.cpp` (pure contract), `game/tests/shield_bash_tests.cpp` (end-to-end).

**Modified (principal)**
- `game/include/badlands_sim.hpp` — `StatusKind`; the reshaped `SkillSpec` (`SkillTrigger`, `SkillTargetMode`, `SkillAttackTest`, constants block); `SkillId::ShieldBash`; `SkillGrantRow` + `CharacterDesc::skill_grants`; `CommandKindId::UseSkill`; `GameEventKind::SkillUsed`/`StatusApplied`.
- `game/src/skills.{h,cpp}` — effect table, context builder, batch applier, grant helpers.
- `game/src/sim.cpp` — status + skill-cooldown ticking, stunned think skip.
- `game/src/combat.{h,cpp}` — `effective_combatant()` at both defender-assembly sites.
- `game/src/movement.cpp` — stunned characters do not move.
- `game/src/command.{h,cpp}` — `CommandKind::UseSkill` handler (the single mutation point).
- `game/src/intention.{h,cpp}` — `BL_ACT_USE_SKILL` branch; unconditional intention abort.
- `game/src/heroes.cpp`, `game/src/progression.cpp` — grants from the desc.
- `game/src/brain_abi.h`, `game/src/wasm_brain.cpp` — ABI v4 skills block + `BL_ST_STUNNED`.
- `scripts/brains/nim/{abi,hero_view,hero}.nim`, `assets/brains/hero.wasm`; `src/crates/brainhost/src/lib.rs` test constants.
- `src/game/skill_manifest.cpp`, `src/game/creature_manifest.cpp`, `assets/skills/skills.json`, `assets/creatures/creatures.json`.
- `src/game/ui/hud.{hpp,cpp}`, `src/executables/game/game_view.{hpp,cpp}` — skill cards + wheel scroll.
- `CMakeLists.txt` — three new test sources + `status.cpp`.

---

## Task 1: Status subsystem

**Files:**
- Create: `game/src/status.h`, `game/src/status.cpp`, `game/tests/status_tests.cpp`
- Modify: `game/include/badlands_sim.hpp`, `game/src/components.h`, `game/src/sim.cpp`, `CMakeLists.txt`

**Interfaces produced:**

```cpp
// badlands_sim.hpp — append-only, same discipline as SkillId/ActivityId
enum class StatusKind : int32_t { None = 0, Stunned };
inline constexpr int32_t kStatusKindCount = 2;
inline constexpr int32_t kMaxStatuses = 8;    // matches BL_MAX_STATUSES
const char* StatusName(int32_t kind);         // "Stunned"; "-" out of range

// components.h
struct StatusEntry {
    StatusKind kind = StatusKind::None;
    int64_t remaining_millis = 0;   // always > 0; expired entries are removed
    uint32_t source_slot = UINT32_MAX;
};
struct Statuses { StatusEntry entries[kMaxStatuses]{}; int32_t count = 0; };

// status.h
bool has_status(const entt::registry& reg, entt::entity e, StatusKind kind);
int64_t remaining_millis_of(const entt::registry& reg, entt::entity e, StatusKind kind);
// Emplaces Statuses on demand. Refresh keeps the LONGER remaining. Non-positive
// millis is a no-op. Component full -> warn + drop. True when applied.
bool apply_status(BadlandsGame& game, entt::entity e, StatusKind kind,
                  int64_t millis, uint32_t source_slot);
// Decrements every entry by kMillisPerTick and compacts out the expired.
void advance_statuses(BadlandsGame& game);
```

- [ ] **Step 1: Write the failing tests** — `game/tests/status_tests.cpp`

```cpp
#include "components.h"
#include "game_state.h"
#include "status.h"
#include <catch_amalgamated.hpp>

using namespace badlands;

TEST_CASE("apply_status stamps a timer that ticks down and expires", "[status]") {
    auto game = make_flat_world();
    const entt::entity e = game->registry.create();
    REQUIRE(apply_status(*game, e, StatusKind::Stunned, 100, 7u));
    CHECK(remaining_millis_of(game->registry, e, StatusKind::Stunned) == 100);
    for (int i = 0; i < 3; ++i) advance_statuses(*game);   // 3 * 33 = 99 ms
    CHECK(has_status(game->registry, e, StatusKind::Stunned));
    advance_statuses(*game);
    CHECK_FALSE(has_status(game->registry, e, StatusKind::Stunned));
    CHECK(game->registry.get<Statuses>(e).count == 0);
}

TEST_CASE("refresh keeps the longer remaining, never shortens", "[status]") {
    auto game = make_flat_world();
    const entt::entity e = game->registry.create();
    apply_status(*game, e, StatusKind::Stunned, 3000, 1u);
    apply_status(*game, e, StatusKind::Stunned, 500, 2u);
    CHECK(remaining_millis_of(game->registry, e, StatusKind::Stunned) == 3000);
    CHECK(game->registry.get<Statuses>(e).count == 1);   // refreshed, not appended
}

TEST_CASE("non-positive duration is a no-op", "[status]") {
    auto game = make_flat_world();
    const entt::entity e = game->registry.create();
    CHECK_FALSE(apply_status(*game, e, StatusKind::Stunned, 0, 1u));
    CHECK_FALSE(has_status(game->registry, e, StatusKind::Stunned));
}
```

- [ ] **Step 2: Register sources, run, confirm failure**

Add `game/src/status.cpp` to `badlands_game_lib` and `game/tests/status_tests.cpp` beside `game/tests/skills_tests.cpp` (`CMakeLists.txt:586`).
Run: `scripts/build.sh badlands_game_tests` → FAIL (no `status.h`).

- [ ] **Step 3: Implement**

Model on `game/src/skills.cpp` (dense name table + `static_assert`) and `game/src/needs.cpp` (per-tick system over a view). `advance_statuses` subtracts `kMillisPerTick` and swaps the last entry down over any that hit `<= 0`.

- [ ] **Step 4: Run the tests**

Run: `scripts/test.sh badlands_game_tests "[status]"` → PASS.

- [ ] **Step 5: Wire the tick**

In `tick_world` (`game/src/sim.cpp:168`), immediately after the per-attack cooldown loop (`sim.cpp:179-184`) and before `advance_needs(g)`, add the skill-cooldown decrement that has never existed, then `advance_statuses(g)`:

```cpp
for (auto [e, skills] : registry.view<Skills>().each()) {
    for (int i = 0; i < skills.count && i < kMaxSkills; ++i) {
        skills.cooldown_remaining[i] = std::max(0.0f, skills.cooldown_remaining[i] - dt);
    }
}
advance_statuses(g);   // statuses gate the think pass further down this same tick
```

Run: `scripts/test.sh` → full suite green.

- [ ] **Step 6: Commit**

```bash
git add game/src/status.h game/src/status.cpp game/src/components.h \
        game/include/badlands_sim.hpp game/src/sim.cpp game/tests/status_tests.cpp CMakeLists.txt
git commit -m "feat(game): status timers subsystem + skill cooldown ticking"
```

---

## Task 2: Stun enforcement

**Files:**
- Modify: `game/src/combat.h`, `game/src/combat.cpp:285` and `:315`, `game/src/movement.cpp:242-250`, `game/src/sim.cpp` (think dispatch, `sim.cpp:290-320`), `game/src/intention.{h,cpp}`, `game/src/status.cpp`
- Test: `game/tests/status_tests.cpp` (extend)

**Interfaces produced:**

```cpp
// combat.h — the defender's tactical stats AS THEY COUNT RIGHT NOW. A stunned
// defender has no ACTIVE defense: parry and evasion read 0. Armour is worn,
// not used, so it is unaffected -- and so is the attacker's own accuracy.
Combatant effective_combatant(const entt::registry& reg, entt::entity e);

// intention.h — abort whatever is running, kind unknown to the caller. The
// body of abort_intention() minus the `expected` gate; pushes
// IntentionEnded(aborted). No-op when nothing is running.
void abort_current_intention(BadlandsGame& game, uint32_t slot);
```

- [ ] **Step 1: Write the failing tests** — append to `game/tests/status_tests.cpp`

```cpp
TEST_CASE("a stunned defender has no defense and no evasion", "[status][combat]") {
    auto game = make_flat_world();
    const entt::entity e = game->registry.create();
    game->registry.emplace<Combatant>(e, Combatant{.accuracy = 0.9f, .evasion = 0.4f,
                                                   .defense = 0.3f, .armour = 2.0f});
    CHECK(effective_combatant(game->registry, e).evasion == Catch::Approx(0.4f));
    apply_status(*game, e, StatusKind::Stunned, 1000, UINT32_MAX);
    const Combatant after = effective_combatant(game->registry, e);
    CHECK(after.evasion == Catch::Approx(0.0f));
    CHECK(after.defense == Catch::Approx(0.0f));
    CHECK(after.armour == Catch::Approx(2.0f));   // armour is still worn
}

TEST_CASE("stunning a hero aborts its running intention", "[status][intention]") {
    // Spawn one hero (see game/tests/intention_tests.cpp's spawn helper), adopt
    // a MoveTo intention via apply_intention, then stun it and assert:
    //   registry.get<CurrentIntention>(hero).kind == IntentionKind::None
    // and that the inbox's newest entry is IntentionEnded with param 0 (aborted).
}

TEST_CASE("a stunned hero does not move", "[status][movement]") {
    // Spawn a hero on a flat world with a MoveTarget ~10 units away. Tick 10x
    // and record the distance travelled (> 0). Reset, stun for 2000 ms, tick
    // 10x, and assert the position is unchanged; then tick past the stun and
    // assert it moves again -- the route survived the stun.
}
```

- [ ] **Step 2: Run, confirm failure**

Run: `scripts/test.sh badlands_game_tests "[status]"` → FAIL (`effective_combatant` undeclared).

- [ ] **Step 3: Implement the four enforcement points**

1. `effective_combatant` in `combat.cpp`; use it in place of `reg.get<Combatant>(target)` at **both** `CombatRequest` assembly sites — melee (`combat.cpp:285`) and projectile arrival (`combat.cpp:315`). The projectile case reading the defender at arrival is correct: a target stunned mid-flight is defenceless when the arrow lands.
2. `movement.cpp` — `if (has_status(reg, e, StatusKind::Stunned)) continue;` inside `follow_paths` (the `entt::exclude<InsideBuilding, MeleeLock, ChattingState>` view, `movement.cpp:242-250`) and in `separate_units`' mover branch. Leave `plan_paths` alone so the planned route survives.
3. `sim.cpp` think dispatch — skip stunned entities alongside the existing `InsideBuilding` skip, covering wasm heroes and `mock_think` archetypes alike.
4. `abort_current_intention` in `intention.cpp` (extract the shared body out of `abort_intention`); `apply_status` calls it for `StatusKind::Stunned` when the entity carries a `CurrentIntention`.

- [ ] **Step 4: Run the tests**

Run: `scripts/test.sh badlands_game_tests "[status]"`, then `scripts/test.sh` → PASS.

- [ ] **Step 5: Commit**

```bash
git add game/src/combat.h game/src/combat.cpp game/src/movement.cpp game/src/sim.cpp \
        game/src/intention.h game/src/intention.cpp game/src/status.cpp game/tests/status_tests.cpp
git commit -m "feat(game): stun gates think, movement, and active defense"
```

---

## Task 3: The skill data contract (spec fields + manifest)

Reshapes `SkillSpec` around the five engine-checked fields. `SkillActivation` (active|passive) and `SkillTargeting` (direct|aoe) are **replaced**, not extended — they encode a narrower vocabulary than the contract needs.

**Files:**
- Modify: `game/include/badlands_sim.hpp`, `game/src/skills.cpp`, `game/src/sim.cpp:736` (`sanitize_skill_catalog`), `src/game/skill_manifest.cpp`, `src/game/ui/hud.cpp` (keep it compiling), `assets/skills/skills.json`
- Test: `game/tests/skills_tests.cpp`, `src/game/tests/skill_manifest_tests.cpp`, `game/tests/game_ui_tests.cpp:460-540` (update to the new fields)

**Interfaces produced:**

```cpp
// badlands_sim.hpp
enum class SkillId : int32_t { Calcify = 0, ShieldBash, Count };   // append-only

// How a skill is initiated. The engine executes Action this slice; Passive and
// Intention parse and validate, and any attempt to cast one is refused with a
// warn (game/src/intention.cpp) rather than quietly treated as an Action.
enum class SkillTrigger : int32_t { Action = 0, Passive, Intention };

// Who a cast may name. None/SelfOnly/Any resolve this slice; Multi (up to
// target_limit) and Point (an aoe centred on a point) parse and are refused.
// SelfOnly cast at anyone else is refused by the engine, never remapped.
enum class SkillTargetMode : int32_t { None = 0, SelfOnly, Any, Multi, Point };

// Whether the engine rolls a combat test per target before the effect runs,
// and off which of the caster's attacks. The declared test also supplies the
// cast's RANGE: Melee -> the caster's melee reach, Ranged -> its ranged reach,
// None -> the optional "range" constant (0 = no range check, e.g. SelfOnly).
enum class SkillAttackTest : int32_t { None = 0, Melee, Ranged };

inline constexpr int32_t kMaxSkillConstants = 8;
struct SkillConstant { std::string name; float value = 0.0f; };

struct SkillSpec {
    SkillTrigger trigger = SkillTrigger::Action;
    SkillTargetMode target = SkillTargetMode::Any;
    int32_t target_limit = 1;                  // Multi only
    float cooldown_seconds = 0.0f;             // 0 = none
    float intention_duration_seconds = 0.0f;   // 0 = none; Intention trigger only
    SkillAttackTest attack_test = SkillAttackTest::None;
    std::string effect;                        // display text
    SkillConstant constants[kMaxSkillConstants];
    int32_t constant_count = 0;
    float constant(const char* name, float fallback = 0.0f) const;
};
```

`ShieldBash` compiled defaults: trigger `Action`, target `Any`, cooldown 12 s, intention-duration 0, attack test `Melee`, effect `"Slams the target with a shield; a landed blow leaves it stunned."`, constants `{{"stun_seconds", 3.0f}}`.
`Calcify` migrates: trigger `Action`, target `SelfOnly`, cooldown 20 s, attack test `None`, effect text unchanged.

Manifest schema (`assets/skills/skills.json`), all keys optional:

```json
"ShieldBash": {
  "trigger": "action",           // action | passive | intention
  "target": "any",               // none | self | any | multi | point
  "target_limit": 1,             // multi only
  "cooldown": 12,                // seconds; 0 = none
  "intention_duration": 0,       // seconds; 0 = none
  "attack_test": "melee",        // none | melee | ranged
  "effect": "Slams the target with a shield; a landed blow leaves it stunned.",
  "constants": { "stun_seconds": 3 }
}
```

- [ ] **Step 1: Write the failing tests**

```cpp
// game/tests/skills_tests.cpp
TEST_CASE("ShieldBash compiled defaults", "[skills]") {
    badlands::SkillCatalog cat;
    const auto& s = cat.specs[static_cast<size_t>(badlands::SkillId::ShieldBash)];
    CHECK(s.trigger == badlands::SkillTrigger::Action);
    CHECK(s.target == badlands::SkillTargetMode::Any);
    CHECK(s.attack_test == badlands::SkillAttackTest::Melee);
    CHECK(s.cooldown_seconds == Catch::Approx(12.0f));
    CHECK(s.intention_duration_seconds == Catch::Approx(0.0f));
    CHECK(s.constant("stun_seconds") == Catch::Approx(3.0f));
    CHECK(s.constant("nope", -1.0f) == Catch::Approx(-1.0f));
    CHECK(badlands::SkillIdFromName("ShieldBash") == badlands::SkillId::ShieldBash);
}

// src/game/tests/skill_manifest_tests.cpp — test-local fixtures only
TEST_CASE("the full vocabulary parses", "[skill_manifest]") {
    TempManifest m(R"({"ShieldBash": {"trigger": "intention", "target": "multi",
                                      "target_limit": 3, "cooldown": 9,
                                      "intention_duration": 2.5, "attack_test": "ranged",
                                      "constants": {"stun_seconds": 4.5, "radius": 2}}})");
    SkillCatalog cat;
    REQUIRE(badlands::LoadSkillCatalog(m.path, cat));
    const auto& s = cat.specs[static_cast<size_t>(SkillId::ShieldBash)];
    CHECK(s.trigger == SkillTrigger::Intention);
    CHECK(s.target == SkillTargetMode::Multi);
    CHECK(s.target_limit == 3);
    CHECK(s.intention_duration_seconds == Catch::Approx(2.5f));
    CHECK(s.attack_test == SkillAttackTest::Ranged);
    CHECK(s.constant("radius") == Catch::Approx(2.0f));
}

TEST_CASE("bad vocabulary and bad constants fail the load", "[skill_manifest]") {
    { TempManifest m(R"({"ShieldBash": {"trigger": "whenever"}})");
      SkillCatalog c; CHECK_FALSE(badlands::LoadSkillCatalog(m.path, c)); }
    { TempManifest m(R"({"ShieldBash": {"constants": {"stun_seconds": "soon"}}})");
      SkillCatalog c; CHECK_FALSE(badlands::LoadSkillCatalog(m.path, c)); }
}
```

- [ ] **Step 2: Run, confirm failure**

Run: `scripts/test.sh badlands_game_tests "[skills]"` → FAIL (no `SkillId::ShieldBash`).

- [ ] **Step 3: Implement**

- `badlands_sim.hpp`: the enums + reshaped `SkillSpec` above; delete `SkillActivation`/`SkillTargeting`.
- `skills.cpp`: append the `ShieldBash` row to `kSkills` — `{SkillId::ShieldBash, "ShieldBash", SkillTriggerKind::MeleeThreatClose, 2.0f}` (the AI *advice* trigger table, unrelated to `SkillTrigger`; keep its comment saying so) — plus both skills' `SkillCatalog()` defaults.
- `skill_manifest.cpp`: parse each key with the existing `ReadChoice`/`ReadNum`/`ReadString` helpers, and `"constants"` as an object of numbers. Every failure warns and returns false: unknown enum name, non-numeric constant, more than `kMaxSkillConstants` entries, `target_limit < 1`. A constant named in JSON but absent from the compiled defaults is **appended**, not rejected — JSON is the tuning surface.
- `sanitize_skill_catalog` (`sim.cpp:736`): clamp `cooldown_seconds`/`intention_duration_seconds` to `>= 0` (as today), clamp `target_limit` into `[1, BL_SKILL_MAX_TARGETS]`, and force non-finite constant values to 0. Signs on constants are the skill's business — leave them.
- `hud.cpp:81` `SkillSummary`: rewrite against the new fields so the build stays green (`"action, any, cd 12s"`); Task 8 replaces it with cards. Update `game_ui_tests.cpp:460-540` accordingly.
- `assets/skills/skills.json`: both skills, mirroring the compiled defaults.

- [ ] **Step 4: Run the tests**

Run: `scripts/test.sh badlands_game_tests "[skills]"`, `"[skill_manifest]"`, `"[game_ui]"` → PASS.

- [ ] **Step 5: Commit**

```bash
git add game/include/badlands_sim.hpp game/src/skills.cpp game/src/sim.cpp \
        src/game/skill_manifest.cpp src/game/ui/hud.cpp game/tests/skills_tests.cpp \
        game/tests/game_ui_tests.cpp src/game/tests/skill_manifest_tests.cpp assets/skills/skills.json
git commit -m "feat(game): skill spec is trigger/target/cooldown/duration/effect + constants"
```

---

## Task 4: The effect contract (`skill_abi.h`) and the shield-bash effect

Pure, world-free, and independently testable: this task adds no engine wiring at all.

**Files:**
- Create: `game/src/skill_abi.h`, `game/tests/skill_effect_tests.cpp`
- Modify: `game/src/skills.h`, `game/src/skills.cpp`, `CMakeLists.txt`

**Interfaces produced:**

```c
/* game/src/skill_abi.h — the skill execution contract. Written as a C header,
   like game/src/brain_abi.h, because this IS the wire a Nim/wasm skill script
   will implement later: flat POD, fixed capacities, explicit padding, no C++
   types and no engine handles. An effect is a PURE function of this input:
   it may not read the world, and it applies nothing itself -- it returns ops
   the engine validates and applies (the same discipline the brain's action
   channel uses). */
#define BL_SKILL_ABI_VERSION 1
#define BL_SKILL_MAX_TARGETS 8
#define BL_SKILL_MAX_CONSTANTS 8
#define BL_SKILL_MAX_OPS 8

/* Pre-rolled attack-test outcome per target (engine-side, seeded). */
#define BL_TEST_NOT_RUN 0   /* the skill declares attack_test = none */
#define BL_TEST_BLOCKED 1
#define BL_TEST_DODGED  2
#define BL_TEST_HIT     3

/* Effect op kinds (append-only). */
#define BL_FX_NONE 0
#define BL_FX_APPLY_STATUS 1   /* param_i = StatusKind, param_f = duration ms */
#define BL_FX_DAMAGE 2         /* param_f = hp to remove */

/* Relation of a target to the caster. */
#define BL_REL_SELF 0
#define BL_REL_FRIEND 1
#define BL_REL_ENEMY 2

typedef struct BlSkillCaster {
    uint32_t slot;
    float pos_x, pos_z;
    float accuracy;
    float health_frac;
    float melee_range;
    float ranged_range;
    uint32_t _pad;
} BlSkillCaster;                     /* 32 */

typedef struct BlSkillTarget {
    uint32_t slot;
    float pos_x, pos_z;
    float dist;
    float health_frac;
    float defense;                   /* AFTER effective_combatant */
    float evasion;
    float armour;
    int32_t attack_test;             /* BL_TEST_* */
    float test_damage;               /* what the roll produced; 0 unless HIT */
    int32_t relation;                /* BL_REL_* */
    uint32_t _pad;
} BlSkillTarget;                     /* 48 */

typedef struct BlSkillConstant { char name[24]; float value; uint32_t _pad; } BlSkillConstant; /* 32 */

typedef struct BlSkillCastContext {
    uint32_t version;                /* == BL_SKILL_ABI_VERSION */
    int32_t skill_id;
    int64_t world_millis;
    uint64_t seed;                   /* deterministic stream, engine-derived */
    BlSkillCaster caster;
    int32_t target_count;
    uint32_t _pad;
    BlSkillTarget targets[BL_SKILL_MAX_TARGETS];
    int32_t constant_count;
    uint32_t _pad2;
    BlSkillConstant constants[BL_SKILL_MAX_CONSTANTS];
} BlSkillCastContext;                /* 712 */

typedef struct BlSkillEffectOp {
    int32_t kind;                    /* BL_FX_* */
    uint32_t target_slot;            /* MUST be one of the context's targets */
    int32_t param_i;
    float param_f;
} BlSkillEffectOp;                   /* 16 */

typedef struct BlSkillEffectBatch {
    int32_t count;
    uint32_t _pad;
    BlSkillEffectOp ops[BL_SKILL_MAX_OPS];
} BlSkillEffectBatch;                /* 136 */
```

```cpp
// skills.h
using SkillEffectFn = void (*)(const BlSkillCastContext&, BlSkillEffectBatch&);
SkillEffectFn SkillEffectOf(SkillId id);   // dense table, static_assert-pinned

// Named lookup over the context's constants; `fallback` when absent. The one
// way effect code reads tuning, host-side or (later) guest-side.
float skill_constant(const BlSkillCastContext& ctx, const char* name, float fallback = 0.0f);
// Appends an op; drops with a warn at BL_SKILL_MAX_OPS.
void push_effect_op(BlSkillEffectBatch& out, const BlSkillEffectOp& op);
```

`shield_bash` (pure control — the engine's melee test decides, the damage is deliberately discarded):

```cpp
void shield_bash(const BlSkillCastContext& ctx, BlSkillEffectBatch& out) {
    const float seconds = skill_constant(ctx, "stun_seconds", 0.0f);
    for (int32_t i = 0; i < ctx.target_count; ++i) {
        if (ctx.targets[i].attack_test != BL_TEST_HIT) continue;   // blocked/dodged -> nothing
        push_effect_op(out, {BL_FX_APPLY_STATUS, ctx.targets[i].slot,
                             static_cast<int32_t>(StatusKind::Stunned), seconds * 1000.0f});
    }
}
```

`calcify` gets a documented no-op effect (its shield mechanic is a later slice; it still displays and still grants at level 5).

- [ ] **Step 1: Write the failing tests** — `game/tests/skill_effect_tests.cpp` (no world, no `Sim`)

```cpp
TEST_CASE("shield-bash stuns a hit target for its constant", "[skill_effect]") {
    BlSkillCastContext ctx{};
    ctx.version = BL_SKILL_ABI_VERSION;
    ctx.skill_id = static_cast<int32_t>(SkillId::ShieldBash);
    ctx.target_count = 1;
    ctx.targets[0] = {.slot = 4u, .attack_test = BL_TEST_HIT, .test_damage = 6.0f,
                      .relation = BL_REL_ENEMY};
    std::snprintf(ctx.constants[0].name, sizeof(ctx.constants[0].name), "stun_seconds");
    ctx.constants[0].value = 3.0f;
    ctx.constant_count = 1;

    BlSkillEffectBatch out{};
    SkillEffectOf(SkillId::ShieldBash)(ctx, out);
    REQUIRE(out.count == 1);
    CHECK(out.ops[0].kind == BL_FX_APPLY_STATUS);
    CHECK(out.ops[0].target_slot == 4u);
    CHECK(out.ops[0].param_i == static_cast<int32_t>(StatusKind::Stunned));
    CHECK(out.ops[0].param_f == Catch::Approx(3000.0f));
}

TEST_CASE("a blocked or dodged test produces no ops", "[skill_effect]") {
    // same context with attack_test = BL_TEST_BLOCKED, then BL_TEST_DODGED
    CHECK(out.count == 0);
}

TEST_CASE("shield-bash never emits damage", "[skill_effect]") {
    // test_damage 99.0f, BL_TEST_HIT -> still exactly one APPLY_STATUS op
}

TEST_CASE("a missing constant falls back rather than stunning forever", "[skill_effect]") {
    // constant_count = 0 -> seconds 0 -> op with param_f 0 (the engine's
    // apply_status then no-ops on a non-positive duration)
}

TEST_CASE("contract layout is pinned", "[skill_effect]") {
    CHECK(sizeof(BlSkillCaster) == 32);
    CHECK(sizeof(BlSkillTarget) == 48);
    CHECK(sizeof(BlSkillConstant) == 32);
    CHECK(sizeof(BlSkillCastContext) == 712);
    CHECK(sizeof(BlSkillEffectOp) == 16);
    CHECK(sizeof(BlSkillEffectBatch) == 136);
}
```

If the compiler disagrees with a size, trust the compiler and update both the `static_assert`s in `skill_abi.h` and this test — a silent mismatch is the one failure mode this contract cannot survive.

- [ ] **Step 2: Run, confirm failure**

Run: `scripts/build.sh badlands_game_tests` → FAIL (no `skill_abi.h`).

- [ ] **Step 3: Implement** `skill_abi.h` (with its `static_assert` block, guarded `#ifdef __cplusplus`, exactly as `brain_abi.h` does at its foot), the effect table in `skills.cpp`, and the two helpers.

- [ ] **Step 4: Run the tests**

Run: `scripts/test.sh badlands_game_tests "[skill_effect]"` → PASS.

- [ ] **Step 5: Commit**

```bash
git add game/src/skill_abi.h game/src/skills.h game/src/skills.cpp \
        game/tests/skill_effect_tests.cpp CMakeLists.txt
git commit -m "feat(game): skill effect contract (cast context in, op batch out)"
```

---

## Task 5: Casting — target resolution, attack test, batch application, `CommandKind::UseSkill`

**Files:**
- Modify: `game/src/skills.h`, `game/src/skills.cpp`, `game/src/command.h`, `game/src/command.cpp`, `game/include/badlands_sim.hpp`
- Test: `game/tests/shield_bash_tests.cpp` (new; register in `CMakeLists.txt`)

**Interfaces consumed:** `apply_status` (T1), `effective_combatant` (T2), `SkillSpec` (T3), `SkillEffectOf` (T4), `resolve_attack`/`melee_range`/`ranged_range` (`combat.h`).

**Interfaces produced:**

```cpp
// skills.h
// Cast range for a skill: melee/ranged reach when it declares an attack test,
// else its optional "range" constant (0 = no range check, e.g. SelfOnly).
float skill_cast_range(const entt::registry& reg, entt::entity caster, const SkillSpec& spec);

// Which slots a cast of `spec` by `caster` at `named_target` legally hits.
// None -> {}; SelfOnly -> the caster (a named OTHER slot is refused, never
// remapped); Any -> exactly the named target. Multi/Point return false --
// declared vocabulary, no engine implementation this slice.
bool resolve_skill_targets(const BadlandsGame& game, entt::entity caster,
                           const SkillSpec& spec, uint32_t named_target_slot,
                           uint32_t out_slots[BL_SKILL_MAX_TARGETS], int32_t& out_count);

// Builds the flat context: caster view, per-target views (defense/evasion via
// effective_combatant), the pre-rolled attack test per target, and the spec's
// constants. Pure over the registry; identical live and on replay.
BlSkillCastContext build_cast_context(const BadlandsGame& game, entt::entity caster,
                                      SkillId id, const SkillSpec& spec,
                                      const uint32_t target_slots[], int32_t target_count);

// Validates and applies each op: the target must be one the context named (an
// effect cannot reach an entity it was not given), the status kind must be in
// range, durations/amounts are clamped to >= 0. Emits StatusApplied / the
// standard hit event per applied op. Unknown op kinds warn and are skipped
// (forward compat: a newer script's op this build has not learned about).
void apply_effect_batch(BadlandsGame& game, uint32_t caster_slot,
                        const BlSkillCastContext& ctx, const BlSkillEffectBatch& batch);
```

Attack-test seeding: the pre-roll builds a `CombatRequest` with `attack_index = kSkillSeedBase + (int32_t)skill_id` (`kSkillSeedBase = 100`, well clear of `kMaxAttacks`), so a bash and a sword swing landing in the same tick on the same target never share a roll (`combat_seed` folds `attack_index`, `combat.cpp:48`).

- [ ] **Step 1: Write the failing tests** — `game/tests/shield_bash_tests.cpp`

```cpp
// Local helper: two adjacent combatants on a flat world, the first holding
// ShieldBash at skill slot 0, with accuracy/defense/evasion dialled so the
// outcome is deterministic rather than probabilistic.

TEST_CASE("a landed bash stuns the target and deals no damage", "[skills][cast]") {
    // caster accuracy 1.0 vs defender defense 0 / evasion 0 -> always lands
    apply_command(*game, {CommandKind::UseSkill, caster_slot, target_slot, {}, 0});
    CHECK(has_status(reg, target, StatusKind::Stunned));
    CHECK(remaining_millis_of(reg, target, StatusKind::Stunned) == 3000);
    CHECK(reg.get<Health>(target).hp == Catch::Approx(start_hp));   // pure control
}

TEST_CASE("a blocked bash stuns nobody", "[skills][cast]") {
    // defender defense 1.0 vs accuracy 0.0 -> gate 1 always blocks
    CHECK_FALSE(has_status(reg, target, StatusKind::Stunned));
}

TEST_CASE("casting stamps only the skill's own cooldown", "[skills][cast]") {
    CHECK(reg.get<Skills>(caster).cooldown_remaining[0] == Catch::Approx(12.0f));
    CHECK(reg.get<Attacks>(caster).cooldown_remaining[0] == Catch::Approx(0.0f));
}

TEST_CASE("a cast on cooldown is a no-op", "[skills][cast]") {
    // apply twice; the second changes nothing and logs no second SkillUsed
}

TEST_CASE("a SelfOnly skill cast at another entity is refused", "[skills][cast]") {
    // Calcify (SelfOnly) named at the enemy slot -> no ops, no cooldown stamped
    CHECK(reg.get<Skills>(caster).cooldown_remaining[calcify_idx] == Catch::Approx(0.0f));
}

TEST_CASE("a Passive- or Intention-trigger skill cannot be cast", "[skills][cast]") {
    // override the catalog spec's trigger, then cast -> refused, nothing stamped
}

TEST_CASE("an op naming an entity outside the context is dropped", "[skills][cast]") {
    // drive apply_effect_batch directly with a bogus target_slot -> no status,
    // and the legitimate op in the same batch still applies
}

TEST_CASE("cast emits SkillUsed and StatusApplied", "[skills][events]") {
    // drain sim.Events() and match kinds (see game/tests/events_tests.cpp)
}
```

- [ ] **Step 2: Run, confirm failure**

Run: `scripts/build.sh badlands_game_tests` → FAIL (no `CommandKind::UseSkill`).

- [ ] **Step 3: Implement**

- `badlands_sim.hpp`: append `UseSkill` to `CommandKindId` (after `Engage`); append `SkillUsed` and `StatusApplied` to `GameEventKind`. `SkillUsed`: `actor` = caster slot, `target_id` = first resolved target (or the caster for `SelfOnly`/`None`), `amount` = `(float)SkillId`. `StatusApplied`: `actor` = source slot, `target_id` = affected slot, `amount` = `(float)StatusKind`.
- `command.h`: append `CommandKind::UseSkill` + its `static_assert` pairing; document `param_a` = the caster's **skill slot index** (not a `SkillId`), `target_id` = the resolved primary target.
- `command.cpp`: the handler, modelled on the `Attack` case (`command.cpp:129`) and **authoritative** — the world may have moved between queue and drain. In order: live caster carrying `Skills`; `param_a` in `[0, count)`; `cooldown_remaining[param_a] <= 0`; `spec.trigger == SkillTrigger::Action` (else warn + return, naming the trigger); `resolve_skill_targets` succeeds; every resolved target within `skill_cast_range`. Then `build_cast_context` → `SkillEffectOf(id)(ctx, batch)` → `apply_effect_batch` → stamp `cooldown_remaining[param_a] = spec.cooldown_seconds` → `emit_event(SkillUsed)`.
- `skills.cpp`: the four functions above.

- [ ] **Step 4: Run the tests**

Run: `scripts/test.sh badlands_game_tests "[skills]"` → PASS, then `scripts/test.sh` (watch `command_tests.cpp`/`events_tests.cpp`, which enumerate kinds).

- [ ] **Step 5: Commit**

```bash
git add game/include/badlands_sim.hpp game/src/command.h game/src/command.cpp \
        game/src/skills.h game/src/skills.cpp game/tests/shield_bash_tests.cpp CMakeLists.txt
git commit -m "feat(game): UseSkill command casts through the effect contract"
```

---

## Task 6: `BL_ACT_USE_SKILL` through the action gateway

**Files:**
- Modify: `game/src/intention.cpp` (`resolve_action`), `game/src/intention.h` (doc), `game/src/brain_abi.h` (drop "reserved")
- Test: `game/tests/intention_tests.cpp`

- [ ] **Step 1: Write the failing tests** — append to `game/tests/intention_tests.cpp`, following its existing `AgentAction` cases

```cpp
TEST_CASE("BL_ACT_USE_SKILL queues a UseSkill command when valid", "[intention][skills]") {
    CHECK(resolve_action(*game, hero_slot, {BL_ACT_USE_SKILL, enemy_slot, 0}));
    REQUIRE(game->command_queue.size() == 1);
    CHECK(game->command_queue[0].kind == CommandKind::UseSkill);
    CHECK(game->command_queue[0].param_a == 0);
    CHECK(game->command_queue[0].target_id == enemy_slot);   // concrete, never UINT32_MAX
}

TEST_CASE("BL_ACT_USE_SKILL drops on a bad index, a cooldown, or out of range",
          "[intention][skills]") {
    CHECK_FALSE(resolve_action(*game, hero_slot, {BL_ACT_USE_SKILL, enemy_slot, 5}));
    // cooldown_remaining[0] = 5.0f ->
    CHECK_FALSE(resolve_action(*game, hero_slot, {BL_ACT_USE_SKILL, enemy_slot, 0}));
    // enemy moved 20 units away ->
    CHECK_FALSE(resolve_action(*game, hero_slot, {BL_ACT_USE_SKILL, enemy_slot, 0}));
    CHECK(game->command_queue.empty());
}

TEST_CASE("a non-Action trigger is refused at the gateway", "[intention][skills]") {
    // spec.trigger = Passive, then Intention -> both dropped, queue empty
}

TEST_CASE("BL_ACT_USE_SKILL infers its target from a running Attack intention",
          "[intention][skills]") {
    // adopt Attack, then pass target_slot = UINT32_MAX
}
```

- [ ] **Step 2: Run, confirm failure**

Run: `scripts/test.sh badlands_game_tests "[intention]"` → FAIL (still warn-and-dropped).

- [ ] **Step 3: Implement**

Restructure `resolve_action`'s head into `switch (action.kind)`: `BL_ACT_ATTACK` unchanged, a new `BL_ACT_USE_SKILL` branch, everything else keeps the existing warn-and-drop. The skill branch validates, each a warn + `false` + no command: live caster carrying `Skills`; `arg` in `[0, count)`; off cooldown; `spec.trigger == Action`; target resolves (named slot live, or inferred from a running `Attack` intention — reuse that resolution block rather than duplicating it); `resolve_skill_targets` accepts the pairing (this is where `SelfOnly` at someone else is refused); within `skill_cast_range`. Valid → push `{CommandKind::UseSkill, slot, resolved_target_slot, {0,0}, action.arg}` with a concrete slot, so the log is self-describing. The command handler re-validates all of it anyway (Task 5) — this is the cheap early refusal that gives the brain a warn.

- [ ] **Step 4: Run the tests**

Run: `scripts/test.sh badlands_game_tests "[intention]"` → PASS, then `scripts/test.sh`.

- [ ] **Step 5: Commit**

```bash
git add game/src/intention.cpp game/src/intention.h game/src/brain_abi.h game/tests/intention_tests.cpp
git commit -m "feat(game): BL_ACT_USE_SKILL is live in the action gateway"
```

---

## Task 7: Data-driven skill acquisition

**Files:**
- Modify: `game/include/badlands_sim.hpp`, `game/src/creature_catalog.cpp`, `game/src/skills.{h,cpp}` (delete `kGrants`/`SkillGrantTable`), `game/src/components.h`, `game/src/heroes.cpp:190-196`, `game/src/progression.cpp:43-47`, `src/game/creature_manifest.cpp`, `assets/creatures/creatures.json`
- Test: `game/tests/skills_tests.cpp`, `game/tests/progression_tests.cpp`, `src/game/tests/creature_manifest_tests.cpp`

**Interfaces produced:**

```cpp
// badlands_sim.hpp
struct SkillGrantRow { int32_t skill = -1; int32_t level = 1; };   // skill = SkillId
struct CharacterDesc { ...; SkillGrantRow skill_grants[kMaxSkills]{}; int32_t skill_grant_count = 0; };

// components.h — the spawn-time copy, so the level-up hook needs no class lookup
struct SkillGrants { SkillGrantRow rows[kMaxSkills]{}; int32_t count = 0; };

// skills.h — replaces grant_skills_for_level(Skills&, int32_t hero_class, int32_t)
void grant_skills_for_level(Skills& s, const SkillGrants& grants, int32_t level);
```

- [ ] **Step 1: Write the failing tests**

```cpp
// skills_tests.cpp
TEST_CASE("grants fire only at their exact level", "[skills]") {
    SkillGrants g; g.rows[0] = {static_cast<int32_t>(SkillId::ShieldBash), 3}; g.count = 1;
    Skills s;
    grant_skills_for_level(s, g, 2);  CHECK(s.count == 0);
    grant_skills_for_level(s, g, 3);  REQUIRE(s.count == 1);
    CHECK(s.ids[0] == SkillId::ShieldBash);
    CHECK(s.cooldown_remaining[0] == Catch::Approx(0.0f));
    grant_skills_for_level(s, g, 3);  CHECK(s.count == 1);   // dupe-proof
}

// progression_tests.cpp — replaces the current Apprentice/Calcify@5 case
TEST_CASE("a mercenary learns ShieldBash on reaching level 3", "[progression][skills]") {
    CHECK(hero_row->skill_count == 1);
    CHECK(hero_row->skills[0] == static_cast<int32_t>(SkillId::ShieldBash));
}

// src/game/tests/creature_manifest_tests.cpp — test-local fixture, never the shipped file
TEST_CASE("creature skills parse from JSON", "[creature_manifest]") {
    TempManifest m(R"({"Mercenary": {"skills": [{"name": "ShieldBash", "level": 4}]}})");
    CreatureCatalog cat;
    REQUIRE(badlands::LoadCreatureCatalog(m.path, cat));
    const CharacterDesc& d = cat.defs[static_cast<int>(CreatureId::Mercenary)];
    REQUIRE(d.skill_grant_count == 1);
    CHECK(d.skill_grants[0].skill == static_cast<int32_t>(SkillId::ShieldBash));
    CHECK(d.skill_grants[0].level == 4);
}

TEST_CASE("an unknown skill name fails the creature load", "[creature_manifest]") {
    TempManifest m(R"({"Mercenary": {"skills": [{"name": "Nope", "level": 1}]}})");
    CreatureCatalog cat;
    CHECK_FALSE(badlands::LoadCreatureCatalog(m.path, cat));
}
```

Create `src/game/tests/creature_manifest_tests.cpp` if absent, modelled on `src/game/tests/skill_manifest_tests.cpp` (same `TempManifest` helper, same never-read-the-shipped-asset rule), and register it in `CMakeLists.txt`.

- [ ] **Step 2: Run, confirm failure**

Run: `scripts/build.sh badlands_game_tests` → FAIL (no `SkillGrants`).

- [ ] **Step 3: Implement**

- `creature_catalog.cpp`: author `Mercenary` → `{ShieldBash, 3}` and `Apprentice` → `{Calcify, 5}`; delete `kGrants`/`SkillGrantTable()` from `skills.{h,cpp}` — the catalog is now the single source of truth.
- `heroes.cpp` spawn (`heroes.cpp:190-196`): copy `desc.skill_grants` into a `SkillGrants` component, emplace it, then `grant_skills_for_level(sk, grants, 1)`.
- `progression.cpp:43`: read `SkillGrants` off the entity instead of `HeroCharacter::hero_class`.
- `creature_manifest.cpp`: parse `"skills"` as an array of `{name, level}`; unknown name, non-object entry, or more than `kMaxSkills` entries warns and **fails the load**, matching the file's existing unknown-creature posture.
- `assets/creatures/creatures.json`: give `Mercenary` its `"skills": [{"name": "ShieldBash", "level": 3}]` block so the shipped data states what the compiled default already does.

- [ ] **Step 4: Run the tests**

Run: `scripts/test.sh` → PASS (watch `progression_tests.cpp`, which asserts through the deleted class table today).

- [ ] **Step 5: Commit**

```bash
git add game/include/badlands_sim.hpp game/src/creature_catalog.cpp game/src/skills.h \
        game/src/skills.cpp game/src/components.h game/src/heroes.cpp game/src/progression.cpp \
        src/game/creature_manifest.cpp src/game/tests/creature_manifest_tests.cpp \
        game/tests/skills_tests.cpp game/tests/progression_tests.cpp \
        assets/creatures/creatures.json CMakeLists.txt
git commit -m "feat(game): skill grants come from creature config, not a compiled table"
```

---

## Task 8: ABI v4 — the brain can see and use its skills

**Files:**
- Modify: `game/src/brain_abi.h`, `game/src/wasm_brain.cpp`, `game/tests/brain_abi_tests.cpp`, `game/tests/wasm_brain_tests.cpp`, `scripts/brains/nim/{abi,hero_view,hero}.nim`, `src/crates/brainhost/src/lib.rs`, `assets/brains/*.wasm`

**Interfaces produced:**

```c
/* brain_abi.h */
#define BL_ABI_VERSION 4
#define BL_MAX_SKILLS 8      /* == badlands::kMaxSkills */
#define BL_ST_STUNNED 4      /* appended to the status vocabulary */

/* One learned skill slot. Its INDEX in this array is what BL_ACT_USE_SKILL's
   `arg` names -- the same index as the caster's own Skills component, packed
   1:1. `trigger`/`target_mode` are badlands::SkillTrigger/SkillTargetMode, so
   a brain knows which skills it may fire as an action at all. */
typedef struct BlViewSkill {
    int32_t  skill_id;
    float    cooldown_remaining;   /* seconds; 0 = ready */
    uint32_t ready;                /* bool: off cooldown */
    uint32_t recommended;          /* bool: evaluate_skill_triggers advice */
    int32_t  trigger;              /* SkillTrigger */
    int32_t  target_mode;          /* SkillTargetMode */
} BlViewSkill;                     /* 24 */

/* Block order (binding): self / suggest / factors / statuses / attacks /
   SKILLS / events / chars. */
    int32_t skill_count;
    uint32_t _pad6;
    BlViewSkill skills[BL_MAX_SKILLS];
```

`sizeof(BlViewWire)` becomes `1560 + 8 + 192 = 1760`. If the compiler disagrees, trust it and propagate to all three mirrors (`brain_abi.h` asserts, `abi.nim` `doAssert`s, `brainhost/src/lib.rs`'s `VIEW_WIRE_LEN`).

- [ ] **Step 1: Write the failing tests**

```cpp
// game/tests/brain_abi_tests.cpp
TEST_CASE("v4 skills block", "[brain_abi]") {
    CHECK(BL_ABI_VERSION == 4);
    CHECK(BL_MAX_SKILLS == badlands::kMaxSkills);
    CHECK(sizeof(BlViewSkill) == 24);
    CHECK(sizeof(BlViewWire) == 1760);
    CHECK(offsetof(BlViewWire, skills) > offsetof(BlViewWire, attacks));
    CHECK(offsetof(BlViewWire, events) > offsetof(BlViewWire, skills));
}
```

Plus `wasm_brain_tests.cpp`: pack a wire for a hero holding `ShieldBash` with 4 s left on cooldown → `skill_count == 1`, `skills[0].skill_id == (int)SkillId::ShieldBash`, `ready == 0`, `cooldown_remaining ≈ 4.0f`, `trigger == (int)SkillTrigger::Action`; and a stunned hero's `statuses` carrying `BL_ST_STUNNED` with its remaining ms.

- [ ] **Step 2: Run, confirm failure**

Run: `scripts/test.sh badlands_game_tests "[brain_abi]"` → FAIL.

- [ ] **Step 3: Implement the host side**

- `brain_abi.h`: bump the version, add `BL_MAX_SKILLS`/`BlViewSkill`/`BL_ST_STUNNED`, insert the block after `attacks`, update every affected `static_assert`, drop the "reserved" wording on `BL_ACT_USE_SKILL`.
- `wasm_brain.cpp`: pack the skills block beside the attacks block (`wasm_brain.cpp:219-231`), filling `ready`/`recommended` from `evaluate_skill_triggers` — the advice function that has sat unused since slice A. Push `BL_ST_STUNNED` next to the existing `BL_ST_MELEE_LOCKED` push (`wasm_brain.cpp:205-212`) using `remaining_millis_of`. Add `static_assert(BL_MAX_SKILLS == kMaxSkills)` beside the existing `BL_MAX_ATTACKS` one (`wasm_brain.cpp:38`).

- [ ] **Step 4: Run the host tests**

Run: `scripts/test.sh badlands_game_tests "[brain_abi]"` and `"[wasm_brain]"` → PASS.

- [ ] **Step 5: Mirror in Nim and teach the brain to bash**

- `abi.nim`: `BL_ABI_VERSION = 4`, `BL_MAX_SKILLS = 8`, `BL_ST_STUNNED = 4`, the `BlViewSkill` packed object, the two new `BlViewWire` fields in the same position as the C struct, updated `doAssert sizeof(...)` lines.
- `hero_view.nim`: copy `skill_count`/`skills` into `HeroView` exactly as `attackCount`/`attacks` are copied (`hero_view.nim:133-135`); mirror `SkillId::ShieldBash` and `SkillTrigger::Action` as local constants with the same hand-sync comment `kAttackCategoryRanged` carries (`hero.nim:106`).
- `hero.nim`: in the combat branch (`hero.nim:210-222`), after the existing `BL_ACT_ATTACK` enqueue:

```nim
let bash = findReadySkill(v, kSkillShieldBash)
if bash >= 0 and v.hasThreat and v.threatDist <= meleeReachOf(v):
  bl_enqueue_action(BL_ACT_USE_SKILL, v.threatSlot, bash)
```

`findReadySkill` scans `v.skills[0 ..< v.skillCount]` for a matching `skill_id` that is `ready` **and** `trigger == kSkillTriggerAction`, returning the slot index or -1; `meleeReachOf` is the longest `range` among non-ranged attacks. Both sit beside `pickBestAttack` (`hero.nim:120`).

- [ ] **Step 6: Rebuild the artifacts, update the Rust constants**

```bash
scripts/build_brains.sh
```

Set `ABI_VERSION: i32 = 4` and `VIEW_WIRE_LEN: usize = 1760` in `src/crates/brainhost/src/lib.rs` (~lines 706, 1048), updating the comments to name v4 and the skills block.

Run: `cargo test --manifest-path src/crates/brainhost/Cargo.toml --lib` → PASS (`real_hero_wasm_conforms` is the gate proving the rebuilt `hero.wasm` really is v4).

- [ ] **Step 7: Commit (artifacts are LFS — stage explicitly)**

```bash
git add game/src/brain_abi.h game/src/wasm_brain.cpp game/tests/brain_abi_tests.cpp \
        game/tests/wasm_brain_tests.cpp scripts/brains/nim/abi.nim \
        scripts/brains/nim/hero_view.nim scripts/brains/nim/hero.nim \
        src/crates/brainhost/src/lib.rs
git add assets/brains/hero.wasm assets/brains/idle_test.wasm assets/brains/trap_test.wasm
git commit -m "feat(brain): ABI v4 exposes skill slots; hero brain bashes with the shield"
```

---

## Task 9: Hero-view skill cards

**Files:**
- Modify: `src/game/ui/hud.{hpp,cpp}`, `src/executables/game/game_view.{hpp,cpp}`
- Test: `game/tests/game_ui_tests.cpp`

**Interfaces produced:**

```cpp
// hud.hpp — one card per learned skill. Pure model; BuildHud lays it out.
struct HudSkillCard {
  std::string name;      // "ShieldBash"
  std::string type;      // "action" | "passive" | "focus"   (SkillTrigger)
  std::string target;    // "none" | "self" | "any" | "multi" | "point"
  std::string cooldown;  // "cd 12s" | "no cd"
};
struct HudSkillCards {
  std::string heading = "Skills";
  std::vector<HudSkillCard> cards;   // already windowed by the caller
  int scroll = 0;                    // cards hidden above the window
  int total = 0;                     // for the "2/4" position line
};
// HudSelection gains: HudSkillCards skill_cards;

// How many cards fit the fixed-height skills region -- the ui crate cannot
// clip, so the windowing and the panel must agree, exactly like
// HudCombatLogCapacity(). Scale-invariant.
uint32_t HudSkillCardCapacity();

// Replaces the rows-only version: still appends level/xp, then fills
// sel.skill_cards from the hero's skills at the caller's scroll offset.
void AppendHeroProgressionRows(HudSelection& sel, const CharacterState& hero,
                               const SkillCatalog& skills, int scroll);
```

- [ ] **Step 1: Write the failing tests** — extend `game/tests/game_ui_tests.cpp` (it already builds `CharacterState` + `SkillCatalog` fixtures at lines 460-540)

```cpp
TEST_CASE("skill cards carry name, type, target and cooldown", "[game_ui]") {
    CharacterState hero{}; hero.level = 3; hero.skill_count = 1;
    hero.skills[0] = static_cast<int32_t>(SkillId::ShieldBash);
    SkillCatalog cat;
    HudSelection sel;
    AppendHeroProgressionRows(sel, hero, cat, 0);
    REQUIRE(sel.skill_cards.cards.size() == 1);
    CHECK(sel.skill_cards.cards[0].name == "ShieldBash");
    CHECK(sel.skill_cards.cards[0].type == "action");
    CHECK(sel.skill_cards.cards[0].target == "any");
    CHECK(sel.skill_cards.cards[0].cooldown == "cd 12s");
    CHECK(sel.skill_cards.total == 1);
}

TEST_CASE("trigger and target vocabulary render", "[game_ui]") {
    // trigger Passive -> "passive"; Intention -> "focus";
    // target SelfOnly -> "self"; Multi -> "multi"; Point -> "point"
}

TEST_CASE("the card window honours the scroll offset", "[game_ui]") {
    // 8 skills, capacity C: scroll 0 shows [0, C); scroll 2 shows [2, 2+C);
    // an over-large scroll clamps to the last full window rather than emptying
    CHECK(sel.skill_cards.cards.size() <= HudSkillCardCapacity());
}

TEST_CASE("a hero with no skills produces no card block", "[game_ui]") {
    CHECK(sel.skill_cards.cards.empty());
}
```

- [ ] **Step 2: Run, confirm failure**

Run: `scripts/test.sh badlands_game_tests "[game_ui]"` → FAIL.

- [ ] **Step 3: Implement**

- `hud.cpp`: constants `kSkillPanelHeight = 200.0f`, `kSkillCardHeight = 52.0f`, `kSkillCardGap = 4.0f` beside the log constants (`hud.cpp:24-26`); `HudSkillCardCapacity()` mirrors that math the way `HudCombatLogCapacity()` does. Each card is a `UI_ELEM_PANEL` (`bg = kLinkBg`, `fixed = kSkillCardHeight`, `id = kHudSkillList` so the region hit-tests as one) holding a name label in `kTitleFg` and a muted `"action · any"` / `"cd 12s"` row; a trailing muted `"n/total"` line renders when `total > cards.size()`. Delete `SkillSummary` and the wrapped-effect rows it fed; keep `AppendWrapped` only if the building panel still uses it.
- `hud.hpp`: add `kHudSkillList` to the `HudId` enum beside `kHudCombatLog`.
- `game_view.{hpp,cpp}`: `int skill_scroll_ = 0;` + `ScrollSkillList(float wheel_y)` mirroring `combat_log_scroll_`/`ScrollCombatLog` (`game_view.cpp:965`); route the wheel in `SDL_EVENT_MOUSE_WHEEL` (`game_view.cpp:762`) — `kHudSkillList` scrolls cards, `kHudCombatLog` scrolls the log, anything else zooms. Reset `skill_scroll_ = 0` on selection change and clamp it when filling the model (`game_view.cpp:1410`).

- [ ] **Step 4: Run the tests, then look at it**

Run: `scripts/test.sh badlands_game_tests "[game_ui]"` → PASS.
Run: `scripts/screenshot.sh badlands_game /tmp/skills_panel.png` and open it. (A level-1 hero has no skills — verify visually after Task 10's scenario exists, or in the ai_sandbox.)

- [ ] **Step 5: Commit**

```bash
git add src/game/ui/hud.hpp src/game/ui/hud.cpp src/executables/game/game_view.hpp \
        src/executables/game/game_view.cpp game/tests/game_ui_tests.cpp
git commit -m "feat(ui): hero panel shows scrollable skill cards"
```

---

## Task 10: End-to-end + determinism

**Files:**
- Modify: `game/tests/shield_bash_tests.cpp`, `game/tests/determinism_tests.cpp`
- Read for patterns: `game/tests/duel_common.h`, `game/tests/rat_tests.cpp`

- [ ] **Step 1: Write the failing tests**

```cpp
// shield_bash_tests.cpp -- the whole loop host-side, no wasm needed
TEST_CASE("a level-3 mercenary bashes and the goblin stops thinking", "[skills][e2e]") {
    // MercenaryDesc + GoblinDesc adjacent on a flat world; award XP to level 3
    // so the grant fires; resolve_action(BL_ACT_USE_SKILL, goblin, 0); tick once:
    CHECK(has_status(reg, goblin, StatusKind::Stunned));
    const glm::vec2 before = reg.get<Position>(goblin).pos;
    for (int i = 0; i < 30; ++i) sim.Tick(1.0f / 30.0f);
    CHECK(glm::distance(reg.get<Position>(goblin).pos, before) == Catch::Approx(0.0f));
    for (int i = 0; i < 120; ++i) sim.Tick(1.0f / 30.0f);
    CHECK_FALSE(has_status(reg, goblin, StatusKind::Stunned));   // and it moves again
}

// determinism_tests.cpp -- follow the file's existing run-twice + replay shape
TEST_CASE("a run containing UseSkill is deterministic and replays", "[determinism]") {
    // Same bash scenario, 200 ticks twice -> identical snapshots; replay the
    // command log into a fresh Sim -> identical snapshots. The log carries a
    // CommandKindId::UseSkill record with a concrete target_id, never UINT32_MAX.
}
```

- [ ] **Step 2: Run, confirm failure**

Run: `scripts/test.sh badlands_game_tests "[e2e]"` → FAIL until the scenario helper exists.

- [ ] **Step 3: Implement the fixtures** in `shield_bash_tests.cpp` — not in `duel_common.h`, which the older duel suites share and which should not grow skill knowledge.

- [ ] **Step 4: Run everything**

```bash
scripts/test.sh
cargo test --manifest-path src/crates/brainhost/Cargo.toml --lib
```

- [ ] **Step 5: Watch it happen live**

```bash
perl -e 'alarm 30; exec @ARGV' ./build/badlands_ai_sandbox
```

Select a mercenary, confirm it reaches level 3 and lands bashes (the log shows `SkillUsed`/`StatusApplied`), and that a stunned goblin stands still and stops dodging.

- [ ] **Step 6: Commit**

```bash
git add game/tests/shield_bash_tests.cpp game/tests/determinism_tests.cpp
git commit -m "test(game): shield-bash end-to-end and UseSkill determinism/replay"
```

---

## Verification

1. `scripts/build.sh` → `BUILD OK`.
2. `scripts/test.sh` → green. Most likely to catch regressions: `[determinism]`, `[brain_abi]`, `[wasm_brain]`, `[intention]`, `[progression]`, `[game_ui]`, `[skill_effect]`.
3. `cargo test --manifest-path src/crates/brainhost/Cargo.toml --lib` → `real_hero_wasm_conforms` passes, proving the committed `hero.wasm` is genuinely v4 and not a stale artifact.
4. `cargo test --manifest-path src/crates/ui/Cargo.toml --lib` → unchanged (no ui-crate edits; run once to confirm that stayed true).
5. `scripts/screenshot.sh badlands_game /tmp/skills_panel.png` → cards render inside the 240 px panel without overflow.
6. `perl -e 'alarm 30; exec @ARGV' ./build/badlands_ai_sandbox` → a level-3 mercenary bashes; a stunned target stops moving, stops deciding, and takes hits it would normally dodge.
7. `git lfs ls-files | grep hero.wasm` → the rebuilt artifact is an LFS object, not a raw binary.

## Declared but not executed this slice

Each is a focused follow-up against a contract that already names it — the engine refuses them loudly today rather than approximating:

- `SkillTrigger::Passive` — needs an application hook (on-defend / on-hit) and a passive-bearing skill.
- `SkillTrigger::Intention` — `BL_INT_USE_SKILL` live, `IntentionKind::UseSkill`, duration tracking, and the abort rules for a focus interrupted by stun/damage/override.
- `SkillTargetMode::Multi` / `Point` — target collection by limit and by radius constant.
- `BL_FX_DAMAGE` — the op exists and `apply_effect_batch` applies it; no shipped effect emits one yet (shield-bash is pure control by design).
- `Calcify`'s own effect (documented no-op; it still displays and still grants at level 5).
- Live cooldown remaining on the card — the snapshot row carries skill ids only; showing it means widening `CharacterState`.
- Effects as wasm — `skill_abi.h` is shaped for it (flat POD in, flat ops out, version field); nothing loads a skill module yet.
