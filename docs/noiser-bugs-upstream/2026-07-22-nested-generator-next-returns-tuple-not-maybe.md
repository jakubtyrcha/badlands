# Resuming a sub-generator from inside the entry generator fails deterministically on the second resume; builtin-produced `Maybe<T>` is never instantiated

**Date**: 2026-07-22
**Version (noiser sha)**: `52174b2c9e517d9daa2ad6f11fb4e264fd5fec0d`
**Label**: vm-bytecode
**Status**: new

**Discovered in**: badlands — evaluating nested generators as the composition
mechanism for game-AI brains (a parent brain generator driving reusable
sub-behaviour generators).
**Backend observed**: VM (`NoiserBackend::kVM`). WASM not tested.

> **Revision note (2026-07-22, third pass).** The first two versions of this
> report were **wrong twice**, in opposite directions. v1 claimed "`match` on
> `.next()` silently executes neither arm". v2 replaced that with
> "non-deterministic; the parent generator's body is never entered at all, and
> merely creating a sub-generator triggers it".
>
> **Both were measurement artifacts of an unrelated bug.** noiser's entry-point
> selection is non-deterministic (`generators: HashMap<String, GenDef>`
> serialized in iteration order, read back as entry `[0]` — see
> `2026-07-22-entry-point-selection-is-nondeterministic.md`). Every repro in
> this report contains a second `gen fn`, so on any given load the *sub-behaviour*
> generator could win the entry lottery and run instead of the brain. That is
> what produced the "body never entered / zero host calls" observation, and it
> is what produced the apparent non-determinism.
>
> Controlled re-measurement — giving the sub-generator a distinguishing host
> call so a lottery loss is visible rather than silent — shows:
>
> - **Creating a sub-generator is harmless.** 20 loads: 12 lost the lottery,
>   8 ran the brain, **8/8 of those fully correct**. The v2 claim that
>   `var p = steps();` alone corrupts the parent is **withdrawn**.
> - **Resuming one is a real, fully deterministic bug.** 25 loads: 15 lost the
>   lottery, 10 ran the brain, **10/10 of those failed identically** on the
>   second resume. 0 correct. There is no non-determinism in this defect.
>
> Anyone reproducing this MUST control for the entry lottery first — see
> *Confounder* below.

## Problem

A generator instance created **and resumed** inside the program's entry
generator (a top-level `pub gen fn`) fails on the **second** resume with:

```
Invalid generator state: RestoreState: state buffer index 0 >= buffer len 0
```

The first `.next()` returns the correct value; the second kills the parent.
This is deterministic: 10/10 loads on which the brain actually ran.

Creating the instance without resuming it is **fine**. Driving the same
sub-generator from a **non-entry** context (a plain `fn`, or a top-level
expression) is also fine, with correct fusing. The defect is specific to
resuming a sub-generator owned by the entry generator.

A **second, independent defect** is that a `Maybe<T>` produced by a builtin
generic method (`Generator::next`, `Vec::pop`) is not registered as an enum
type, so it degrades to the raw `Tuple([I32, F32])` tag+payload pair that
`ResumeGenerator` pushes (`noiser-vm/rust/context.rs:5348`). This one is **not
generator-specific** — it reproduces on `Vec::pop` in a plain `fn` with no
generator in the program — and surfaces as a compile error, an ICE, or a
mistyped binding depending on the consuming construct.

## Confounder — read this before reproducing

Every program in this report declares two generators (the sub-behaviour and the
brain), which makes it subject to
`2026-07-22-entry-point-selection-is-nondeterministic.md`: which one becomes the
entry point is decided by HashMap iteration order and varies per load. A load
that "does nothing" has almost certainly just run the other generator.

**Control for it** by giving the sub-generator a distinguishing host call:

```noiser
gen fn steps() -> f32 { loop { @fn.report(-999.0); yield 1.0; } }
```

A load reporting `-999` lost the lottery and must be discarded from the
measurement, not counted as a failure. All counts below were taken this way.

## Minimum Reproduction

### R1 — sub-generator created but never resumed (NOT a trigger; contrast case)

```noiser
@fn.report: fn(x: f32) -> void;
gen fn steps() -> f32 { loop { @fn.report(-999.0); yield 1.0; yield 2.0; } }
pub gen fn brain(e: i32) -> i32 {
    var p = steps();          // created, never resumed
    var i = 0.0;
    loop { i = i + 1.0; @fn.report(i); yield 0; }
}
0.0
```

Controlled, 20 loads × 4 ticks: **12 lost the entry lottery** (reported `-999`),
**8 ran the brain — 8/8 fully correct** (`1,2,3,4`), 0 misbehaved.

Creation alone is therefore **harmless**. A previous revision of this report
listed this program as the minimal trigger; that was the lottery, not a defect.
It is retained as the control that separates the two bugs.

### R2 — the intended composition pattern

```noiser
@fn.report: fn(x: f32) -> void;
gen fn steps() -> f32 { loop { yield 1.0; yield 2.0; yield 3.0; } }
pub gen fn brain(entity: i32) -> i32 {
    var p = steps();
    loop {
        match p.next() { .Just(v) => @fn.report(v), .Nothing => @fn.report(-1.0), }
        yield 0;
    }
}
0.0
```

Controlled, 25 loads × 4 ticks: **15 lost the entry lottery** (discarded),
**10 ran the brain — 10/10 failed identically**: `report(1)` on tick 0, then
`Invalid generator state: RestoreState: state buffer index 0 >= buffer len 0`
on tick 1. **0 correct.** The failure is deterministic given the brain runs.

### R3 — the A/B that localises it to the *entry* generator

Identical `go()` in both. Only the entry point differs.

```noiser
// PASSES — no entry generator. 4/4 loads: report(106), correct fusing.
@fn.report: fn(x: f32) -> void;
gen fn steps() -> f32 { yield 1.0; yield 2.0; yield 3.0; }
fn go() -> f32 {
    var g = steps();
    var acc = 0.0;
    match g.next() { .Just(v) => { acc = acc + v; } .Nothing => {} }
    match g.next() { .Just(v) => { acc = acc + v; } .Nothing => {} }
    match g.next() { .Just(v) => { acc = acc + v; } .Nothing => {} }
    match g.next() { .Just(v) => { acc = acc + v; } .Nothing => { acc = acc + 100.0; } }
    @fn.report(acc);
    acc
}
go()
```

```noiser
// FAILS — same go(), wrapped in a pub gen fn entry.
// 5/5 loads: entry generator reports "completed" on tick 0, go() never runs, zero host calls.
@fn.report: fn(x: f32) -> void;
gen fn steps() -> f32 { yield 1.0; yield 2.0; yield 3.0; }
fn go() -> f32 { /* …identical to above… */ }
pub gen fn brain(e: i32) -> i32 { loop { let r = go(); yield 0; } }
0.0
```

## Expected

Per `internals/memory-model.md` § *Generator Memory*:

> When `counter(0)` is called, the VM creates a fresh ExecutionContext with its
> own stack, locals, and heap. The sub-context is stored in the parent's
> `generators` vector, indexed by a simple i32 handle. On `.next()`, the
> sub-context is taken out, `resume()` is called, and the result is wrapped in
> `Maybe<T>` — Yielded values become `Just(value)`, Completion becomes `Nothing`
> (fusing — subsequent `.next()` always returns `Nothing`).

and `core-language/generics-protocols.md`, which specifies the `Iterator`
protocol as `fn next(mut self) -> ?Item` and states "Generators automatically
implement this protocol", with `while let .Just(val) = iter.next()` as the
idiom.

Precise semantics a correct implementation must have:

1. Creating a generator instance inside **any** function body — including the
   program's top-level `pub gen fn` — must be side-effect-free with respect to
   the enclosing generator's own suspend/resume state.
2. A sub-generator instance stored in a local of a suspended parent generator
   must survive across the parent's yield boundaries, and `.next()` on it must
   advance it: `Just(1), Just(2), Just(3), …`.
3. `.next()` must be statically typed `Maybe<T>` / `?T` in every context —
   `match` scrutinee, `while let` scrutinee, `let` binding, function argument.
4. Behaviour must be deterministic across program loads.
5. If any of the above cannot be supported, it must be a **compile error**, not
   a silently skipped generator body.

## Actual

### The runtime failure

Controlled measurement (sub-generator instrumented with `@fn.report(-999.0)` so
lottery losses are visible and excluded):

| Program | Lottery losses (discarded) | Brain ran | Of those: correct | Of those: failed |
|---|---|---|---|---|
| R1 — create only, never resume | 12 / 20 | 8 | **8** | 0 |
| R2 — create and resume per tick | 15 / 25 | 10 | **0** | **10** |

Every R2 failure is identical:

```
COMPILE OK
    report(1)                     <-- first .next() is CORRECT
  tick 1 RESUME FAIL: Invalid generator state: RestoreState: state buffer index 0 >= buffer len 0
```

So: the sub-generator yields correctly once, and the parent's own state restore
is broken from the second resume onward. Note it is the **parent** that fails —
the error surfaces on the parent's resume, not on `.next()`.

A previous revision reported this as a 35–65% non-deterministic split between
"silent no-op" and this failure. That was entirely the entry lottery. Once
controlled, there is **no non-determinism in this defect** and no silent mode:
whenever the brain is the entry point, it fails this way every time.

### The `Maybe` typing defect

Same `p.next()` call, four different consumers, four different failures:

| Consumer (inside the entry generator) | Result |
|---|---|
| `match p.next() { .Just(v) => …, .Nothing => … }` | compiles; hits the runtime corruption above |
| `while let .Just(v) = p.next() { … }` | `COMPILE FAIL: Cannot extract variant payload from non-enum type: Tuple([I32, F32])` |
| `let r = p.next();` | `ICE: make_enum_descriptor: enum 'Maybe$f32' has no registered layout — ensure_type_instantiated must be called first` |
| `let (tag, val) = p.next();` | same ICE (4/4 loads) |

The `Tuple([I32, F32])` is literally the shape `ResumeGenerator` pushes —
`MAYBE_JUST_TAG` (i32) followed by the payload slots
(`noiser-vm/rust/context.rs:5347-5352`). When `Maybe$f32` has no registered
layout the tag+payload pair is never re-typed as the enum.

**Correction to the original report:** this is *not* generator-specific. The
same compile error reproduces on `Vec::pop` inside an ordinary function, with no
generator anywhere in the program:

```noiser
@fn.report: fn(x: f32) -> void;
fn s() -> f32 {
    var v = vec_new::<f32>(); v.push(1.0); v.push(2.0); v.push(3.0);
    var acc = 0.0;
    while let .Just(x) = v.pop() { acc = acc + x; }     // <-- same error
    acc
}
pub gen fn brain(e: i32) -> i32 { loop { @fn.report(s()); yield 0; } }
0.0
```
→ `COMPILE FAIL: Cannot extract variant payload from non-enum type: Tuple([I32, F32])` (3/3)

Whereas `while let` over a **hand-written** `?f32` compiles and runs correctly
(reports 6, 3/3), in both the `?f32` and `Maybe<f32>` spellings:

```noiser
fn maybe_val(x: f32) -> ?f32 { if x > 0.0 { .Just(x) } else { .Nothing } }
// while let .Just(v) = maybe_val(i) { … }   -> works
```

So the defect is: **`Maybe<T>` returned by a builtin generic method is not run
through `ensure_type_instantiated`**, while a user-declared `?T` is.

A third manifestation — the payload binds to the **tag** slot. Statement-position
match on `v.pop()` in a plain function:

```noiser
match v.pop() { .Just(x) => @fn.report(x), .Nothing => @fn.report(-1.0), }
```
→ `COMPILE FAIL: @fn.report argument 'x' (position 0): expected F32, got I32`

`x` is typed `I32` — element 0 of `Tuple([I32, F32])`, i.e. the discriminant,
not the `f32` payload. The identical shape over a hand-written `?f32` compiles.

## Trace

No panic for the runtime half; the VM returns `VmError::InvalidState` from the
`RestoreState` guard at `noiser-vm/rust/context.rs:4835-4841`:

```rust
if i >= self.state_buffer.len() {
    return Err(VmError::InvalidState {
        message: format!(
            "RestoreState: state buffer index {} >= buffer len {}",
            i, self.state_buffer.len()
        ),
    });
}
```

`state_buffer` is a single per-`ExecutionContext` `Vec<Slot>`
(`context.rs:463`), cleared by `SaveState` (`context.rs:4809`) and by `reset()`
(`context.rs:878`). Seeing it at length 0 on the parent's resume means the
parent's saved state was cleared or never written.

The ICE is `noiser-compiler/src/bytecode/type_utils.rs:1109`.

### Root-cause lead

`internals/memory-model.md` § *Standalone Generators*:

> Programs whose top-level **is** a generator use a flattened state model where
> generator context fields are stored directly in `locals[0..ctx_size]` with
> `locals[0]` as the state discriminant. This avoids the overhead of a
> sub-context for the common case.

Every failing case here has a top-level `pub gen fn` entry; every passing case
does not (R3). The hypothesis that fits all observations is that the flattened
standalone model has no provision for the parent owning `generators` /
`state_buffer` entries, so a sub-generator created underneath it clobbers
`locals[0]` (→ the discriminant selects a "completed" state → outcome A, and the
garbage value explains the non-determinism) or leaves `state_buffer` empty (→
outcome B).

Consistent with this, the **compile-time** ICE has the same entry-generator
dependency — see the A/B in the companion report
`2026-07-22-generic-maybe-instantiation-ices-inside-generator-body.md`:
`let a = v.pop();` ICEs in a `pub gen fn` entry but compiles fine in a
non-entry `gen fn`. Both halves of this bug appear to live in the standalone
entry-generator compilation path.

The label is `vm-bytecode` on the theory that the standalone lowering emits
state save/restore against storage it does not reserve; if the flattened model
is correct as emitted, re-label `vm`.

## Use cases / why it matters

badlands runs one `pub gen fn brain(entity: i32) -> i32` coroutine per entity,
resumed once per 30 Hz tick (`game/src/brain.cpp`,
`scripts/brains/hero.noiser:200`). The whole point of the coroutine encoding is
that **sequential behaviour is expressed as straight-line code** and the
resume point *is* the state.

Composing brains from reusable sub-behaviours is the natural next step, and it
is exactly what this bug blocks:

- **Patrol a route** — `gen fn patrol(a, b, c)` yields once per tick while
  walking each leg; the brain drives it while no threat is present. The
  sub-behaviour owns "which leg am I on".
- **Commute home → work → home** — `gen fn commute(home, work)` owns the phase
  and the dwell timers.
- **Engage then return to post** — `gen fn engage(target, post)` owns
  "approaching / fighting / walking back", and the brain abandons it by simply
  dropping the generator.

Each of these wants **private coroutine state**. Without nested generators,
every sub-behaviour's position must be hand-rolled as an explicit index/phase
variable in the brain's own locals, or hoisted into an ECS component on the C++
side — which means:

- Behaviours cannot own private state; every one leaks its phase into the
  brain's variable namespace.
- Behaviours cannot be reused across brains without also copying their phase
  variables and the `match phase { … }` dispatch.
- The straight-line-code advantage that motivates using coroutines at all is
  lost precisely where it would pay off most.

For a game the failure is at least loud: the brain dies on its second tick with
a VM error the host can catch and report (badlands downgrades that entity to its
C++ fallback brain and counts a `noiser_bug`). The genuinely dangerous silent
mode originally attributed to this bug belongs to the entry-point lottery
instead — see `2026-07-22-entry-point-selection-is-nondeterministic.md`.

## Minimum test cases

All run against sha `52174b2c…` on the VM backend, via a harness that compiles
the source and resumes the entry generator N times. Non-deterministic rows were
run 20× and are reported as counts.

### Group 1 — the runtime failure (entry generator + sub-generator)

All counts controlled for the entry lottery (sub-generator instrumented; losing
loads discarded, not counted as failures).

| # | Program shape | Expected | Actual |
|---|---|---|---|
| 1.1 | entry gen, plain counter, **no** sub-generator | `1,2,3,…` | **PASS**, 15/15 |
| 1.2 | entry gen + `var p = steps();`, `p` never resumed | `1,2,3,4` | **PASS**, 8/8 brain-wins (12/20 loads lost the lottery) |
| 1.3 | entry gen + `match p.next()` in the tick loop | `1,2,3,1,2,3,…` | **FAIL 10/10** brain-wins: `report(1)` then `RestoreState` on resume 2 |
| 1.4 | as 1.3 plus a `_ =>` wildcard arm | same | same failure; wildcard never reached |
| 1.5 | as 1.3, fresh `steps()` per tick (`steps().next()`) | `1,1,1,…` | needs re-measurement under lottery control |
| 1.6 | plain `fn` creates **and** fully drives a sub-gen; called from entry gen | reports each tick | needs re-measurement under lottery control |
| 1.7 | `for v in steps() { … }` inside a plain fn, called from entry gen | reports each tick | needs re-measurement under lottery control |
| 1.8 | `for x in v { … }` over a `Vec` (for-in control) | `6` | **PASS**, 3/3 |
| 1.9 | **no entry generator**; top-level expression drives a sub-gen 4× | `106` | **PASS**, 4/4, correct fusing |
| 1.10 | 1.9's exact `go()` wrapped in a `pub gen fn` entry | `106` per tick | needs re-measurement under lottery control |
| 1.11 | sub-generator stored in a struct field | works | **ICE**: `type_descriptor_to_abi_layout: Unknown has no physical layout` |

1.2 vs 1.3 is the pair that isolates this bug: identical program except one
resumes the sub-generator. 1.9 vs 1.10 localises it to the entry generator, but
1.10's count predates lottery control and is flagged for re-measurement rather
than restated — its "entry generator completed on tick 0, zero host calls"
signature is exactly what a lottery loss looks like.

### Group 2 — the `Maybe` typing defect

| # | Expression | Context | Expected | Actual |
|---|---|---|---|---|
| 2.1 | `while let .Just(v) = p.next()` | entry gen | compiles | `Cannot extract variant payload from non-enum type: Tuple([I32, F32])` |
| 2.2 | `while let .Just(x) = v.pop()` | plain fn, **no generator in program** | compiles | same error, 3/3 |
| 2.3 | `while let .Just(v) = maybe_val(i)`, `maybe_val` returns `?f32` | plain fn | `6` | **PASS**, 3/3 |
| 2.4 | 2.3 with `Maybe<f32>` spelling instead of `?f32` | plain fn | `6` | **PASS**, 3/3 |
| 2.5 | `let r = p.next();` | entry gen | compiles | ICE `Maybe$f32 has no registered layout` |
| 2.6 | `let (tag, val) = p.next();` | entry gen | compiles | same ICE, 4/4 |
| 2.7 | `match v.pop() { .Just(x) => @fn.report(x), … }` (statement position) | plain fn | compiles | `expected F32, got I32` — payload bound to the tag slot |
| 2.8 | `match maybe_val(3.0) { .Just(x) => @fn.report(x), … }` | plain fn | compiles | **PASS** |
| 2.9 | `match v.pop() { … }` (expression position, arms yield values) | entry gen | `2` | **PASS**, 3/3 |

2.2 vs 2.3 is the key pair: identical construct, builtin-produced `Maybe` fails,
user-produced `Maybe` passes.

## Example verification programs

Each is complete and runnable. "Currently" = observed at sha `52174b2c…`.

### V1 — the composition pattern (the thing that must work)

```noiser
@fn.report: fn(x: f32) -> void;

gen fn steps() -> f32 { loop { yield 1.0; yield 2.0; yield 3.0; } }

pub gen fn brain(entity: i32) -> i32 {
    var p = steps();
    loop {
        match p.next() { .Just(v) => @fn.report(v), .Nothing => @fn.report(-1.0), }
        yield 0;
    }
}
0.0
```

Expected over 6 resumes: `report(1) report(2) report(3) report(1) report(2)
report(3)`.

Currently, controlled for the entry lottery over 25 loads: 15 discarded
(sub-generator won the entry lottery), 10 ran the brain, and **all 10 produced
`report(1)` then `Invalid generator state: RestoreState: state buffer index 0 >=
buffer len 0` on the second resume**. Never the expected sequence.

### V2 — creation without resume (the control; must keep passing)

```noiser
@fn.report: fn(x: f32) -> void;

gen fn steps() -> f32 { loop { @fn.report(-999.0); yield 1.0; yield 2.0; } }

pub gen fn brain(e: i32) -> i32 {
    var p = steps();          // created, never resumed
    var i = 0.0;
    loop { i = i + 1.0; @fn.report(i); yield 0; }
}
0.0
```

Expected over 4 resumes: `report(1) report(2) report(3) report(4)`.

Currently: on the 8 of 20 loads where the brain wins the entry lottery, exactly
that — **8/8 correct**. The other 12 loads report `-999`, meaning `steps` became
the entry point (a separate bug). Creation alone does not corrupt the parent.

This is the discriminator between V1 and V2: the only difference is whether the
sub-generator is resumed. Keep both as regression tests — a fix must make V1
pass without regressing V2.

### V3 — the entry-generator A/B (fix localiser)

```noiser
// --- V3a: passes today (4/4). Keep as the "must not regress" side. ---
@fn.report: fn(x: f32) -> void;
gen fn steps() -> f32 { yield 1.0; yield 2.0; yield 3.0; }
fn go() -> f32 {
    var g = steps();
    var acc = 0.0;
    match g.next() { .Just(v) => { acc = acc + v; } .Nothing => {} }
    match g.next() { .Just(v) => { acc = acc + v; } .Nothing => {} }
    match g.next() { .Just(v) => { acc = acc + v; } .Nothing => {} }
    match g.next() { .Just(v) => { acc = acc + v; } .Nothing => { acc = acc + 100.0; } }
    @fn.report(acc);
    acc
}
go()
```

Currently prints `report(106)` on every load — correct: three `Just`es then a
fused `Nothing`.

```noiser
// --- V3b: identical go(), entry is now a generator. Must produce report(106) per tick. ---
@fn.report: fn(x: f32) -> void;
gen fn steps() -> f32 { yield 1.0; yield 2.0; yield 3.0; }
fn go() -> f32 {
    var g = steps();
    var acc = 0.0;
    match g.next() { .Just(v) => { acc = acc + v; } .Nothing => {} }
    match g.next() { .Just(v) => { acc = acc + v; } .Nothing => {} }
    match g.next() { .Just(v) => { acc = acc + v; } .Nothing => {} }
    match g.next() { .Just(v) => { acc = acc + v; } .Nothing => { acc = acc + 100.0; } }
    @fn.report(acc);
    acc
}
pub gen fn brain(e: i32) -> i32 { loop { let r = go(); yield 0; } }
0.0
```

Expected: `report(106)` per resume. Currently observed as the entry generator
reporting **completed on the first resume** with zero host calls — but that
measurement was taken before the entry lottery was known, and its signature is
indistinguishable from a lottery loss (`steps` is a second generator in the same
file). **Re-measure under lottery control before relying on this row.** V3a,
which has no `pub gen fn` and so cannot lose the lottery, passes 4/4.

## Workarounds

**None.** Every alternative encoding was tried and fails:

| Attempt | Outcome |
|---|---|
| `while let .Just(v) = p.next()` | compile error (`Tuple([I32, F32])`) |
| `match p.next() { … }` directly | `RestoreState` failure on the 2nd resume (deterministic) |
| `let r = p.next(); match r { … }` | ICE |
| Pass the generator into a `fn` and `.next()` there | see companion report — handles DO pass correctly across `fn` boundaries; the failure returns once an entry generator owns the instance |
| Create **and** drive the sub-generator entirely inside a plain `fn` | previously recorded as failing; needs re-measurement under lottery control |
| `for v in steps() { … }` inside a plain `fn` | previously recorded as failing; needs re-measurement under lottery control |
| Store the generator in a struct field | ICE (`Unknown has no physical layout`) |
| Don't use a generator entry point | works — but badlands' brain **must** be a resumable per-tick coroutine, so this is not available |

badlands therefore continues to hand-roll sequential behaviour as an explicit
phase variable in the brain's locals.

## Related

- `docs/noiser-bugs-upstream/2026-07-22-generic-maybe-instantiation-ices-inside-generator-body.md`
  — the compile-time half (`Maybe$f32` ICE) with the same entry-generator
  dependency. Almost certainly the same root cause.
- `docs/noiser-bugs-upstream/2026-07-22-generator-passed-to-function-loses-state.md`
  — passing a generator to a function; shows handles DO survive `fn` boundaries
  with correct fusing, and that `.next()` **does** resolve to `Maybe` inside a
  plain `fn`.
- `docs/noiser-bugs-upstream/2026-07-13-struct-of-enums-return-from-perception-corrupts.md`
  — another silent-wrong-result bug around enum payload handling; may share the
  variant layout/extraction root cause.
- `docs/noiser-bugs-upstream/2026-07-22-entry-point-selection-is-nondeterministic.md`
  — **the confounder**. Read it before reproducing anything here.
- `docs/noiser-feedback.md` — badlands' running noiser integration log.
- Docs contradicted: `internals/memory-model.md` §§ *Generator Memory*,
  *Standalone Generators*; `core-language/generics-protocols.md` (Iterator
  protocol); `core-library/iterators.md` ("Generators automatically implement
  Iterator<T>").
