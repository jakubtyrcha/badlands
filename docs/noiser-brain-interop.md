# Noiser brain interop — ideal contract & language improvements

The badlands hero brain is a **composable behaviour framework**: perception-in, goals-and-commands-out,
a shared core parameterized by per-class factors. This note specifies the **ideal** noiser↔engine
interop for it, records **what today's compiler already supports** (empirically verified), states
**what we ship now** (pragmatic scaffolding), and lists the concrete **language / host improvements**
that would let us delete that scaffolding.

Companion docs: `docs/noiser-feedback.md` (running integration log) and `docs/noiser-bugs-upstream/`
(one-file-per-bug repros). This file is the design SSOT for the brain interop specifically.

## The framework in one paragraph

The brain observes a **`WorldView`** (its idea of the world: *my state* — pos, hp, cooldown, inventory,
class — plus *what I see* — nearest enemy, nearby buildings by kind, home door). It composes per-class
weighted **`Behaviour`s** (a shared core + a per-class `Profile`) and produces a **`Decision { goal,
command }`**. Combat/survival are **hard, deterministic pre-empts** layered above a **soft, seeded-random
preference** layer, so the critical paths stay reproducible while errand/roam choices diverge per hero.

## Contracts

### 1. `WorldView` — perception *in*
- **Ideal:** one aggregated struct handed to the generator — a single host-boundary crossing; the engine
  owns the shape.
- **Now:** assembled **script-side** from the existing pull host-calls (`perceive_self` / `perceive_target`
  / `perceive_building` / `perceive_home`, `inventory_count`) + one new `perceive_class(e) -> i32`. An
  `observe(entity) -> WorldView` helper destructures the flat `(f32,f32,f32,f32)` perception tuples into
  the struct. **Verified: compiles + emits bytecode.**

### 2. `Decision` — goals *and* commands *out*
`struct Decision { goal: Goal, command: Command }`, where
`Goal` = `Idle | GoTo(f32,f32) | GoHome | Rest` (durable "where to be") and
`Command` = `None | Attack | Enter(i32) | Buy | EnterHome` (immediate "act now").
- **Ideal:** `pub gen fn brain(entity) -> Decision { … yield decide(view); }`; the engine decodes each
  yield via `ExecutionContext::ResumeView()` / `EvaluationResult::AsStruct()` → `.Get("goal").AsEnum()`
  and applies it in **one** pass — retiring the scattered `intent_*` output surface (perception host-calls
  stay). **Verified feasible:** the generator-yields-`Decision` script full-compiles, and the C++ decode
  API (`AsStruct`/`AsEnum`/`AsTuple`, `GenericEnum{tag,payload}`) exists and is already exercised by
  `third_party/noiser/noiser-tests/api/generator_tests.cpp`.
- **Now (bridge):** the brain **computes** the decision as a **`(Goal, Command)` tuple** (not a
  `Decision` struct — see the bug below) and an in-script `apply` pattern-matches it, fanning out to the
  **existing, proven** `intent_move_to` / `intent_attack` / `intent_enter` / `intent_enter_home` /
  `intent_buy` host calls. Zero new engine transport machinery; the "brain returns goals+commands"
  property holds at the logic layer. The engine-decoded yield above is deferred to avoid an engine
  tick-pipeline refactor for v1 (see improvement #4).
  - **⚠ Bug found (blocks the struct form):** wrapping the two enums in a `struct Decision { goal, command }`
    and returning it from a function fed by `observe()`/host-call data **silently corrupts** the value —
    every downstream `match` mis-dispatches, so no host call fires (no compile error, no trap). Returning
    the enums as a **tuple** is correct. Repro + controls:
    `docs/noiser-bugs-upstream/2026-07-13-struct-of-enums-return-from-perception-corrupts.md`. This is why
    the shipping `WorldView` is all-flat and the decision is a tuple.

### 3. `Behaviour` composition
`protocol Behaviour { fn score(self, view: WorldView) -> f32; fn act(self, view: WorldView) -> Decision; }`
A core library — `Combat`, `Buy`, `VisitTavern`, `GoHome`, `Roam`, `Idle` — plus a per-class **`Profile`**
of weights. Selection: **hard pre-empts first** (Combat/survival, deterministic given `WorldView`), then a
weighted argmax over the soft behaviours' `score * profile.weight`, sampled with a **slot-seeded**
`core::random::Rng` (`from_seed(entity)`, threaded via a `var` that persists across `yield`). **Verified:
protocol + impl + `core::random` threading all compile.**

## Verified capability matrix (via `noiserc` full-compile, 2026-07-13)

| Construct | Result |
|---|---|
| enums with data + `match` | ✅ compiles |
| structs (fields, defaults) + methods / `impl` | ✅ |
| `protocol` + generic `impl` (Behaviour) | ✅ |
| generator yielding a `struct` containing enums (`-> Decision`) | ✅ compiles; ⚠ see runtime bug below |
| `core::random` (`from_seed`, `next_float`, `range_f32`) threaded via `var` across `yield` | ✅ compiles + runs |
| host perception tuple `(f32,f32,f32,f32)` → flat `WorldView` struct assembly | ✅ compiles + runs |
| enum `match` argmax over a fixed array (`for b in [...]`) | ✅ compiles + runs |
| C++ decode of a yielded struct/enum (`AsStruct`/`AsEnum`/`AsTuple`) | ✅ API present + used in `generator_tests.cpp` |
| **struct-of-enums returned from a host-call-sourced fn** (`Decision {goal,command}`) | ❌ **runtime corruption** — see bug below; use a `(Goal,Command)` tuple |
| `core::linalg::Vec2` struct as a struct **field** (vs tuple-`vec2`) | ❌ rejected (Path A distinct types); use flat f32 |
| **local multi-file module import** in the game FFI compile path | ❌ unsupported — see #1 below |
| host fn returning `vecN` | ❌ ICE (known — `feedback.md` bug #1) |

Runtime bugs surfaced (compile ≠ run): the **struct-of-enums return corruption** — a `Decision` struct
built from `observe()` data mis-dispatches every downstream `match` with no error
(`docs/noiser-bugs-upstream/2026-07-13-struct-of-enums-return-from-perception-corrupts.md`). Worked around
by returning a `(Goal, Command)` tuple. No compile-time ICE was hit on the framework surface itself.

## Language / host improvements toward the ideal (the ask)

Ordered by leverage for this framework. Each is an *interoperability* improvement — the game as a first-
class noiser embedder — not a request to change core language semantics.

1. **Local multi-file module resolution in the FFI `Compile` path.** `NoiserProgram::Compile(source,
   backend)` (`noiser-vm/src/noiser.hpp:1247`) takes a single source string with **no include path**;
   embedded corelib (`core::linalg`, `core::random`) resolves, but a local `import { Combat } from
   brains::combat;` hits the `NoOpModuleResolver` and fails (see `feedback.md` positive-finding on the
   stale `noiser.cpp:1966` comment). **Consequence:** the whole brain must live in **one file**; "combat is
   a composable module" is realized as a logical section (`struct Combat` + `impl Behaviour`) rather than a
   separate file. **Want:** an include-path / resolver parameter on `Compile` (mirroring the CLI's
   `compile_with_resolver`) so embedders can ship modular scripts.

2. **Aggregated perception struct passed *into* the generator.** Today perception is N pull host-calls
   reassembled into `WorldView` script-side. **Want:** hand the generator a host-populated `WorldView`
   (struct-valued host input / arg), so the engine owns the perception shape and the boundary is crossed
   once per tick, not once per field.

3. **`vecN` host-fn returns** (ICE today — `feedback.md` bug #1, repro under `docs/noiser-bugs-upstream/`).
   Would let `perceive_*` return `Vec2` / `vec4` instead of flat `(f32,f32,f32,f32)` tuples, removing the
   destructure boilerplate in `observe()`.

4. **Engine-consumed yielded `Decision` as the first-class brain-output channel.** Today `resume_brain`
   reads only `.has_value()` (a heartbeat); output travels via `intent_*` side-effects. **Want:** adopt
   `ResumeView`/`AsStruct` decoding of `yield Decision` in the tick, retiring the `intent_*` *output*
   surface (perception host-calls remain). Feasibility is proven (matrix above); the blocker is engine
   blast-radius, not the language.

5. **(minor) struct as a host-fn argument** — would allow a single `intent_apply(entity, decision)` host
   call instead of the in-script `apply()` fan-out, if we keep a host-side apply rather than #4.

6. **Fix the struct-of-enums return corruption** (bug, not a missing feature). Until fixed, decisions and
   perception must be tuples/flat scalars, not the more ergonomic `struct Decision { goal, command }` /
   nested `WorldView`. Fixing it makes the natural product-type modeling usable and is a prerequisite for
   #4 (the engine would decode a yielded `Decision` struct). Repro:
   `docs/noiser-bugs-upstream/2026-07-13-struct-of-enums-return-from-perception-corrupts.md`.

## Ships-now vs ideal (summary)

| Contract | Ideal | Ships now |
|---|---|---|
| Perception | aggregated `WorldView` host-struct in | pull host-calls → `WorldView` script-side (+ new `perceive_class`) |
| Output | `yield Decision`, engine decodes+applies | rich `Decision` computed in-script → `intent_*` bridge |
| Modularity | `import` combat/core as files | one file, logical sections |
| Perception return shape | `Vec2`/`vec4` | flat f32 tuples (vecN ICE) |

The framework *logic* (WorldView, Behaviour protocol, Profiles, seeded Rng, Decision) lives in noiser
today; only the *transport* uses proven scaffolding. Closing #1–#4 turns the scaffolding into the ideal
without touching the behaviour code.
