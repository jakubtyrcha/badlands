# Inline generic bounds: parameterized protocols don't parse, and bare-protocol bounds ignore user `impl` blocks

- **Label:** `compiler` (bound → `impl` resolution for inline constraints; the
  parse half below is `parser`/diagnostics)
- **Date:** 2026-07-22
- **Version (noiser sha):** `52174b2c9e517d9daa2ad6f11fb4e264fd5fec0d`
- **Status:** new
- **Discovered in:** badlands — writing generic helpers over reusable AI
  behaviour blocks (a `drive` that advances any behaviour implementing
  `Iterator`)
- **Backends observed:** VM (badlands runs `NoiserBackend::kVM`); WASM not tested

## Problem

Two separate defects in the **inline** generic-constraint syntax `<T: P>`. The
`where`-clause form is unaffected in both cases.

1. **Parameterized protocol in an inline bound does not parse.**
   `fn drive<I: Iterator<f32>>(it: I) -> f32 { … }` fails with
   `found '<' expected identifier, '+', ',', or '>'`. This is a *documented*
   limitation (`core-language/generics-protocols.md` → "Inline Constraints":
   *"Inline constraints support bare protocol names only … For parameterized
   protocols like `Add<f32>`, use a where clause instead"*), and the where-clause
   form does work — so the defect is that the diagnostic is a bare parser error
   that never names the restriction or the supported alternative.

2. **Inline bound on a *user-declared* protocol does not see the user's `impl`.**
   The documented-supported form `fn f<T: Foo>(x: T)` with a user
   `protocol Foo { fn foo(self) -> f32; }` and `impl Foo for Bar { … }` in the
   same file fails at monomorphization with
   `type 'Bar' does not implement method 'foo' required by protocol 'Foo'` —
   even though the `impl` is right there. Replacing the inline bound with
   `where T: Foo` and changing nothing else compiles and runs correctly.
   This one is a real compiler bug, not a documented restriction.

Defect 2 also fires for `impl Foo for f32` (a builtin receiver), and for
multi-constraint inline bounds `<T: Foo + Baz>`. Inline bounds on **corelib**
protocols with builtin impls (`<T: Zero + Ord>` on `f32`) do work — the
resolution failure is specific to `impl` blocks declared in user source.

## Minimum Reproduction

### Defect 1 — parameterized protocol in an inline bound

```noiser
@fn.report: fn(x: f32) -> void;
gen fn steps() -> f32 { loop { yield 1.0; yield 2.0; } }
fn drive<I: Iterator<f32>>(it: I) -> f32 { match it.next() { .Just(v) => v, .Nothing => -1.0, } }
pub gen fn brain(e: i32) -> i32 { var p = steps(); loop { @fn.report(drive(p)); yield 0; } }
0.0
```

### Defect 2 — inline bound on a user protocol ignores `impl`

```noiser
@fn.report: fn(x: f32) -> void;
protocol Foo { fn foo(self) -> f32; }
struct Bar { v: f32 }
impl Foo for Bar { fn foo(self) -> f32 { self.v } }
fn call_foo<T: Foo>(x: T) -> f32 { x.foo() }          // <-- inline bound
pub gen fn brain(e: i32) -> i32 { loop { @fn.report(call_foo(Bar { v: 7.0 })); yield 0; } }
0.0
```

Changing **only** line 5 to the where-clause form makes it compile and run:

```noiser
fn call_foo<T>(x: T) -> f32 where T: Foo { x.foo() }  // <-- identical semantics, works
```

## Expected

1. `<I: Iterator<f32>>` either parses (the docs define
   `protocol Iterator<Item> { fn next(mut self) -> ?Item; }` and state that
   generators automatically implement it, so a bound naming it should be
   expressible), or — if the `>`/`>>` ambiguity makes that intentional — the
   error should say so and point at `where`, e.g.
   *"parameterized protocol bounds are not supported inline; use `where I: Iterator<f32>`"*.
2. `fn call_foo<T: Foo>(x: T) -> f32 { x.foo() }` resolves `Bar`'s `impl Foo for Bar`
   exactly as the `where T: Foo` spelling does, and reports `7`.

## Actual (verbatim)

Defect 1:

```
COMPILE FAIL: found '<' expected identifier, '+', ',', or '>'  [main:3:21]
```

Defect 2:

```
COMPILE FAIL: type 'Bar' does not implement method 'foo' required by protocol 'Foo'
```

with `impl Foo for f32` instead of a struct:

```
COMPILE FAIL: type 'f32' does not implement method 'foo' required by protocol 'Foo'
```

Both messages carry no span. Neither is an ICE; both are hard compile errors.

## Use cases

badlands composes hero/town AI out of reusable behaviour blocks and wants
generic helpers over them — the canonical one being a `drive` that advances
*any* behaviour implementing `Iterator`, so a brain can hold heterogeneous
sub-behaviours (patrol, commute, engage-then-return) behind one call:

```noiser
fn drive<I>(it: I) -> f32 where I: Iterator<f32> { match it.next() { .Just(v) => v, .Nothing => -1.0, } }
```

The wider pattern is protocol-bounded utility functions over game-side
"capability" protocols declared in script (`protocol Behaviour`,
`protocol Scorable`, `protocol Steerable`) — i.e. exactly defect 2's shape:
a protocol *and* its impls written in user source, then constrained generically.
Being pushed onto `where` for every such helper is survivable; silently losing
the inline form for all user-declared protocols is the part that bites, because
the inline form is the one the docs present first and the error blames the
user's `impl` rather than the bound syntax.

## Minimum test cases

All run through the badlands snippet runner (VM backend, host `report` bound).

| # | Form | Result |
|---|------|--------|
| 1 | inline, parameterized protocol: `fn drive<I: Iterator<f32>>(it: I)` | ❌ `found '<' expected identifier, '+', ',', or '>'` |
| 2 | **where**, parameterized protocol: `fn drive<I>(it: I) -> f32 where I: Iterator<f32>` | ✅ compiles (runtime then hits a *different* bug — see Related) |
| 3 | inline, bare **user** protocol: `fn f<T: Foo>(x: T)` + `impl Foo for Bar` | ❌ `type 'Bar' does not implement method 'foo' required by protocol 'Foo'` |
| 4 | **where**, bare user protocol: `fn f<T>(x: T) where T: Foo` | ✅ compiles, reports `8` |
| 5 | inline, bare user protocol, generic fn **declared but never called** | ✅ compiles (failure is at monomorphization, not declaration) |
| 6 | inline, **corelib** protocol w/ builtin impl: `fn c<T: Zero + Ord>(x: T)` on `f32` | ✅ compiles, runs (docs' own example) |
| 7 | inline, two user protocols: `fn b<T: Foo + Baz>(x: T)` | ❌ same as #3 |
| 8 | inline, user protocol implemented for a builtin: `impl Foo for f32`, `fn f<T: Foo>` | ❌ `type 'f32' does not implement method 'foo' …` |
| 9 | **where**, user *parameterized* protocol: `impl Src<f32> for Bar`, `where T: Src<f32>` | ✅ compiles, reports `4` |
| 10 | **where**, docs' `Addable<T>` example verbatim shape | ✅ compiles, reports `5` |
| 11 | unconstrained generic: `fn ident<T>(x: T) -> T` | ✅ compiles, reports `5` |
| 12 | method call on the concrete type, no generics: `Bar{v:6.0}.foo()` | ✅ compiles, reports `6` — the `impl` itself is sound |
| 13 | where-bound + reference param: `fn drive<I>(it: &mut I) … where I: Iterator<f32>` | ❌ `function call 'next' not found or not yet supported in NoIR compilation` |
| 14 | where-bound, argument type does **not** implement the protocol | ❌ `function call 'foo' not found or not yet supported in NoIR compilation` — bound *is* enforced, but the message doesn't mention the bound |

#5 + #12 together localize defect 2: the `impl` is valid and the bound parses;
only the inline-bound → `impl` lookup at monomorphization fails.

## Example verification programs

### A. Defect 2 repro — inline bound (expected `report(7)` per tick; actually fails to compile)

```noiser
@fn.report: fn(x: f32) -> void;
protocol Foo { fn foo(self) -> f32; }
struct Bar { v: f32 }
impl Foo for Bar { fn foo(self) -> f32 { self.v } }
fn call_foo<T: Foo>(x: T) -> f32 { x.foo() }
pub gen fn brain(e: i32) -> i32 { loop { @fn.report(call_foo(Bar { v: 7.0 })); yield 0; } }
0.0
```

Actual: `COMPILE FAIL: type 'Bar' does not implement method 'foo' required by protocol 'Foo'`

### B. Same program, where-clause — works (control for A)

```noiser
@fn.report: fn(x: f32) -> void;
protocol Foo { fn foo(self) -> f32; }
struct Bar { v: f32 }
impl Foo for Bar { fn foo(self) -> f32 { self.v } }
fn call_foo<T>(x: T) -> f32 where T: Foo { x.foo() }
pub gen fn brain(e: i32) -> i32 { loop { @fn.report(call_foo(Bar { v: 8.0 })); yield 0; } }
0.0
```

Actual (2 ticks): `COMPILE OK` / `report(8)` / `report(8)` — correct.

### C. Parameterized user protocol via `where` — works end to end

```noiser
@fn.report: fn(x: f32) -> void;
protocol Addable<T> { fn add(self, other: T) -> T; }
struct N { v: f32 }
impl Addable<N> for N { fn add(self, other: N) -> N { N { v: self.v + other.v } } }
fn unwrap(n: N) -> f32 { n.v }
fn sum<T>(a: T, b: T) -> T where T: Addable<T> { a.add(b) }
pub gen fn brain(e: i32) -> i32 { loop { @fn.report(unwrap(sum(N{v:2.0}, N{v:3.0}))); yield 0; } }
0.0
```

Actual (3 ticks): `COMPILE OK` / `report(5)` ×3 — correct. Note the trip hazard:
inlining `sum(...).v` instead of routing through `unwrap` fails with
`unknown field 'v' on type 'T'`, i.e. the monomorphized return type is not
resolved back to `N` at the call site (possibly a third, separate defect —
not pursued here).

## Workarounds

- **Always spell protocol bounds with `where`.** `where T: Foo`,
  `where T: Foo + Baz`, `where I: Iterator<f32>` all work, for user-declared and
  corelib protocols alike, generic and non-generic. This is a complete
  workaround for both defects — badlands uses it unconditionally and treats the
  inline `<T: P>` form as unavailable.
- Pass generic values **by value**, not `&mut` (test #13).
- Bind a generic function's result to a concrete-typed local via a helper
  before projecting fields off it (Example C's `unwrap`).

## Related

- `docs/noiser-feedback.md` — integration log; this is one of the four bugs in
  the generics/generator cluster filed 2026-07-22.
- `docs/noiser-bugs-upstream/2026-07-22-nested-generator-next-returns-tuple-not-maybe.md`
  — `.next()` inside a `gen fn` body; the `where I: Iterator<f32>` form here is
  the workaround that *does* type-check that call, which is how the two
  interact.
- `docs/noiser-bugs-upstream/2026-07-22-generator-passed-to-function-loses-state.md`
  — test case #2 above compiles, but its *runtime* behaviour is **nondeterministic
  run to run** on an unchanged binary and unchanged source (25 runs: 12× "reports
  `1`, then the second resume fails with `Invalid generator state: RestoreState:
  state buffer index 0 >= buffer len 0`", 13× "three resumes all succeed and
  make **zero** host calls"). The unconstrained-generic spelling (test #11 shape,
  `fn drive<T>(g: T)`) splits the same two ways (8 / 17 of 25). That runtime
  failure belongs to that bug, not this one — but the coin-flip between two
  different wrong behaviours points at uninitialized/dangling sub-context state
  rather than a deterministic codegen mistake, and is worth attaching there.
- Docs: `core-language/generics-protocols.md` — "Protocols", "Where Clauses" →
  "Inline Constraints", "Iterator Protocol".
