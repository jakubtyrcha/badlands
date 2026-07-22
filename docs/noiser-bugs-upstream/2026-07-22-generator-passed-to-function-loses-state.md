# Passing a sub-generator into a function and calling `.next()` there fails inside the entry generator (and works everywhere else)

**Date**: 2026-07-22
**Version (noiser sha)**: `52174b2c9e517d9daa2ad6f11fb4e264fd5fec0d`
**Label**: vm-bytecode
**Status**: new

**Discovered in**: badlands — attempting to factor game-AI brain logic into a
reusable `drive(behaviour)` helper that resumes a sub-behaviour generator and
returns its yielded value.
**Backend observed**: VM (`NoiserBackend::kVM`). WASM not tested.

> **Hypothesis tested and disproved.** This report was opened on the theory that
> the sub-generator is *passed by value*, so the resumed state is not written
> back into the caller's local — hence the filename. **That is not what
> happens.** Passing a generator into a function and resuming it there advances
> the generator correctly (`1, 2, 3, Nothing`) — provided the program's entry
> point is not itself a generator. The evidence is in *Actual* below. The real
> defect is the entry-generator corruption documented in the companion report;
> this file records the by-value experiment, its refutation, and the
> function-boundary test cases that upstream should keep as regression tests.

## Problem

The following compiles, and on the first resume behaves correctly — then breaks:

```noiser
gen fn steps() -> f32 { loop { yield 1.0; yield 2.0; yield 3.0; } }
fn drive<T>(g: T) -> f32 { match g.next() { .Just(v) => v, .Nothing => -1.0, } }
pub gen fn brain(e: i32) -> i32 { var p = steps(); loop { @fn.report(drive(p)); yield 0; } }
```

Over 20 program loads: **7 produce zero host calls at all** (no error), and
**13 report `1` on the first resume and then fail** with
`Invalid generator state: RestoreState: state buffer index 0 >= buffer len 0`.
Zero produce the expected `1, 2, 3, 1, 2, 3, …`.

Two things are worth separating:

1. **The generic `fn` boundary is fine.** `drive<T>` resolves `.next()` to
   `Maybe<f32>` correctly, the `.Just` arm runs, and the value is returned
   intact — see the `report(1)`. The generator is *not* deep-copied: driving it
   through the helper four times yields `1, 2, 3, Nothing`, exactly as if
   `.next()` were called inline.
2. **The entry generator is what breaks.** Wrapping the identical code in a
   `pub gen fn` entry turns a program that works 3/3 into one that fails 5/5.

## Minimum Reproduction

```noiser
@fn.report: fn(x: f32) -> void;

gen fn steps() -> f32 { loop { yield 1.0; yield 2.0; yield 3.0; } }

fn drive<T>(g: T) -> f32 { match g.next() { .Just(v) => v, .Nothing => -1.0, } }

pub gen fn brain(e: i32) -> i32 {
    var p = steps();
    loop { @fn.report(drive(p)); yield 0; }
}
0.0
```

## Expected

Per `internals/memory-model.md` § *Generator Memory*, a generator instance is a
sub-context stored in the parent's `generators` vector and referred to by "a
simple `i32` handle". Passing that handle into a function and resuming through
it must therefore advance the one shared sub-context.

Precise semantics a correct implementation must have:

1. `drive(p)` resumes the **same** sub-context that `p` names in the caller;
   consecutive calls yield `Just(1), Just(2), Just(3), …`, then `Nothing`
   (fused) once the generator completes.
2. This holds identically whether `drive` is generic (`fn drive<T>(g: T)`) or
   monomorphic, and whether the caller is a plain `fn`, a non-entry `gen fn`, or
   the program's entry `pub gen fn`.
3. `g.next()` is typed `?f32` inside `drive`, usable as a `match` **and** a
   `while let` scrutinee.
4. Behaviour is deterministic across program loads.

Expected output for the reproduction, over 6 resumes:
`report(1) report(2) report(3) report(1) report(2) report(3)`.

## Actual

### The reproduction

Two non-deterministically chosen outcomes, 20 loads:

```
# 13/20
COMPILE OK
    report(1)
  tick 1 RESUME FAIL: Invalid generator state: RestoreState: state buffer index 0 >= buffer len 0
```

```
# 7/20
COMPILE OK
  tick 0: (no host calls)
  tick 1: (no host calls)
  tick 2: (no host calls)
DONE (0 host calls over 3 ticks)
```

Never the expected sequence.

### Refutation of the by-value hypothesis

Remove the generator entry point, keep `drive<T>` exactly as-is, and call it
four times:

```noiser
@fn.report: fn(x: f32) -> void;
gen fn steps() -> f32 { yield 1.0; yield 2.0; yield 3.0; }
fn drive<T>(g: T) -> f32 { match g.next() { .Just(v) => v, .Nothing => -1.0, } }
fn go() -> f32 {
    var p = steps();
    @fn.report(drive(p));
    @fn.report(drive(p));
    @fn.report(drive(p));
    @fn.report(drive(p));
    0.0
}
go()
```

Result, 3/3 loads:

```
COMPILE OK
    report(1)
    report(2)
    report(3)
    report(-1)
```

The generator **advances correctly across the function boundary** and fuses to
`Nothing` on exhaustion. By-value copying would have produced `1, 1, 1, 1`. The
handle is passed by value, but it indexes the caller's `generators` vector, so
the semantics are correct.

The same program with `p.next()` called inline instead of through `drive`
produces byte-identical output (`1, 2, 3, -1`, 3/3), confirming the helper adds
no distortion.

### The actual discriminator: the entry generator

Same `go()`, same `drive<T>`, only the entry point changes:

```noiser
@fn.report: fn(x: f32) -> void;
gen fn steps() -> f32 { yield 1.0; yield 2.0; yield 3.0; }
fn drive<T>(g: T) -> f32 { match g.next() { .Just(v) => v, .Nothing => -1.0, } }
fn go() -> f32 {
    var p = steps();
    @fn.report(drive(p));
    @fn.report(drive(p));
    @fn.report(drive(p));
    @fn.report(drive(p));
    0.0
}
pub gen fn brain(e: i32) -> i32 { loop { let r = go(); yield 0; } }
0.0
```

Result, 5/5 loads:

```
COMPILE OK
  tick 0 RESUME FAIL: (completed/no message)
```

The entry generator reports **completed on its first resume**. `go()` never
runs; not one host call fires. One line of difference between a program that is
correct 3/3 and one that is dead 5/5.

## Trace

No panic. The VM returns `VmError::InvalidState` from the `RestoreState` bounds
guard, `noiser-vm/rust/context.rs:4835-4841`. `state_buffer` is a per-context
`Vec<Slot>` (`context.rs:463`) written by `SaveState` (`context.rs:4809`) and
cleared by `reset()` (`context.rs:878`); observing it empty on the parent's
resume means the parent's saved state was cleared or never written.

The "completed on first resume" outcome produces no error at all — `Resume`
returns a normal completion.

For the root-cause lead (`internals/memory-model.md` § *Standalone Generators*,
the flattened `locals[0..ctx_size]` model used when the program's top level is
a generator), see
`2026-07-22-nested-generator-next-returns-tuple-not-maybe.md` § *Trace*. Every
failing case in this report has a `pub gen fn` entry; every passing case does
not.

## Use cases / why it matters

badlands runs one `pub gen fn brain(entity: i32) -> i32` per entity, resumed
once per 30 Hz tick (`game/src/brain.cpp`,
`scripts/brains/hero.noiser:200`). A `drive(behaviour)` helper is the specific
thing this report was trying to build, because it is what makes sub-behaviours
*composable* rather than merely *possible*:

```noiser
// the shape we want
pub gen fn brain(entity: i32) -> i32 {
    var patrol_route = patrol(post_a, post_b, post_c);
    var go_home      = commute(work, home);
    loop {
        if threat_visible(entity) {
            drive(engage_then_return(entity));
        } else if is_night(entity) {
            drive(go_home);
        } else {
            drive(patrol_route);
        }
        yield 0;
    }
}
```

Each sub-behaviour keeps **its own position as coroutine state**: `patrol` knows
which leg it is on, `commute` knows which end of the trip it is at, `engage`
knows whether it is approaching, fighting, or walking back. `drive` is the one
place that knows how to advance a behaviour and detect completion — so
behaviours can be written, tested, and swapped independently.

Without it, every sub-behaviour's position must be hand-rolled as an explicit
index or phase variable in the brain's own locals (or hoisted into an ECS
component on the C++ side). That means:

- Behaviours cannot own private state — each one's phase leaks into the shared
  brain namespace, and two instances of the same behaviour need two sets of
  variables.
- Behaviours cannot be reused across brains without copying their phase
  variables and their `match phase { … }` dispatch along with them.
- There is no `drive`: every brain re-implements advance-and-detect-completion
  inline.

The **silent** failure mode is what makes this urgent for a game: a brain that
does nothing reads as an idle NPC, not as a crash, and the mode is re-rolled on
every program load — so a script can pass a smoke test and no-op on the next
run.

## Minimum test cases

Non-deterministic rows were run 20×; deterministic rows 3–5×.

### Group 1 — function-boundary semantics (these all pass; keep as regressions)

| # | Case | Expected | Actual |
|---|---|---|---|
| 1.1 | `fn drive<T>(g: T)` called 4× on one generator, **no entry generator** | `1, 2, 3, -1` | **PASS**, 3/3 |
| 1.2 | inline `p.next()` 4× in the same shape (control for 1.1) | `1, 2, 3, -1` | **PASS**, 3/3 |
| 1.3 | `match g.next()` inside a plain `fn` resolves to `Maybe<f32>` | `.Just` arm runs | **PASS** — this is what disproves "returns a tuple" as a plain-`fn` claim |
| 1.4 | generic `fn pick<T>(m: T)` matching a hand-written `?f32`, called from the entry generator | `4` | **PASS**, 4/4 |

1.4 matters: generics + `Maybe` + entry generator is fine *as long as no
generator instance is involved*, which localises the defect away from generics.

### Group 2 — the failures

| # | Case | Expected | Actual |
|---|---|---|---|
| 2.1 | the minimum reproduction (`drive(p)` from the entry generator) | `1,2,3,1,2,3` | **0/20 pass**; 7 zero-host-calls, 13 `report(1)` then `RestoreState` |
| 2.2 | 1.1's exact `go()` wrapped in a `pub gen fn` entry | `1,2,3,-1` per tick | **0/5**; entry gen completed on tick 0, zero host calls |
| 2.3 | entry gen calls a plain `fn` that creates **and** drives its own sub-generator | reports per tick | **0/4**; entry gen completed on tick 0 |
| 2.4 | entry gen calls a plain `fn` that drives a sub-generator to completion in a `while` loop | reports per tick | **0/3**; entry gen completed on tick 0 |
| 2.5 | `while let .Just(v) = g.next()` inside a plain `fn` (no entry generator needed) | compiles | `Cannot extract variant payload from non-enum type: Tuple([I32, F32])`, 4/4 |

2.5 is the one compile-time failure that is **not** entry-generator-dependent —
it is the `Maybe` instantiation defect tracked in the two companion reports.

### Group 3 — determinism

| # | Case | Expected | Actual |
|---|---|---|---|
| 3.1 | the minimum reproduction, loaded 20× | identical output every load | **two different outcomes**, ~7/13 split |
| 3.2 | same program with `var p = steps();` deleted | identical output every load | **PASS**, stable 15/15 |

Any fix must make 3.1 deterministic. A program whose observable behaviour
changes between loads with no input change is the most damaging property here,
independent of which behaviour is correct.

## Example verification programs

### V1 — the composition helper (the thing that must work)

```noiser
@fn.report: fn(x: f32) -> void;

gen fn steps() -> f32 { loop { yield 1.0; yield 2.0; yield 3.0; } }

fn drive<T>(g: T) -> f32 { match g.next() { .Just(v) => v, .Nothing => -1.0, } }

pub gen fn brain(e: i32) -> i32 {
    var p = steps();
    loop { @fn.report(drive(p)); yield 0; }
}
0.0
```

Expected over 6 resumes:
`report(1) report(2) report(3) report(1) report(2) report(3)`.

Currently, over 20 loads: 7 × zero host calls with no error; 13 × `report(1)`
then `Invalid generator state: RestoreState: state buffer index 0 >= buffer len
0` on the second resume.

### V2 — the by-value control (passes today; must not regress)

```noiser
@fn.report: fn(x: f32) -> void;

gen fn steps() -> f32 { yield 1.0; yield 2.0; yield 3.0; }

fn drive<T>(g: T) -> f32 { match g.next() { .Just(v) => v, .Nothing => -1.0, } }

fn go() -> f32 {
    var p = steps();
    @fn.report(drive(p));
    @fn.report(drive(p));
    @fn.report(drive(p));
    @fn.report(drive(p));
    0.0
}
go()
```

Expected and **currently observed**, 3/3 loads:
`report(1) report(2) report(3) report(-1)`.

This program is the proof that the function boundary is not the problem, and it
is the natural pair for V3.

### V3 — the entry-generator A/B (fix criterion)

```noiser
@fn.report: fn(x: f32) -> void;

gen fn steps() -> f32 { yield 1.0; yield 2.0; yield 3.0; }

fn drive<T>(g: T) -> f32 { match g.next() { .Just(v) => v, .Nothing => -1.0, } }

fn go() -> f32 {
    var p = steps();
    @fn.report(drive(p));
    @fn.report(drive(p));
    @fn.report(drive(p));
    @fn.report(drive(p));
    0.0
}

pub gen fn brain(e: i32) -> i32 { loop { let r = go(); yield 0; } }
0.0
```

Expected per resume: `report(1) report(2) report(3) report(-1)` — byte-identical
to V2, since `go()` is byte-identical.

Currently, 5/5 loads: `tick 0 RESUME FAIL: (completed/no message)` — the entry
generator completes immediately and `go()` never executes. **V2 passing while V3
fails is the fix criterion**: after the fix, V3's per-tick output must equal V2's
output.

## Workarounds

**None for the intended use.** Everything that would let a brain drive a
sub-behaviour was tried and fails:

| Attempt | Outcome |
|---|---|
| `drive<T>(g: T)` generic helper (this report) | non-deterministic corruption |
| Monomorphic helper / inline `p.next()` in the entry generator | same corruption — companion report |
| Create **and** drive the sub-generator entirely inside a plain `fn` | entry generator completes on tick 0 (0/4) |
| Drive to completion in a `while` loop inside a plain `fn` | entry generator completes on tick 0 (0/3) |
| `for v in steps() { … }` inside a plain `fn` | entry generator completes on tick 0 (0/4) |
| Store the generator in a struct field | ICE `type_descriptor_to_abi_layout: Unknown has no physical layout` |
| Bind `g.next()` to a local before matching | ICE `Maybe$f32 has no registered layout` |
| `while let .Just(v) = g.next()` | compile error `Tuple([I32, F32])` |
| Drop the generator entry point | works — unavailable to badlands, whose brain must be a resumable per-tick coroutine |

badlands continues to encode sequential behaviour as an explicit phase variable
inside the single brain generator.

## Related

- `docs/noiser-bugs-upstream/2026-07-22-nested-generator-next-returns-tuple-not-maybe.md`
  — the primary report: same corruption reached without any function boundary,
  plus the 20-run non-determinism data and the root-cause lead.
- `docs/noiser-bugs-upstream/2026-07-22-generic-maybe-instantiation-ices-inside-generator-body.md`
  — the compile-time half (`Maybe$f32` ICE), with the same entry-generator A/B.
- `docs/noiser-bugs-upstream/2026-07-13-struct-of-enums-return-from-perception-corrupts.md`
  — earlier enum-payload-handling bug.
- `docs/noiser-feedback.md` — badlands' running noiser integration log.
- Source: `noiser-vm/rust/context.rs:4835` (`RestoreState` guard), `:5324-5370`
  (`ResumeGenerator`), `:3851-3887` (`CreateGenerator`).
- Docs: `internals/memory-model.md` §§ *Generator Memory*, *Standalone
  Generators*; `core-language/generics-protocols.md`.
