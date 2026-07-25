# Contract v3 Alignment (Slice B2) — Design

Approved in discussion 2026-07-25, on `feat/intention-contract` (the branch is
not spec-compliant with its own canonical doc until this lands). Canonical
contract: `docs/design/intention-contract.html` — this slice ships the v3
semantics that doc already describes, plus the combat-authority corrections
decided in review of the design.

## Decisions (user, verbatim in spirit)

- **Defense is PASSIVE mechanically.** A brainless hero defends itself only
  through `resolve_attack`'s defender gates (evasion/defense/armour). No brain
  involvement. Nothing else "defends" anyone.
- **Attacking is an ACTION.** No entity swings without a brain deciding it.
  Brainless heroes issue no actions — they idle, roll passive defense, and
  may die. There is no host attack policy on behalf of an agent.
- **One API for all acting entities.** Rats/goblins issue intentions and
  actions actively via the SAME seams as heroes — through a simple in-engine
  brain ("brain fake"), not a privileged path. The tier split is where the
  brain runs (wasm contract module vs engine code), never which door it uses.
  `combat_preempt` is deleted outright.
- **MeleeLock is an engine mechanic** for validation and penalties (a locked
  character cannot shoot), and it is inspectable by the brain
  (`BL_ST_MELEE_LOCKED` in statuses): statuses tell a brain which validation
  constraints it is currently subject to.
- Cadence/resume/action-channel semantics: as the doc's v3 sections
  (tiered wake guarantees; always-restate + engine diff; `bl_enqueue_action`
  write-only import validated per action at resolve time).

## 1. ABI v3

- `BL_ABI_VERSION 3`; `abi.nim` mirrored; both-side size/offset asserts.
- New host import `bl_enqueue_action(kind: i32, target_slot: u32, arg: i32)`
  — registered by the brainhost crate exactly like the `bl_log` callback
  precedent (host registers an action callback at instantiation;
  `wasm_brain.cpp`'s callback appends `{kind, target, arg}` to the wake's
  ordered action list). Write-only; no return; calling it is not permission.
- View gains the **attack loadout block** (a brain cannot pick what it cannot
  see): `attack_count` + per-attack `{category, damage_type, base_damage,
  range, cooldown_remaining}` × `kMaxAttacks`, int64-first layout rules.
- Action vocabulary `BL_ACT_*` (append-only): `NONE 0`, `USE_SKILL 1`
  (reserved), `USE_POTION 2` (reserved), `ATTACK 3` — **ATTACK ships live**;
  `arg` = attack index into the loadout, `target_slot` = victim
  (`UINT32_MAX` = the current Attack-intention target).

## 2. Single-gateway combat

- **`combat_preempt` is deleted** (both halves: auto-engagement and
  auto-swing). Its logic is redistributed:
  - Engagement movement becomes execution of the durable `Attack(target)`
    intention: the intention executor (`advance_intentions` /
    `apply_intention`) maintains the approach `MoveTarget` at the stance's
    engagement range while the intention runs, aborts on dead/gone target
    (`IntentionEnded`), exactly like MoveTo executes a movement goal.
  - Target picking + swing timing move into brains: the wasm hero brain and
    the simple monster brain both emit `Attack` intentions and per-swing
    `BL_ACT_ATTACK` actions.
- **Action resolver** (engine): resolves the wake's actions in enqueue order,
  during the same tick, each validated AT RESOLVE TIME: attack index in
  range, off cooldown, category legal under MeleeLock (no ranged while
  locked), target in range. Invalid → `spdlog::warn` + drop that action;
  the batch and the yielded intention are unaffected. A valid ATTACK action
  produces the logged `Attack` command carrying the explicit attack index
  (`param_a`; `-1` remains "engine picks" for command-level compatibility,
  but no producer emits `-1` for agents anymore).
- `fire_attack` stays the authoritative resolution point (re-validates,
  cooldowns, projectile spawn). `pick_attack`'s host policy is no longer
  consulted for any brain-driven swing; it survives only inside
  `select_attack` if simple brains reuse it GUEST-side as their picking
  policy helper (allowed: it is decision logic now owned by a brain, called
  by the simple brain, not by an executor).
- **Simple brains (tier doctrine made concrete):** `monster_think` becomes an
  intention/action producer: pick target (nearest living enemy — reusing
  `nearest_enemy`), emit/restate `Attack(target)` intention via
  `apply_intention`, emit `BL_ACT_ATTACK` through the SAME action-resolver
  entry point (a host-side `enqueue_action(game, slot, action)` function —
  the same function the wasm callback feeds). Consulted every tick (engine
  code is cheap); same validation, same logs. Rat building-gnawing
  (`AttackBuilding`) keeps its current command path this slice.
  Deer and tax collectors (non-combat) migrate to the intention layer in a
  later sweep — out of scope here.
- **Brainless heroes:** no intentions, no actions. Passive defense only.

## 3. Cadence + resume (shipping the doc's v3 rules)

- `should_wake`: threats in view (`threat_count > 0`) OR MeleeLock status →
  consult every tick (the engine's high-stakes offer). Otherwise: idle hint
  > 0 honored as given (long yields respected); hint 0 = no preference →
  `kDefaultWakeCadenceMillis = 1000` (sim time, 1× rate). Guaranteed events
  and the rejection backoff (500 ms) unchanged.
- `apply_intention`: an intention identical to the running one is an explicit
  resume no-op (no state change, nothing logged). `hero.nim` ALWAYS restates
  its current goal each wake; the engine diffs.

## 4. Nim hero brain

- Restates its intention every wake. In combat (threats visible / locked):
  emits/keeps `Attack(target)` and enqueues one `BL_ACT_ATTACK` per wake when
  an attack is ready — policy: highest `base_damage` among ready,
  lock-legal, in-range attacks (the old host `pick_attack` preference now
  lives guest-side, informed by the view's attacks block + statuses:
  "I can't shoot, I'm in melee" is readable from `BL_ST_MELEE_LOCKED`).
- Soft one-action convention observed (one swing per wake).

## 5. Doc + spec corrections (same slice)

- Flip shipped v3 badges: cadence, resume/diff, `bl_enqueue_action`,
  `BL_ACT_ATTACK`. `USE_SKILL`/`USE_POTION` keep the reserved badge.
- Correct "host combat still defends brainless heroes" phrasing everywhere it
  appears (doc + CLAUDE.md if present): passive defense only.
- Add the statuses-as-validation-constraints principle and the single-gateway
  statement (no privileged combat path; simple brains use the same API).

## 6. Tests (red/green)

- Cadence truth table: threat present → wake every tick; hint 0 → next wake
  at +1 s; 4-hour yield honored absent events; guaranteed events pre-empt.
- Restate-dedup: identical restatement logs nothing, changes nothing.
- Action resolver: on-cooldown drop (warn, batch continues); in-batch
  self-invalidation (first ATTACK's cooldown rejects a duplicate second);
  ranged-while-locked drop; explicit index flows into the logged Attack
  command and the resolved swing.
- Single gateway: brainless hero NEVER logs Attack commands nor deals damage
  (one-directional duel: goblin swings via its simple brain, merc only rolls
  passive gates); rat/goblin behavior parity-in-spirit (rats still fight
  heroes and gnaw buildings; suite migrations for the one-directional duels).
- Engagement-as-intention: wasm hero with a threat acquires Attack intention
  + approach movement via the executor (no combat_preempt symbol remains).
- Determinism: replay reproduces swings from logged commands alone (actions
  are never re-resolved from brains on replay); run-twice/replay suites
  green; wasm two-run identity.
- brainhost: callback registration + ordering test (enqueues arrive in call
  order); artifacts rebuilt (`build_brains.sh`), idempotent.

## Out of scope

Skills/potions execution (`BL_ACT_USE_SKILL`/`USE_POTION` stay reserved),
critter/townfolk migration to the intention layer, boss multi-action
patterns, LLM/NN agent implementations, retiring vestigial think_* factors
(next ABI window after v3).
