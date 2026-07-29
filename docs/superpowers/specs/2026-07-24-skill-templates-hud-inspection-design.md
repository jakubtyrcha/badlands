# Skill Templates + Game-HUD Inspection (Slice A) — Design

Approved in brainstorming 2026-07-24. First of two slices from the "game-mode
inspection + intention/execution contract" request; slice B (skill execution,
SkillUsed events, intention refusal/reconsideration, ABI change) is a separate
spec. Builds on the XP/levels/skill-catalog branch (`feat/xp-levels-skills`).

## Goal

In the game app (game UI surface, not ImGui), a selected hero's detail panel
shows level, XP progress, and the hero's skills with data-driven details.
Skills gain a designer-authorable template (JSON), establishing the catalog
that slice B's execution logic will consume.

## Direction note (supersedes an earlier decision)

The brain **decides intentions**; the game **executes** combat, skills, and
movement, refusing intentions the actor cannot perform and surfacing refusal
events the brain must reconsider against (e.g. path-blocked). This replaces
the earlier "brain owns combat" slice-2 direction. Slice A does not implement
the contract; it only avoids fighting it.

## 1. Skill template data model

- Identity stays internal and compiled (`game/src/skills.h` `SkillDef`:
  `id / name / trigger / trigger_param`, append-only). `SkillDef.cooldown`
  is REMOVED — cooldown moves to the template so it has one source of truth.
- New public (in `badlands_sim.hpp`), per-Sim, JSON-overridable template:

```cpp
enum class SkillActivation : int32_t { Active = 0, Passive };
enum class SkillTargeting : int32_t { Direct = 0, Aoe };

// Per-skill template: designer-authored presentation + mechanic data.
// Initial config in the determinism contract (slice B's cooldown ticking
// reads it); display-only in slice A.
struct SkillSpec {
    SkillActivation activation = SkillActivation::Active;
    SkillTargeting targeting = SkillTargeting::Direct;
    float duration_seconds = 0.0f;  // <= 0 => instant
    float cooldown_seconds = 0.0f;  // <= 0 => none
    std::string effect;             // brief descriptive string
};

struct SkillCatalog {
    SkillCatalog();  // compiled defaults
    SkillSpec specs[kSkillCount];
};
```

- Compiled Calcify default: active, direct, instant (duration 0; the shield
  persists until consumed — the effect string carries that), cooldown 20 s,
  effect "Absorbs the next physical strike, then shatters."
- `SkillIdFromName(const char*)` mirrors `CreatureIdFromName` (public).

## 2. JSON manifest

- Path: `assets/skills/skills.json`. Keyed by skill NAME (identity — not
  overridable), values override template fields; absent keys keep compiled
  defaults; wrong types and unknown names warn loudly (creature-manifest
  policy).

```json
{
  "Calcify": {
    "activation": "active",
    "targeting": "direct",
    "duration": 0,
    "cooldown": 20,
    "effect": "Absorbs the next physical strike, then shatters."
  }
}
```

- Loader: `src/game/skill_manifest.{h,cpp}` → `bool LoadSkillCatalog(const
  std::string& path, SkillCatalog& out)`.
- Sim plumbing: `Sim::SetSkillCatalog(const SkillCatalog&)` +
  `const SkillCatalog& Sim::Skills()`; the set boundary clamps
  `duration_seconds`/`cooldown_seconds` non-negative (sanitize discipline).
  Catalog lives on `BadlandsGame` (initial config, like `CreatureCatalog`).
- App wiring: the game app loads it at startup next to `creatures.json`.

## 3. Game-HUD hero panel

- Detail rows added to the hero branch of the HUD model build
  (`src/executables/game/game_view.cpp`): `level → "5"`,
  `xp → "137 / 303"` (`CharacterState.xp` / `.xp_next`).
- New `HudList` "Skills": two `HudRow`s per learned skill —
  `{name, "active · direct · instant · cd 20s"}` and `{"", effect}` —
  composed from `Sim::Skills().specs[id]` for each id in
  `CharacterState.skills[0..skill_count)`. Passive/no-cooldown omits the
  `cd` fragment; instant vs `Ns` from `duration_seconds`. No layout changes;
  existing rows/lists machinery only.

## 4. Out of scope (slice B)

Skill usage + `SkillUsed` game events, UseSkill command, Calcify effect,
cooldown ticking, intention validation/refusal events, brain reconsideration,
ABI/parity work.

## 5. Tests

- `skill_manifest` tests: parse + partial override, absent file keeps
  defaults, unknown skill name fails loudly, wrong type fails loudly.
- Catalog compiled-defaults test; set-boundary clamp test.
- `game_ui_tests`: a hero `HudModel` with skills yields the level/xp rows and
  the skills list entries (pure CPU).

## Notes

- Branch: stacked on `feat/xp-levels-skills` (unmerged; integration decision
  open).
- `SkillName` (public, compiled) remains the name source; ai_sandbox is
  unchanged this slice.
