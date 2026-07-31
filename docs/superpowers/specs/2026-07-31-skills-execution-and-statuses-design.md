# Skills Execution + Status Effects — Design

Approved in brainstorming 2026-07-31. Slice B of the skills work; slice A
(identity + JSON template + HUD inspection,
`2026-07-24-skill-templates-hud-inspection-design.md`) shipped the catalog and
deliberately left execution inert.

## Goal

Ship one skill (**shield-bash**, mercenary, level 3) and one status
(**stunned**) — and, underneath them, the four seams every later mechanic
reuses: data-driven acquisition, data-driven tuning, status timers, and skill
application through the existing intention/action gateway.

## 1. A skill is data the engine checks + an effect

Five engine-checked fields on `SkillSpec` (`badlands_sim.hpp`):

| field | vocabulary | who enforces it |
|---|---|---|
| `trigger` | `action \| passive \| intention` | `validate_cast` — only `action` executes |
| `target` | `none \| self \| any \| multi(limit) \| point` | `resolve_skill_targets` — `self` at another entity is REFUSED, never remapped |
| `cooldown_seconds` | `0` = none | `validate_cast` + the per-tick sweep |
| `intention_duration_seconds` | `0` = none | parsed and validated; the `intention` trigger is not executed yet |
| `effect` | display text | — |

Plus `attack_test` (`none | melee | ranged`): whether the engine pre-rolls a
combat test per target, and — because it names a weapon — **the cast range**.
One source of truth, so a bash cannot reach further than the sword whose test
it borrows.

`assets/skills/skills.json` overrides these by skill NAME, and carries
per-skill **constants** (named floats). The manifest never carries behaviour.

## 2. Effects are a wire, not a call

`game/src/skill_abi.h` is a C header in the style of `brain_abi.h`, because it
is the wire a Nim/wasm skill script will implement later:

```
BlSkillCastContext (712 B)  →  effect  →  BlSkillEffectBatch (136 B)
  caster view                              ops: APPLY_STATUS | DAMAGE
  per-target views + PRE-ROLLED test
  named constants, clock, seed
```

An effect cannot read the world, cannot roll, and cannot apply anything. The
engine resolves targets, rolls the declared test, runs the effect, then
validates every op against the context it came from — an op naming an entity
the effect was not shown is dropped. Same discipline as the brain's action
channel: the guest requests, the host decides.

`shield_bash` is pure control: it stuns every target whose test landed and
discards the damage that roll produced.

## 3. Statuses

`StatusKind` (append-only) + a `Statuses` component of `{kind,
remaining_millis, source_slot}`, ticked in int64 ms by the compile-time
per-tick constant. The subsystem owns TIMERS ONLY; what a status means is
enforced where the decision already happens:

- **think** — `sim.cpp`'s dispatch skips a stunned entity (wasm hero and
  engine-side brain alike).
- **movement** — `follow_paths` skips it but leaves the `NavPath` intact, so
  the route resumes rather than resets.
- **defense** — `effective_combatant` zeroes `defense`/`evasion`; both
  `CombatRequest` assembly sites route through it, so a shot already in flight
  lands against the same defencelessness. Armour is unaffected (worn, not used).
- **plans** — `apply_status` aborts the running `CurrentIntention`
  (`IntentionEnded`), so the brain re-decides from scratch on its first wake
  afterwards.

Collision separation still nudges a stunned body, or units would stack on it.

## 4. Casting goes through the existing gateway

`BL_ACT_USE_SKILL` with `arg` = index into the caster's **own** `Skills`,
mirroring `BL_ACT_ATTACK`'s attack index. The gateway's refusal is the cheap
early one; the `CommandKind::UseSkill` handler re-runs the SAME `validate_cast`
authoritatively at drain time. Every cast is a logged Command, so the trace
replays.

Cost: the skill's own cooldown, nothing else — a bash and a swing may both
land in one wake.

## 5. Acquisition is creature config

`CharacterDesc::skill_grants` (`{skill, level}` rows), authored in the creature
catalog and parsed from `assets/creatures/creatures.json`. The list travels
with the desc and is copied onto the entity at spawn, so no consumer re-derives
a hero's class to know what it should learn. The compiled class→skill table is
deleted. An override REPLACES rather than merges — the only way to author a
class that learns nothing.

**Known limit (pre-existing, inherited):** `hero_desc` (`heroes.cpp`) reads the
COMPILED catalog for recruited heroes; only directly-spawned creatures and
arena scenarios read the per-`Sim` catalog that `LoadCreatureCatalog` fills. So
a `creatures.json` grant edit changes an arena mercenary and not a
guild-recruited one — exactly as it already does for hp/armour/attack stats.
Making recruits read the per-`Sim` catalog would fix both at once, but it
changes stat behaviour for every recruited hero and so is a decision to take
deliberately, not a side effect of this slice.

## 6. Hero view

One bordered card per learned skill (name, then `action / any / cd 12s`) in a
fixed-height region. The `ui` crate can neither clip nor scroll, so the caller
windows to `HudSkillCardCapacity()` and the wheel over the region walks the
window — the arrangement the combat log already uses. The heading says what is
off-screen (`Skills  3-5 / 8`) rather than truncating silently.

## Declared but not executed

Refused loudly at the cast, never approximated: `trigger = passive`,
`trigger = intention`, `target = multi`, `target = point`. `BL_FX_DAMAGE`
exists and applies, but no shipped effect emits one. `Calcify`'s own mechanic
(an absorb charge, not a timer) remains a later slice.
