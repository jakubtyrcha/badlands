# `@fn` host function taking a `vec2/vec3/vec4` argument compiles, then rejects every call with `expected Unknown`

**Date**: 2026-07-22
**Version (noiser sha)**: `52174b2c9e517d9daa2ad6f11fb4e264fd5fec0d`
**Label**: `compiler`
**Status**: new

**Discovered in**: badlands — noiser drives per-entity game-AI brains
(`pub gen fn brain(entity: i32)`); command host calls want to take a target
position, perception host calls want to return one.
**Backends observed**: VM (`NoiserBackend::kVM`). WASM/JIT not tested — the
failure is in the shared compiler front end (`expr_compiler.rs` argument
inference), so both should be affected.

## Problem

A `@fn` host function declared with a **lowercase** vector parameter type
(`vec2`, `vec3`, `vec4` — the spelling the book documents) **compiles cleanly as
a declaration** and then makes **every call to it a compile error**, with a
diagnostic that names an internal type the user never wrote:

```
COMPILE FAIL: @fn.sink argument `v` (position 0): expected Unknown, got HomogTuple { element_type: F32, size: 4 }
```

**The declaration-vs-call split is the headline and is new information.** Older
badlands notes claimed the *declaration* failed; it does not. `@fn.sink: fn(v: vec4) -> f32;`
with no call site in the program is `COMPILE OK` and the program runs. The error
appears only when the function is invoked — which means the type error is
reported at the wrong place (the call, in the user's brain logic) instead of at
the wrong thing (the signature, one line at the top of the file).

**Nothing can satisfy the parameter.** Passing a 4-tuple literal
`@fn.sink((1.0, 2.0, 3.0, 4.0))` fails with the *identical* message — the
"expected" side is `Unknown`, so no argument type can ever match. The parameter
is uninhabitable, not merely mistyped.

Root cause is shared with
`docs/noiser-bugs-upstream/2026-07-22-host-fn-vecn-return-ices-compiler.md`:
`NOISE_STDLIB` never registers a lowercase `vecN` type alias, and an unresolved
`Type::Named` in a `@fn` signature is **silently degraded to
`TypeDescriptor::Unknown`** at declaration time instead of being rejected. In
*return* position that degradation reaches a layout query and ICEs the compiler;
in *argument* position it reaches `infer_call_with_signature`, which dutifully
reports `expected Unknown`. **The two should be fixed together.**

## Minimum Reproduction

Smallest failing snippet:

```noiser
@fn.sink: fn(v: vec4) -> f32;
@fn.report: fn(x: f32) -> void;
pub gen fn brain(e: i32) -> i32 {
    loop { let v = vec4(1.0, 2.0, 3.0, 4.0); @fn.report(@fn.sink(v)); yield 0; }
}
0.0
```

```
COMPILE FAIL: @fn.sink argument `v` (position 0): expected Unknown, got HomogTuple { element_type: F32, size: 4 }  [main:4:57]
```

**Delete the call and it compiles** — this is the declaration-vs-call split:

```noiser
@fn.sink: fn(v: vec4) -> f32;          // <- never called
@fn.report: fn(x: f32) -> void;
pub gen fn brain(e: i32) -> i32 { loop { @fn.report(1.0); yield 0; } }
0.0
```

```
COMPILE OK
    report(1)
DONE (1 host calls over 1 ticks)
```

**Two-character diff that fixes it** — `vec4` → `Vec4` (the registered stdlib
alias). **Two-line diff that also fixes it** — `type vec4 = (f32; 4);` at the
top of the script.

## Expected

1. **`vec2`/`vec3`/`vec4` must resolve as parameter types.** The book lists them
   as the float vector types alongside `ivec2`/`ivec3`/`ivec4`
   (`core-language/type-system.md:18-19`). `ivecN` *is* registered lowercase
   (`stdlib.rs:637-639`) and works as a `@fn` parameter today; `vecN` is not
   registered at all. That asymmetry is the bug in one line.
2. **A `@fn` signature naming an unresolvable type must fail at the
   declaration**, with the declaration's span:
   `unknown type 'vec4' in host function signature @fn.sink  [main:1:18]`.
   Not silently accepted and then relitigated at every call site.
3. **`expected Unknown` must never reach a user.** `TypeDescriptor::Unknown` is
   an internal sentinel; surfacing it in a diagnostic tells the user nothing and
   actively misleads (it reads as "this parameter is generic", not "the compiler
   forgot this type"). If a signature slot is `Unknown` at call-check time,
   that is an internal invariant violation, not a type mismatch.
4. **`vecN` should accept exactly what an N-tuple accepts, because it *is* one.**
   `vec4` should lower identically to `(f32; 4)` / `(f32, f32, f32, f32)`:
   N consecutive f32 VM stack slots. `@fn.sink(vec4(1.0,2.0,3.0,4.0))` and
   `@fn.sink((1.0,2.0,3.0,4.0))` should both be accepted for a `vec4` parameter,
   and both should be accepted for a `(f32,f32,f32,f32)` parameter (the latter
   pair already works — rows `N09`/`N10`).

### How `vecN` maps across the C ABI

Host-side (`noiser-vm/src/noiser.hpp`) already collapses `glm::vecN` and
`std::tuple` to the same descriptor, so a fix costs nothing at the boundary:

| C++ type | `TypeEnumFor<T>()` | `TupleSizeFor<T>()` | `VmSlotCountFor<T>()` |
|---|---|---|---|
| `glm::vec2` | `TypeEnum::kTuple` | 2 | 2 |
| `glm::vec3` | `TypeEnum::kTuple` | 3 | 3 |
| `glm::vec4` | `TypeEnum::kTuple` | 4 | 4 |
| `std::tuple<float,float,float,float>` | `TypeEnum::kTuple` | 4 | 4 |

(`noiser.hpp:1576-1641`; `arg_slots` is the fold
`(0 + ... + VmSlotCountFor<Args>())`, `noiser.hpp:2269-2271`, `2350-2352`.)

Because both are **kTuple/size N**, a C++ callable taking or returning
`glm::vec4` binds against a script-side 4-tuple declaration with no cast and no
shim — **that is exactly the workaround badlands ships**, pinned by
`game/tests/noiser_smoke_tests.cpp`. Argument marshalling for a `vecN`
parameter therefore needs no runtime work at all; only the front end has to
resolve the name.

Proof the ABI is already correct, from the harness (`Vec2`, capital — the
registered spelling): a host `void(float,float)` bound to a script
`@fn.report2: fn(v: Vec2) -> void;` and called as `@fn.report2(vec2(7.0, 9.0))`
delivers both components intact:

```
COMPILE OK
    report2(7, 9)
DONE (1 host calls over 1 ticks)
```

## Actual

All output verbatim from the badlands snippet harness at `52174b2`.

### `vec4` parameter, generator context

```noiser
@fn.sink: fn(v: vec4) -> f32;
@fn.report: fn(x: f32) -> void;
pub gen fn brain(e: i32) -> i32 {
    loop { let v = vec4(1.0, 2.0, 3.0, 4.0); @fn.report(@fn.sink(v)); yield 0; }
}
0.0
```

```
COMPILE FAIL: @fn.sink argument `v` (position 0): expected Unknown, got HomogTuple { element_type: F32, size: 4 }  [main:4:57]
```

### `vec4` parameter, plain-fn context — identical

```noiser
@fn.sink: fn(v: vec4) -> f32;
pub fn main() -> f32 { let v = vec4(1.0, 2.0, 3.0, 4.0); @fn.sink(v) }
```

```
COMPILE FAIL: @fn.sink argument `v` (position 0): expected Unknown, got HomogTuple { element_type: F32, size: 4 }  [main:2:58]
```

### `vec3` and `vec2` — same, with the size varying

```
COMPILE FAIL: @fn.sink argument `v` (position 0): expected Unknown, got HomogTuple { element_type: F32, size: 3 }  [main:4:52]
COMPILE FAIL: @fn.sink argument `v` (position 0): expected Unknown, got HomogTuple { element_type: F32, size: 2 }  [main:4:47]
```

### Passing a plain 4-tuple to the same declaration — **still fails**

```noiser
@fn.sink: fn(v: vec4) -> f32;
@fn.report: fn(x: f32) -> void;
pub gen fn brain(e: i32) -> i32 { loop { @fn.report(@fn.sink((1.0, 2.0, 3.0, 4.0))); yield 0; } }
0.0
```

```
COMPILE FAIL: @fn.sink argument `v` (position 0): expected Unknown, got HomogTuple { element_type: F32, size: 4 }  [main:4:23]
```

The parameter cannot be satisfied by anything. (Note the `got` side is
`HomogTuple{F32,4}` for both `vec4(…)` and the tuple literal — further
confirmation that `vec4(…)` is nothing more than a tuple constructor.)

### Declaration alone — **compiles and runs**

```noiser
@fn.sink: fn(v: vec4) -> f32;
@fn.report: fn(x: f32) -> void;
pub gen fn brain(e: i32) -> i32 { loop { @fn.report(1.0); yield 0; } }
0.0
```

```
COMPILE OK
    report(1)
DONE (1 host calls over 1 ticks)
```

Same for `fn(v: ivec4)`, `fn(v: Wobble)` — any unresolvable name declares fine.

### With `import { Vec4 } from core::linalg;` — unchanged

```
COMPILE FAIL: @fn.sink argument `v` (position 0): expected Unknown, got HomogTuple { element_type: F32, size: 4 }  [main:4:53]
```

The import is irrelevant: `noiser-corelib/core/linalg.noiser` declares
`pub struct Vec4` (nominal), and there is no lowercase alias to import. (The
generated doc page `core-library/generated/linalg.md:27` still advertises
`pub type vec4 = GVec<4>` — **stale, needs regenerating**.)

### Root-cause probe — a typo'd type name gives the identical shape

```noiser
@fn.sink: fn(v: Wobble) -> f32;
@fn.report: fn(x: f32) -> void;
pub gen fn brain(e: i32) -> i32 { loop { @fn.report(@fn.sink(1.0)); yield 0; } }
0.0
```

```
COMPILE FAIL: @fn.sink argument `v` (position 0): expected Unknown, got F32  [main:3:53]
```

Not vector-specific. Any unregistered name in a `@fn` parameter becomes
`Unknown` and poisons every call site.

### Mirror image — capitalised `IVec4` fails the same way

```
COMPILE FAIL: @fn.sink argument `v` (position 0): expected Unknown, got Tuple([I32, I32, I32, I32])  [main:3:53]
```

while lowercase `ivec4` is fine. Exactly one spelling of each pair is
registered, and they disagree about which.

## Trace

1. `NOISE_STDLIB` (`noiser-compiler/src/stdlib.rs:158-160`) registers **only**
   `type Vec2 = (f32; 2)`, `type Vec3 = (f32; 3)`, `type Vec4 = (f32; 4)` —
   **capitalised**. Lines `637-639` register
   `type ivec2/ivec3/ivec4 = (i32; N)` — **lowercase**. No lowercase `vecN`, no
   capitalised `IVecN`. Both spellings are documented together at
   `core-language/type-system.md:18-19`.
2. Lowercase `vec2/vec3/vec4` do exist in the stdlib as constructor
   **functions** (`fn vec4(x,y,z,w) -> (f32,f32,f32,f32)`), which is why
   `vec4(1.0,2.0,3.0,4.0)` is a valid *expression* whose type is
   `HomogTuple{F32,4}` while `vec4` is not a valid *type*.
3. `@fn` declaration registration maps the AST parameter type to a
   `TypeDescriptor`. An unregistered `Type::Named` is **silently mapped to
   `TypeDescriptor::Unknown`** instead of erroring — hence declaration-only
   scripts compile clean.
4. At the call site, `noiser-compiler/src/bytecode/expr_compiler.rs:12008-12034`
   runs `infer::infer_call_with_signature(name, sig, &arg_td_vec, &ctx)`. The
   signature slot is `Unknown`, the argument is `HomogTuple{F32,4}`, so
   `InferError::TypeMismatch { param_index, expected, got }` comes back and is
   formatted verbatim into the user-facing string:

   ```rust
   return Err(format!(
       "{}: @fn.{} argument `{}` (position {}): expected {}, got {}",
       loc, name, param_name, param_index, expected, got,
   ));
   ```

   `expected` is `Unknown` — an internal sentinel rendered as if it were a type
   the user should have supplied.

The return-position twin of step 3 detonates instead of diagnosing: see
`abi_layout.rs:282` / `type_layout.rs:1172` in the companion report.

## Use cases / why it matters

badlands runs one noiser generator per entity as its AI brain
(`scripts/brains/hero.noiser`, host functions in `game/src/brain.cpp`). Positions
are the unit of currency in both directions:

- **In:** every perception call hands the script a world position, or a
  `(pos, stat, flag)` bundle. All six are `glm::vec4` on the C++ side.
- **Out:** every movement command takes a destination.

With `vecN` unusable in a signature, both directions flatten:

```noiser
// commands out — a destination has to be splayed into loose scalars
@fn.intent_move_to: fn(e: i32, x: f32, z: f32) -> void;
```

and the perception side pays for it at every call site
(`scripts/brains/hero.noiser:76-81`):

```noiser
let (sx, sz, hp, cd)       = @fn.perceive_self(e);
let (tx, tz, thp, has)     = @fn.perceive_target(e);
let (ax, az, aex, _)       = @fn.perceive_building(e, APOTHECARY);
let (vx, vz, vex, _)       = @fn.perceive_building(e, TAVERN);
let (hx, hz, hex, _)       = @fn.perceive_home(e);
let (fat, bor, tod, night) = @fn.perceive_needs(e);
```

Twenty-four positional bindings whose meaning lives only in a trailing comment
on the declaration. Concretely, the argument bug costs:

- **No vector values may cross the host boundary in either direction.** A
  perceived position cannot be handed back to a command host call as a unit —
  it has to be taken apart and passed component-wise, which is where field-order
  mistakes become silent type-correct miscompiles.
- **`core::linalg` is unreachable on perceived data.** A brain wants
  `distance(self_pos, target_pos)`, `normalize(target - self)`, `dot(…)`.
  With flat tuples the script open-codes each against loose `sx`/`sz` floats and
  the corelib vector ops sit unused. Repacking into `core::linalg::Vec2` is not
  an escape either — it is a *nominal struct*, distinct from the
  tuple-optimised `(f32, f32)`, which is already one of the two workarounds
  called out in `hero.noiser`'s header comment.
- **The diagnostic actively misleads.** `expected Unknown` reads as "this
  parameter is generic / inference failed on your side". The real cause — a
  type name the stdlib never registered — is invisible, and it is reported on
  the *call* line rather than the *declaration* line. Diagnosing this took a
  full spelling/position matrix (below); a one-line
  `unknown type 'vec4' in host function signature` would have taken seconds.

The fix is cheap and the payoff is immediate: with `vecN` resolving, commands
become `@fn.intent_move_to: fn(e: i32, dest: vec2) -> void;`, perception becomes
`let s = @fn.perceive_self(e);` with named `.x/.y/.z/.w`, and `core::linalg`
becomes usable on perceived data — **with zero change to the C++ bindings**,
which already deal in `glm::vec4`.

## Minimum test cases

All rows run through the badlands snippet harness against `52174b2`; corelib
rows via `--module-path noiser-corelib`. `report2` is bound host-side to
`void(float, float)` and prints its arguments; `sink` is intentionally unbound
(so a clean compile surfaces as a runtime "not bound", which is the expected
result for a compile-only row).

### Argument type — dimension × context

| # | Declaration | Call site | Observed |
|---|---|---|---|
| N01 | `fn(v: vec4) -> f32` | **not called** | **Clean**, program runs |
| N02 | `fn(v: vec4) -> f32` | `@fn.sink(vec4(1.0,2.0,3.0,4.0))`, `gen fn` body | **FAIL** `expected Unknown, got HomogTuple { element_type: F32, size: 4 }` |
| N03 | `fn(v: vec4) -> f32` | same, plain `fn` body | **FAIL**, identical message |
| N04 | `fn(v: vec4) -> f32` | same, **top level** | **FAIL**, identical message |
| N05 | `fn(v: vec4) -> void` | `@fn.sink(vec4(…))` (void return) | **FAIL**, identical — return type irrelevant |
| N06 | `fn(v: vec3) -> f32` | `@fn.sink(vec3(1.0,2.0,3.0))` | **FAIL** `… got HomogTuple { element_type: F32, size: 3 }` |
| N07 | `fn(v: vec2) -> f32` | `@fn.sink(vec2(1.0,2.0))` | **FAIL** `… got HomogTuple { element_type: F32, size: 2 }` |
| N08 | `fn(v: vec4) -> f32` | `@fn.sink((1.0,2.0,3.0,4.0))` — **4-tuple literal** | **FAIL**, identical — parameter is uninhabitable |

### Argument type — spelling axis (the fix, and the mirror image)

| # | Declaration | Call site | Observed |
|---|---|---|---|
| N09 | `fn(v: Vec4) -> f32` | `@fn.sink(vec4(…))` | **Clean** (registered stdlib alias) |
| N10 | `fn(v: (f32,f32,f32,f32)) -> f32` | `@fn.sink((1.0,2.0,3.0,4.0))` | **Clean** |
| N11 | `fn(v: (f32,f32,f32,f32)) -> f32` | `@fn.sink(vec4(…))` | **Clean** — `vec4(…)` *is* a 4-tuple |
| N12 | `fn(v: (f32; 4)) -> f32` | `@fn.sink(vec4(…))` | **Clean** |
| N13 | `fn(v: ivec4) -> f32` | **not called** | **Clean** |
| N14 | `fn(v: ivec4) -> f32` | `@fn.sink((1, 2, 3, 4))` | **Clean** — lowercase `ivecN` *is* registered |
| N15 | `fn(v: ivec2) -> f32` | `@fn.sink((1, 2))` | **Clean** |
| N16 | `fn(v: IVec4) -> f32` | `@fn.sink((1, 2, 3, 4))` | **FAIL** `expected Unknown, got Tuple([I32, I32, I32, I32])` — exact mirror image |
| N17 | `fn(v: ivec4) -> f32` | `@fn.sink(ivec4(1,2,3,4))` | **FAIL** `function call 'ivec4' not found or not yet supported in NoIR compilation` — no `ivecN` **constructor** exists (separate gap: `ivecN` has a type but no constructor; `vecN` has a constructor but no type) |

### Argument type — import / alias axis

| # | Setup | Observed |
|---|---|---|
| N18 | `import { Vec4 } from core::linalg;` + `fn(v: vec4)`, called | **FAIL**, message unchanged |
| N19 | `type vec4 = (f32; 4);` + `fn(v: vec4)`, called | **Clean** — user-supplied alias fixes it |

### Root-cause probes (not vector-specific)

| # | Snippet | Observed |
|---|---|---|
| N20 | `@fn.sink: fn(v: Wobble) -> f32;` called with `1.0` | **FAIL** `expected Unknown, got F32` — identical shape for a typo'd name |
| N21 | `@fn.sink: fn(v: Wobble) -> f32;` **not called** | **Clean** — declaration never validates its own types |

### Runtime-observable pair (the row that proves the ABI is fine)

| # | Declaration | Call | Observed |
|---|---|---|---|
| N22 | `@fn.report2: fn(v: vec2) -> void;` | `@fn.report2(vec2(7.0, 9.0))` | **FAIL** `expected Unknown, got HomogTuple { element_type: F32, size: 2 }` |
| N23 | `@fn.report2: fn(v: Vec2) -> void;` | `@fn.report2(vec2(7.0, 9.0))` | **Clean** → prints `report2(7, 9)` |
| N24 | `@fn.report2: fn(v: (f32, f32)) -> void;` | `@fn.report2((7.0, 9.0))` | **Clean** → prints `report2(7, 9)` |

N22 vs N23/N24 is the whole bug in three rows: same host binding
(`void(float,float)`), same VM slot count (2), same call expression — only the
spelling of the parameter's type differs.

### Suggested new rows for the existing matrix

`noiser-compiler/src/bytecode/host_fn_vec_return_ice_tests.rs` Matrix C
currently has exactly two param rows, both using registered spellings
(`C1 fn(v: Vec4)`, `C2 fn(v: (f32,f32,f32,f32))`) — both Clean. It needs the
lowercase twins, plus the declared-vs-called split:

```rust
#[test] fn c3_lower_vec4_param_called()  { assert_clean("@fn.set: fn(v: vec4) -> void;\n@fn.set(vec4(0.0,0.0,0.0,0.0));\n0.0", "C3 vec4-param"); }
#[test] fn c4_lower_vec2_param_called()  { assert_clean("@fn.set: fn(v: vec2) -> void;\n@fn.set(vec2(0.0,0.0));\n0.0", "C4 vec2-param"); }
#[test] fn c5_lower_vec4_param_tuple_arg() { assert_clean("@fn.set: fn(v: vec4) -> void;\n@fn.set((0.0,0.0,0.0,0.0));\n0.0", "C5 vec4-param/tuple-arg"); }
#[test] fn c6_unknown_named_param_errors_at_decl() { /* expect Err naming the DECL span, not `expected Unknown` at the call */ }
```

C3-C5 currently fail with `expected Unknown`; C6 currently compiles clean and
defers to the call site.

## Example verification programs

Run with the badlands snippet harness (or any host binding the same probes).
Bindings assumed: `report2(f32, f32) -> void` prints both arguments;
`report(f32) -> void` prints its argument; `feed4(i32) -> glm::vec4` returns
`glm::vec4(1, 2, 3, 1)`; `sink` intentionally left unbound.

### V-A1 — headline, runtime-observable: `vec2` parameter round-trips to the host

```noiser
@fn.report2: fn(v: vec2) -> void;
pub gen fn brain(e: i32) -> i32 { loop { @fn.report2(vec2(7.0, 9.0)); yield 0; } }
0.0
```

Expected after a fix (1 resume) — **this exact output is already produced today
by the `Vec2` twin**, so it is a measured expectation, not a guess:

```
COMPILE OK
    report2(7, 9)
DONE (1 host calls over 1 ticks)
```

Today:

```
COMPILE FAIL: @fn.report2 argument `v` (position 0): expected Unknown, got HomogTuple { element_type: F32, size: 2 }  [main:2:42]
```

Note the host callable is an ordinary `void(float, float)` — no `glm` type
involved. If this row passes, `vecN` argument marshalling is correct end to end.

### V-A2 — `vec4` parameter, compile-only

```noiser
@fn.sink: fn(v: vec4) -> f32;
@fn.report: fn(x: f32) -> void;
pub gen fn brain(e: i32) -> i32 {
    loop { let v = vec4(1.0, 2.0, 3.0, 4.0); @fn.report(@fn.sink(v)); yield 0; }
}
0.0
```

Expected after a fix — compiles; the harness then reports the deliberately
unbound `sink`, which is precisely what the `Vec4` twin does today:

```
COMPILE OK
[Rust VM] Error status=2: Host function 'sink' (slot 16) not bound
  tick 0 RESUME FAIL: Host function 'sink' (slot 16) not bound
```

Today: `COMPILE FAIL: @fn.sink argument 'v' (position 0): expected Unknown, got HomogTuple { element_type: F32, size: 4 }`.

### V-A3 — end-to-end round trip: host `vec4` in, host `vec2` out (needs **both** bugs fixed)

```noiser
@fn.feed4: fn(e: i32) -> vec4;
@fn.report2: fn(v: vec2) -> void;
pub gen fn brain(e: i32) -> i32 {
    loop { let v = @fn.feed4(e); @fn.report2(vec2(v.x, v.y)); yield 0; }
}
0.0
```

Expected after a fix (verified against the `Vec4`/`Vec2` twin):

```
COMPILE OK
    report2(1, 2)
DONE (1 host calls over 1 ticks)
```

Today it does not even reach the argument check — the `-> vec4` return ICEs
first:

```
ICE: type_descriptor_to_abi_layout: Unknown has no physical layout  (abi_layout.rs:282)
```

This is the shape badlands actually wants (perceive a position, hand it to a
command host call), and it is the best single regression guard for the pair.

### V-A4 — regression guard: an unknown type name must diagnose at the declaration

```noiser
@fn.sink: fn(v: Wobble) -> f32;
@fn.report: fn(x: f32) -> void;
pub gen fn brain(e: i32) -> i32 { loop { @fn.report(@fn.sink(1.0)); yield 0; } }
0.0
```

Expected after a fix: a compile error naming the **declaration** span, e.g.
`COMPILE FAIL: unknown type 'Wobble' in host function signature @fn.sink  [main:1:14]`.
Today: `COMPILE FAIL: @fn.sink argument 'v' (position 0): expected Unknown, got F32  [main:3:53]`
— wrong span, and `Unknown` leaked to the user. Fixing only the stdlib name
table would leave this row broken.

## Workarounds

Two work; badlands ships the first because it predates the diagnosis of the
second.

### 1. Flat scalar parameters / tuple types (shipped)

Never name a vector in a `@fn` signature. Commands take loose scalars; anything
composite crosses as an explicit tuple. Host side is unchanged — `glm::vec4` and
a 4-tuple are the same `kTuple`/4 descriptor.

**Before (what we wanted, rejected at every call site):**

```noiser
@fn.perceive_self:  fn(e: i32) -> vec4;
@fn.intent_move_to: fn(e: i32, dest: vec2) -> void;

let s = @fn.perceive_self(e);
@fn.intent_move_to(e, vec2(s.x, s.y));
```

**After (`scripts/brains/hero.noiser`, shipped):**

```noiser
// Perception returns are flat f32 tuples: host fns declared `-> vecN` ICE the
// compiler (docs/noiser-feedback.md #1). The C++ side binds glm::vec4.
@fn.perceive_self:  fn(e: i32) -> (f32, f32, f32, f32);  // (x, z, hp, cooldown)
@fn.intent_move_to: fn(e: i32, x: f32, z: f32) -> void;

let (sx, sz, hp, cd) = @fn.perceive_self(e);
@fn.intent_move_to(e, sx, sz);
```

C++ side is identical either way (`game/src/brain.cpp`):

```cpp
glm::vec4 perceive_self(BadlandsGame& game, int32_t slot) { … }
bind("perceive_self", [g](int32_t e) { return perceive_self(*g, e); });
```

Cost: destinations travel as two unrelated scalars (nothing stops
`intent_move_to(e, sz, sx)` type-checking), and `core::linalg` cannot touch
anything that crosses the boundary.

### 2. Declare the missing aliases yourself (better; not yet adopted)

One line per dimension at the top of the script restores the documented
spelling and fixes the **return** bug at the same time:

```noiser
type vec4 = (f32; 4);
type vec2 = (f32; 2);

@fn.feed4:   fn(e: i32) -> vec4;   // return position — otherwise ICEs
@fn.report2: fn(v: vec2) -> void;  // argument position — otherwise `expected Unknown`
@fn.report:  fn(x: f32) -> void;

pub gen fn brain(e: i32) -> i32 {
    loop {
        let v = @fn.feed4(e);
        @fn.report(v.x); @fn.report(v.y); @fn.report(v.z); @fn.report(v.w);
        @fn.report2(vec2(v.x, v.y));
        yield 0;
    }
}
0.0
```

Verified output:

```
COMPILE OK
    report(1)
    report(2)
    report(3)
    report(1)
    report2(1, 2)
DONE (5 host calls over 1 ticks)
```

Equivalently write `Vec4`/`Vec2` capitalised — but that is the spelling the book
does not document for float vectors, so it reads as a typo to anyone following
the docs.

Neither workaround makes `core::linalg`'s nominal `Vec2`/`Vec3`/`Vec4` struct
methods usable on host-call data — those remain distinct types from the
tuple-optimised form.

## Related

- `docs/noiser-bugs-upstream/2026-07-22-host-fn-vecn-return-ices-compiler.md` —
  **companion bug, same root cause**, `vecN` in *return* position. There the
  `Unknown` degradation reaches a layout query and ICEs
  (`abi_layout.rs:282` / `type_layout.rs:1172`) instead of producing a
  diagnostic. Fix them together; V-A3 above is the joint regression guard.
- `docs/noiser-feedback.md` — badlands' running noiser integration log; item #1
  covers this family and is the reason the brain's host surface is all-flat.
- `game/tests/noiser_smoke_tests.cpp`:
  - `TEST_CASE("KNOWN BUG: host functions returning vecN ICE the compiler")` —
    pins the return-position twin; the argument-position half is **not yet
    pinned** and should be added alongside it.
  - `TEST_CASE("tuple-returning host call (bound as glm::vec4) feeds a generator")` —
    the cross-boundary proof that `glm::vec4` and a 4-tuple are the same ABI.
- `scripts/brains/hero.noiser` (header comment, lines 12-18 and 22-24; call sites 76-81) —
  the workaround in production.
- `noiser-compiler/src/bytecode/expr_compiler.rs:12008-12034` — where
  `expected Unknown` is formatted and returned.
- `noiser-compiler/src/stdlib.rs:158-160` (capitalised float aliases) vs
  `:637-639` (lowercase int aliases) — the registration inconsistency.
- `noiser-book/src/core-language/type-system.md:18-19` — documents lowercase
  `vec2`/`vec3`/`vec4`, which do not exist as types.
- `noiser-book/src/core-library/generated/linalg.md:27` — advertises
  `pub type vec4 = GVec<4>`, stale relative to
  `noiser-corelib/core/linalg.noiser` (`pub struct Vec4`); needs regenerating.
- **Upstream, must be reconciled:**
  `noiser-bugs/2026-07-12-host-fn-vec-return-inline-slot-count-ice.md`
  (status "NOT REPRODUCIBLE") and
  `noiser-compiler/src/bytecode/host_fn_vec_return_ice_tests.rs` Matrix C, whose
  two param rows use only registered spellings.
- `noiser-bugs/KNOWN_ISSUES.md` — the OPEN `Maybe::Nothing` arg-position
  residual is the same failure family (unresolved bare `Type::Named` surviving
  registration).
