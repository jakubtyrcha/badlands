# Core Classes — Combat Design

Brainstormed 2026-07-31, against `/Users/jakub/repos/docs/game-design.html` §5.2,
§5.3, §6. Follows the skills-execution slice
(`2026-07-31-skills-execution-and-statuses-design.md`), which shipped the skill
contract and the status subsystem this design builds on.

## Goal

The four core classes fight as themselves: stats that differ and grow with
level, attacks that cost time to throw, ranged brains that keep their distance
because contact is ruinous, and one combat skill each. Roughly — exact balance
is explicitly not the deliverable; the duel matrix is.

**Validation:** 1v1 duels at level 1. The Mercenary wins every class duel; the
Apprentice loses almost everything (beating only the Rat); Hunter and Grave
Robber land near even.

**Out of scope:** exact balance, the Dogma classes (Cleric/Inquisitor/
Crusader), the Ghost/esoteric line (needs ethereal damage), the psychology
layer (`willpower`, already reserved in `Combatant`).

## 1. Where a class's identity lives

The design doc's rating table divides along a line the engine already draws:

| Doc rating | Delivered by |
|---|---|
| Damage: `dps`, `ranged` · Survival: `armour`, `evasion`, `hitpoints` | **numeric stats** |
| Damage: `aoe`, `armour-piercing`, `ethereal` · all of Utility | **skills** |
| Survival: `willpower` | deferred |

This is why the Apprentice's Σdamage of 10 does not belong in its bolt: its
curve is spells. A stats-only Apprentice will visibly undershoot its intended
power at level 15, and that is correct, not a bug to tune away.

## 2. Stats and level progression

`stat = base + growth × (level − 1)`. Both rows are authored per creature in
`creature_catalog.cpp` and overridable by name from `assets/creatures/
creatures.json`, exactly like every other `CharacterDesc` field.

`CharacterDesc` gains a `StatGrowth` block; it is copied onto the entity at
spawn as a component, the same pattern `SkillGrants` already uses, so the
level-up hook can recompute without re-deriving a class. `apply_level_stats`
recomputes `Health.max`, `Combatant`, `Attacks[].base_damage`, and the `Stats`
mirror; it runs at spawn and inside `award_xp`'s level loop, beside
`grant_skills_for_level`. Current HP scales with the max so a level-up is not a
free heal to full.

Level-1 base (rough). The Grave Robber's crossbow is the one real change: at
`cd 0.6` it out-DPSes the Hunter's bow and wins that duel outright, against the
doc's "opener, then blades".

| | Mercenary | Hunter | Grave Robber | Apprentice |
|---|---|---|---|---|
| hp / speed | 30 / 2.5 | 22 / 3.2 | 24 / 2.8 | 16 / 2.4 |
| accuracy | .90 | .85 | .85 | .90 |
| defense | .10 | .05 | .10 | 0 |
| evasion | .05 | .15 | .20 | .05 |
| armour | 3 | 1 | 1 | 0 |
| primary | sword slash 6, r1.5, cd1.0, crit.10 | bow pierce 5, r8, cd1.2, crit.25 | crossbow pierce 5, r5, **cd2.5**, crit.30 | bolt pierce 4, r6, cd1.5, crit.10 |
| secondary | — | knife slash 3, r1.2, cd0.8, crit.10 | blades slash 4, r1.3, cd0.7, crit.20 | — |

Growth per level, the doc's level-15 rating times a per-stat step:

| stat | step per rating point | Merc | Hunter | GR | Appr |
|---|---|---|---|---|---|
| hitpoints (all rated 1) | +1.5 | +1.5 | +1.5 | +1.5 | +1.5 |
| armour (2/1/1/0) | +0.10 | +0.20 | +0.10 | +0.10 | 0 |
| evasion (1/1/2/0) | +0.004 | +.004 | +.004 | +.008 | 0 |
| base_damage, dps (1/2/2/2) | +4% of base | +4% | +8% | +8% | +8% |
| accuracy | +0.005 flat, all classes | | | | |

Two properties fall out of the existing combat math rather than needing
machinery:

- **Linear stat growth gives a convex power curve.** Power ≈ dps × ehp, a
  product of two rising terms, matching the doc's keyframes without an
  authored curve.
- **Armour self-plateaus.** Flat reduction loses relative value as damage
  climbs — the doc's "armour scaling hits diminishing returns", for free.

## 3. An attack takes time

`Attack` gains `wind_up_seconds` and `recovery_seconds`. `cooldown` keeps its
current meaning (time between uses of this attack, measured from resolve), with
recovery running inside it, so no third redundant number per weapon.

Declaring an attack emplaces a `PendingStrike` carrying the resolve deadline
and the attacker's captured stats — the pattern `advance_projectiles` already
uses, where the attacker is captured at fire time and the defender read at
resolution. Through the wind-up the actor does not move and is not woken to
think. At the deadline the strike resolves (melee damage, or a projectile
spawns) and the recovery lock begins; recovery blocks movement and thinking
but cannot be cancelled.

**The wind-up is cancellable.** A stun landing mid-wind-up drops the
`PendingStrike` with no damage — which is what turns Stunned from a debuff into
an interrupt and gives shield-bash a second purpose.

Indicative wind-up / recovery: sword .35/.25, bow .60/.30, crossbow .50/.40,
bolt .80/.35, knife .25/.15, blades .25/.15.

This is also the tax that makes kiting cost distance. A Hunter shooting on a
1.2 s cycle with a .6 s wind-up retreats at an effective 1.6 u/s against the
Mercenary's 2.5 — so the Mercenary closes, and wins that duel with roughly two
thirds of its health.

## 4. Melee contact, and the price of leaving it

`MeleeLock` today is a hard freeze: contact locks you, `follow_paths` skips you
entirely, and the lock releases only past `reach × hysteresis` — a distance you
can never reach, because you cannot move. A ranged unit that is touched once is
trapped for the rest of the fight, so "move, shoot, move" is unreachable.

New meaning: **`MeleeLock` forbids ranged attacks while in contact, and no
longer freezes movement.** Leaving contact is possible, and prohibitively
expensive:

- `StatusKind::Disengaged` — a few seconds during which the entity may take no
  action at all: no attack, no skill. Enforced at `resolve_action` (the brain's
  channel) and re-checked in the `Attack`/`UseSkill` command handlers, the same
  double-gate the skill slice uses.
- It is stamped mechanically, not requested. An entity that was melee-locked,
  is out of contact this tick, and got there **by its own path-following**
  earns it. If the contact broke because the other side left or died, it does
  not. Only `follow_paths` movement counts — a collision nudge from
  `separate_units` never triggers it.

No new verb: retreating is movement, the brain already expresses movement, and
the engine charges for the outcome. The intended consequence is that a brain
that can count never disengages — the Hunter's real job is spacing, and being
caught is a genuine loss condition rather than an inconvenience.

## 5. Combat behaviour is a brain decision

Standoff distance is tactics, so it lives in the brain; the engine grows no
kiting policy. What the engine owes the brain is enough information to decide,
and `BlThreat` today is `{pos, dist, slot}` — nothing about what it is standing
off from.

**ABI v5:** `BlThreat` gains `reach` (the threat's longest melee range),
`ranged_reach`, and `move_speed`. `BL_ST_DISENGAGED` joins the status
vocabulary. Mirrored in `abi.nim`/`hero_view.nim` and `brainhost`'s wire
constants, as v4 was.

`hero.nim` gains a skirmish block for a hero holding a ranged attack that
outranges its threat: hold outside `threat.reach + margin`, shoot when the
window allows, and step back when the margin closes. It never chooses to break
contact — with `Disengaged` on the table that is always the losing move.

The engine-side monster brains get the mirror of this for the Bandit Archer,
written in `monster_brain.cpp` — the tier split is where a brain runs, never
which seam it uses.

## 6. One combat skill per class

On the shipped skill contract: trigger/target/cooldown/duration/effect plus
named constants in `assets/skills/skills.json`. Levels follow the doc.

| Class | Skill | Level | Shape |
|---|---|---|---|
| Mercenary | Shield Bash | 3 | shipped — melee test, stuns on a hit |
| Hunter | Dress Wounds | 2 | action, self, long cooldown, long wind-up; heals a constant |
| Grave Robber | Backstab | 3 | action, any, melee test; large bonus damage when the target is not currently engaging the caster |
| Apprentice | Curse | 1 | action, any, no test; applies `Cursed` — accuracy and armour down for a duration |

New in the contract: one op kind, `BL_FX_HEAL`; two statuses, `Cursed` and
`Disengaged`. Each exercises a different direction of the effect wire —
restore, conditional damage, debuff.

Deferred, and named so: Precision Shot (5), Sneak (3, needs a stealth
mechanic), Teleport (8), Skin Game and Rob Grave (economy, not combat), and
Calcify's own effect — it still grants, at level 4 per the doc rather than the
5 currently compiled, and still does nothing.

## 7. Enemy variation

Against the doc's threat anchors, and chosen so each one tests something:

| Creature | Threat | Tests |
|---|---|---|
| Rat | 0.25 | shipped — the Apprentice's only win |
| Goblin | ~1 | shipped |
| Bandit | 2 | a level-1 hero's fair fight; armed and armoured like a hero |
| Bandit Archer | ~2 | the enemy-side kiter — the skirmish behaviour from the other side |
| Bandit Leader | 5 | beats every level-1 core class; the matrix's ceiling |
| Mud Golem | ~6 | Goliath: huge hp, heavy armour, slow **blunt** — the only exercise of the blunt-vs-armour path |

### Threat has two roles

Threat approximates a creature's combat potential — what it is worth in a
fight. It lives in a compiled table of authored anchors
(`game/src/threat_table.h`), keyed by creature and level, never derived from
stats. One number, deliberately serving two roles:

1. **A source of calibration.** The anchors are FIXED targets. The duel matrix
   measures the gap between what a creature is supposed to be worth and what it
   actually wins; closing that gap means moving the STATS toward the anchors,
   never the anchors toward the stats. Deriving threat from stats would make the
   report circular — it would validate the numbers against a restatement of
   themselves.
2. **A source of brain decisions.** Because role 1 keeps it a fair
   approximation, a brain can compare its own threat against a hostile's and be
   smart about whether a fight is worth taking. Both sides ride the brain wire
   for exactly this.

The dependency runs one way: **role 2 is only as trustworthy as role 1 has been
done.** A brain deciding on a badly-calibrated number makes confident, wrong
choices — which is why the calibration report exists before any fleeing
behaviour does.

Threat never feeds combat resolution. It approximates the outcome, so it must
not become an input to it.

Anchors at level 1: Mercenary 2.5, Hunter 1.5, Grave Robber 1.0, Apprentice
0.75, Rat 0.25, Goblin 1, Bandit and Bandit Archer 2, Bandit Leader 5, Mud
Golem 6, Deer 0. These supersede this document's own power-curve keyframes
where they differ — notably the Hunter and Grave Robber, which the curves had
sharing a line and these split.

## 8. Validation

Balance is an approximation, not the deliverable. Outcomes are **reported**,
never asserted into a target: tests cover mechanism (growth arithmetic, wind-up
cancellation, disengage gating, determinism), and a headless tool
(`badlands_duelsim`) covers behaviour at scale.

- **A duel matrix.** Every pairing of the four classes with each other and with
  the six monsters, at level 1, staged in an arena and run to resolution. Each
  pairing runs several times at varied starting separations — combat rolls are
  seeded off `(attacker, target, world_millis, attack_index)`, so a different
  closing time is a genuinely different roll stream. Written as a markdown
  table with each side's threat target in the headers.
- **A calibration report.** One row per pairing: both threat targets, their
  difference, and the observed win rate, sorted by difference. This reads the
  design doc's invariant straight off the top (equal threat should sit near
  50%) and answers its open question — how a threat difference shifts the
  expected win ratio — empirically rather than by modelling it.
- **Parameter charts** (SVG, self-contained, no external references): the
  authored threat curve, the observed win-rate-vs-threat-difference scatter,
  and level → hp / damage / armour per class straight from the growth rows.

No balance assertion anywhere. The threat table is the fixed post; the matrix
and the calibration report measure the distance to it.

## 9. Ordering

Four slices; the matrix only means anything once all four exist.

1. **Stats and progression** — `StatGrowth`, `apply_level_stats`, the rebalanced
   level-1 rows, JSON schema, the new enemies.
2. **Attack commitment** — `wind_up`/`recovery`, `PendingStrike`, stun
   cancellation.
3. **Contact and disengage** — `MeleeLock`'s new meaning, `Disengaged`, ABI v5,
   the skirmish behaviour in `hero.nim` and the Bandit Archer.
4. **Class skills** — Dress Wounds, Backstab, Curse, `BL_FX_HEAL`, `Cursed`.

Then the duel matrix.

## Known limits, stated rather than fixed

- The Apprentice cannot reach its intended level-15 power on stats alone; its
  curve is spells, and only Curse lands here.
- `hero_desc` (`heroes.cpp`) still reads the compiled catalog for
  guild-recruited heroes, so a `creatures.json` edit reaches an arena mercenary
  and not a recruited one — inherited from the skills slice, unchanged here,
  and now covering growth rows as well as stats.
- **Fleeing is a follow-up.** Threat's second role is fully plumbed — both
  sides of the comparison ride the wire, and the standoff decision already
  spends it — but no brain runs away yet. It sits deliberately downstream of
  the calibration report.
- **One-vs-many calibration stays open.** Every duel here is 1v1, which is the
  case the design doc's invariant is stated for.
