# WASM brain contract — design

Replace the noiser hero brain with a Nim→WASM brain running in a wasmtime host. Co-designed
2026-07-23; amended the same day after PR #18 (banded-activity-ai) landed on main.

## Why

The Noiser-vs-Nim assessment concluded that a wasm-hosted brain restores the properties the noiser
embedding was built for — sandbox, determinism-by-construction, trap containment — as **runtime**
properties, independent of the source language. Nim (mature compiler, stackless closure iterators,
fast compiles) writes the brains now; noiser keeps mapgen/texgen, and its wasm backend (upstream
`noiser-wasm` already runs on wasmtime) can target the same socket later. The host layer is shared
infrastructure either way; the front-end language becomes a per-script choice.

## Memory model

Each character ticks against a **host-maintained view**: a constrained block of memory holding the
characters it can see or saw recently, plus what it knows of buildings. Conceptually a knowledge
sandbox — the entity knows nothing outside its view. The host updates the memory (visibility,
recency decay, eviction); the script views into it, updates itself, and yields a behaviour.

Division of labour (matches the existing `observe_hero` / blocks boundary in
`game/src/town_brain.cpp` and `game/src/behaviours/`):

- **Host (perception):** fills the wire from registry/placement scans exactly as `observe_hero`
  does today, plus the new `EntityMemory` component (seen/recently-seen characters; seeded building
  knowledge — residents know their town at spawn).
- **Script (decision):** the pure layer — activity scores/acts, banded selection, deliberation —
  reading only the view, yielding one decision per tick.
- v1 parity stance: suggestion fields (prey, chat partner, threats, doors) stay registry-computed
  because their radii (hunt 22, chat 18) exceed vision (14); `EntityMemory` is additive and is the
  single seam where knowledge-limiting lands later (as `behaviours/perception.h` already promises).

## ABI v1

Single source of truth `game/src/brain_abi.h` (packed C structs, compile-time caps,
`static_assert`ed sizes), mirrored size-asserted in `scripts/brains/nim/abi.nim`.

**Imports (module → host):** `bl_log(level, ptr, len)` only. Anything else — including leaked WASI
imports — fails instantiation, which keeps last-good. Pathfinding/rng host services may be added
later behind an ABI version bump.

**Exports (host → module):** `bl_abi_version()`, `bl_init(world_seed)`, `bl_spawn(slot, class,
seed)`, `bl_despawn(slot)`, `bl_view_buf()`, `bl_out_buf()`, `bl_tick(slot) -> i32`, `memory`.

**ViewWire (~2 KB, per entity per tick):**
- `ViewSelf` — slot, class, world_millis, tod, night, pos, health_frac, fatigue, content,
  inventory, attack_range, current_activity, think_until_millis, roam_epoch.
- `ViewSuggest` — roam_goal; explore_goal (+has), move_blocked, blocked_point; chat partner
  pos/slot/dist (+has), chatting; prey pos/slot/dist (+has); home/apothecary/tavern doors (+has);
  threats[8] {pos, dist, slot} + count, nearest-first.
- `ViewFactors` — the class's `ActivityWeights` row + the HeroFactors scalars the decision layer
  reads. Re-sent every tick so `Sim::SetFactors` live-tuning keeps working.
- `ViewChars` — the sandbox memory: cap-16 records {slot, archetype, team, last_x, last_z, last_hp,
  visible_now, last_seen_millis} from `EntityMemory`.

**DecisionWire:** `{activity_id, goal_kind, goal, command_kind, command_arg, follow_up_on_arrival,
pause_kind (none/start/continue), pause_duration_millis}` — applied by the shared `town_think`
tail: pause-start ⇒ Think + hold; pause-continue ⇒ nothing; else set_behavior + move_to +
arrival-gated follow-up command.

## Runtime

wasmtime embedded via a new `src/crates/brainhost` Rust staticlib behind a narrow, data-only C ABI
(the `nav` crate pattern). NaN canonicalization on; fuel reset per `bl_tick` with a fixed budget
(exhaustion is a deterministic trap); no WASI; threads/relaxed-simd off. Trap / nonzero return /
fuel-out ⇒ `report_bug` + the entity idles that tick and retries next tick — no downgrade path.

One instance drives all brains (per-slot state, if a brain ever needs it, lives in module tables).
Reload = fresh instantiation (brains restart; accepted). Hot reload of the Nim source is deferred;
keep-last-good load semantics apply from day one.

## Toolchain & artifacts

`scripts/build_brains.sh`: Nim (brew) → C → wasi-sdk clang (pinned release, cached under
`third_party/toolchains/`, not vendored) → `assets/brains/hero.wasm`. The built artifact is
committed via LFS (`*.wasm`) so the repo builds and tests without the Nim toolchain installed.

## Scope

Hero brain only; critters/townfolk/monsters stay on C++ brains. Combat remains a host pre-empt on
both paths. The noiser brain path stays dormant behind the load flag (`BrainDesc { noiser_source |
wasm_bytes }`) — compiled, test-covered, unused by the apps.

## Verification

brainhost crate tests (`.wat` fixtures: trap, fuel, version, imports); `EntityMemory` unit tests;
integration tests (decision→command_log, trap containment, run-twice determinism with wasm on);
**twin-brain parity** — same world/seeds, C++ `town_think` vs wasm brain, identical command logs;
visual check in `badlands_ai_sandbox`.
