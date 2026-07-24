# XP, Levels & Skill Catalog (Slice 1) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Heroes accumulate integer XP (exploration discovery + even-split monster-kill spread), level up on a `floor(base * level^exp)` curve, and learn class skills from a data-driven grant table (Apprentice → Calcify @ 5), with skill trigger conditions + host recommendations defined and unit-tested.

**Architecture:** XP accrual and level-up are engine **system rules** (pure functions of the tick — no new Commands; replay reproduces them). Exploration XP is attributed at fog stamp time in `resolve_vision`; kill XP is spread by the death sweep. Skills are inert data this slice: a compiled catalog + grant table + a pure trigger evaluator whose output slice 2 (brain-owned combat, ABI v2) will copy into the wasm view. All tunables are runtime factors (`ProgressionFactors` in `SimFactors`, JSON-overridable, sanitized); per-monster XP is authored in `CreatureCatalog`.

**Tech Stack:** C++23, EnTT, glm, Catch2 (`badlands_game_tests`), nlohmann::json manifests, CMake/Ninja.

## Context

Approved in brainstorming (2026-07-24):
- **Slice 1 of 2.** Slice 2 (separate spec/plan) migrates hero combat authority into the Nim wasm brain (ABI v2) and implements Calcify's effect + `UseSkill` command. This slice must leave the trigger/recommendation seam ready but consumed only by tests.
- **Decisions:** kill XP **splits evenly, round UP** over heroes within `kill_xp_radius` of the corpse (euclidean, obstacles ignored, killer-independent, evaporates if nobody's near; comment marks the seam for future per-impact weighting). Exploration XP = `xp_per_texel × newly-discovered texels`, credited to the hero whose vision stamp first flips the texel (buildings/townfolk discover silently; first-stamper-wins via the existing deterministic order). **XP is int** (`int32_t`). Level curve `floor(level_base_xp * level^level_exponent)`, defaults 100/1.6 (matches the user's table: L1=100, L2=303, L4=918). Level-up grants skills per `(class, level)` table and emits `GameEventKind::HeroLeveledUp`. "It's about systems, not details" — structure over tuning values.
- **Key existing code:** death sweep `game/src/sim.cpp:327-336`; fog stamping `game/src/vision.cpp` (`stamp_rect_expanded`/`stamp_cone`, resolve order: buildings then player characters); factors sanitize boundary `game/src/sim.cpp:606-663` (`clamp_nonneg`, `warn_adjusted`); factors JSON `src/game/factors_manifest.cpp` (templated `ReadNum`, optional sections); creature JSON `src/game/creature_manifest.cpp` (float-only `ReadNum` — needs an int32 overload); catalog pattern `game/src/activity_catalog.cpp` (dense array + `static_assert` + name lookup); spawn recipe `game/src/heroes.cpp:83-205` (hero branch emplaces `HeroSimulationState`); snapshot fill `game/src/sim.cpp:417-493` (`characters_of`, designated init + post-`push_back` array fill for `name`); events via `emit_event` (`game/src/game_state.h:137`); tests use `sim_internal.hpp` internals (`make_world(BrainDesc{})`, `make_flat_world()`, `make_world(BrainDesc{}, WorldConfig{...})`, `spawn_into`, `tick_world`, `entity_for_slot`).

## Global Constraints

- CLAUDE.md working agreement: concise; no debug controls/knobs beyond what was asked; sim time is int64 ms; event-sourced mutations — but XP/level-up are **system rules**, deliberately NOT Commands (approved).
- Factors are initial config in the determinism contract; every new divisor/scale goes through `sanitize_factors`.
- `GameEventKind`, `SkillId`, grant table: **append-only** id spaces.
- Commit style: `feat(game): ...` / `test(game): ...`, ending with `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>`.
- Build/tests from repo root: `cmake --build build && ./build/badlands_game_tests '<filter>'`.
- **Known caveat (documented, approved scope):** `Sim::ResolveVision()` (the pre-first-tick render priming call) already mutates discovered history outside the tick contract; with stamp-time crediting it discovers texels *creditlessly*. Document this at its declaration; do not try to fix it this slice.

---

### Task 1: `ProgressionFactors` + sanitize + JSON manifest

**Files:**
- Modify: `game/include/badlands_sim.hpp` (after `MonsterFactors`, ~line 303; add member to `SimFactors`)
- Modify: `game/src/sim.cpp` (`sanitize_factors`, after the `MonsterFactors` block ~line 660)
- Modify: `src/game/factors_manifest.cpp` (new optional section, after `"monster"` ~line 242)
- Test: `game/tests/factors_sanitize_tests.cpp`, `src/game/tests/factors_manifest_tests.cpp`

**Interfaces:**
- Produces: `struct ProgressionFactors { int32_t xp_per_texel = 1; float kill_xp_radius = 10.0f; int32_t level_base_xp = 100; float level_exponent = 1.6f; }` as `SimFactors::progression`. Later tasks read `game.factors.progression`.

- [ ] **Step 1: Write the failing tests**

In `game/tests/factors_sanitize_tests.cpp`, following the file's existing pattern:

```cpp
TEST_CASE("progression factors sanitize at the set_factors_of boundary") {
    badlands::Sim sim{badlands::BrainDesc{}};
    badlands::SimFactors f;
    f.progression.xp_per_texel = -3;
    f.progression.kill_xp_radius = -1.0f;
    f.progression.level_base_xp = 0;
    f.progression.level_exponent = -2.0f;
    sim.SetFactors(f);
    const badlands::ProgressionFactors& p = sim.Factors().progression;
    CHECK(p.xp_per_texel == 0);
    CHECK(p.kill_xp_radius == 0.0f);
    CHECK(p.level_base_xp == 1);
    CHECK(p.level_exponent == 0.0f);
}
```

In `src/game/tests/factors_manifest_tests.cpp`, following its temp-file pattern:

```cpp
TEST_CASE("progression section parses; absent keys keep defaults") {
    // Use this file's existing write-temp-json helper/pattern.
    const std::string json = R"({"progression": {"xp_per_texel": 2, "level_base_xp": 50}})";
    // ... write to temp path, then:
    badlands::SimFactors f;
    REQUIRE(badlands::LoadSimFactors(path, f));
    CHECK(f.progression.xp_per_texel == 2);
    CHECK(f.progression.level_base_xp == 50);
    CHECK(f.progression.kill_xp_radius == 10.0f);   // untouched default
    CHECK(f.progression.level_exponent == 1.6f);
}
```

- [ ] **Step 2: Run to verify both fail** (no `progression` member → compile error is the failure)

Run: `cmake --build build 2>&1 | head -30`
Expected: compile errors on `f.progression`.

- [ ] **Step 3: Implement**

`badlands_sim.hpp` after `MonsterFactors`:

```cpp
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
```

Add `ProgressionFactors progression;` to `SimFactors` (after `monster`).

`sim.cpp` `sanitize_factors`, after the monster block:

```cpp
ProgressionFactors& p = f.progression;
clamp_nonneg("progression.xp_per_texel", p.xp_per_texel);
clamp_nonneg("progression.kill_xp_radius", p.kill_xp_radius);
clamp_nonneg("progression.level_exponent", p.level_exponent);
// The curve's scale: xp_to_next floors its result at 1 anyway, but a base
// below 1 collapses every threshold and the warn is the designer's signal.
if (p.level_base_xp < 1) {
    warn_adjusted("progression.level_base_xp", p.level_base_xp, 1);
    p.level_base_xp = 1;
}
```

`factors_manifest.cpp` after the `"monster"` section:

```cpp
if (!section("progression", s)) {
    return false;
}
if (s != nullptr) {
    ProgressionFactors& p = parsed.progression;
    if (!ReadNum(*s, "progression", "xp_per_texel", manifest_path, p.xp_per_texel) ||
        !ReadNum(*s, "progression", "kill_xp_radius", manifest_path, p.kill_xp_radius) ||
        !ReadNum(*s, "progression", "level_base_xp", manifest_path, p.level_base_xp) ||
        !ReadNum(*s, "progression", "level_exponent", manifest_path, p.level_exponent)) {
        return false;
    }
}
```

- [ ] **Step 4: Build + run both test binaries, verify pass**

Run: `cmake --build build && ./build/badlands_game_tests "progression factors*"` and the manifest-tests binary (find its target name via `grep -n factors_manifest_tests CMakeLists.txt`).
Expected: PASS.

- [ ] **Step 5: Commit** — `feat(game): add ProgressionFactors (sanitized, manifest-loadable)`

---

### Task 2: Skill vocabulary, catalog, grant table, `Skills` component

**Files:**
- Modify: `game/include/badlands_sim.hpp` (after the ActivityCatalog accessors, ~line 170)
- Create: `game/src/skills.h`, `game/src/skills.cpp`
- Modify: `game/src/components.h` (new `Skills` component, near `Attacks`)
- Modify: `game/src/heroes.cpp` (hero branch of `spawn_entity`, ~line 180)
- Modify: `CMakeLists.txt` (add `game/src/skills.cpp` to `badlands_game_lib` ~line 275; add `game/tests/skills_tests.cpp` to `badlands_game_tests` ~line 596)
- Test: `game/tests/skills_tests.cpp`

**Interfaces:**
- Produces (public, `badlands_sim.hpp`): `enum class SkillId : int32_t { Calcify = 0, Count }`, `kSkillCount`, `inline constexpr int32_t kMaxSkills = 8`, `const char* SkillName(int32_t id)` ("-" out of range).
- Produces (internal, `skills.h`): `enum class SkillTriggerKind : int32_t { MeleeThreatClose = 0, LowHealth }`; `struct SkillDef { SkillId id; const char* name; SkillTriggerKind trigger; float trigger_param; float cooldown; }`; `std::span<const SkillDef> SkillCatalog()`; `const SkillDef& SkillDefOf(SkillId)`; `struct SkillGrant { int32_t hero_class; int32_t level; SkillId skill; }`; `std::span<const SkillGrant> SkillGrantTable()`; `bool learn_skill(Skills&, SkillId)`; `void grant_skills_for_level(Skills&, int32_t hero_class, int32_t level)`.
- Produces (component, `components.h`): `struct Skills { SkillId ids[kMaxSkills]{}; float cooldown_remaining[kMaxSkills]{}; int32_t count = 0; }` — emplaced on every hero at spawn.

- [ ] **Step 1: Write the failing tests** (`game/tests/skills_tests.cpp`)

```cpp
#include "components.h"
#include "skills.h"

#include <catch_amalgamated.hpp>

using badlands::SkillId;
using badlands::Skills;

TEST_CASE("skill catalog is dense and named") {
    REQUIRE(badlands::SkillCatalog().size() == static_cast<size_t>(badlands::kSkillCount));
    CHECK(badlands::SkillCatalog()[0].id == SkillId::Calcify);
    CHECK(std::string(badlands::SkillName(0)) == "Calcify");
    CHECK(std::string(badlands::SkillName(-1)) == "-");
    CHECK(std::string(badlands::SkillName(badlands::kSkillCount)) == "-");
}

TEST_CASE("learn_skill is dupe-proof and bounded") {
    Skills s{};
    CHECK(badlands::learn_skill(s, SkillId::Calcify));
    CHECK_FALSE(badlands::learn_skill(s, SkillId::Calcify));
    REQUIRE(s.count == 1);
    CHECK(s.ids[0] == SkillId::Calcify);
    CHECK(s.cooldown_remaining[0] == 0.0f);
}

TEST_CASE("the grant table teaches the Apprentice Calcify at level 5") {
    Skills s{};
    badlands::grant_skills_for_level(s, badlands::HERO_APPRENTICE, 4);
    CHECK(s.count == 0);
    badlands::grant_skills_for_level(s, badlands::HERO_APPRENTICE, 5);
    REQUIRE(s.count == 1);
    CHECK(s.ids[0] == SkillId::Calcify);
    Skills merc{};
    badlands::grant_skills_for_level(merc, badlands::HERO_MERCENARY, 5);
    CHECK(merc.count == 0);
}
```

- [ ] **Step 2: Run to verify it fails** (compile error: no `skills.h`)

- [ ] **Step 3: Implement**

`badlands_sim.hpp` (after `ActivityName`, before the factors block):

```cpp
// ---- skills (identity only; defs/triggers live in game/src/skills.h) -------
// Append-only id space, same discipline as ActivityId.
enum class SkillId : int32_t {
    Calcify = 0,  // Apprentice: absorb the next physical strike (effect in slice 2)
    Count,
};
inline constexpr int32_t kSkillCount = static_cast<int32_t>(SkillId::Count);
// Fixed component/snapshot capacity (kMaxAttacks precedent).
inline constexpr int32_t kMaxSkills = 8;
// Stable inspection name ("Calcify"); "-" for an out-of-range id.
const char* SkillName(int32_t id);
```

`components.h` (after `Attacks`):

```cpp
// A hero's learned skills + per-skill cooldown state (POD fixed array, like
// Attacks). Granted by the level-up hook (progression.cpp) from
// SkillGrantTable; cooldowns start ready. INERT this slice: effects, cooldown
// ticking, and the UseSkill decision arrive with brain-owned combat (slice 2).
struct Skills {
    SkillId ids[kMaxSkills]{};
    float cooldown_remaining[kMaxSkills]{};
    int32_t count = 0;
};
```

`game/src/skills.h`:

```cpp
// The skill vocabulary tables: what skills exist (SkillCatalog), what
// condition recommends each one (SkillTriggerKind + trigger_param), and who
// learns what at which level (SkillGrantTable). Pure data + pure helpers,
// activity_catalog.cpp's pattern; consumers read the catalog instead of
// hardcoding a switch.

#pragma once

#include "badlands_sim.hpp"
#include "components.h"  // Skills

#include <span>

namespace badlands {

// The data-driven trigger vocabulary. Grows as skills need new conditions.
enum class SkillTriggerKind : int32_t {
    MeleeThreatClose = 0,  // a threat within trigger_param world units
    LowHealth,             // health fraction at or below trigger_param
};

struct SkillDef {
    SkillId id;
    const char* name;
    SkillTriggerKind trigger;
    float trigger_param;
    float cooldown;  // seconds between uses (ticked in slice 2)
};

// Dense, indexed by SkillId (static_assert-pinned).
std::span<const SkillDef> SkillCatalog();
// Out-of-range ids resolve to the Calcify row (id 0), mirroring ActivityInfoOf.
const SkillDef& SkillDefOf(SkillId id);

// One row of "class X learns skill Y at level L". Append-only.
struct SkillGrant {
    int32_t hero_class;  // HeroClassId
    int32_t level;
    SkillId skill;
};
std::span<const SkillGrant> SkillGrantTable();

// Dupe-proof append; false when already known or the component is full.
bool learn_skill(Skills& s, SkillId id);
// Applies every grant row matching (hero_class, level) exactly.
void grant_skills_for_level(Skills& s, int32_t hero_class, int32_t level);

}  // namespace badlands
```

`game/src/skills.cpp`:

```cpp
#include "skills.h"

#include <array>

namespace badlands {

namespace {

constexpr std::array<SkillDef, static_cast<size_t>(kSkillCount)> kSkills{{
    {SkillId::Calcify, "Calcify", SkillTriggerKind::MeleeThreatClose,
     /*trigger_param=*/3.0f, /*cooldown=*/20.0f},
}};

constexpr bool skills_dense() {
    for (size_t i = 0; i < kSkills.size(); ++i) {
        if (static_cast<int32_t>(kSkills[i].id) != static_cast<int32_t>(i)) {
            return false;
        }
    }
    return true;
}
static_assert(skills_dense(), "SkillCatalog must be indexed by SkillId");

constexpr std::array<SkillGrant, 1> kGrants{{
    {HERO_APPRENTICE, 5, SkillId::Calcify},
}};

}  // namespace

std::span<const SkillDef> SkillCatalog() { return kSkills; }

const SkillDef& SkillDefOf(SkillId id) {
    const int32_t i = static_cast<int32_t>(id);
    if (i < 0 || i >= kSkillCount) {
        return kSkills[0];
    }
    return kSkills[static_cast<size_t>(i)];
}

const char* SkillName(int32_t id) {
    if (id < 0 || id >= kSkillCount) {
        return "-";
    }
    return kSkills[static_cast<size_t>(id)].name;
}

std::span<const SkillGrant> SkillGrantTable() { return kGrants; }

bool learn_skill(Skills& s, SkillId id) {
    for (int32_t i = 0; i < s.count; ++i) {
        if (s.ids[i] == id) {
            return false;
        }
    }
    if (s.count >= kMaxSkills) {
        return false;
    }
    s.ids[s.count] = id;
    s.cooldown_remaining[s.count] = 0.0f;
    ++s.count;
    return true;
}

void grant_skills_for_level(Skills& s, int32_t hero_class, int32_t level) {
    for (const SkillGrant& g : SkillGrantTable()) {
        if (g.hero_class == hero_class && g.level == level) {
            learn_skill(s, g.skill);
        }
    }
}

}  // namespace badlands
```

`heroes.cpp` hero branch (after the `EntityMemory` emplace, before `break`), plus `#include "skills.h"`:

```cpp
// Learned-skill loadout: starts with the class's level-1 grants (none
// authored yet); the level-up hook (progression.cpp) adds the rest.
Skills sk{};
grant_skills_for_level(sk, hero_class, 1);
reg.emplace<Skills>(e, sk);
```

CMake: add `game/src/skills.cpp` next to `game/src/heroes.cpp` in `badlands_game_lib`; add `game/tests/skills_tests.cpp` to `badlands_game_tests`.

- [ ] **Step 4: Build + run** `./build/badlands_game_tests "*skill*" "*grant*"` — PASS; full suite still green.

- [ ] **Step 5: Commit** — `feat(game): skill catalog, grant table, Skills component`

---

### Task 3: Progression core — level/xp fields, `award_xp`, `HeroLeveledUp`

**Files:**
- Modify: `game/src/components.h` (`HeroSimulationState`, ~line 139)
- Modify: `game/include/badlands_sim.hpp` (`GameEventKind`, ~line 652)
- Create: `game/src/progression.h`, `game/src/progression.cpp`
- Modify: `CMakeLists.txt` (add `game/src/progression.cpp` to lib; `game/tests/progression_tests.cpp` to tests)
- Test: `game/tests/progression_tests.cpp`

**Interfaces:**
- Consumes: Task 1 `ProgressionFactors`; Task 2 `grant_skills_for_level`, `Skills`.
- Produces: `HeroSimulationState.level` (`int32_t`, = 1) and `.xp` (`int32_t`, = 0, progress toward next level); `GameEventKind::HeroLeveledUp` (appended after `HeroDied`); `int32_t xp_to_next(const ProgressionFactors&, int32_t level)`; `void award_xp(BadlandsGame&, uint32_t slot, int32_t amount)`; `struct PendingKillXp { glm::vec2 pos; int32_t amount; }`; `void spread_kill_xp(BadlandsGame&, const std::vector<PendingKillXp>&)` (body in Task 4 uses it; declare + implement both here).

- [ ] **Step 1: Write the failing tests** (`game/tests/progression_tests.cpp`)

```cpp
#include "badlands_sim.hpp"
#include "components.h"
#include "game_state.h"
#include "progression.h"
#include "sim_internal.hpp"

#include <catch_amalgamated.hpp>

using badlands::BrainDesc;
using badlands::GameEvent;
using badlands::GameEventKind;
using badlands::ProgressionFactors;
using badlands::SkillId;

TEST_CASE("xp_to_next follows floor(base * level^exponent)") {
    ProgressionFactors p;  // defaults 100 / 1.6 (the design table)
    CHECK(badlands::xp_to_next(p, 1) == 100);
    CHECK(badlands::xp_to_next(p, 2) == 303);
    CHECK(badlands::xp_to_next(p, 3) == 579);
    CHECK(badlands::xp_to_next(p, 4) == 918);
}

TEST_CASE("award_xp levels up, grants class skills, emits one event per level") {
    auto owned = badlands::make_world(BrainDesc{});
    BadlandsGame& g = *owned;
    const uint32_t slot =
        badlands::spawn_into(g, badlands::MercenaryDesc(0.0f, 20.0f));
    entt::entity e = badlands::entity_for_slot(g, static_cast<int32_t>(slot));
    // Homeless spawn has class -1; the grant table keys on class, so set it.
    g.registry.get<badlands::HeroCharacter>(e).hero_class = badlands::HERO_APPRENTICE;
    auto& sim = g.registry.get<badlands::HeroSimulationState>(e);
    REQUIRE(sim.level == 1);
    REQUIRE(sim.xp == 0);

    badlands::award_xp(g, slot, 150);   // crosses L1 (cost 100), 50 left over
    CHECK(sim.level == 2);
    CHECK(sim.xp == 50);

    badlands::award_xp(g, slot, 1750);  // 303 + 579 + 918 - 50: lands exactly at L5
    CHECK(sim.level == 5);
    CHECK(sim.xp == 0);

    const auto& sk = g.registry.get<badlands::Skills>(e);
    REQUIRE(sk.count == 1);
    CHECK(sk.ids[0] == SkillId::Calcify);

    int leveled = 0;
    for (const GameEvent& ev : g.events) {
        if (ev.kind == GameEventKind::HeroLeveledUp && ev.actor_id == slot) {
            ++leveled;
        }
    }
    CHECK(leveled == 4);  // 2, 3, 4, 5
}

TEST_CASE("award_xp is a no-op for non-heroes and non-positive amounts") {
    auto owned = badlands::make_world(BrainDesc{});
    BadlandsGame& g = *owned;
    const uint32_t rat = badlands::spawn_creature_into(
        g, badlands::CreatureId::Rat, 1, {0.0f, 0.0f});
    badlands::award_xp(g, rat, 100);          // not a hero: ignored
    badlands::award_xp(g, 9999u, 100);        // invalid slot: ignored
    const uint32_t h = badlands::spawn_into(g, badlands::MercenaryDesc(0.0f, 20.0f));
    badlands::award_xp(g, h, 0);
    badlands::award_xp(g, h, -5);
    entt::entity e = badlands::entity_for_slot(g, static_cast<int32_t>(h));
    CHECK(g.registry.get<badlands::HeroSimulationState>(e).xp == 0);
}
```

- [ ] **Step 2: Run to verify failure** (no `progression.h`).

- [ ] **Step 3: Implement**

`components.h` — add to `HeroSimulationState` (after `home_building_id`), and extend its doc comment with one line (`level/xp: hero progression; xp counts toward the NEXT level`):

```cpp
    int32_t level = 1;
    int32_t xp = 0;
```

`badlands_sim.hpp` — append to `GameEventKind`:

```cpp
    HeroLeveledUp,  // a hero crossed a level threshold; actor = hero slot, amount = new level
```

`game/src/progression.h`:

```cpp
// Hero progression: the leveling curve and the XP accrual entry points.
// SYSTEM RULES, deliberately not Commands: XP is a pure function of what the
// tick already did (kills applied, texels discovered), so a replayed log
// reproduces it exactly; logging it would re-state the tick, not a decision.

#pragma once

#include "badlands_sim.hpp"  // ProgressionFactors

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

struct BadlandsGame;

namespace badlands {

// XP needed to advance FROM `level`: floor(base * level^exponent), never
// below 1 (sanitize keeps base >= 1, exponent >= 0). Saturates to INT32_MAX
// if the curve runs past int range (an effective level cap, not an overflow).
int32_t xp_to_next(const ProgressionFactors& p, int32_t level);

// Adds XP to a hero (no-op for invalid slots, non-heroes, amount <= 0),
// looping level-ups: each crossing subtracts the cost, bumps level, grants
// that level's class skills (SkillGrantTable) and emits HeroLeveledUp.
void award_xp(BadlandsGame& game, uint32_t slot, int32_t amount);

// One dead entity's XP payout, collected by the death sweep BEFORE the
// destroys (Position and XpReward die with the entity).
struct PendingKillXp {
    glm::vec2 pos;
    int32_t amount;
};

// Splits each payout evenly (round up) over heroes within
// progression.kill_xp_radius of the corpse -- euclidean, obstacles ignored,
// heroes hidden inside buildings excluded. Nobody in range -> the XP
// evaporates. Even split is the v1 rule; per-impact weighting (damage done,
// tanking, support) replaces the `share` computation here later.
void spread_kill_xp(BadlandsGame& game, const std::vector<PendingKillXp>& payouts);

}  // namespace badlands
```

`game/src/progression.cpp`:

```cpp
#include "progression.h"

#include "components.h"
#include "game_state.h"
#include "skills.h"

#include <cmath>

namespace badlands {

int32_t xp_to_next(const ProgressionFactors& p, int32_t level) {
    const double cost = std::floor(static_cast<double>(p.level_base_xp) *
                                   std::pow(static_cast<double>(level),
                                            static_cast<double>(p.level_exponent)));
    if (cost >= 2.0e9) {
        return INT32_MAX;
    }
    return std::max(1, static_cast<int32_t>(cost));
}

void award_xp(BadlandsGame& game, uint32_t slot, int32_t amount) {
    if (amount <= 0) {
        return;
    }
    entt::entity e = entity_for_slot(game, static_cast<int32_t>(slot));
    if (e == entt::null || !game.registry.all_of<HeroSimulationState>(e)) {
        return;
    }
    auto& sim = game.registry.get<HeroSimulationState>(e);
    sim.xp += amount;
    while (sim.xp >= xp_to_next(game.factors.progression, sim.level)) {
        sim.xp -= xp_to_next(game.factors.progression, sim.level);
        ++sim.level;
        if (const auto* hero = game.registry.try_get<HeroCharacter>(e);
            hero != nullptr && game.registry.all_of<Skills>(e)) {
            grant_skills_for_level(game.registry.get<Skills>(e), hero->hero_class,
                                   sim.level);
        }
        const glm::vec2 pos = game.registry.get<Position>(e).pos;
        emit_event(game, GameEvent{.kind = GameEventKind::HeroLeveledUp,
                                   .actor_id = slot,
                                   .target_id = UINT32_MAX,
                                   .target_kind = kEventTargetCharacter,
                                   .amount = static_cast<float>(sim.level),
                                   .x = pos.x,
                                   .z = pos.y,
                                   .at_millis = game.world_millis});
    }
}

void spread_kill_xp(BadlandsGame& game, const std::vector<PendingKillXp>& payouts) {
    const float radius = game.factors.progression.kill_xp_radius;
    const float r2 = radius * radius;
    std::vector<uint32_t> near;
    for (const PendingKillXp& p : payouts) {
        if (p.amount <= 0) {
            continue;
        }
        near.clear();
        // Slot order, not view order: award (and event) order is stable.
        for (uint32_t slot = 0; slot < game.slots.size(); ++slot) {
            entt::entity e = game.slots[slot];
            if (!game.registry.valid(e) ||
                !game.registry.all_of<HeroSimulationState, Position>(e) ||
                game.registry.all_of<InsideBuilding>(e)) {
                continue;
            }
            const glm::vec2 d = game.registry.get<Position>(e).pos - p.pos;
            if (glm::dot(d, d) <= r2) {
                near.push_back(slot);
            }
        }
        if (near.empty()) {
            continue;  // nobody close enough: the XP evaporates
        }
        const int32_t n = static_cast<int32_t>(near.size());
        const int32_t share = (p.amount + n - 1) / n;  // even split, round UP
        for (uint32_t slot : near) {
            award_xp(game, slot, share);
        }
    }
}

}  // namespace badlands
```

CMake: add `game/src/progression.cpp` to `badlands_game_lib`, `game/tests/progression_tests.cpp` to `badlands_game_tests`.

- [ ] **Step 4: Build + run** `./build/badlands_game_tests "*xp*" "*award*"` — PASS; full suite green.

- [ ] **Step 5: Commit** — `feat(game): int XP + leveling curve + HeroLeveledUp grants (award_xp)`

---

### Task 4: Kill XP — `xp_reward` authoring + death-sweep spread

**Files:**
- Modify: `game/include/badlands_sim.hpp` (`CharacterDesc`, end of struct ~line 388)
- Modify: `game/src/components.h` (new `XpReward` component)
- Modify: `game/src/creature_catalog.cpp` (Rat ~line 118, Goblin ~line 131)
- Modify: `src/game/creature_manifest.cpp` (int32 `ReadNum` overload + `xp_reward` key ~line 66)
- Modify: `game/src/heroes.cpp` (`spawn_entity` generic section, after the `Attacks` emplace)
- Modify: `game/src/sim.cpp` (death sweep ~line 327)
- Test: `game/tests/progression_tests.cpp`

**Interfaces:**
- Consumes: Task 3 `spread_kill_xp`, `PendingKillXp`.
- Produces: `CharacterDesc.xp_reward` (`int32_t`, = 0; Rat 10, Goblin 25, Deer stays 0); component `struct XpReward { int32_t amount; }` (present only when authored > 0); creature-JSON key `"xp_reward"`.

- [ ] **Step 1: Write the failing tests** (append to `progression_tests.cpp`)

```cpp
namespace {
constexpr float kTickDt = 1.0f / 30.0f;

int32_t xp_of(BadlandsGame& g, uint32_t slot) {
    entt::entity e = badlands::entity_for_slot(g, static_cast<int32_t>(slot));
    return g.registry.get<badlands::HeroSimulationState>(e).xp;
}
}  // namespace

TEST_CASE("a monster's xp_reward splits evenly (round up) over nearby heroes") {
    auto owned = badlands::make_flat_world();
    BadlandsGame& g = *owned;
    badlands::CharacterDesc hero = badlands::MercenaryDesc(0.0f, 0.0f);
    const uint32_t h1 = badlands::spawn_into(g, hero);
    hero.pos_x = 2.0f;
    const uint32_t h2 = badlands::spawn_into(g, hero);
    hero.pos_x = 50.0f;  // far outside the default 10u radius
    const uint32_t far = badlands::spawn_into(g, hero);

    const uint32_t rat =
        badlands::spawn_creature_into(g, badlands::CreatureId::Rat, 1, {1.0f, 0.0f});
    g.registry.get<badlands::Health>(
        badlands::entity_for_slot(g, static_cast<int32_t>(rat))).hp = 0.0f;
    badlands::tick_world(g, kTickDt);  // death sweep spreads the reward

    CHECK(xp_of(g, h1) == 5);  // ceil(10 / 2)
    CHECK(xp_of(g, h2) == 5);
    CHECK(xp_of(g, far) == 0);
}

TEST_CASE("heroes hidden inside buildings get no kill XP; alone gets it all") {
    auto owned = badlands::make_flat_world();
    BadlandsGame& g = *owned;
    badlands::CharacterDesc hero = badlands::MercenaryDesc(0.0f, 0.0f);
    const uint32_t outside = badlands::spawn_into(g, hero);
    hero.pos_x = 1.0f;
    const uint32_t hidden = badlands::spawn_into(g, hero);
    g.registry.emplace<badlands::InsideBuilding>(
        badlands::entity_for_slot(g, static_cast<int32_t>(hidden)), 0, 0);

    const uint32_t rat =
        badlands::spawn_creature_into(g, badlands::CreatureId::Rat, 1, {0.5f, 0.0f});
    g.registry.get<badlands::Health>(
        badlands::entity_for_slot(g, static_cast<int32_t>(rat))).hp = 0.0f;
    badlands::tick_world(g, kTickDt);

    CHECK(xp_of(g, outside) == 10);  // whole reward: the only eligible hero
    CHECK(xp_of(g, hidden) == 0);
}
```

- [ ] **Step 2: Run to verify failure** (heroes get 0 XP: nothing spreads yet — `xp_reward` doesn't exist, so first a compile failure on nothing; the tests fail at the `CHECK`s once it compiles).

- [ ] **Step 3: Implement**

`badlands_sim.hpp` — append to `CharacterDesc` (after `attack_count`):

```cpp
    // XP paid out on this creature's death, split over nearby heroes (see
    // ProgressionFactors.kill_xp_radius). 0 = no reward (deer, heroes, dummies).
    int32_t xp_reward = 0;
```

`components.h` (near `XpReward`'s consumers — after `Skills`):

```cpp
// Present only on entities whose death pays XP (CharacterDesc.xp_reward > 0).
// Read by the death sweep (sim.cpp), which collects the payout before the
// destroy and spreads it via spread_kill_xp (progression.h).
struct XpReward {
    int32_t amount;
};
```

`creature_catalog.cpp`: in the Rat block add `d.xp_reward = 10;`, in the Goblin block `d.xp_reward = 25;` (match surrounding style).

`creature_manifest.cpp`: add an `int32_t` overload mirroring the `float` `ReadNum` (same shape, `int32_t& dst`), then in the per-creature reads add:

```cpp
ok = ok && ReadNum(o, name.c_str(), "xp_reward", d.xp_reward);
```

`heroes.cpp` `spawn_entity`, generic section (after the `Attacks` emplace):

```cpp
if (desc.xp_reward > 0) {
    reg.emplace<XpReward>(e, desc.xp_reward);
}
```

`sim.cpp` — replace the death sweep (add `#include "progression.h"`):

```cpp
    // Death. Collect each dead entity's XP payout BEFORE the destroys
    // (Position/XpReward die with it), spread AFTER them so a hero that died
    // this tick neither blocks nor receives a share.
    std::vector<entt::entity> dead;
    std::vector<PendingKillXp> kill_xp;
    for (auto [e, health] : registry.view<const Health>().each()) {
        if (health.hp <= 0.0f) {
            dead.push_back(e);
            if (const auto* reward = registry.try_get<XpReward>(e);
                reward != nullptr && registry.all_of<Position>(e)) {
                kill_xp.push_back({registry.get<Position>(e).pos, reward->amount});
            }
        }
    }
    for (entt::entity e : dead) {
        registry.destroy(e);
    }
    spread_kill_xp(g, kill_xp);
```

- [ ] **Step 4: Build + run** `./build/badlands_game_tests "*kill*"` — PASS; full suite green (watch `determinism_tests`, `rat_tests`, `combat_tests`).

- [ ] **Step 5: Commit** — `feat(game): per-creature xp_reward spread evenly by the death sweep`

---

### Task 5: Exploration XP — stamp-time discovery crediting

**Files:**
- Modify: `game/src/vision.h` (`DiscoveryCredit`, `resolve_vision` signature)
- Modify: `game/src/vision.cpp` (stamp helpers count + set discovered; character loop credits heroes; drop the now-redundant final OR pass)
- Modify: `game/src/sim.cpp` (tick_world's `resolve_vision` call ~line 340; `Sim::ResolveVision` doc caveat)
- Modify: `game/include/badlands_sim.hpp` (`Sim::ResolveVision` comment: add the creditless-priming caveat)
- Test: `game/tests/progression_tests.cpp`

**Interfaces:**
- Consumes: Task 3 `award_xp`; Task 1 `progression.xp_per_texel`.
- Produces: `struct DiscoveryCredit { uint32_t slot; int32_t texels; }` (in `vision.h`); `void resolve_vision(BadlandsGame&, std::vector<DiscoveryCredit>* credits = nullptr)`.

- [ ] **Step 1: Write the failing tests** (append to `progression_tests.cpp`; add `#include "vision.h"`)

```cpp
namespace {
int discovered_texels(const badlands::VisionGrid& vg) {
    int n = 0;
    for (int k = 0; k < vg.nx * vg.nz; ++k) {
        n += vg.front[2 * k] ? 1 : 0;
    }
    return n;
}
}  // namespace

TEST_CASE("newly discovered texels award xp_per_texel to the discovering hero") {
    // Empty flat world: no colony, so every discovered texel is hero-stamped.
    auto owned = badlands::make_world(
        badlands::BrainDesc{},
        badlands::WorldConfig{.prebuild_colony = false, .terrain_blocking = false});
    BadlandsGame& g = *owned;
    badlands::configure_vision(g.vision, -32.0f, -32.0f, 64.0f, 64.0f, 1.0f);

    badlands::CharacterDesc d = badlands::MercenaryDesc(0.0f, 0.0f);
    d.vision_radius = 6.0f;
    d.vision_cone_half_angle_deg = 180.0f;  // full circle
    const uint32_t slot = badlands::spawn_into(g, d);

    badlands::tick_world(g, kTickDt);
    const int total = discovered_texels(g.vision);
    REQUIRE(total > 0);
    CHECK(xp_of(g, slot) == total * g.factors.progression.xp_per_texel);

    // The invariant holds tick over tick (the hero may roam and reveal more):
    badlands::tick_world(g, kTickDt);
    CHECK(xp_of(g, slot) ==
          discovered_texels(g.vision) * g.factors.progression.xp_per_texel);
}

TEST_CASE("overlapping discoveries are credited exactly once (union, no double)") {
    auto owned = badlands::make_world(
        badlands::BrainDesc{},
        badlands::WorldConfig{.prebuild_colony = false, .terrain_blocking = false});
    BadlandsGame& g = *owned;
    badlands::configure_vision(g.vision, -32.0f, -32.0f, 64.0f, 64.0f, 1.0f);

    badlands::CharacterDesc d = badlands::MercenaryDesc(0.0f, 0.0f);
    d.vision_radius = 6.0f;
    d.vision_cone_half_angle_deg = 180.0f;
    const uint32_t a = badlands::spawn_into(g, d);
    d.pos_x = 4.0f;  // overlapping circles
    const uint32_t b = badlands::spawn_into(g, d);

    badlands::tick_world(g, kTickDt);
    CHECK(xp_of(g, a) + xp_of(g, b) == discovered_texels(g.vision));
    CHECK(xp_of(g, a) > 0);
    CHECK(xp_of(g, b) > 0);
}
```

- [ ] **Step 2: Run to verify failure** (`xp` stays 0: no crediting yet).

- [ ] **Step 3: Implement**

`vision.h`: add above `resolve_vision`, and change its signature:

```cpp
// One vision source's newly-discovered texel count this resolve -- the
// exploration-XP input. Only heroes are credited; buildings and non-hero
// characters discover silently. First stamper wins a texel (the resolve's
// deterministic source order breaks same-tick ties).
struct DiscoveryCredit {
    uint32_t slot;
    int32_t texels;
};

// Resolve the next visibility field from the world's player vision sources and
// publish it (swap). No-op when the grid is unconfigured. Call once per tick.
// When `credits` is non-null it is filled with per-hero discovery counts.
void resolve_vision(BadlandsGame& game, std::vector<DiscoveryCredit>* credits = nullptr);
```

`vision.cpp`: make both stamp helpers return the count of texels whose discovered bit THEY set, writing discovered alongside visible. The write inside each loop becomes:

```cpp
const size_t k2 = 2 * texel_k(g, i, j);
g.back[k2 + 1] = 255;
if (!g.back[k2]) {
    g.back[k2] = 255;
    ++fresh;
}
```

(`int fresh = 0;` at the top, `return fresh;` at the bottom of `stamp_rect_expanded` and `stamp_cone`; the own-texel write at the end of `stamp_cone` uses the same pattern.)

In `resolve_vision`:
- signature per `vision.h`; building loop ignores the return value;
- character loop becomes:

```cpp
    for (auto [e, pos, team, vis, facing] :
         game.registry
             .view<const Position, const Team, const Vision, const Facing>(
                 entt::exclude<InsideBuilding>)
             .each()) {
        if (team.id != kPlayerTeam || vis.radius <= 0.0f) continue;
        const int fresh = stamp_cone(g, pos.pos, facing.dir, vis.radius, vis.cone_half_cos);
        if (credits != nullptr && fresh > 0 &&
            game.registry.all_of<HeroSimulationState>(e)) {
            credits->push_back({slot_for_entity(game, e), fresh});
        }
    }
```

- delete the final "Discovered is cumulative" OR loop — every visible write now sets discovered at the stamp, so the pass is dead; leave a one-line comment stating the invariant (`visible ⊆ discovered, maintained by the stamps`).

`sim.cpp` `tick_world` (~line 338):

```cpp
    // Fog-of-war: resolve next visibility from the post-movement world state and
    // publish it. Newly-discovered texels credit the stamping hero with
    // exploration XP -- a system rule, applied here so it lands in the same tick.
    std::vector<DiscoveryCredit> discoveries;
    resolve_vision(g, &discoveries);
    if (g.factors.progression.xp_per_texel > 0) {
        for (const DiscoveryCredit& d : discoveries) {
            award_xp(g, d.slot, d.texels * g.factors.progression.xp_per_texel);
        }
    }
```

`Sim::ResolveVision` (public header comment + impl untouched apart from the note): append to its doc comment:

```
// NB: priming discovers texels CREDITLESSLY (no exploration XP) -- it is a
// presentation affordance outside the tick/determinism contract.
```

- [ ] **Step 4: Build + run** `./build/badlands_game_tests "*discover*"` — PASS. Then the fog/vision suites: `./build/badlands_game_tests "*fog*" "*vision*" "*explor*"` and full suite (determinism's "fog of war and explorers replays exactly" is the key gate).

- [ ] **Step 5: Commit** — `feat(game): exploration XP via stamp-time discovery crediting`

---

### Task 6: Trigger evaluator (`SkillContext` → recommendations)

**Files:**
- Modify: `game/src/skills.h` / `game/src/skills.cpp`
- Test: `game/tests/skills_tests.cpp`

**Interfaces:**
- Consumes: `PerceivedThreat` (`game/src/behaviours/world_view.h`, nearest-first contract), Task 2 `Skills`/`SkillDefOf`.
- Produces: `struct SkillContext { float health_frac = 1.0f; const PerceivedThreat* threats = nullptr; int32_t threat_count = 0; }`; `struct SkillRecommendation { SkillId id; bool ready; bool recommended; }`; `int32_t evaluate_skill_triggers(const Skills&, const SkillContext&, SkillRecommendation out[kMaxSkills])` (returns count filled). **This is the seam slice 2 packs into the ABI view** — the brain gets available skills + host trigger recommendations and makes the final call.

- [ ] **Step 1: Write the failing tests** (append to `skills_tests.cpp`; add `#include "behaviours/world_view.h"`)

```cpp
TEST_CASE("Calcify recommends on a close melee threat, gated by cooldown") {
    Skills s{};
    badlands::learn_skill(s, SkillId::Calcify);
    badlands::PerceivedThreat threats[1] = {{{1.0f, 0.0f}, 2.5f, 7u}};
    badlands::SkillContext ctx{1.0f, threats, 1};
    badlands::SkillRecommendation rec[badlands::kMaxSkills];

    REQUIRE(badlands::evaluate_skill_triggers(s, ctx, rec) == 1);
    CHECK(rec[0].id == SkillId::Calcify);
    CHECK(rec[0].ready);
    CHECK(rec[0].recommended);  // threat at 2.5 <= trigger_param 3.0

    threats[0].dist = 5.0f;  // nearest threat too far
    badlands::evaluate_skill_triggers(s, ctx, rec);
    CHECK_FALSE(rec[0].recommended);

    threats[0].dist = 2.5f;
    s.cooldown_remaining[0] = 5.0f;  // on cooldown: still recommended, not ready
    badlands::evaluate_skill_triggers(s, ctx, rec);
    CHECK(rec[0].recommended);
    CHECK_FALSE(rec[0].ready);

    badlands::SkillContext no_threats{1.0f, nullptr, 0};
    badlands::evaluate_skill_triggers(s, no_threats, rec);
    CHECK_FALSE(rec[0].recommended);
}
```

- [ ] **Step 2: Run to verify failure** (no `SkillContext`).

- [ ] **Step 3: Implement**

`skills.h` (add `#include "behaviours/world_view.h"` and, at the end of the namespace):

```cpp
// What the host tells a brain about each learned skill: `ready` = off
// cooldown, `recommended` = the skill's trigger condition currently holds.
// ADVICE, not a command -- slice 2 copies this into the wasm view and the
// brain makes the final call. Pure over its inputs (unit-testable; identical
// live and on replay).
struct SkillContext {
    float health_frac = 1.0f;
    const PerceivedThreat* threats = nullptr;  // nearest-first (WorldView contract)
    int32_t threat_count = 0;
};

struct SkillRecommendation {
    SkillId id;
    bool ready;
    bool recommended;
};

// Fills out[0 .. Skills.count); returns Skills.count.
int32_t evaluate_skill_triggers(const Skills& s, const SkillContext& ctx,
                                SkillRecommendation out[kMaxSkills]);
```

`skills.cpp`:

```cpp
int32_t evaluate_skill_triggers(const Skills& s, const SkillContext& ctx,
                                SkillRecommendation out[kMaxSkills]) {
    for (int32_t i = 0; i < s.count; ++i) {
        const SkillDef& def = SkillDefOf(s.ids[i]);
        bool recommended = false;
        switch (def.trigger) {
            case SkillTriggerKind::MeleeThreatClose:
                // threats are nearest-first, so [0] decides "anything close?".
                recommended = ctx.threat_count > 0 && ctx.threats != nullptr &&
                              ctx.threats[0].dist <= def.trigger_param;
                break;
            case SkillTriggerKind::LowHealth:
                recommended = ctx.health_frac <= def.trigger_param;
                break;
        }
        out[i] = {s.ids[i], s.cooldown_remaining[i] <= 0.0f, recommended};
    }
    return s.count;
}
```

- [ ] **Step 4: Build + run** `./build/badlands_game_tests "*Calcify*"` — PASS.

- [ ] **Step 5: Commit** — `feat(game): skill trigger evaluator (host recommendations seam for the brain)`

---

### Task 7: Snapshot + determinism strengthening + inspector readout

**Files:**
- Modify: `game/include/badlands_sim.hpp` (`CharacterState`, append at end ~line 475)
- Modify: `game/src/sim.cpp` (`characters_of` ~line 453; add `#include "progression.h"` — already there from Task 4, and `#include "skills.h"`)
- Modify: `game/tests/determinism_tests.cpp` (`require_same` ~line 78)
- Modify: `src/executables/ai_sandbox/ai_sandbox_view.cpp` (hero needs cell ~line 723)
- Test: `game/tests/progression_tests.cpp`

**Interfaces:**
- Consumes: Task 3 fields + `xp_to_next`; Task 2 `Skills`, `SkillName`.
- Produces: `CharacterState` gains `int32_t level, xp, xp_next, skill_count; int32_t skills[kMaxSkills];` — zeroed for non-heroes (`level >= 1` identifies a hero row).

- [ ] **Step 1: Write the failing test** (append to `progression_tests.cpp`)

```cpp
TEST_CASE("the snapshot carries level/xp/skills; zeroed for non-heroes") {
    auto owned = badlands::make_world(badlands::BrainDesc{});
    BadlandsGame& g = *owned;
    const uint32_t h = badlands::spawn_into(g, badlands::MercenaryDesc(0.0f, 20.0f));
    entt::entity e = badlands::entity_for_slot(g, static_cast<int32_t>(h));
    g.registry.get<badlands::HeroCharacter>(e).hero_class = badlands::HERO_APPRENTICE;
    badlands::award_xp(g, h, 2000);  // past level 5: Calcify granted
    const uint32_t rat =
        badlands::spawn_creature_into(g, badlands::CreatureId::Rat, 1, {5.0f, 0.0f});

    const auto rows = badlands::characters_of(g);
    const badlands::CharacterState* hero_row = nullptr;
    const badlands::CharacterState* rat_row = nullptr;
    for (const auto& r : rows) {
        if (r.id == h) hero_row = &r;
        if (r.id == rat) rat_row = &r;
    }
    REQUIRE(hero_row != nullptr);
    REQUIRE(rat_row != nullptr);
    CHECK(hero_row->level == 5);
    CHECK(hero_row->xp == 100);  // 2000 - 1900 (100+303+579+918)
    CHECK(hero_row->xp_next ==
          badlands::xp_to_next(g.factors.progression, hero_row->level));
    REQUIRE(hero_row->skill_count == 1);
    CHECK(hero_row->skills[0] == static_cast<int32_t>(SkillId::Calcify));
    CHECK(rat_row->level == 0);
    CHECK(rat_row->skill_count == 0);
}
```

- [ ] **Step 2: Run to verify failure** (no `CharacterState::level`).

- [ ] **Step 3: Implement**

`badlands_sim.hpp` — append to `CharacterState` (after `vision_cone_half_angle_deg`):

```cpp
    // --- hero progression (zeroed for non-heroes; level >= 1 marks a hero) --
    int32_t level;
    int32_t xp;          // progress toward the next level
    int32_t xp_next;     // cost of the next level at current factors
    int32_t skill_count;
    int32_t skills[kMaxSkills];  // SkillId values; only [0, skill_count) valid
```

`sim.cpp` `characters_of` — extend the designated initializer:

```cpp
            .level = sim ? sim->level : 0,
            .xp = sim ? sim->xp : 0,
            .xp_next = sim ? xp_to_next(g.factors.progression, sim->level) : 0,
```

and fill the array after `push_back` (next to the `name` copy):

```cpp
        const Skills* sk = g.registry.try_get<Skills>(e);
        CharacterState& row = out.back();
        row.skill_count = sk ? sk->count : 0;
        for (int32_t i = 0; i < kMaxSkills; ++i) {
            row.skills[i] =
                (sk != nullptr && i < sk->count) ? static_cast<int32_t>(sk->ids[i]) : 0;
        }
```

`determinism_tests.cpp` `require_same` — add:

```cpp
        CHECK(x.level == y.level);
        CHECK(x.xp == y.xp);
```

`ai_sandbox_view.cpp` — extend the existing hero needs cell (the `ImGui::Text("f%.2f c%.2f", ...)` branch):

```cpp
        ImGui::Text("L%d %d/%d  f%.2f c%.2f", c.level, c.xp, c.xp_next,
                    c.fatigue, c.content);
        for (int32_t i = 0; i < c.skill_count; ++i) {
            ImGui::SameLine();
            ImGui::TextUnformatted(badlands::SkillName(c.skills[i]));
        }
```

- [ ] **Step 4: Full verification (see below)** — everything green.

- [ ] **Step 5: Commit** — `feat(game): expose level/xp/skills in the snapshot + sandbox inspector`

---

## Verification (end of plan)

1. `cmake --build build` — clean build (lib + all apps + tests).
2. `ctest --test-dir build` (or `./build/badlands_game_tests` directly) — full suite green; the determinism suite (run-twice, replay, fog-of-war explorer replay) is the load-bearing gate for the new system rules.
3. Headless smoke: `perl -e 'alarm 30; exec @ARGV' ./build/badlands_ai_sandbox` from the repo root — runs and exits by alarm without crashing; optionally `./build/badlands_game --screenshot out.png`.
4. Interactive spot-check (optional): run `./build/badlands_ai_sandbox`, watch the inspector's `L1 0/100` climb as heroes explore; kill XP visible when heroes fight rats.
5. Copy this plan to `docs/superpowers/plans/2026-07-24-xp-levels-skill-catalog.md` and commit it (plan mode barred writing it there during planning).

## Out of scope (slice 2 — brain-owned combat)

- ABI v2: skills + recommendations + combat state in `BlViewWire`, use-skill/attack decisions in `BlDecisionWire`, `BL_ABI_VERSION` bump, `abi.nim` mirror, parity rebaseline.
- `CommandKind::UseSkill`, Calcify's absorb effect, cooldown ticking.
- Per-impact kill-XP weighting (the `share` seam in `spread_kill_xp`).
