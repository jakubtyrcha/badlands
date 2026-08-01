# Class Skills Completion — Design

Brainstormed 2026-07-31, against `docs/design/game-design.html` §6.1–6.4 and the
two slices this builds on: `2026-07-31-skills-execution-and-statuses-design.md`
(the skill contract and the status subsystem) and
`2026-07-31-core-classes-combat-design.md` (stats, growth, wind-up, disengage,
one combat skill per class).

## Goal

Every skill the design doc gives the four implemented classes exists and fires,
except the two that need an economy that does not exist. Four skills ship —
**Sneak**, **Precision Shot**, **Calcify**, **Teleport** — and each one is
chosen because it forces open a different seam the contract declared and
refused:

| Skill | Class(es) | Level | The seam it opens |
|---|---|---|---|
| Sneak | Grave Robber | 3 | perception: an entity nobody can see |
| Precision Shot | Grave Robber 5, **Hunter 2** | 5 / 2 | `SkillTrigger::Intention` — a skill that focuses |
| Calcify | Apprentice | 4 | none; it is the cheap one, and it makes a grant that exists do something |
| Teleport | Apprentice | 8 | `SkillTargetMode::Point`, and a navmesh window on the brain wire |

**Validation:** the duel sandbox spawns heroes at a random level 1–8, so a
level-8 Apprentice teleports and a level-5 Grave Robber focuses a shot, live,
in an arena. Watching it is the deliverable; no balance is asserted.

**Deferred, and named:** Skin Game (Hunter, lvl 1) and Rob Grave (Grave Robber,
lvl 1). Both are economy skills and the sim has no corpses, no graves, no
salvage, and no items — only gold and tax collectors. Building that substrate
is its own slice, ahead of the skills that spend it.

**Deviation from the design doc, deliberate:** the doc gives the Hunter Skin
Game at level 1; with Skin Game deferred, the Hunter gets **Precision Shot at
level 2** instead, so it is not a one-skill class. Grave Robber keeps Precision
Shot at 5.

## 1. Random level is what makes any of this visible

Nothing above level 3 is reachable in the sandbox today: `SpawnCreature` spawns
at level 1 and the duel mode never levels anyone. So this comes **first**, not
last — without it, none of the four skills can be watched.

```cpp
// Sim
uint32_t SpawnCreature(CreatureId id, int32_t team, float pos_x, float pos_z,
                       int32_t level = 1);
```

Spawn applies `apply_level_stats(reg, e, level)` and then
`grant_skills_for_level` for **every level 1..level** — grant rows fire at their
exact level, so a hero dropped in at 8 must replay the ladder to learn what 3
and 5 taught. `HeroSimulationState::level/xp_next` are stamped to match, so the
hero's next level-up costs what it should.

**Monsters have no level.** They carry a zeroed growth row and their threat
anchors are authored at level 1, so the level argument is ignored for them
rather than being invented. The duel keeps sampling both sides from the full
pool at random; only the hero side draws a level, from the same splitmix stream
that already picks the pairing, so a session stays reproducible from `(seed,
round)`. The log line prints it:

```
duel 7: GraveRobber(lvl 6) beats Bandit on octagon in 24.3s
```

## 2. Sneak — an entity nobody can see

`StatusKind::Sneaking` (append-only). A sneaking entity is **imperceptible**:
it is skipped by `select_target` (combat.h) and it is never written into a
`PerceivedThreat` (behaviours/perception.cpp), which is the one place every
brain — wasm hero and engine-side monster alike — learns that anything is
there. One predicate, two call sites, and no brain gets a special case.

An Attack intention whose target becomes imperceptible aborts with
`IntentionEnded`, exactly as one whose target died does. A pursuer must
re-decide rather than keep walking at a memory.

- **Duration ~20 s**, authored as the skill's `duration_seconds` constant.
- **Cannot be cast while melee-locked.** This is engine-checked data, not effect
  logic: `SkillSpec` gains `castable_in_melee` (default true), Sneak authors it
  false, and `validate_cast` refuses. The gateway and the command handler both
  run the same check, as they already do for every other cast rule.
- **Ends on an aggressive act**, which is precisely two things: declaring a
  strike (`declare_strike`), or a cast of the sneaker's whose effect batch
  damages another entity. Calcify, Teleport and Dress Wounds are therefore
  silent; Backstab and Precision Shot break it, on the way out.

### Attacking out of sneak

Two compiled constants, following Curse's own rule — *how much a status is
worth is a property of the status, not of whatever applied it* — so they live
beside `kCurseAccuracyPenalty` in `combat.h`, not in `skills.json`:

- `kSneakAccuracyBonus` — added by `effective_combatant`, the same hook Stunned
  and Cursed already use.
- `kSneakCritMultiplier` — `Combatant` gains `crit_multiplier` (defaulting to
  today's global `kCritMultiplier`), `effective_combatant` raises it while
  Sneaking, and `resolve_attack` reads `req.attacker.crit_multiplier` instead of
  the file-scope constant. No new `CombatRequest` field, and every existing call
  site is unchanged.

`declare_strike` captures the attacker's stats at declaration and *then* clears
Sneaking — so the blow that breaks stealth is the blow that benefits from it,
which is the whole point of the skill.

## 3. Precision Shot — the Intention trigger, finally executed

`SkillTrigger::Intention` has been declared-and-refused since the skill
contract shipped. Its documented meaning is exactly what a focus is: *adopted
as an intention, its effect landing after `intention_duration_seconds` of
uninterrupted execution.* Precision Shot is what executes it.

- **Range 30 m**, `attack_test = ranged`, `intention_duration = 2 s`.
- **Always lands and always crits.** `SkillSpec` gains `guaranteed_test`
  (default false); when set, the engine's per-target pre-roll skips the defense
  and evasion gates and forces the crit, at the skill's own
  `crit_multiplier` constant. The effect then just asks for `BL_FX_DAMAGE` with
  the damage the roll produced.

### How a focus runs

Casting an Intention-triggered skill goes through the **suggestion** channel
(`BL_INT_USE_SKILL`, reserved on the wire since v3), never the action channel.
The two are strictly split: `BL_ACT_USE_SKILL` accepts only `Action` triggers
and `BL_INT_USE_SKILL` only `Intention` ones, so neither can be used to skip
the other's rules.

`apply_intention` runs the ordinary `validate_cast` and stamps a `SkillFocus`
component — `{skill id, skill index, target slot, resolve deadline}` — in a new
`game/src/skill_focus.{h,cpp}` that mirrors `strike.h` deliberately, phase
derived from the clock rather than stored. `advance_focus` resolves it in the
per-tick sweep by calling the same `run_cast` every other cast uses.

What ends a focus, and nothing else does:

| | |
|---|---|
| **Resolves** | the deadline elapses and the target is still in range |
| **Cancelled by a stun** | `apply_status(Stunned)` drops it with no shot — the same interrupt that already drops a wind-up |
| **Cancelled by re-deciding** | the brain adopts any intention that is not this identical focus |
| **Cancelled by range** | the target is out of range at the deadline (a shove, or the target ran) |

A focusing entity **does not move** (`follow_paths` skips it, as it already
skips a striking one). It is still woken, which is what makes "moving abandons
it" reachable at all: the only way to move is to abandon the focus by
suggesting something else. That is a brain decision, and it costs the 2 s
already spent.

## 4. Calcify — the grant that finally does something

`StatusKind::Calcified` (append-only), 30 s, `+kCalcifyArmourBonus` armour via
`effective_combatant`. SelfOnly, no attack test, the cooldown already authored.
The compiled-constant rule above applies here too: the skill's constant is the
*duration*; how much armour a calcification is worth belongs to the status.

`SkillId::Calcify`'s doc comment and its `skills.json` effect text both still
promise an absorb charge. They change to say what it does.

## 5. Teleport — Point targeting, and a navmesh window

The largest change, and the one that touches two ABIs.

**Skill contract (`skill_abi.h`), `BL_SKILL_ABI_VERSION` 2 → 3:**

- `BlSkillCastContext` gains `point_x`, `point_z` (712 → **720 bytes**) — a
  Point cast has to tell the effect where it landed.
- `BL_FX_TELEPORT` (op kind 4): move `target_slot` **to the cast's own
  validated point**. The op carries no destination of its own, which is the
  whole safety argument in miniature — an effect can ask that someone be moved,
  but only to the place the engine already checked. `BlSkillEffectOp` and
  `BlSkillEffectBatch` keep their sizes.

**Engine:** `SkillTargetMode::Point` stops being refused. `validate_cast` gains
a point parameter and `CastPlan` a point field; the point must be within
`skill_cast_range` (Teleport's `range` constant, 30 m) and on a **passable
navmesh cell**. `CommandRecord` already carries `point_x/point_z`, so `UseSkill`
records the cast point with no change to the log format.

**Brain contract (`brain_abi.h`), `BL_ABI_VERSION` 5 → 6:** a brain cannot pick
a destination it cannot see. `BlViewWire` gains a nav-poly block, mirrored in
`abi.nim` / `hero_view.nim` and `brainhost`'s wire constants as v4 and v5 were:

```c
typedef struct BlNavPoly {      // 24 bytes
    float min_x, min_z, max_x, max_z;
    int32_t passable;
    uint32_t _pad;
} BlNavPoly;

#define BL_MAX_NAV_POLYS 32
// in BlViewWire, after the skills block:
//   int32_t nav_poly_count; uint32_t _pad7; BlNavPoly nav_polys[BL_MAX_NAV_POLYS];
```

The actual navmesh rectangles near the hero, **nearest-first by centre
distance** and truncated at 32, so what survives truncation is always the
closest ground. The block is filled only for a hero that owns a Point-targeted
skill; everyone else reads `nav_poly_count == 0` and pays nothing.

`bl_enqueue_action` grows two arguments —
`(kind, target_slot, arg, point_x, point_z)` — a breaking host-import change to
`brainhost.h`, the Rust host, and `abi.nim`, which is what the version bump is
for. Non-point actions pass zeroes.

## 6. What the brain does with all this

`hero.nim` grows one gate per skill, each mirroring what `validate_cast` checks
host-side so a wake is never spent asking for a refusal:

- **Sneak** — not melee-locked, a threat in view beyond melee reach, Sneak
  ready. Then close and open with Backstab: the bonus damage and the sneak crit
  stack, which is the pairing the Grave Robber's kit is for.
- **Precision Shot** — a threat between melee reach and 30 m, and nothing
  already in contact. Suggested as an intention, so it replaces whatever the
  hero was doing for 2 s.
- **Calcify** — a threat in view, Calcify ready. Cheap, self, always worth it.
- **Teleport** — health low and a threat inside melee reach: pick the passable
  nav poly whose centre is farthest from the threat within 30 m, and blink. The
  Apprentice's escape, and the reason the nav window exists.

The engine-side monster brains gain nothing here; none of the six monsters
learns a skill.

## 7. Grants

Authored in `creature_catalog.cpp`, overridable by name from
`assets/creatures/creatures.json`, exactly as the shipped rows are:

| Class | Grants |
|---|---|
| Mercenary | ShieldBash 3 |
| Hunter | **PrecisionShot 2**, DressWounds 2 |
| Grave Robber | **Sneak 3**, Backstab 3, **PrecisionShot 5** |
| Apprentice | Curse 1, Calcify 4, **Teleport 8** |

`SkillId` appends `Sneak`, `PrecisionShot`, `Teleport`; `StatusKind` appends
`Sneaking`, `Calcified`. Both id spaces stay append-only, and the `BL_ST_*` /
`BL_SKILL_*` mirrors on the brain wire grow with them.

## 8. Testing

Mechanism, never authored numbers and never a balance outcome:

- **Sneak** — a sneaking entity is not returned by `select_target` and not
  perceived; a cast while melee-locked is refused; a declared strike clears the
  status and the strike it clears carries the bonus; the timer expires.
- **Precision Shot** — a focus resolves at its deadline; a stun mid-focus drops
  it with no damage; a new intention drops it; a focusing entity does not move;
  the guaranteed test never blocks or dodges and always crits.
- **Calcify** — armour rises for the duration and returns after it.
- **Teleport** — a point off the navmesh is refused, a point past 30 m is
  refused, a legal cast moves the caster to exactly the validated point, and an
  effect op naming a point other than the cast's own cannot exist (it carries
  none).
- **Determinism** — the existing run-twice + replay-the-log contract covers all
  four, and a level-8 spawn replays to identical stats and an identical
  loadout.
- **ABI** — size static_asserts on both wires, on both sides.

## 9. Ordering

1. **Random level** — `SpawnCreature`'s level argument, the ladder replay, the
   duel's draw and log line. Nothing after this is watchable without it.
2. **Calcify + Sneak** — one trivial status and one perception gate; they share
   the `effective_combatant` hook and the constant-placement rule.
3. **Precision Shot** — the Intention trigger, `SkillFocus`, the guaranteed
   test.
4. **Teleport** — Point targeting, skill ABI v3, brain ABI v6, the nav window.

## Declared but not executed

- **Skin Game and Rob Grave**, above — they need an economy substrate that does
  not exist.
- **Precision Shot spawns no projectile.** It resolves instantly at the end of
  its focus, so a 30 m shot has no visible tracer. Adding one means an effect op
  that spawns a projectile whose damage lands on arrival, which the batch
  contract cannot express today.
- **Nobody sees through Sneak.** The user's stated intent is that some creatures
  eventually will; today imperceptible means imperceptible to everything.
- **Teleport ignores line of sight.** A hero may blink through a wall to any
  passable cell in range; the walls stop pathing, not teleports.
- **`hero_desc` still reads the compiled catalog for guild-recruited heroes**,
  so a `creatures.json` grant edit reaches an arena hero and not a recruited
  one. Inherited from the skills slice, unchanged here.
- **Monsters do not level**, so a level-8 hero against a level-1 monster is a
  rout. That is the point of printing the level in the log line rather than
  hiding it.
