# `@fn` host function declared `-> vec2/vec3/vec4` ICEs the compiler at the call site

**Date**: 2026-07-22
**Version (noiser sha)**: `52174b2c9e517d9daa2ad6f11fb4e264fd5fec0d`
**Label**: `compiler`
**Status**: new

**Discovered in**: badlands — noiser drives per-entity game-AI brains
(`pub gen fn brain(entity: i32)`); every perception host call wants to return a
world position or a `(pos, stat, flag)` bundle.
**Backends observed**: VM (`NoiserBackend::kVM`). WASM/JIT not tested — the
failure is in the shared compiler front end, so both should be affected.

> **Read this first if you are about to close it as a duplicate.** Upstream
> `noiser-bugs/2026-07-12-host-fn-vec-return-inline-slot-count-ice.md` marks this
> family **"INVESTIGATED — NOT REPRODUCIBLE"**, backed by the 24-row matrix in
> `noiser-compiler/src/bytecode/host_fn_vec_return_ice_tests.rs`. That
> falsification is correct *for what it tested* and **misses the real trigger on
> two independent axes**:
>
> 1. **Spelling.** Every row of that matrix uses capitalised `Vec2`/`Vec3`/`Vec4`
>    — the names `NOISE_STDLIB` actually registers (`stdlib.rs:158-160`). The
>    book documents the float vector types as **lowercase** `vec2`, `vec3`,
>    `vec4` (`noiser-book/src/core-language/type-system.md:18`), and lowercase is
>    what a user writes. Lowercase is *not* registered as a type alias — only as
>    a constructor **function** (`stdlib.rs`, `fn vec4(x,y,z,w) -> (f32,f32,f32,f32)`).
> 2. **Position.** The matrix's frame is a **top-level** `let c = @fn.get(); 0.0`.
>    Even with the lowercase spelling that exact frame compiles clean (row `M01`
>    below). The ICE needs the call to sit **inside a function or generator body**
>    — which is where every real brain script puts it.
>
> Swapping `Vec4` → `vec4` in Matrix A3 and moving it into a `fn` body flips the
> row from Clean to ICE. See "Suggested new rows for the existing matrix".

## Problem

A `@fn` host function whose return type is written with the **documented
lowercase** vector spelling (`vec2`, `vec3`, `vec4`) compiles fine *as a
declaration*, then **panics the compiler (ICE) the moment the function is
called** from inside a `fn` or `gen fn` body. Two different ICE sites fire
depending on which layout query is reached first.

Root cause is not vector-specific: **an unresolved `Type::Named` in a `@fn`
signature is silently degraded to `TypeDescriptor::Unknown` at declaration time
instead of being rejected**, and `Unknown` has no layout. An outright typo
(`@fn.feed: fn(e: i32) -> Wobble;`) produces the *identical* ICE rather than
"unknown type `Wobble`" (row `M28`). `vecN` is simply the case a user hits by
following the documentation.

The companion bug — the same degradation in **argument** position, where it
surfaces as the nonsense diagnostic ``expected Unknown, got HomogTuple {…}``
instead of an ICE — is filed separately as
`docs/noiser-bugs-upstream/2026-07-22-host-fn-vecn-argument-rejected-at-callsite.md`.
**They share one root cause and should be fixed together.**

## Minimum Reproduction

Smallest failing snippet (generator context — badlands' real shape):

```noiser
@fn.feed4: fn(e: i32) -> vec4;
@fn.report: fn(x: f32) -> void;
pub gen fn brain(e: i32) -> i32 { loop { let v = @fn.feed4(e); @fn.report(v.x); yield 0; } }
0.0
```

Smallest failing snippet (plain-fn context — hits the *other* ICE site):

```noiser
@fn.fetch: fn(e: i32) -> vec4;
pub fn main() -> f32 { let v = @fn.fetch(1); v.x }
```

**Two-character diff that makes both compile and run correctly** — `vec4` → `Vec4`:

```noiser
@fn.feed4: fn(e: i32) -> Vec4;   // capital V: registered stdlib alias -> clean
```

**Two-line diff that also fixes it** — declare the missing alias yourself:

```noiser
type vec4 = (f32; 4);            // supply what NOISE_STDLIB omits -> clean
@fn.feed4: fn(e: i32) -> vec4;
```

## Expected

A correct implementation should:

1. **Resolve `vec2`/`vec3`/`vec4` as type names.** The book lists them as the
   float vector types alongside `ivec2`/`ivec3`/`ivec4`
   (`core-language/type-system.md:18-19`). `ivecN` *is* registered lowercase
   (`stdlib.rs:637-639`), `vecN` is not — the stdlib table is
   internally inconsistent. Either register `type vec2/vec3/vec4 = (f32; N)`
   alongside the existing capitalised aliases, or drop lowercase from the docs
   and reject it with a real diagnostic.
2. **Never ICE on an unresolved name in a `@fn` signature.** `@fn` declaration
   registration must alias-resolve / `ensure_type_instantiated` the return type
   and emit a normal compile error at the **declaration** span
   (`unknown type 'vec4' in host function signature`) rather than storing
   `TypeDescriptor::Unknown` and detonating at a later layout query in a
   different file. This is the "candidate (A) — instantiate/resolve at the
   source" fix the earlier upstream doc already identified as the better-targeted
   shape.
3. **Give `vecN` the same ABI as an N-tuple, because it already has one.**
   `vecN` should lower exactly like `(f32; N)` / `(f32, …, f32)`: N consecutive
   f32 VM stack slots, no header, no indirection. Nothing in the runtime needs
   to change — this is verified below.

### How `vecN` maps across the C ABI

Host-side (`noiser-vm/src/noiser.hpp`) **already treats `glm::vecN` and
`std::tuple` as the same thing**, so a fix costs nothing at the boundary:

| C++ type | `TypeEnumFor<T>()` | `TupleSizeFor<T>()` | `VmSlotCountFor<T>()` |
|---|---|---|---|
| `glm::vec2` | `TypeEnum::kTuple` | 2 | 2 |
| `glm::vec3` | `TypeEnum::kTuple` | 3 | 3 |
| `glm::vec4` | `TypeEnum::kTuple` | 4 | 4 |
| `std::tuple<float,float,float,float>` | `TypeEnum::kTuple` | 4 | 4 |

(`noiser.hpp:1576-1641`; `BindCallable*` computes `return_slots` as
`std::is_same_v<R, glm::vec4> ? 4 : …`, `noiser.hpp:2280-2290`, `2377-2390`.)

Because both collapse to **kTuple/size N**, `BindCallableByName` accepts a
`std::function<glm::vec4(int32_t)>` against a script-side
`fn(e: i32) -> (f32,f32,f32,f32)` declaration with no cast, no shim, no
reinterpretation — **this is exactly the workaround badlands ships**, and it is
pinned by `game/tests/noiser_smoke_tests.cpp` ("tuple-returning host call (bound
as `glm::vec4`) feeds a generator"). So the only thing missing is the compiler
front end resolving the name; the layout, the thunk, and the slot arithmetic are
all already correct for it.

Proof that the ABI is correct today — `Vec4` (capital) round-trips a real
`glm::vec4(1,2,3,1)` from the host with every component intact (harness row
`V-R1-twin`):

```
COMPILE OK
    report(1)
    report(2)
    report(3)
    report(1)
DONE (4 host calls over 1 ticks)
```

## Actual

Every ICE below is verbatim from the harness. Both sites are `ice!` macro
panics; the C++ FFI catches the panic and surfaces it as
`COMPILE FAIL: Noiser module compilation panicked`.

### Variant 1 — plain `fn` body, result let-bound (`type_layout.rs:1172`)

```noiser
@fn.fetch: fn(e: i32) -> vec4;
pub fn main() -> f32 { let v = @fn.fetch(1); v.x }
```

```
thread '<unnamed>' panicked at noiser-compiler/src/bytecode/type_layout.rs:1172:17:
ICE: inline_slot_count: Named type 'vec4' not instantiated - ensure_type_instantiated was not called. This is a compiler bug - please report it.
COMPILE FAIL: Noiser module compilation panicked
```

`vec2` and `vec3` produce the identical message with the name substituted:

```
ICE: inline_slot_count: Named type 'vec2' not instantiated - ensure_type_instantiated was not called. …
ICE: inline_slot_count: Named type 'vec3' not instantiated - ensure_type_instantiated was not called. …
```

**Line-number note for whoever greps:** the earlier upstream doc cites
`type_layout.rs:1213` for this arm. At `52174b2` it is **`type_layout.rs:1172`**
— same arm, drifted line.

### Variant 2 — `gen fn` body, or any context where the result is discarded (`abi_layout.rs:282`)

```noiser
@fn.feed4: fn(e: i32) -> vec4;
@fn.report: fn(x: f32) -> void;
pub gen fn brain(e: i32) -> i32 { loop { let v = @fn.feed4(e); @fn.report(v.x); yield 0; } }
0.0
```

```
thread '<unnamed>' panicked at noiser-compiler/src/bytecode/abi_layout.rs:282:13:
ICE: type_descriptor_to_abi_layout: Unknown has no physical layout. This is a compiler bug - please report it.
COMPILE FAIL: Noiser module compilation panicked
```

Note the message **names no type at all** — the declared name is gone by this
point, which is the direct evidence that the `@fn` return type was already
degraded to `TypeDescriptor::Unknown` during declaration registration.
`vec2`/`vec3` give the byte-identical message.

### Variant 3 — with `import { Vec4 } from core::linalg;` (**claim not reproduced**)

Older badlands notes claimed that adding the corelib import changes the
diagnostic to
`ICE [post-monomorphize/named-type-registered]: Type::Named("vec4") is not registered in struct_types, enum_layouts, or type_aliases`.
**I could not reproduce that.** Tested with `--module-path noiser-corelib`, in
both generator and plain-fn contexts (`M03`, `M31`), the import changes
**nothing**:

```
[info] FileModuleResolver: Loaded module 'core::linalg' from '…/noiser-corelib/core/linalg.noiser'
thread '<unnamed>' panicked at noiser-compiler/src/bytecode/abi_layout.rs:282:13:
ICE: type_descriptor_to_abi_layout: Unknown has no physical layout. …
```

i.e. generator context still gives Variant 2 and plain-fn context still gives
Variant 1. The quoted message **does exist** in the tree —
`noiser-compiler/src/bytecode/verifier.rs:664-677`, the
`post-monomorphize/named-type-registered` arm of `verify_type`, reached from
`verify_monomorphized_ast` (`bytecode/mod.rs:1012`, `:1223`) — but the two
layout panics fire *before* the verifier runs, so it is preempted. Treating that
third message as a separate variant would be wrong; it is the same unresolved
`Type::Named("vec4")` seen by a third consumer that never gets a turn.

Importing `vec4` **lowercase** from the corelib does not help either
(`import { vec4 } from core::linalg;`, row `M27` — still Variant 2):
`noiser-corelib/core/linalg.noiser` declares `pub struct Vec2/Vec3/Vec4`
(nominal structs), not a lowercase alias. The generated doc page
`core-library/generated/linalg.md:27` still advertises `pub type vec4 = GVec<4>`,
which no longer exists in the source — **that doc page is stale and should be
regenerated.**

## Trace

Root cause chain, as far as it can be read off the tree:

1. `NOISE_STDLIB` (`noiser-compiler/src/stdlib.rs:158-160`) registers
   **only** `type Vec2 = (f32; 2)`, `type Vec3 = (f32; 3)`,
   `type Vec4 = (f32; 4)` — capitalised. Lines `637-639` register
   `type ivec2/ivec3/ivec4 = (i32; N)` — **lowercase**. There is no lowercase
   `vecN` alias and no capitalised `IVecN` alias. Both spellings are documented
   together at `core-language/type-system.md:18-19`; exactly one of each pair
   works.
2. Lowercase `vec2/vec3/vec4` **do** exist in the stdlib, as constructor
   **functions** (`fn vec4(x: f32, y: f32, z: f32, w: f32) -> (f32,f32,f32,f32)`).
   This is why `let v = vec4(1.0,2.0,3.0,4.0); v.x + v.w` compiles and evaluates
   to `5` (row `M35`) while `-> vec4` in a signature explodes — the name is a
   value-namespace binding, never a type-namespace one.
3. `@fn` declaration registration converts the AST return type to a
   `TypeDescriptor` (`ast_type_to_descriptor` / `compute_call_descriptor`).
   An unregistered `Type::Named` is **silently mapped to
   `TypeDescriptor::Unknown`** rather than erroring — declaration-only scripts
   therefore compile clean (rows `M04`, `M28-decl`).
4. At the first call site, one of two layout queries dereferences that:
   - `inline_slot_count` still holding the AST `Type::Named("vec4")` →
     `type_layout.rs:1172` `ice!("inline_slot_count: Named type '{}' not instantiated …")`;
   - `type_descriptor_to_abi_layout` holding the degraded `Unknown` →
     `abi_layout.rs:282` `ice!("type_descriptor_to_abi_layout: Unknown has no physical layout")`.

Which arm fires is a function of which query runs first, not of anything
user-meaningful (observed: any generator body → `abi_layout`; plain-fn with the
result let-bound → `type_layout`; plain-fn with the result discarded →
`abi_layout`). Both are the same defect.

## Use cases / why it matters

badlands runs one noiser generator per entity as its AI brain
(`scripts/brains/hero.noiser`, driven from `game/src/brain.cpp`). The host
surface is almost entirely **perception calls that hand the script a position**,
because that is the shape world data has:

```noiser
@fn.perceive_self:     fn(e: i32) -> ???;  // (x, z, hp, cooldown)
@fn.perceive_target:   fn(e: i32) -> ???;  // (x, z, hp, has_target)
@fn.perceive_building: fn(e: i32, kind: i32) -> ???;  // (door_x, door_z, exists, _)
@fn.perceive_home:     fn(e: i32) -> ???;  // (door_x, door_z, exists, _)
@fn.perceive_needs:    fn(e: i32) -> ???;  // (fatigue, boredom, time_of_day, is_night)
```

Every one of those is a `glm::vec4` on the C++ side and *wants* to be `vec4` on
the script side. Because it cannot be, the cost lands in three places:

- **Readability.** Every perception site destructures four unnamed floats. The
  shipped `hero.noiser:76-81` opens its decision function with a six-line wall:
  ```noiser
  let (sx, sz, hp, cd)     = @fn.perceive_self(e);
  let (tx, tz, thp, has)   = @fn.perceive_target(e);
  let (ax, az, aex, _)     = @fn.perceive_building(e, APOTHECARY);
  let (vx, vz, vex, _)     = @fn.perceive_building(e, TAVERN);
  let (hx, hz, hex, _)     = @fn.perceive_home(e);
  let (fat, bor, tod, night) = @fn.perceive_needs(e);
  ```
  Twenty-four positional bindings whose meaning lives only in the trailing
  comment on the declaration. Reordering two fields host-side is a silent,
  type-checked-as-fine miscompile of the whole brain.
- **`core::linalg` is unreachable on perceived data.** A brain naturally wants
  `distance(self_pos, target_pos)`, `normalize(target - self)`, `dot(…)`. With
  flat tuples the script open-codes each of those against loose `sx`/`sz`
  floats, and the corelib vector ops sit unused. Repacking into
  `core::linalg::Vec2` is not a way out either — it is a *nominal struct* here,
  distinct from the tuple-optimised `(f32, f32)`, which is one of the two
  workarounds already documented in `hero.noiser`'s header.
- **The whole data model went flat.** `hero.noiser`'s `WorldView` is
  "ALL-FLAT f32 (no `core::linalg::Vec2`, no nested structs)" and the brain's
  decision crosses function boundaries as a bare `(Goal, Command)` tuple rather
  than a `Decision` struct. That choice is partly forced by the sibling
  struct-of-enums corruption bug, but this one is what removes the vector escape
  hatch.

The fix is cheap and the payoff is immediate: with `vecN` resolving, the
declarations above become `-> vec4`, the six-line destructure becomes
`let s = @fn.perceive_self(e);` with `s.x`/`s.y`/`s.z`/`s.w`, and `core::linalg`
becomes usable on perceived data — **with zero change to the C++ bindings**,
which already return `glm::vec4`.

## Minimum test cases

All rows run through the badlands snippet harness against `52174b2`; corelib
rows via `--module-path noiser-corelib`. `report`/`report2` print their
arguments; `feed4` is bound host-side to `[](int32_t){ return glm::vec4(1,2,3,1); }`.

### Return type — spelling axis (the falsification-breaker)

| # | Declaration | Context | Observed |
|---|---|---|---|
| M01 | `-> vec4` | top level, `let c = @fn.get(); 0.0` (**upstream matrix A3 frame**) | **Clean** — why the matrix missed it |
| M02 | `-> vec4` | top level, `let c = @fn.get(); c.x` | **ICE** `type_layout.rs:1172` `'vec4'` |
| M03 | `-> vec4` | top level, result discarded `@fn.get();` | **ICE** `abi_layout.rs:282` |
| M04 | `-> vec4` | top level, `let x = @fn.get().0;` | **Clean** |
| M05 | `-> Vec4` | top level, `let c = @fn.get(); 0.0` | Clean (upstream A3 verbatim) |
| M06 | `-> Vec4` | `gen fn` body, all 4 components read | Clean, **correct**: `1, 2, 3, 1` |
| M07 | `-> Vec4` | plain `fn` body, `v.x+v.y+v.z+v.w` | Clean, **correct**: `7` |

### Return type — dimension × context

| # | Declaration | Context | Observed |
|---|---|---|---|
| M08 | `-> vec4` | `pub fn` body, let-bound + field read | **ICE** `type_layout.rs:1172` `'vec4'` |
| M09 | `-> vec4` | `pub fn` body, let-bound, **unused** | **ICE** `type_layout.rs:1172` `'vec4'` |
| M10 | `-> vec4` | `pub fn` body, result discarded | **ICE** `abi_layout.rs:282` |
| M11 | `-> vec4` | `pub gen fn` body, let-bound + field read | **ICE** `abi_layout.rs:282` |
| M12 | `-> vec4` | `pub gen fn` body, let-bound, **unused** | **ICE** `abi_layout.rs:282` |
| M13 | `-> vec4` | `pub gen fn` body, result discarded | **ICE** `abi_layout.rs:282` |
| M14 | `-> vec4` | `pub gen fn` body, `yield v.x` | **ICE** `abi_layout.rs:282` |
| M15 | `-> vec3` | `pub fn` body, let-bound + field read | **ICE** `type_layout.rs:1172` `'vec3'` |
| M16 | `-> vec3` | `pub gen fn` body | **ICE** `abi_layout.rs:282` |
| M17 | `-> vec2` | `pub fn` body, let-bound + field read | **ICE** `type_layout.rs:1172` `'vec2'` |
| M18 | `-> vec2` | `pub gen fn` body | **ICE** `abi_layout.rs:282` |
| M19 | `-> vec4` | **declared, never called** | **Clean** — declaration alone is fine |

### Return type — integer vectors (mirror-image spelling)

| # | Declaration | Context | Observed |
|---|---|---|---|
| M20 | `-> ivec4` | `pub fn` body, let-bound | **Clean** (lowercase `ivecN` *is* registered) |
| M21 | `-> ivec4` | `pub gen fn` body, `.x` read | **Clean**, runs |
| M22 | `-> ivec2` | `pub gen fn` body, `.x` read | **Clean**, runs |

> Caveat on M21/M22: the harness has no i32-tuple-returning binding, so these
> ran against the `glm::vec4`-returning `feed4`. They print raw f32 bit patterns
> (`1065353216` = `0x3F800000` = `1.0f`) — i.e. `BindCallableByName` **silently
> accepts a slot-count/element-type-mismatched host callable** and reinterprets.
> That is a separate host-binding-validation gap, noted here only so the numbers
> are not misread as an `ivecN` codegen bug. `ivecN` itself compiles clean.

### Return type — working shapes (controls)

| # | Declaration | Observed |
|---|---|---|
| M23 | `-> (f32, f32, f32, f32)` | Clean, correct — **badlands' shipped workaround** |
| M24 | `-> (f32; 4)` | Clean, correct |
| M25 | `-> (f32, f32)` | Clean, correct |
| M26 | `-> Vec4` + `import { Vec4 } from core::linalg;` | Clean, correct (nominal struct path) |

### Return type — import / alias axis

| # | Setup | Observed |
|---|---|---|
| M27 | `import { vec4 } from core::linalg;` + `-> vec4`, gen | **ICE** `abi_layout.rs:282` (no lowercase alias in corelib) |
| M31 | `import { Vec4 } from core::linalg;` + `-> vec4`, gen | **ICE** `abi_layout.rs:282` — import is irrelevant |
| M32 | `import { Vec4 } from core::linalg;` + `-> vec4`, plain fn | **ICE** `type_layout.rs:1172` — import is irrelevant |
| M33 | `type vec4 = (f32; 4);` + `-> vec4`, gen | **Clean, correct** — user-supplied alias fixes it |

### Root-cause probes (not vector-specific)

| # | Snippet | Observed |
|---|---|---|
| M28 | `@fn.feed: fn(e: i32) -> Wobble;` called in a gen | **ICE** `abi_layout.rs:282` — identical to `vec4`; a typo'd type name should be a diagnostic, not a panic |
| M29 | `struct P { p: vec4 }` (no `@fn` at all) | **ICE** `type_layout.rs:1172` `'vec4'` — scope is wider than `@fn` |
| M30 | `let v: vec4 = vec4(1.0,2.0,3.0,4.0);` in a gen | **ICE** `abi_layout.rs:282` — annotation alone triggers it |
| M34 | `fn make() -> vec4 { vec4(1.0,2.0,3.0,4.0) }` | `COMPILE FAIL: function 'make' declared return type Named("vec4") but body returns Tuple([F32, F32, F32, F32])` — the constructor's own return type does not match the documented name of its type |
| M35 | `let v = vec4(1.0,2.0,3.0,4.0); v.x + v.w` (no annotation) | Clean, `5` — lowercase `vec4` works as a *function*, never as a *type* |
| M36 | `gen fn sub(v: vec4) -> f32 { … }` (generator param) | **ICE** `abi_layout.rs:282` |

**Scope note for triage:** M29/M30/M34/M36 show this is *not* an `@fn`-only bug
— lowercase `vecN` is unusable as a type annotation anywhere (struct fields,
`let` annotations, plain-fn returns, generator params). `@fn` is just where it
turns into an ICE instead of a compile error.

### Suggested new rows for the existing matrix

`noiser-compiler/src/bytecode/host_fn_vec_return_ice_tests.rs` should gain the
lowercase twins of A1-A3 **inside a function body** (its current frame is
top-level, which is row `M01` — Clean either way):

```rust
// Lowercase spelling — the documented one (type-system.md:18) — inside a fn body.
#[test] fn a1_lower_vec2_return_in_fn_body() { assert_clean("@fn.get: fn() -> vec2;\nfn f() -> f32 { let c = @fn.get(); c.x }\nf()", "A1' vec2"); }
#[test] fn a2_lower_vec3_return_in_fn_body() { assert_clean("@fn.get: fn() -> vec3;\nfn f() -> f32 { let c = @fn.get(); c.x }\nf()", "A2' vec3"); }
#[test] fn a3_lower_vec4_return_in_fn_body() { assert_clean("@fn.get: fn() -> vec4;\nfn f() -> f32 { let c = @fn.get(); c.x }\nf()", "A3' vec4"); }
// Not vector-specific: an unknown name must be a diagnostic, not an ICE.
#[test] fn a17_unknown_named_return_is_error_not_ice() { /* expect Err, not panic */ }
```

All four currently panic.

## Example verification programs

Run with the badlands snippet harness (or any host that binds the same probes).
Bindings assumed: `report(f32) -> void` prints its argument;
`feed4(i32) -> glm::vec4` returns `glm::vec4(1, 2, 3, 1)`.

### V-R1 — headline: `vec4` return in a generator, all components

```noiser
@fn.feed4: fn(e: i32) -> vec4;
@fn.report: fn(x: f32) -> void;
pub gen fn brain(e: i32) -> i32 {
    loop { let v = @fn.feed4(e); @fn.report(v.x); @fn.report(v.y); @fn.report(v.z); @fn.report(v.w); yield 0; }
}
0.0
```

Expected after a fix (1 resume) — **this exact output is already produced today
by the `-> Vec4` twin**, so it is a measured expectation, not a guess:

```
COMPILE OK
    report(1)
    report(2)
    report(3)
    report(1)
DONE (4 host calls over 1 ticks)
```

Today: `ICE: type_descriptor_to_abi_layout: Unknown has no physical layout` (`abi_layout.rs:282`).

### V-R2 — `vec4` return in a plain `fn` (covers the other ICE site)

```noiser
@fn.feed4: fn(e: i32) -> vec4;
@fn.report: fn(x: f32) -> void;
pub fn main() -> f32 { let v = @fn.feed4(1); @fn.report(v.x + v.y + v.z + v.w); 0.0 }
main()
```

Expected after a fix (verified against the `-> Vec4` twin):

```
COMPILE OK
    report(7)
DONE (1 host calls over 1 ticks)
```

Today: `ICE: inline_slot_count: Named type 'vec4' not instantiated` (`type_layout.rs:1172`).

### V-R3 — regression guard: unknown type names must diagnose, not panic

```noiser
@fn.feed: fn(e: i32) -> Wobble;
@fn.report: fn(x: f32) -> void;
pub gen fn brain(e: i32) -> i32 { loop { let v = @fn.feed(e); @fn.report(1.0); yield 0; } }
0.0
```

Expected after a fix: a normal compile error naming the **declaration** span,
e.g. `COMPILE FAIL: unknown type 'Wobble' in host function signature @fn.feed  [main:1:25]`.
Today: `ICE: type_descriptor_to_abi_layout: Unknown has no physical layout`
(`abi_layout.rs:282`) — same panic as `vec4`, which is the evidence that fixing
the name table alone is not enough; the `Unknown` degradation must go too.

## Workarounds

Two work. badlands ships the first because it predates the diagnosis of the
second.

### 1. Flat tuple returns (shipped)

Declare `(f32, f32, f32, f32)` script-side and keep returning `glm::vec4`
host-side — both are `kTuple`/4, so nothing changes at the boundary.

**Before (what we wanted, ICEs):**

```noiser
@fn.perceive_self:   fn(e: i32) -> vec4;
@fn.perceive_target: fn(e: i32) -> vec4;

let s = @fn.perceive_self(e);
let t = @fn.perceive_target(e);
let dx = t.x - s.x;
let dz = t.y - s.y;
```

**After (`scripts/brains/hero.noiser`, shipped):**

```noiser
// Perception returns are flat f32 tuples: host fns declared `-> vecN` ICE the
// compiler (docs/noiser-feedback.md #1). The C++ side binds glm::vec4.
@fn.perceive_self:   fn(e: i32) -> (f32, f32, f32, f32);  // (x, z, hp, cooldown)
@fn.perceive_target: fn(e: i32) -> (f32, f32, f32, f32);  // (x, z, hp, has_target)

let (sx, sz, hp, cd)   = @fn.perceive_self(e);
let (tx, tz, thp, has) = @fn.perceive_target(e);
let dx = tx - sx;
let dz = tz - sz;
```

C++ side is unchanged in both cases (`game/src/brain.cpp`):

```cpp
glm::vec4 perceive_self(BadlandsGame& game, int32_t slot) { … }
bind("perceive_self", [g](int32_t e) { return perceive_self(*g, e); });
```

Cost: 24 positional bindings across the brain's six perception calls, field
meaning carried only in comments, and `core::linalg` unusable on the results.

### 2. Declare the missing aliases yourself (better; not yet adopted)

One line per dimension at the top of the script restores the documented
spelling everywhere, including in `@fn` **arguments** — it fixes the companion
bug too:

```noiser
type vec4 = (f32; 4);
type vec2 = (f32; 2);

@fn.feed4:   fn(e: i32) -> vec4;
@fn.report2: fn(v: vec2) -> void;
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

Equivalently, write `Vec4`/`Vec2` capitalised — but that is the spelling the
book does not document for float vectors, so it reads as a typo to anyone
following the docs.

Neither workaround unblocks `core::linalg`'s nominal `Vec2`/`Vec3`/`Vec4`
struct methods on perceived data — those remain distinct types from the
tuple-optimised form.

## Related

- `docs/noiser-feedback.md` — badlands' running noiser integration log; item #1
  is this bug, recorded as the reason perception returns are flat tuples.
- `docs/noiser-bugs-upstream/2026-07-22-host-fn-vecn-argument-rejected-at-callsite.md`
  — **companion bug, same root cause**: `vecN` in `@fn` *argument* position.
  Fixing the name/`Unknown` degradation fixes both.
- `game/tests/noiser_smoke_tests.cpp`:
  - `TEST_CASE("KNOWN BUG: host functions returning vecN ICE the compiler")` —
    pins Variant 1 verbatim; when it starts failing, upstream fixed this and
    badlands should switch perception back to `vec4`.
  - `TEST_CASE("tuple-returning host call (bound as glm::vec4) feeds a generator")` —
    pins the workaround, and is the cross-boundary proof that `glm::vec4` and a
    4-tuple are the same ABI.
- `scripts/brains/hero.noiser` (header comment, lines 12-18 and 22-24) — the workaround in
  production.
- **Upstream, must be reconciled:**
  `noiser-bugs/2026-07-12-host-fn-vec-return-inline-slot-count-ice.md` (status
  "NOT REPRODUCIBLE") and its 24-row matrix
  `noiser-compiler/src/bytecode/host_fn_vec_return_ice_tests.rs`. Both test
  capitalised `Vec4` at top level; the trigger is lowercase `vec4` inside a
  function/generator body. The doc's own "candidate (A) — instantiate/resolve at
  the source" is the right fix and should be reopened.
- `noiser-compiler/src/stdlib.rs:158-160` (capitalised float aliases) vs
  `:637-639` (lowercase int aliases) — the inconsistency.
- `noiser-book/src/core-language/type-system.md:18-19` — documents lowercase
  `vec2`/`vec3`/`vec4`, which do not exist as types.
- `noiser-book/src/core-library/generated/linalg.md:27` — advertises
  `pub type vec4 = GVec<4>`, stale relative to
  `noiser-corelib/core/linalg.noiser` (which declares `pub struct Vec4`);
  needs regenerating.
- `noiser-bugs/KNOWN_ISSUES.md` — the OPEN `Maybe::Nothing` arg-position
  residual is the same failure family (unresolved bare `Type::Named` reaching a
  layout query).
