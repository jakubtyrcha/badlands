# ICE: `Maybe$f32 has no registered layout` when a builtin generic returning `?T` is bound to a local inside the entry generator

**Date**: 2026-07-22
**Version (noiser sha)**: `52174b2c9e517d9daa2ad6f11fb4e264fd5fec0d`
**Label**: compiler
**Status**: new

**Discovered in**: badlands — using `Vec<f32>` as scratch storage inside a
game-AI brain coroutine (`pub gen fn brain(entity: i32) -> i32`, resumed once
per 30 Hz tick).
**Backend observed**: VM (`NoiserBackend::kVM`). WASM not tested.

## Problem

Binding the `Maybe<T>` returned by a builtin generic method (`Vec::pop`,
`Generator::next`) to a local **inside the program's entry generator** panics
the compiler:

```
ICE: make_enum_descriptor: enum 'Maybe$f32' has no registered layout —
ensure_type_instantiated must be called first. This is a compiler bug - please report it.
```

The instantiation of `Maybe$f32` is skipped on the entry-generator compilation
path. Three properties make this sharp:

- It is **deterministic** (5/5 loads).
- It depends on the enclosing function being the **entry** generator, not on
  being a generator: the identical statement inside a non-entry `gen fn`
  compiles fine.
- It depends on the `Maybe` being **builtin-produced**: a user-written
  `fn f() -> ?f32` bound to a local in the same place compiles fine.

A **secondary finding** (see below): `vec_new` cannot be imported from
`core::vec` even though the corelib's own `core/vec.noiser` calls it.

## Minimum Reproduction

```noiser
@fn.report: fn(x: f32) -> void;

pub gen fn brain(e: i32) -> i32 {
    loop {
        var v = vec_new::<f32>();
        v.push(1.0);
        v.push(2.0);
        let a = v.pop();          // <-- ICE here
        @fn.report(1.0);
        yield 0;
    }
}
0.0
```

Reduced further — the `loop`/`yield` are not needed, only the entry generator:

```noiser
@fn.report: fn(x: f32) -> void;

pub gen fn maker(e: i32) -> i32 {
    var v = vec_new::<f32>(); v.push(1.0); v.push(2.0);
    let a = v.pop();              // <-- ICE
    loop { @fn.report(1.0); yield 0; }
}
0.0
```

### The A/B that localises it — identical statement, non-entry generator

This **compiles cleanly** (3/3 loads). The only difference is that `maker` is
not the program's entry point:

```noiser
@fn.report: fn(x: f32) -> void;

gen fn maker() -> f32 {
    var v = vec_new::<f32>(); v.push(1.0); v.push(2.0);
    let a = v.pop();              // <-- identical statement, compiles OK
    yield 1.0;
}
fn go() -> f32 { var g = maker(); match g.next() { .Just(v) => v, .Nothing => -1.0, } }
go()
```

And so does this — non-entry `gen fn` with the ICE-ing statement, alongside a
*separate* `pub gen fn` entry (reports 0 correctly, 2/2 ticks):

```noiser
@fn.report: fn(x: f32) -> void;
gen fn maker() -> f32 {
    var v = vec_new::<f32>(); v.push(1.0); v.push(2.0);
    let a = v.pop();
    yield 1.0;
}
pub gen fn brain(e: i32) -> i32 { loop { @fn.report(0.0); yield 0; } }
0.0
```

## Expected

`Vec::pop` is documented to return `Maybe<T>`; `?f32` is a first-class type.
A correct implementation must:

1. Run `ensure_type_instantiated` for **every** monomorphised `Maybe$T` reached
   through a builtin generic method, on every compilation path — including the
   standalone/flattened entry-generator path described in
   `internals/memory-model.md` § *Standalone Generators*.
2. Compile `let a = v.pop();` identically regardless of whether the enclosing
   function is the entry generator, a non-entry generator, or a plain `fn` —
   this is a pure typing question with no runtime component.
3. Never ICE on a well-typed program. If a construct is genuinely unsupported,
   emit a diagnostic naming the construct and its source location.

Both minimum reproductions above should compile and report `1.0` (or `2.0` for
the variant that consumes `a`) once per resume.

## Actual

```
ICE: make_enum_descriptor: enum 'Maybe$f32' has no registered layout — ensure_type_instantiated must be called first. This is a compiler bug - please report it.
COMPILE FAIL: Noiser module compilation panicked
```

Deterministic, 5/5 loads. No source location is reported — the panic carries no
line/column, so in a real script the offending statement has to be bisected by
hand.

### What does and does not trigger it

| # | Statement | Enclosing function | Result |
|---|---|---|---|
| 1 | `let a = v.pop();` | **entry** `pub gen fn` | **ICE**, 5/5 |
| 2 | `let a = v.pop();` | non-entry `gen fn` | **compiles**, 3/3 |
| 3 | `let a = v.pop(); match a { … }` | plain `fn` | **PASS** — reports 2, 3/3 |
| 4 | `match v.pop() { .Just(x) => @fn.report(x), … }` (no binding) | **entry** `pub gen fn` | **PASS** — reports 2, 3/3 |
| 5 | `v.push(…); @fn.report(v.len() as f32);` (no `pop`) | **entry** `pub gen fn` | **PASS** — reports 2, 3/3 |
| 6 | `let r = p.next();` (`p` a sub-generator) | **entry** `pub gen fn` | **ICE**, same message |
| 7 | `let (tag, val) = p.next();` | **entry** `pub gen fn` | **ICE**, same message, 4/4 |
| 8 | `let m = maybe_val(3.0);` where `fn maybe_val(x: f32) -> ?f32` | **entry** `pub gen fn` | **PASS** |

Rows 1 vs 2 isolate "entry generator". Rows 1 vs 8 isolate
"builtin-produced `Maybe`". Rows 1 vs 4 isolate "bound to a local" — a
*directly matched* `v.pop()` in the same entry generator is fine, which is why
the bug is easy to hit only once code is refactored to name the intermediate.

Row 6 links this report to
`2026-07-22-nested-generator-next-returns-tuple-not-maybe.md`: the same ICE,
same entry-generator dependency, reached through `Generator::next` instead of
`Vec::pop`. When the un-instantiated `Maybe$f32` is consumed by a construct that
does not ICE, it degrades to the raw `Tuple([I32, F32])` tag+payload pair and
produces `Cannot extract variant payload from non-enum type: Tuple([I32, F32])`
instead. The two reports are almost certainly one root cause with two surfaces.

## Trace

`noiser-compiler/src/bytecode/type_utils.rs:1109`:

```rust
pub(super) fn make_enum_descriptor(&self, name: String) -> TypeDescriptor {
    let layout = self.enum_layouts.get(&name).unwrap_or_else(|| {
        ice!("make_enum_descriptor: enum '{}' has no registered layout — ensure_type_instantiated must be called first", name)
    });
    …
}
```

The panic unwinds through the C ABI's `catch_unwind` thunk and surfaces to the
host as `Noiser module compilation panicked`, i.e. **the host loses the ICE
message entirely** unless it is also capturing the Rust panic on stderr. Any
fix should route this through the normal `CompileError` path with a source span
rather than a panic.

## Secondary finding — `vec_new` is not importable from `core::vec`

`vec_new` works only as an unqualified builtin. Importing it fails:

```noiser
import { vec_new } from core::vec;
@fn.report: fn(x: f32) -> void;
pub fn main() -> f32 { var v = vec_new::<f32>(); v.push(1.0); @fn.report(1.0); 0.0 }
0.0
```

```
COMPILE FAIL: symbol 'vec_new' not found in module 'core::vec' (available: any, map,
last, min, first, swap, reverse, max, filter, max_f32, is_empty, max_i32, all,
min_i32, min_f32, contains, fold, index_of)
```

This is inconsistent, because the corelib module itself uses the symbol —
`noiser-corelib/core/vec.noiser`:

```
118:    var result = vec_new::<U>();      // inside `map`
127:    var result = vec_new::<T>();      // inside `filter`
```

So `core::vec` is implemented in terms of a symbol that user code cannot import
from `core::vec`. Either `vec_new` should be re-exported from `core::vec`, or
the error should say "`vec_new` is a builtin; do not import it".

Minor diagnostics nit in the same message: the "available:" list is emitted in a
**different order on every run** (three consecutive runs gave three different
orderings), which suggests it is being iterated from a `HashMap`. Sorting it
would make compiler output diffable and test snapshots stable.

## Use cases / why it matters

badlands' brains are one `pub gen fn brain(entity: i32) -> i32` coroutine per
entity, resumed once per tick (`game/src/brain.cpp`,
`scripts/brains/hero.noiser:200`). That means **the entry generator is where all
the game logic lives** — it is not an edge case, it is the only place brain code
is written. Every affected construct is therefore hit at the worst possible
spot.

Concretely, brains want `Vec` + `pop` for:

- **A behaviour stack.** `engage-then-return-to-post` pushes "return to post"
  before pushing "engage"; when engage finishes, `pop` restores the previous
  behaviour. This is the standard encoding for interruptible AI, and it is
  exactly `let next = stack.pop();` — the ICE-ing form.
- **A route queue.** `patrol` and `commute home->work->home` hold a `Vec` of
  waypoints and pop the next one on arrival. Naming the popped waypoint
  (`let wp = route.pop();`) before deciding what to do with it is the obvious
  way to write it.
- **Perception scratch buffers.** Collecting nearby entities from host calls
  into a `Vec`, then draining it.

The workaround (below) exists, but it costs the thing that makes coroutine
brains worth having: any logic that needs a named `Maybe` intermediate has to be
lifted out of the coroutine into a plain function, which means it **cannot span
a `yield`**. A behaviour stack whose pop happens in a plain helper cannot then
suspend inside the popped behaviour. So the workaround covers the "compute a
value" cases and not the "drive a multi-tick behaviour" cases — and the latter
are separately blocked by the companion report.

## Minimum test cases

Ordered so the first two are the smallest useful regression tests.

| # | Case | Expected | Actual |
|---|---|---|---|
| 1 | `let a = v.pop();` in the **entry** `pub gen fn` | compiles | **ICE** `Maybe$f32 has no registered layout` (5/5) |
| 2 | same statement in a **non-entry** `gen fn` | compiles | **PASS** (3/3) — the A/B partner for #1 |
| 3 | same statement in a plain `fn`, result matched | reports `2` | **PASS** (3/3) |
| 4 | `let m = maybe_val(3.0);`, `fn maybe_val(x:f32)->?f32`, in the entry gen | compiles | **PASS** — user-written `Maybe` is fine |
| 5 | `match v.pop() { .Just(x) => …, .Nothing => … }` in the entry gen, no binding | reports `2` | **PASS** (3/3) |
| 6 | `let r = p.next();`, `p` a sub-generator, in the entry gen | compiles | **ICE**, same message |
| 7 | `let (tag, val) = p.next();` in the entry gen | compile **error** (`.next()` is `?f32`, not a tuple) | **ICE**, same message (4/4) |
| 8 | `while let .Just(x) = v.pop() { … }` in a plain `fn` | compiles | `Cannot extract variant payload from non-enum type: Tuple([I32, F32])` (3/3) |
| 9 | `while let .Just(v) = maybe_val(i) { … }` in a plain `fn` | reports `6` | **PASS** (3/3) |
| 10 | `import { vec_new } from core::vec;` | resolves, or a "this is a builtin" diagnostic | `symbol 'vec_new' not found in module 'core::vec'` |
| 11 | the #10 error message's `available:` list, run 3× | identical text each run | **three different orderings** |

#8 and #9 belong here rather than in the companion report because they are the
same "builtin-produced `Maybe` was never instantiated" defect reaching a
different consumer — `while let` sees the un-instantiated enum as
`Tuple([I32, F32])` instead of ICE-ing.

## Example verification programs

### V1 — the ICE (must compile and report `2.0` per resume)

```noiser
@fn.report: fn(x: f32) -> void;

pub gen fn brain(e: i32) -> i32 {
    loop {
        var v = vec_new::<f32>();
        v.push(1.0);
        v.push(2.0);
        let a = v.pop();
        match a { .Just(x) => @fn.report(x), .Nothing => @fn.report(-1.0), }
        yield 0;
    }
}
0.0
```

Expected over 3 resumes: `report(2) report(2) report(2)`.

Currently: `ICE: make_enum_descriptor: enum 'Maybe$f32' has no registered layout
— ensure_type_instantiated must be called first`, surfaced to the host as
`COMPILE FAIL: Noiser module compilation panicked`. Deterministic.

### V2 — the entry/non-entry A/B (fix localiser)

```noiser
// --- V2a: compiles today. Must keep compiling. ---
@fn.report: fn(x: f32) -> void;
gen fn maker() -> f32 {
    var v = vec_new::<f32>(); v.push(1.0); v.push(2.0);
    let a = v.pop();
    yield 1.0;
}
pub gen fn brain(e: i32) -> i32 { loop { @fn.report(0.0); yield 0; } }
0.0
```

Currently prints `report(0)` per resume — correct.

```noiser
// --- V2b: identical `let a = v.pop();`, now in the entry generator. ---
@fn.report: fn(x: f32) -> void;
pub gen fn maker(e: i32) -> i32 {
    var v = vec_new::<f32>(); v.push(1.0); v.push(2.0);
    let a = v.pop();
    loop { @fn.report(1.0); yield 0; }
}
0.0
```

Expected `report(1)` per resume. Currently ICEs (3/3). **V2a compiling while
V2b ICEs is the fix criterion**: after the fix both must compile.

### V3 — the workaround, as a "must not regress" test

```noiser
@fn.report: fn(x: f32) -> void;

fn use_pop() -> f32 {
    var v = vec_new::<f32>();
    v.push(1.0);
    v.push(2.0);
    match v.pop() { .Just(x) => x, .Nothing => -1.0, }
}

pub gen fn brain(e: i32) -> i32 { loop { @fn.report(use_pop()); yield 0; } }
0.0
```

Currently prints `report(2) report(2) report(2)` — correct, 5/5 loads. This is
what badlands ships today.

## Workarounds

**Yes — one exists, with two caveats.**

Hoist the `Maybe`-producing call into a plain `fn` and return an already-decided
value to the generator (V3 above). Verified working, 5/5 loads.

Equivalently, avoid naming the intermediate: `match v.pop() { … }` directly in
the entry generator also works (row 4 in the table above) — the ICE needs the
`let` binding.

**Caveat 1 — the hoisted function cannot yield.** A plain `fn` cannot suspend,
so anything that must span a tick boundary (drive a behaviour, wait for arrival)
cannot use this workaround. Those cases are blocked by
`2026-07-22-nested-generator-next-returns-tuple-not-maybe.md`.

**Caveat 2 — host calls in the hoisted function can be silently dropped.**
While validating this workaround we found that a `void` host call in statement
position inside a plain `fn` is eliminated when its arguments derive only from
parameters and constants:

```noiser
@fn.report: fn(x: f32) -> void;
fn side(k: f32) -> f32 { @fn.report(k); k + 1.0 }   // this @fn.report NEVER fires
pub gen fn brain(e: i32) -> i32 {
    var i = 0.0;
    loop { i = i + 1.0; let q = side(i * 100.0); @fn.report(q); yield 0; }
}
0.0
```
Output is `report(101) report(201) report(301)` — the inner `report(100)`,
`report(200)`, `report(300)` never happen. If the inner call's argument depends
on another host call (e.g. `@fn.feed(e)`), it survives. Likewise a whole call
whose result is unused (`let q = side();` with `q` dead, or a bare `side();`
statement) is eliminated along with its host calls. This is a separate purity/DCE
issue, filed here only as a hazard note for anyone applying this workaround —
it makes the workaround unsafe for helpers that exist for their side effects.

## Related

- `docs/noiser-bugs-upstream/2026-07-22-nested-generator-next-returns-tuple-not-maybe.md`
  — same ICE via `Generator::next`, plus the runtime corruption of the entry
  generator. Shares the entry-generator dependency; likely one root cause.
- `docs/noiser-bugs-upstream/2026-07-22-generator-passed-to-function-loses-state.md`
  — shows `.next()` typing correctly inside a plain `fn`, consistent with the
  "instantiation is skipped on the entry-generator path" reading.
- `docs/noiser-bugs-upstream/2026-07-13-struct-of-enums-return-from-perception-corrupts.md`
  — earlier enum-payload-handling bug.
- `docs/noiser-feedback.md` — badlands' running noiser integration log.
- Source: `noiser-compiler/src/bytecode/type_utils.rs:1109`;
  `noiser-corelib/core/vec.noiser:118,127`.
- Docs: `internals/memory-model.md` § *Standalone Generators*;
  `core-library/iterators.md`.
