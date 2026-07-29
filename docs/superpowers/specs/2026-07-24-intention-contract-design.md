# The Intention Contract (Slice B) — Design

Approved in brainstorming 2026-07-24. Successor to the direction note in
`2026-07-24-skill-templates-hud-inspection-design.md`; supersedes both the
"brain owns combat" idea and the twin-brain parity architecture.

## Goals (the user's, verbatim in spirit)

- The brain models decision making. **There is no second C++ brain.**
- The host models execution of intentions; it modifies world state, and that
  state (plus events) is the only interface back to the brain.
- The brain **suggests**; the **engine decides** — it validates suggestions,
  may ignore them (warn + ignore for invalid ones, e.g. a skill on cooldown),
  and may bypass the brain entirely (statuses like a future Incapacitated).
- Results are deferred BY DESIGN: the engine collects intentions, resolves
  them in its own pass, and the loop repeats.
- The brain–world boundary is an INTERFACE: replaceable later by an LLM or a
  neural network. This is why the brain is consulted sparsely (wakeups), not
  30×/second.
- The engine provides **wakeup guarantees** (damage taken, skill applied,
  ...); the brain provides **idleness hints** to help scheduling. A brain
  that wanted to idle 4 hours must tolerate being woken early (spurious
  wakeups are contractual).
- Lack of a brain = idleness. The proven Nim brain is THE hero brain; the
  C++ mock fallback is dead weight and is deleted, not preserved.

## 1. The interface — wire v2 (`BL_ABI_VERSION 2`)

A clean redesign of the wire pair in `game/src/brain_abi.h`, hand-mirrored in
`scripts/brains/nim/abi.nim` as today (size/offset asserted on both sides).

**In (state view):**
- Self: needs, hp fraction, clock, position, current-intention summary
  (what the engine is executing for this hero, if anything).
- **Statuses**: a bounded list of `{kind, remaining_millis}`. The vocabulary
  is append-only; v1 populates from existing conditions (Chatting,
  MeleeLocked, InsideBuilding). Curses/shields join with the skills slice.
  The brain may inspect or ignore them.
- Perception suggestions (host-side, unchanged in spirit): threats
  (nearest-first), roam/explore goals, home/tavern/apothecary locations,
  chat partner, prey.
- Factors: the tuning scalars the brain's scoring reads (as today).
- Known characters (entity memory), as today.
- **Timed-event inbox**: a bounded ring of `{kind, source_slot, param,
  ttl_millis}`. Engine-written; TTLs decrement each tick; expired entries
  drop. Sticky by design: a sleeping brain still sees what happened when it
  finally wakes. v1 kinds: DamageTaken, MoveBlocked, ThreatSighted,
  IntentionEnded (completed | aborted). Append-only vocabulary.

**Out (suggestion):** exactly ONE intention per wake:
- `{kind, point, target_slot, arg, duration_millis}` with kind ∈
  `MoveTo | Attack | Shoot | Enter | EnterHome | Buy | Chat | Idle`
  (today's command vocabulary + explicit Idle-for-X; `UseSkill` is reserved
  in the enum and lands with the skills slice).
- An activity label (inspection/histogram only — not semantics).
- An **idleness hint**: "you don't need me for X ms". Scheduling advice, not
  a promise in either direction.

## 2. The engine — decides, executes, wakes

- **Intention executor**: a durable per-hero current intention. The engine
  maps it onto the existing command/movement machinery (event-sourced: the
  accepted intention IS the logged command, as today), detects completion
  and abortion, and writes IntentionEnded into the inbox.
- **Validation**: before execution, the engine checks feasibility. Invalid
  suggestions (unknown target, skill on cooldown once skills exist) are
  ignored with a logged warning — the brain is advisory, never authoritative.
- **Wakeup scheduler**: the brain thinks only when
  (a) it has no active intention, (b) its intention ended,
  (c) a guaranteed-wake event arrived (v1: DamageTaken, ThreatSighted,
  MoveBlocked, IntentionEnded), or (d) its idleness hint expired.
  Spurious wakeups are allowed at any time; the brain must re-decide
  idempotently. Bypass statuses (vocabulary exists from v1; first real
  entries arrive with skills) skip the think entirely.
- **Determinism**: the idleness hint travels IN the logged command (the
  existing deliberation-pause pattern), so the wake schedule is a pure
  function of logged state. Replay skips thinking entirely, exactly as
  today: `state = f(initial config, seed, command log, N ticks)` holds.

## 3. The Nim brain — sole owner

`scripts/brains/nim/hero.nim` is rewritten as `react(state, events) →
suggestion`. It keeps the needs/scoring behavior (reading factors from the
view) but is now the ONLY implementation of hero decision making. No parity
oracle: the acceptance bar is behavior-in-spirit, validated through
ActivityHistogram expectations (hunters hunt; night sends the tired home;
threats interrupt idling) plus targeted wasm-driven behavior tests.

## 4. Deletion — no legacy

- Deleted outright: the C++ hero decision layer — `town_brain`'s hero think
  path, hero activity blocks/selectors/deliberation
  (`game/src/behaviours/*` hero scoring + `act_*` decision parts), the
  twin-brain parity tests, and the mock-hero fallback.
- A hero without a loaded wasm brain **idles** (stands; the host's combat
  execution rules still defend it when attacked). No downgrade path, no shim.
- Stays host-side: perception/world-view building, movement/nav, combat
  execution, needs drain/refill systems, non-hero brains (critter, monster,
  townfolk — they were never "the brain").
- Also deleted (user: "delete the dead weight"): the noiser BRAIN path —
  `BrainRuntime`/`game.brains`, `BrainDesc.noiser_source`, the
  script-downgrade machinery (`noiser_bugs`/`script_intents` counters leave
  `SimStats`), `reload_script`, the legacy per-tick Intent movement loop,
  the shipped brain scripts (`hero.noiser`/`combat_test.noiser`) and their
  fixtures (`duel_test`, `noiser_smoke_tests`). Noiser itself STAYS for
  mapgen/texgen (`noiser-bundle` is a different subsystem and untouched).
- Test migration: suites that drove heroes through the mock (determinism,
  needs/chat/exploration behavior) load the committed `hero.wasm` artifact
  instead; block-level unit tests are deleted with their subjects. Some
  behavioral coverage narrows — accepted.

## 5. Out of scope (next slice: skills-through-the-contract)

Skill execution (UseSkill validation + Calcify status + cooldown ticking),
SkillUsed game events + HUD combat-log lines, skill/status-driven wakeups
("skill applied to you"), bypass statuses with real entries (Incapacitated),
LLM/NN brain implementations (the contract merely makes them possible).

## Public-surface changes (approval recorded here)

- `BrainDesc` loses `noiser_source` (wasm-only); `SimStats` loses
  `script_intents`/`noiser_bugs`; `Sim::ReloadScript` is removed.
- `brain_abi.h`/`abi.nim` wire structs replaced wholesale (v2).
- `Sim` gains nothing new publicly this slice; the contract is internal to
  the sim + brain host.

## Notes

- Branch: `feat/intention-contract` off the merged `feat/wasm-brain-nim`
  (PR #30 merged).
- The ABI version bump is breaking by design; `build_brains.sh` rebuilds the
  committed `.wasm` artifacts (LFS).
