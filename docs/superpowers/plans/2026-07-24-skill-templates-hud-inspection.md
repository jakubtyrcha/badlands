# Skill Templates + Game-HUD Inspection (Slice A) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Skills gain a designer-authorable JSON template (activation/targeting/duration/cooldown/effect) held per-Sim, and the game app's hero detail panel shows level, XP progress, and the skill list with those details.

**Architecture:** Identity stays in the compiled internal `SkillDef` table (id/name/trigger); a new public `SkillSpec`/`SkillCatalog` carries the designer template as per-Sim **initial config** (`CreatureCatalog` pattern: compiled defaults + JSON override via `Sim::SetSkillCatalog`, sanitized at the boundary). Cooldown moves from `SkillDef` into `SkillSpec` (single source; the execution slice will read it from the per-Sim catalog). HUD composition is a pure model-building helper in `src/game/ui/hud.cpp` so `game_ui_tests` can cover it; `game_view.cpp` only calls it.

**Tech Stack:** C++23, nlohmann::json manifests, `ui` Rust crate HUD (unchanged), Catch2.

Spec: `docs/superpowers/specs/2026-07-24-skill-templates-hud-inspection-design.md` (committed 9a4acdb).

## Global Constraints

- Skill NAMES are identity: JSON keys by name, names are not overridable; unknown names and wrong types are loud failures; absent keys keep compiled defaults (creature-manifest policy). `SkillIdFromName` mirrors `CreatureIdFromName`.
- Catalog is per-Sim initial config (determinism contract); `duration_seconds`/`cooldown_seconds` clamp non-negative at `SetSkillCatalog`.
- Calcify defaults exactly: active, direct, duration 0 (instant), cooldown 20 s, effect "Absorbs the next physical strike, then shatters."
- HUD is the GAME UI surface (`src/game/ui/` + `game_view.cpp`), NOT ImGui; no layout-engine changes; ASCII-only display strings (`", "` separators — the game font's glyph coverage is not guaranteed beyond ASCII).
- Naming collision (pre-found): public `struct SkillCatalog` requires renaming the internal `std::span<const SkillDef> SkillCatalog()` function (game/src/skills.h:32) to `SkillDefs()` — update its call sites in `skills.cpp` and `game/tests/skills_tests.cpp`.
- Slice B is OUT OF SCOPE: no skill execution, no SkillUsed events, no cooldown ticking, no ABI change.
- Commits: `feat(...)` style + trailer `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>`. Build `cmake --build build`; run tests from the repo root.

---

### Task 1: Public `SkillSpec`/`SkillCatalog` + per-Sim plumbing

**Files:**
- Modify: `game/include/badlands_sim.hpp` (after the existing public skills block ~line 83-91; Sim methods after `Creatures()` ~line 792)
- Modify: `game/src/skills.h` (drop `SkillDef.cooldown`; rename `SkillCatalog()` → `SkillDefs()`)
- Modify: `game/src/skills.cpp` (row literal, rename, `SkillCatalog` ctor, `SkillIdFromName`)
- Modify: `game/src/game_state.h` (`BadlandsGame` member), `game/src/sim.cpp` (sanitize + Sim methods)
- Test: `game/tests/skills_tests.cpp`

**Interfaces:**
- Produces (public): `enum class SkillActivation : int32_t { Active = 0, Passive }`; `enum class SkillTargeting : int32_t { Direct = 0, Aoe }`; `struct SkillSpec { SkillActivation activation; SkillTargeting targeting; float duration_seconds; float cooldown_seconds; std::string effect; }` (defaults Active/Direct/0/0/""); `struct SkillCatalog { SkillCatalog(); SkillSpec specs[kSkillCount]; }`; `SkillId SkillIdFromName(const char* name)` (Count if unknown); `Sim::SetSkillCatalog(const SkillCatalog&)`, `const SkillCatalog& Sim::Skills() const`.
- Produces (internal): `std::span<const SkillDef> SkillDefs()` (renamed); `BadlandsGame::skills` (`SkillCatalog`).

- [ ] **Step 1: Write the failing tests** (append to `game/tests/skills_tests.cpp`; also update the existing "skill catalog is dense and named" test to call `badlands::SkillDefs()` — that rename edit happens with the implementation step, so expect compile-red first)

```cpp
TEST_CASE("skill template catalog carries the compiled Calcify defaults") {
    badlands::SkillCatalog cat;
    const badlands::SkillSpec& c =
        cat.specs[static_cast<size_t>(SkillId::Calcify)];
    CHECK(c.activation == badlands::SkillActivation::Active);
    CHECK(c.targeting == badlands::SkillTargeting::Direct);
    CHECK(c.duration_seconds == 0.0f);
    CHECK(c.cooldown_seconds == 20.0f);
    CHECK(c.effect == "Absorbs the next physical strike, then shatters.");
}

TEST_CASE("SkillIdFromName round-trips catalog names") {
    CHECK(badlands::SkillIdFromName("Calcify") == SkillId::Calcify);
    CHECK(badlands::SkillIdFromName("NotASkill") == badlands::SkillId::Count);
}

TEST_CASE("SetSkillCatalog clamps negative durations and cooldowns") {
    badlands::Sim sim{badlands::BrainDesc{}};
    badlands::SkillCatalog cat;
    cat.specs[0].duration_seconds = -3.0f;
    cat.specs[0].cooldown_seconds = -20.0f;
    sim.SetSkillCatalog(cat);
    CHECK(sim.Skills().specs[0].duration_seconds == 0.0f);
    CHECK(sim.Skills().specs[0].cooldown_seconds == 0.0f);
}
```

(`skills_tests.cpp` already includes `badlands_sim.hpp` transitively via `skills.h`; add `#include "badlands_sim.hpp"` explicitly if missing.)

- [ ] **Step 2: Run to verify failure**

Run: `cmake --build build 2>&1 | head -30`
Expected: compile errors on `badlands::SkillCatalog` (type vs function) / `SkillIdFromName`.

- [ ] **Step 3: Implement**

`badlands_sim.hpp`, directly after the existing `SkillName` declaration (~line 91):

```cpp
// ---- skill templates --------------------------------------------------------
// Designer-authored per-skill data (presentation + mechanics). The AI
// vocabulary (triggers, grants) stays internal in game/src/skills.h.
enum class SkillActivation : int32_t { Active = 0, Passive };
enum class SkillTargeting : int32_t { Direct = 0, Aoe };

// One skill's template. Initial config in the determinism contract (the
// execution slice reads cooldown/duration from here); display-only today.
struct SkillSpec {
    SkillActivation activation = SkillActivation::Active;
    SkillTargeting targeting = SkillTargeting::Direct;
    float duration_seconds = 0.0f;  // <= 0 => instant
    float cooldown_seconds = 0.0f;  // <= 0 => none
    std::string effect;             // brief descriptive string
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
```

`badlands_sim.hpp`, Sim class after `Creatures()`:

```cpp
    // Replaces the skill template catalog (see SkillCatalog). Durations and
    // cooldowns are clamped non-negative at this boundary. Initial config:
    // call before ticking; a replay must use the same catalog.
    void SetSkillCatalog(const SkillCatalog& catalog);
    const SkillCatalog& Skills() const;
```

`game/src/skills.h`: delete the `float cooldown;` member (and its comment) from `SkillDef`; rename the accessor declaration to `std::span<const SkillDef> SkillDefs();` (keep `SkillDefOf`).

`game/src/skills.cpp`: drop `/*cooldown=*/20.0f` from the `kSkills` row; rename the function definition to `SkillDefs()` (and any internal uses — `SkillGrantTable`/`learn_skill` don't use it; `evaluate_skill_triggers` uses `SkillDefOf`, unchanged); add `#include <cstring>` and:

```cpp
SkillCatalog::SkillCatalog() {
    SkillSpec& calcify = specs[static_cast<size_t>(SkillId::Calcify)];
    calcify.activation = SkillActivation::Active;
    calcify.targeting = SkillTargeting::Direct;
    calcify.duration_seconds = 0.0f;  // instant to apply; the shield persists until consumed
    calcify.cooldown_seconds = 20.0f;
    calcify.effect = "Absorbs the next physical strike, then shatters.";
}

SkillId SkillIdFromName(const char* name) {
    for (int32_t i = 0; i < kSkillCount; ++i) {
        if (std::strcmp(name, kSkills[static_cast<size_t>(i)].name) == 0) {
            return static_cast<SkillId>(i);
        }
    }
    return SkillId::Count;
}
```

`game/tests/skills_tests.cpp`: change `badlands::SkillCatalog()` calls in the dense-catalog test to `badlands::SkillDefs()`.

`game/src/game_state.h`: after the `creatures` member:

```cpp
    // Skill template catalog (see SkillCatalog). Compiled defaults; an app may
    // override by name from JSON via Sim::SetSkillCatalog. Initial config.
    badlands::SkillCatalog skills;
```

`game/src/sim.cpp`: in the anonymous namespace next to `sanitize_factors` (reusing its `clamp_nonneg`):

```cpp
// The SetSkillCatalog validation boundary, sanitize_factors' sibling: the
// execution slice divides/waits on these, so negatives are clamped here.
SkillCatalog sanitize_skill_catalog(SkillCatalog c) {
    for (int32_t i = 0; i < kSkillCount; ++i) {
        SkillSpec& s = c.specs[i];
        clamp_nonneg("skill.duration_seconds", s.duration_seconds);
        clamp_nonneg("skill.cooldown_seconds", s.cooldown_seconds);
    }
    return c;
}
```

and with the other Sim methods:

```cpp
void Sim::SetSkillCatalog(const SkillCatalog& catalog) {
    world_->skills = sanitize_skill_catalog(catalog);
}
const SkillCatalog& Sim::Skills() const { return world_->skills; }
```

- [ ] **Step 4: Build + run** `cmake --build build && ./build/badlands_game_tests "*skill*" "*Skill*"` — new tests PASS, renamed test PASS; then the full suite once.

- [ ] **Step 5: Commit** — `feat(game): public SkillSpec/SkillCatalog template (per-Sim, sanitized)`

---

### Task 2: `skills.json` manifest + loader

**Files:**
- Create: `assets/skills/skills.json`, `src/game/skill_manifest.hpp`, `src/game/skill_manifest.cpp`, `src/game/tests/skill_manifest_tests.cpp`
- Modify: `CMakeLists.txt` (add `src/game/skill_manifest.cpp` wherever `src/game/creature_manifest.cpp` is listed for app targets, plus a new test target mirroring `badlands_factors_manifest_tests` — grep both names for the exact blocks)

**Interfaces:**
- Consumes: Task 1 `SkillCatalog`, `SkillSpec`, `SkillIdFromName`.
- Produces: `bool badlands::LoadSkillCatalog(const std::string& path, SkillCatalog& out)` — false (out untouched) on open/parse/shape error; scratch-copy commit semantics.

- [ ] **Step 1: Write the failing tests** (`src/game/tests/skill_manifest_tests.cpp`, mirroring `src/game/tests/factors_manifest_tests.cpp`'s temp-file helper pattern — read that file first and reuse its helper shape)

```cpp
#include "game/skill_manifest.hpp"

#include <catch_amalgamated.hpp>

#include <cstdio>
#include <fstream>
#include <string>

using badlands::SkillActivation;
using badlands::SkillCatalog;
using badlands::SkillId;
using badlands::SkillTargeting;

namespace {

// Writes `json` to a temp path and deletes it on scope exit (mirror
// factors_manifest_tests' helper; reuse its exact mechanism).
struct TempManifest {
    std::string path;
    explicit TempManifest(const std::string& json) {
        path = "build/skill_manifest_test.json";
        std::ofstream f(path);
        f << json;
    }
    ~TempManifest() { std::remove(path.c_str()); }
};

}  // namespace

TEST_CASE("partial override keeps unspecified fields at compiled defaults") {
    TempManifest m(R"({"Calcify": {"cooldown": 5, "effect": "New text."}})");
    SkillCatalog cat;
    REQUIRE(badlands::LoadSkillCatalog(m.path, cat));
    const auto& c = cat.specs[static_cast<size_t>(SkillId::Calcify)];
    CHECK(c.cooldown_seconds == 5.0f);
    CHECK(c.effect == "New text.");
    CHECK(c.activation == SkillActivation::Active);   // untouched default
    CHECK(c.targeting == SkillTargeting::Direct);     // untouched default
    CHECK(c.duration_seconds == 0.0f);                // untouched default
}

TEST_CASE("enum fields parse their named choices") {
    TempManifest m(R"({"Calcify": {"activation": "passive", "targeting": "aoe", "duration": 4}})");
    SkillCatalog cat;
    REQUIRE(badlands::LoadSkillCatalog(m.path, cat));
    const auto& c = cat.specs[static_cast<size_t>(SkillId::Calcify)];
    CHECK(c.activation == SkillActivation::Passive);
    CHECK(c.targeting == SkillTargeting::Aoe);
    CHECK(c.duration_seconds == 4.0f);
}

TEST_CASE("unknown skill names, bad choices, and wrong types fail loudly") {
    SkillCatalog cat;
    const float default_cd = cat.specs[0].cooldown_seconds;
    {
        TempManifest m(R"({"NotASkill": {"cooldown": 5}})");
        CHECK_FALSE(badlands::LoadSkillCatalog(m.path, cat));
    }
    {
        TempManifest m(R"({"Calcify": {"activation": "sometimes"}})");
        CHECK_FALSE(badlands::LoadSkillCatalog(m.path, cat));
    }
    {
        TempManifest m(R"({"Calcify": {"cooldown": "fast"}})");
        CHECK_FALSE(badlands::LoadSkillCatalog(m.path, cat));
    }
    CHECK(cat.specs[0].cooldown_seconds == default_cd);  // out untouched on failure
}

TEST_CASE("missing file returns false and leaves the catalog untouched") {
    SkillCatalog cat;
    CHECK_FALSE(badlands::LoadSkillCatalog("build/definitely_absent.json", cat));
    CHECK(cat.specs[0].cooldown_seconds == 20.0f);
}
```

- [ ] **Step 2: Run to verify failure** (no `skill_manifest.hpp`; target doesn't exist yet — expect the build of the new test target to fail; wire CMake in step 3).

- [ ] **Step 3: Implement**

`src/game/skill_manifest.hpp`:

```cpp
#pragma once

// Optional JSON overrides for the skill template catalog (SkillCatalog):
// assets/skills/skills.json, an object keyed by skill NAME ("Calcify"). Keys
// are identity (unknown names fail loudly); absent fields keep the compiled
// defaults; "_"-prefixed keys are comments. Returns false (out untouched) on
// any open/parse/shape error -- creature-manifest policy.

#include <string>

#include "badlands_sim.hpp"

namespace badlands {

bool LoadSkillCatalog(const std::string& path, SkillCatalog& out);

}  // namespace badlands
```

`src/game/skill_manifest.cpp`:

```cpp
#include "game/skill_manifest.hpp"

#include <fstream>
#include <utility>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

namespace badlands {

namespace {

// Read an optional numeric key into `dst` (creature_manifest's contract).
template <typename T>
bool ReadNum(const nlohmann::json& obj, const std::string& skill, const char* key, T& dst) {
    if (!obj.contains(key)) {
        return true;
    }
    if (!obj[key].is_number()) {
        spdlog::warn("LoadSkillCatalog: {}.{} is not a number", skill, key);
        return false;
    }
    dst = obj[key].get<T>();
    return true;
}

bool ReadString(const nlohmann::json& obj, const std::string& skill, const char* key,
                std::string& dst) {
    if (!obj.contains(key)) {
        return true;
    }
    if (!obj[key].is_string()) {
        spdlog::warn("LoadSkillCatalog: {}.{} is not a string", skill, key);
        return false;
    }
    dst = obj[key].get<std::string>();
    return true;
}

// Read an optional named-choice key: the value must be one of `choices`'
// names; its paired int lands in `dst`. Unknown names fail loudly (silently
// keeping a default would be indistinguishable from a typo doing nothing).
bool ReadChoice(const nlohmann::json& obj, const std::string& skill, const char* key,
                std::initializer_list<std::pair<const char*, int32_t>> choices,
                int32_t& dst) {
    if (!obj.contains(key)) {
        return true;
    }
    if (!obj[key].is_string()) {
        spdlog::warn("LoadSkillCatalog: {}.{} is not a string", skill, key);
        return false;
    }
    const std::string v = obj[key].get<std::string>();
    for (const auto& [name, value] : choices) {
        if (v == name) {
            dst = value;
            return true;
        }
    }
    spdlog::warn("LoadSkillCatalog: {}.{} has unknown value '{}'", skill, key, v);
    return false;
}

}  // namespace

bool LoadSkillCatalog(const std::string& path, SkillCatalog& out) {
    std::ifstream file(path);
    if (!file.good()) {
        spdlog::warn("LoadSkillCatalog: cannot open '{}'", path);
        return false;
    }
    nlohmann::json manifest;
    try {
        file >> manifest;
    } catch (const nlohmann::json::exception& e) {
        spdlog::warn("LoadSkillCatalog: '{}' is not valid JSON: {}", path, e.what());
        return false;
    }
    if (!manifest.is_object()) {
        spdlog::warn("LoadSkillCatalog: '{}' top level is not an object", path);
        return false;
    }

    // Parse into a scratch copy so a failure half-way leaves `out` untouched.
    SkillCatalog parsed = out;
    for (const auto& [name, o] : manifest.items()) {
        if (!name.empty() && name[0] == '_') {
            continue;  // "_comment"-style keys are documentation, not skills
        }
        const SkillId id = SkillIdFromName(name.c_str());
        if (id == SkillId::Count) {
            spdlog::warn("LoadSkillCatalog: '{}' -> unknown skill '{}'", path, name);
            return false;
        }
        if (!o.is_object()) {
            spdlog::warn("LoadSkillCatalog: '{}' -> {} is not an object", path, name);
            return false;
        }
        SkillSpec& s = parsed.specs[static_cast<size_t>(id)];
        int32_t activation = static_cast<int32_t>(s.activation);
        int32_t targeting = static_cast<int32_t>(s.targeting);
        const bool ok =
            ReadChoice(o, name, "activation", {{"active", 0}, {"passive", 1}}, activation) &&
            ReadChoice(o, name, "targeting", {{"direct", 0}, {"aoe", 1}}, targeting) &&
            ReadNum(o, name, "duration", s.duration_seconds) &&
            ReadNum(o, name, "cooldown", s.cooldown_seconds) &&
            ReadString(o, name, "effect", s.effect);
        if (!ok) {
            return false;
        }
        s.activation = static_cast<SkillActivation>(activation);
        s.targeting = static_cast<SkillTargeting>(targeting);
    }

    out = parsed;
    return true;
}

}  // namespace badlands
```

`assets/skills/skills.json`:

```json
{
  "_comment": "Skill template overrides; keys are skill names (identity). Absent fields keep compiled defaults.",
  "Calcify": {
    "activation": "active",
    "targeting": "direct",
    "duration": 0,
    "cooldown": 20,
    "effect": "Absorbs the next physical strike, then shatters."
  }
}
```

CMake: `grep -n "creature_manifest.cpp\|factors_manifest_tests" CMakeLists.txt`; add `src/game/skill_manifest.cpp` next to `creature_manifest.cpp` in each app target that lists it, and clone the `badlands_factors_manifest_tests` executable/include/`add_test` block as `badlands_skill_manifest_tests` (sources: `src/game/tests/skill_manifest_tests.cpp`, `src/game/skill_manifest.cpp`, plus whatever lib/link/include lines the factors block carries — keep them identical).

- [ ] **Step 4: Build + run** `cmake --build build && ./build/badlands_skill_manifest_tests` — all PASS; `ctest --test-dir build` green (new target registered).

- [ ] **Step 5: Commit** — `feat(game): skills.json template manifest loader` (note: `.json` is not LFS-tracked; plain `git add assets/skills/skills.json` is correct).

---

### Task 3: HUD hero panel — level/xp rows + skills list + app wiring

**Files:**
- Modify: `src/game/ui/hud.hpp`, `src/game/ui/hud.cpp` (pure composition helper)
- Modify: `src/executables/game/game_view.cpp` (hero branch ~line 1348; startup skill-catalog load near the `sim_.ConfigureVision` init ~line 344)
- Test: `game/tests/game_ui_tests.cpp`

**Interfaces:**
- Consumes: Task 1 `SkillCatalog`/`SkillSpec`/`kSkillCount`/`SkillName`; Task 2 `LoadSkillCatalog`; `CharacterState.level/xp/xp_next/skill_count/skills`.
- Produces: `void badlands::AppendHeroProgressionRows(HudSelection& sel, const CharacterState& hero, const SkillCatalog& skills)` — appends `level` + `xp` rows and a "Skills" `HudList` (summary row + effect row per skill); no-op for non-heroes (`level <= 0`).

- [ ] **Step 1: Write the failing tests** (append to `game/tests/game_ui_tests.cpp`)

```cpp
TEST_CASE("hero progression rows: level, xp, and the skills list") {
  CharacterState hero{};
  hero.level = 5;
  hero.xp = 137;
  hero.xp_next = 918;
  hero.skill_count = 1;
  hero.skills[0] = static_cast<int32_t>(SkillId::Calcify);
  SkillCatalog cat;  // compiled defaults
  HudSelection sel;
  AppendHeroProgressionRows(sel, hero, cat);
  REQUIRE(sel.rows.size() == 2);
  CHECK(sel.rows[0].label == "level");
  CHECK(sel.rows[0].value == "5");
  CHECK(sel.rows[1].label == "xp");
  CHECK(sel.rows[1].value == "137 / 918");
  REQUIRE(sel.lists.size() == 1);
  CHECK(sel.lists[0].heading == "Skills");
  REQUIRE(sel.lists[0].entries.size() == 2);
  CHECK(sel.lists[0].entries[0].label == "Calcify");
  CHECK(sel.lists[0].entries[0].value == "active, direct, instant, cd 20s");
  CHECK(sel.lists[0].entries[1].label == "");
  CHECK(sel.lists[0].entries[1].value ==
        "Absorbs the next physical strike, then shatters.");
}

TEST_CASE("progression rows: non-heroes add nothing; skill-less heroes skip the list") {
  SkillCatalog cat;
  HudSelection sel;
  CharacterState rat{};  // level 0 marks a non-hero row
  AppendHeroProgressionRows(sel, rat, cat);
  CHECK(sel.rows.empty());
  CHECK(sel.lists.empty());
  CharacterState hero{};
  hero.level = 1;
  hero.xp_next = 100;
  AppendHeroProgressionRows(sel, hero, cat);
  REQUIRE(sel.rows.size() == 2);
  CHECK(sel.rows[1].value == "0 / 100");
  CHECK(sel.lists.empty());
}

TEST_CASE("skill summary omits cd when none and shows duration seconds") {
  SkillCatalog cat;
  cat.specs[0].activation = SkillActivation::Passive;
  cat.specs[0].targeting = SkillTargeting::Aoe;
  cat.specs[0].duration_seconds = 6.0f;
  cat.specs[0].cooldown_seconds = 0.0f;
  CharacterState hero{};
  hero.level = 1;
  hero.skill_count = 1;
  hero.skills[0] = 0;
  HudSelection sel;
  AppendHeroProgressionRows(sel, hero, cat);
  REQUIRE(sel.lists.size() == 1);
  CHECK(sel.lists[0].entries[0].value == "passive, aoe, 6s");
}
```

- [ ] **Step 2: Run to verify failure** — compile error: no `AppendHeroProgressionRows`.

- [ ] **Step 3: Implement**

`src/game/ui/hud.hpp` (add `#include "badlands_sim.hpp"` to the includes; declaration after `BuildHud`):

```cpp
// Appends a hero's progression detail to a selection: `level` and `xp` rows,
// then a "Skills" list with two entries per learned skill -- a summary line
// composed from the skill's template ("active, direct, instant, cd 20s") and
// its effect text. Pure model-building (no layout, no GPU), so tests cover
// the composition; no-op for non-hero rows (level <= 0).
void AppendHeroProgressionRows(HudSelection& sel, const CharacterState& hero,
                               const SkillCatalog& skills);
```

`src/game/ui/hud.cpp` (anonymous namespace helper + definition, near the other model-side helpers; ASCII separators only):

```cpp
namespace {

// "active, direct, instant, cd 20s" -- duration shows as seconds when timed,
// "instant" otherwise; the cd fragment is omitted when the skill has none.
std::string SkillSummary(const badlands::SkillSpec& s) {
  std::string out =
      s.activation == badlands::SkillActivation::Passive ? "passive" : "active";
  out += s.targeting == badlands::SkillTargeting::Aoe ? ", aoe" : ", direct";
  if (s.duration_seconds > 0.0f) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), ", %.0fs", s.duration_seconds);
    out += buf;
  } else {
    out += ", instant";
  }
  if (s.cooldown_seconds > 0.0f) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), ", cd %.0fs", s.cooldown_seconds);
    out += buf;
  }
  return out;
}

}  // namespace

void AppendHeroProgressionRows(HudSelection& sel, const CharacterState& hero,
                               const SkillCatalog& skills) {
  if (hero.level <= 0) {
    return;  // level >= 1 marks a hero row (snapshot contract)
  }
  sel.rows.emplace_back("level", std::to_string(hero.level));
  sel.rows.emplace_back("xp", std::to_string(hero.xp) + " / " +
                                  std::to_string(hero.xp_next));
  if (hero.skill_count <= 0) {
    return;
  }
  HudList list;
  list.heading = "Skills";
  for (int32_t i = 0; i < hero.skill_count && i < kMaxSkills; ++i) {
    const int32_t id = hero.skills[i];
    const bool known = id >= 0 && id < kSkillCount;
    list.entries.emplace_back(SkillName(id),
                              known ? SkillSummary(skills.specs[id]) : "");
    if (known && !skills.specs[id].effect.empty()) {
      list.entries.emplace_back("", skills.specs[id].effect);
    }
  }
  sel.lists.push_back(std::move(list));
}
```

(If `hud.cpp` lacks `<cstdio>`, add it.)

`src/executables/game/game_view.cpp`:
- Add `#include "game/skill_manifest.hpp"` with the other `game/` includes.
- In the startup init that already calls `sim_.ConfigureVision(...)` (~line 344), BEFORE the first `sim_.Tick` (`ConfigureVision` is in that pre-tick init — place adjacent):

```cpp
  // Skill templates as data: load over the compiled defaults (a missing file
  // keeps them). Initial config -- must happen before ticking.
  {
    badlands::SkillCatalog skills = sim_.Skills();
    if (badlands::LoadSkillCatalog("assets/skills/skills.json", skills)) {
      sim_.SetSkillCatalog(skills);
    }
  }
```

- In the hero selection branch (~line 1348, after the guild-link row, before `model.has_selection = true`):

```cpp
    AppendHeroProgressionRows(s, *hero, sim_.Skills());
```

- [ ] **Step 4: Build + run** `cmake --build build && ./build/badlands_game_tests "*progression rows*" "*skill summary*"` — PASS; full `badlands_game_tests` + `ctest --test-dir build` green; headless smoke `./build/badlands_game --screenshot /tmp/hud_smoke.png` exits 0.

- [ ] **Step 5: Commit** — `feat(ui): hero level/xp/skills detail in the game HUD`

---

## Verification (end of plan)

1. `cmake --build build` clean; `ctest --test-dir build` — 33/33 (new `badlands_skill_manifest_tests` target included).
2. `./build/badlands_game --screenshot out.png` exits 0 (panel composition is test-covered; the screenshot has no selection, so it only proves the app still runs).
3. Manual spot-check (optional): run `./build/badlands_game`, click a hero — panel shows `level`, `xp`, and the Skills list once a hero has one (Apprentice at level 5).
4. Determinism suite green (catalog is initial config; display-only this slice).

## Out of scope (slice B)

Skill execution (`UseSkill`, Calcify effect, cooldown ticking), `SkillUsed` game events + HUD log lines, intention validation/refusal events, brain reconsideration, ABI/parity work.
