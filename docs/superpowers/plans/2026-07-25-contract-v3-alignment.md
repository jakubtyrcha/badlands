# Contract v3 Alignment (Slice B2) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ship the canonical doc's v3 semantics on `feat/intention-contract`: `bl_enqueue_action` with `BL_ACT_ATTACK` live, single-gateway combat (`combat_preempt` deleted; every swing is a brain action; defense is passive-only), engagement-as-intention execution, tiered wake cadence, and always-restate/resume-diff — with rats/goblins issuing intentions/actions through the SAME API via a simple in-engine brain.

**Architecture:** Six tasks ordered to keep every commit green: (1) ABI v3 wire + brainhost action import, behavior-neutral; (2) the engine action resolver + explicit-index Attack plumbing, inert until producers exist; (3) cadence + restate-resume engine semantics; (4) `hero.nim` v3 behavior (restate + swings) with a transitional swing-suppression guard; (5) the single-gateway cutover — delete `combat_preempt`, engagement executor, monster simple brain, test migration; (6) doc badge flips + corrections + gates.

**Tech Stack:** C++23/EnTT, Rust brainhost (wasmtime; production change this time — the import), Nim→wasm32-wasi (`scripts/build_brains.sh`, LFS artifacts), Catch2.

Spec: `docs/superpowers/specs/2026-07-25-contract-v3-alignment-design.md` (commit 3db8cee). Canonical doc: `docs/design/intention-contract.html`.

## Global Constraints

- **Defense is PASSIVE mechanically** — `resolve_attack`'s defender gates only. **Attacking is an ACTION**: no entity swings without a brain deciding it; brainless heroes issue nothing (idle, passive gates, may die). **No host attack policy on behalf of any agent.**
- **One API for all acting entities**: simple in-engine brains (monsters) and wasm brains feed the SAME `apply_intention` + action-resolver seams, same validation, same logged commands. The tier split is where the brain runs, never which door it uses. `combat_preempt` is deleted, not relocated.
- Actions validated **at resolve time, per action**, in enqueue order (cooldown, index, range, MeleeLock category — no ranged while locked); invalid → `spdlog::warn` + drop, batch and intention unaffected. Enqueueing is not permission.
- Cadence: threats in view OR MeleeLock → consult every tick; idle hint > 0 honored; hint 0 → `kDefaultWakeCadenceMillis = 1000` (sim ms). Rejection backoff (500 ms) unchanged.
- Resume: identical restated intention = explicit no-op resume (nothing logged); `hero.nim` always restates.
- Determinism contract unchanged: replay never consults brains nor re-resolves actions; swings replay from logged `Attack` commands (now carrying the explicit index in `param_a`; `-1` = legacy auto retained for log compatibility, no live producer emits it for agents).
- Wire discipline: int64-first, explicit pads, both-side sizeof/offset asserts; append-only vocabularies; `abi.nim` hand-mirrored; artifacts LFS, staged deliberately, `build_brains.sh` idempotent.
- brainhost: the crate's production code MAY change this slice (the import) — keep the delta minimal: `BhActionFn` + `bh_instantiate` params + linker definition + import allowlist extension; nothing else.
- Commits `feat(game)`/`refactor(game)`/`docs` + trailer `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>`. Canonical runner `ctest --test-dir build` from repo root.

## Context (key existing code)

`game/src/intention.{h,cpp}`: `Intention`, `apply_intention` (validate/adopt, per-kind field zeroing, force-logged wake schedule via SetBehavior `param_b`), `note_think_outcome` (seq bookkeeping + rejection backoff), `abort_intention`, `advance_intentions` (kind-dispatched lifecycle), `should_wake` (seq-based event clause + deadline + none-clause). `game/src/sim.cpp`: think dispatch (`combat_preempt` at ~:56-77 — DELETED in task 5; ThreatSighted writer pass + `nearest_enemy_scratch` cache whose sole consumer is combat_preempt — the cache's consumer moves), `mock_think` (monster leg). `game/src/monster_brain.cpp` (~100 lines): building-gnaw via `AttackBuilding` (KEPT this slice) + direct MoveTarget writes; unit combat currently reaches monsters via `combat_preempt`. `game/src/combat.{h,cpp}`: `fire_attack(game, attacker, target)` authoritative, `select_attack`/`pick_attack` (host picking policy — retired for live producers; `pick_attack` stays as a pure helper simple brains may call as THEIR policy). `game/src/command.cpp:113-120`: Attack handler → `fire_attack`. `src/crates/brainhost/include/brainhost.h:119`: `bh_instantiate(p, expected_abi_version, world_seed, log_fn, log_user)`; import allowlist currently "at most env.bl_log". `game/src/wasm_brain.cpp`: `forward_log` callback precedent (:38-55); `tick_wasm_brain` think flow. `scripts/brains/nim/`: `abi.nim` (mirror), `hero.nim` (react-on-wake; already emits `BL_INT_ATTACK` as its danger response), `blocks.nim` (reads wire factors). Components: `EventInbox`/`CurrentIntention` are hero-spawn-only today (`game/src/heroes.cpp` hero branch) — task 5 extends them to monsters.

---

### Task 1: ABI v3 — wire + brainhost action import (behavior-neutral)

**Files:**
- Modify: `game/src/brain_abi.h` (v3), `scripts/brains/nim/abi.nim` (mirror), `scripts/brains/nim/hero.nim` + fixtures `idle_test.nim`/`trap_test.nim` (recompile against v3 — no behavior change), `game/tests/brain_abi_tests.cpp`
- Modify: `src/crates/brainhost/include/brainhost.h`, `src/crates/brainhost/src/lib.rs` (+ its tests)
- Modify: `game/src/wasm_brain.cpp` (pass the new instantiate params; pack the attacks block; callback stub appends to the runtime's pending list — resolver arrives in Task 2), `game/src/wasm_brain.h` (`WasmBrainRuntime` gains the pending-actions sink)
- Rebuild: `assets/brains/*.wasm` + test fixtures via `scripts/build_brains.sh`

**Interfaces (produces):**

```c
/* brain_abi.h */
#define BL_ABI_VERSION 3
#define BL_ACT_NONE 0
#define BL_ACT_USE_SKILL 1   /* reserved */
#define BL_ACT_USE_POTION 2  /* reserved */
#define BL_ACT_ATTACK 3      /* live: arg = attack index, target = victim slot
                                (UINT32_MAX = current Attack-intention target) */
/* View gains the attack loadout (a brain cannot pick what it cannot see): */
typedef struct BlViewAttack {
    int32_t category;      /* AttackCategory */
    int32_t damage_type;   /* DamageType */
    float base_damage;
    float range;
    float cooldown_remaining;  /* seconds; 0 = ready */
    uint32_t _pad;
} BlViewAttack;                /* 24 bytes */
/* BlViewWire gains, after the statuses block (keep int64-first rules per
   containing struct; recompute + re-assert all sizes/offsets both sides):
     int32_t attack_count; uint32_t _padN; BlViewAttack attacks[BL_MAX_ATTACKS(3)]; */
```

```c
/* brainhost.h */
typedef void (*BhActionFn)(int32_t kind, uint32_t target_slot, int32_t arg, void* user);
BhInstance* bh_instantiate(const BhProgram* p, int32_t expected_abi_version, int32_t world_seed,
                           BhLogFn log_fn, void* log_user,
                           BhActionFn action_fn, void* action_user);
/* import allowlist becomes: at most env.bl_log + env.bl_enqueue_action; a
   module importing neither still instantiates (fixtures). */
```

```cpp
/* wasm_brain.h — the sink (user ptr is fixed at instantiation, so it lives on
   the runtime; tick_wasm_brain clears it before each bh_tick): */
struct PendingAction { int32_t kind; uint32_t target_slot; int32_t arg; };
std::vector<PendingAction> pending_actions;  // this wake's enqueues, in call order
```

- `hero.nim`/`abi.nim`: mirror the view addition + `proc bl_enqueue_action(kind: int32; target: uint32; arg: int32) {.importc.}` declared but NOT yet called. Fixtures updated to `BL_ABI_VERSION 3`.
- Nim-side size `doAssert`s updated; `brain_abi_tests.cpp` re-pins sizes/offsets (compute real values during implementation — the layout RULES are the contract, don't guess totals in this plan).
- brainhost lib.rs: define the linker import `env.bl_enqueue_action` forwarding to the callback (mirror the `bl_log` shim); extend the import allowlist; crate tests: callback receives calls in order; a module importing neither still instantiates; version gate now expects 3.

**TDD:** brain_abi_tests red on v3 asserts → implement → green; brainhost `cargo test --manifest-path src/crates/brainhost/Cargo.toml --lib` including a new ordered-callback test; full ctest green (no behavior change — hero.nim recompiled, silent on actions). Commit: `feat(game): ABI v3 — bl_enqueue_action import + attack-loadout view block`.

---

### Task 2: Engine action resolver + explicit-index Attack plumbing (inert)

**Files:**
- Create: additions to `game/src/intention.h/.cpp` (the action side of the contract lives with the intention seams)
- Modify: `game/src/combat.h/.cpp` (`fire_attack` explicit-index param), `game/src/command.cpp` (Attack handler passes `param_a`), `game/src/command.h` (comment: Attack `param_a` = attack index, -1 auto)
- Test: `game/tests/intention_tests.cpp` (+ combat_tests row for explicit-index fire)

**Interfaces (produces):**

```cpp
/* intention.h */
struct AgentAction {              // host-native BL_ACT_* (append-only mirror)
    int32_t kind = 0;             // BL_ACT_*
    uint32_t target_slot = UINT32_MAX;
    int32_t arg = 0;              // BL_ACT_ATTACK: attack index
};
// The single action gateway (wasm callback drain AND simple brains call this):
// validates ONE action at resolve time against the world as it stands NOW —
// BL_ACT_ATTACK: index in [0, Attacks.count), cooldown_remaining <= 0,
// category legal under MeleeLock (no Ranged while locked), target resolvable
// (UINT32_MAX -> the actor's CurrentIntention Attack target; else the named
// slot) and within that attack's range. Valid -> pushes the logged Attack
// command carrying the explicit index (param_a) and returns true. Invalid or
// reserved kind (USE_SKILL/USE_POTION) -> spdlog::warn + false. Never touches
// CurrentIntention.
bool resolve_action(BadlandsGame& game, uint32_t slot, const AgentAction& action);
```

```cpp
/* combat.h — fire_attack gains the explicit pick: */
// attack_index >= 0: fire exactly that attack (re-validated: range/cooldown/
// lock as always); -1: legacy auto-pick via select_attack (log compatibility;
// no live producer emits -1 for agents after task 5).
void fire_attack(BadlandsGame& game, uint32_t attacker_slot, uint32_t target_slot,
                 int32_t attack_index = -1);
```

- Command flow: `resolve_action` pushes `{CommandKind::Attack, slot, target, param_a = index}` through the queue (drained same tick, ordered — actions resolve in enqueue order because the queue is FIFO and `resolve_action` is called in pending order). Handler: `fire_attack(game, cmd.actor, cmd.target_id, cmd.param_a)`.
- `tick_wasm_brain`: after `apply_intention`/`note_think_outcome`, drain `runtime->pending_actions` in order through `resolve_action` (soft convention documented: warn-free; multiple actions allowed).

**TDD (all red-first):** on-cooldown drop (warn, false, no command); index out of range; ranged-while-locked drop; UINT32_MAX resolves to the intention target; in-batch self-invalidation (two ATTACK actions same wake: first fires + starts cooldown, second drops); the logged command carries the index and `fire_attack` uses exactly that attack (assert via cooldown of THAT index consumed); reserved kinds warn+drop. Full ctest green (nothing produces actions yet). Commit: `feat(game): action resolver — BL_ACT_ATTACK with resolve-time validation`.

---

### Task 3: Cadence + restate-resume semantics

**Files:**
- Modify: `game/src/intention.cpp` (`should_wake`, `apply_intention`), `game/src/components.h` (`kDefaultWakeCadenceMillis = 1000` beside the backoff constant)
- Test: `game/tests/intention_tests.cpp`

- `should_wake` high-stakes clause (first): agent's threats present (reuse the ThreatSighted pass's per-tick knowledge — `EventInbox::threat_was_present` is the already-maintained per-tick flag; document that choice) OR `MeleeLock` component present → true every tick.
- Hint default: in `apply_intention`'s adopt tail (and `note_think_outcome`'s rejected path — verify both), a hint of 0 arms `wake_at = now + kDefaultWakeCadenceMillis` instead of 0/"no deadline". Idle-intention duration keeps its own semantics (duration 0 = until woken stays? NO — spec: hint 0 = no preference → 1 s. Apply the same default to a duration-0 Idle: `wake_at = now + kDefaultWakeCadenceMillis`; update the B2-era "idle until woken" comment + its pinning test to the v3 rule).
- Restate-resume: `apply_intention` first checks "identical to running intention" (same kind + same point/target/arg for the kind's live fields) → return true WITHOUT re-executing producers or re-stamping `started_at`; refresh only `wake_at` from the hint (a restate is also a fresh yield with a fresh hint). Comment: the edge-triggered producers already dedup commands; this makes resume an explicit contract behavior.

**TDD:** cadence truth table (threat present → wake every tick even mid-Idle-with-long-hint; melee-locked → every tick; hint 0 → next wake exactly +1000 ms; 4-hour hint honored absent events; guaranteed event still pre-empts); duration-0 Idle now re-wakes at +1000 (updating the old pinning test); identical restatement: no new commands in the log, `started_at` unchanged, `wake_at` refreshed. Full ctest. Commit: `feat(game): tiered wake cadence + explicit resume-on-restate`.

---

### Task 4: hero.nim v3 behavior (restate + swings) with transitional guard

**Files:**
- Modify: `scripts/brains/nim/hero.nim` (+ `hero_view.nim` to unpack the attacks block + statuses)
- Modify: `game/src/sim.cpp` (TRANSITIONAL: `combat_preempt` swing-enqueue suppressed for a wasm hero whose wake just resolved a valid ATTACK action this tick — a one-line guard with a `// deleted in the single-gateway task` comment; engagement half untouched)
- Rebuild artifacts; Test: `game/tests/wasm_brain_tests.cpp`, `game/tests/hero_behavior_tests.cpp`

- `hero.nim`: (a) always restate the current goal each wake (the engine diffs — Task 3); (b) on combat wakes (threats visible or `BL_ST_MELEE_LOCKED`): keep/restate `BL_INT_ATTACK(target)` and enqueue ONE `bl_enqueue_action(BL_ACT_ATTACK, target, best)` where `best` = highest `base_damage` among attacks with `cooldown_remaining <= 0`, category legal (no Ranged when the locked status is present), `range` covering the threat distance; if none ready, no action this wake (soft one-action convention observed either way).
- Rebuild via `build_brains.sh`; idempotency check.

**TDD:** wasm integration — a wasm hero vs a spawned rat: the command log gains `Attack` commands with explicit `param_a >= 0` (brain-picked), exactly one swing per ready-window (no double-swing with the transitional guard — pin it); the hero prefers the higher-damage ready attack (author a two-attack hero desc in the test; assert the logged index); restatement produces no log churn during steady state. Full ctest. Commit: `feat(game): hero brain picks its swings (BL_ACT_ATTACK) and restates intentions`.

---

### Task 5: Single-gateway cutover — delete combat_preempt, engagement executor, monster simple brain, test migration

**Files:**
- Modify: `game/src/sim.cpp` (delete `combat_preempt` + the transitional guard; `mock_think` monster leg calls the new simple brain; the `nearest_enemy_scratch` cache's consumer note — the cache now feeds the simple brains/executor or is folded into the ThreatSighted pass consumers; keep exactly one scan per agent per tick)
- Modify: `game/src/intention.cpp` (`advance_intentions` + `apply_intention` Attack-intention leg: EXECUTION of "fight that" — maintain the engagement `MoveTarget` at `engagement_range(cb, atk)` toward the live target each tick while the intention runs; abort → `IntentionEnded` on dead/gone target, exactly the MoveTo pattern)
- Modify: `game/src/monster_brain.cpp` (simple brain: per tick — pick target via `nearest_enemy`; `apply_intention(Attack(target))` restated; `resolve_action(BL_ACT_ATTACK, target, pick)` where pick may reuse `pick_attack` as ITS policy helper; no unit target → existing building-gnaw path unchanged (`AttackBuilding`))
- Modify: `game/src/heroes.cpp` (`EventInbox` + `CurrentIntention` emplaced for `Archetype::Monster` too — the simple brain runs through the same seams)
- Tests: migration per the fallout list below
- Modify: `game/src/combat.cpp` (`select_attack`'s doc comment: host picking retired for live producers; `pick_attack` = a policy helper brains may call)

**Test fallout map (verify each; the theme is one-directional duels for brainless heroes):**
- `events_tests.cpp` (Merc-vs-Goblin trading blows): goblin (simple brain) still swings; brainless merc only rolls passive gates → assert the asymmetry (DamageDealt only goblin→merc; merc never appears as attacker; HeroDowned still fires) or flip the merc to a wasm world where two-directional matters.
- `combat_tests.cpp` / `sim_tests.cpp` / `movement_tests.cpp` duel-shaped cases: same treatment — spawn-driven mechanics tests that need a hero to SWING must either drive `resolve_action`/Attack commands directly (mechanics under test, not decisions) or use `make_wasm_world`.
- `rat_tests.cpp`: behavior parity in spirit — rats still fight heroes (now via intention+action; assert Attack commands with explicit index from the rat) and still gnaw buildings.
- `hero_behavior_tests.cpp` "rat interrupts idle": engagement must now arrive via the brain's Attack intention through the executor (assert `CurrentIntention.kind == Attack` + engagement MoveTarget), not combat_preempt.
- `wasm_brain_tests.cpp`: drop/replace any case that referenced combat_preempt's pre-empt-before-think ordering.
- Determinism suites: replay reproduces all swings from logged commands (no brain, no resolver on replay) — the fog-explorer + run-twice gates must stay green; add a fight-replay case NOW that combat decisions are fully logged (this closes the pre-existing "no suite replays a fight" hole from the ledger — combat_preempt's unlogged MoveTarget writes are gone, engagement comes from the logged intention path).

**TDD shape:** land the executor + simple brain green FIRST (combat_preempt still present but now redundant for monsters — verify no double-swing: the simple brain suppresses when... simpler: cut monsters over and delete combat_preempt in the same commit, using the suite red-list as the worklist), then the deletion, then migrate per the map. Commit: `refactor(game): single-gateway combat — every swing is a brain action`.

---

### Task 6: Docs + gates

- Doc badge flips (`docs/design/intention-contract.html`): cadence, resume/restate, `bl_enqueue_action`, `BL_ACT_ATTACK` → shipped; USE_SKILL/USE_POTION keep reserved badges. Correct every "host combat defends brainless heroes" phrasing → passive-only defense (doc + spec cross-refs + CLAUDE.md if present). Add the single-gateway statement + statuses-as-validation-constraints line if not already present. Self-containment + tag-balance checks.
- CLAUDE.md repo-state line updated (combat is brain-driven through the intention/action gateway; defense passive).
- Gates: full `ctest --test-dir build` green; `cargo test --manifest-path src/crates/brainhost/Cargo.toml --lib`; headless smokes (`badlands_game --screenshot`, 30 s ai_sandbox — heroes fight rats visibly); `build_brains.sh` idempotent; doc-vs-code drift check (BL_ACT_/BL_INT_/BL_EV_/BL_ST_ tables + attacks block vs `brain_abi.h`).
- Copy this plan to `docs/superpowers/plans/2026-07-25-contract-v3-alignment.md` and commit (plan mode barred writing it there now).
- Commit: `docs: v3 shipped — badge flips + passive-defense corrections`.

## Verification (end of plan)

1. `ctest --test-dir build` all green (record counts in the ledger); brainhost crate tests green with the rebuilt real `hero.wasm`.
2. The new fight-replay determinism case is the load-bearing gate: a battle replays bit-identically from the log with no brains and no resolver.
3. Behavior: ai_sandbox — heroes engage and damage rats again (two-directional, brain-driven both sides); a brainless-hero world shows the merc passive-only.
4. `build_brains.sh` idempotent; `git status` clean.

## Out of scope

Skills/potions execution (BL_ACT rows stay reserved), critter/townfolk migration to the intention layer, boss multi-action patterns, retiring vestigial think_* factors and `BlViewSelf.think_until_millis` (next ABI window), LLM/NN agents.
