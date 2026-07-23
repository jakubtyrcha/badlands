# Noiser feedback from the badlands integration

**Status (2026-07-24):** the shipping hero brain is now Nim→WASM (see
`docs/superpowers/specs/2026-07-23-wasm-brain-contract-design.md`). The noiser brain path
described below is dormant but still compiled, and its tests/bug-pins remain valid. This
file stays as the upstream-facing noiser integration log.

Collected while embedding noiser (submodule @ `52174b2c9e517d9daa2ad6f11fb4e264fd5fec0d`,
last re-verified 2026-07-22) as the
game-AI scripting layer: C++ host (`sampo::noiser` wrapper) driving per-entity
`gen fn` coroutines with host-call perception/intents (the Weave Plan-1b
app-space idiom). Environment: macOS arm64, Homebrew clang 22 / Apple clang,
Rust 1.96.0, VM backend only.

## Observed bugs

### 1. Host functions declared `-> vecN` ICE the compiler

**Repro** (any of vec2/vec3/vec4):

```noiser
@fn.f: fn(e: i32) -> vec4;
pub fn main() -> f32 { let v = @fn.f(1); v.x }
```

Panics in `noiser-compiler/src/bytecode/type_layout.rs:1213`:

```
ICE: inline_slot_count: Named type 'vec4' not instantiated - ensure_type_instantiated
was not called. This is a compiler bug - please report it.
```

With `import { Vec4 } from core::linalg;` also present, the message becomes
`ICE [post-monomorphize/named-type-registered]: Type::Named("vec4") is not
registered in struct_types, enum_layouts, or type_aliases`.

Notes:
- `vec4` locals and returns of ordinary functions are fine; only the `@fn.`
  return position triggers it.
- Tuple returns work: `@fn.f: fn(e: i32) -> (f32, f32, f32, f32);` compiles
  and runs, and the C++ wrapper's `BindCallableByName` still accepts a
  `glm::vec4`-returning callable against it (both map to kTuple/4) — that is
  the workaround badlands ships (`scripts/brains/warrior.noiser`).
- Pinned by `game/tests/noiser_smoke_tests.cpp`
  ("KNOWN BUG: host functions returning vecN ICE the compiler") — when the
  pin starts failing, the bug is fixed upstream and the brain script can go
  back to `-> vec4`.

### 2. `vecN` as a host-function *argument* is rejected only at the call site

**Declaring** such a host function compiles fine — this whole program is
`COMPILE OK` and runs:

```noiser
@fn.sink: fn(v: vec4) -> f32;                 // declared, never called
@fn.report: fn(x: f32) -> void;
pub gen fn brain(e: i32) -> i32 { loop { @fn.report(1.0); yield 0; } }
0.0
```

The failure is at the **call site**. Passing a 4-tuple:

```noiser
let v = (1.0, 2.0, 3.0, 4.0); @fn.report(@fn.sink(v));
```

```
@fn.sink argument `v` (position 0): expected Unknown, got HomogTuple { element_type: F32, size: 4 }
```

"expected Unknown" says the host-fn parameter type never resolved — the
declaration is accepted with an unresolved type and nothing can then satisfy it.
Annotating the local as `vec4` instead turns the same call into an ICE:

```noiser
let v: vec4 = (1.0, 2.0, 3.0, 4.0); @fn.report(@fn.sink(v));
```

```
ICE: type_descriptor_to_abi_layout: Unknown has no physical layout.
  noiser-compiler/src/bytecode/abi_layout.rs:282
```

So this is the argument-position twin of bug 1: `vecN` in an `@fn.` signature is
unresolved in both directions. Either reject it at the declaration with a clear
message, or resolve it like any other `vecN`. Workaround is the same — flat
`(f32, f32, f32, f32)` parameters, which the C++ wrapper still binds against a
`glm::vec4`-taking callable.

### 3. Generics / generators cluster — generic calls inside a `gen fn` body fail

Four separate defects filed 2026-07-22 that share one trigger: **a generic or
`Maybe`-producing call written lexically inside a `gen fn` body misbehaves,
while the identical call inside a plain `fn` — including a plain `fn` invoked
*from* a generator — works.** Together they block composing brains out of
reusable sub-behaviour generators, the natural encoding for sequential AI
(patrol, commute, engage-then-return).

- `docs/noiser-bugs-upstream/2026-07-22-nested-generator-next-returns-tuple-not-maybe.md`
  — `p.next()` on a generator instance created inside another generator is typed
  `Tuple([I32, F32])`, not the documented `Maybe<T>`. `while let .Just(v) =
  p.next()` fails to compile; `match p.next() { .Just(v) => …, .Nothing => … }`
  compiles and **silently runs neither arm**.
- `docs/noiser-bugs-upstream/2026-07-22-generic-maybe-instantiation-ices-inside-generator-body.md`
  — instantiating a generic `Maybe` in a generator body ICEs the compiler.
- `docs/noiser-bugs-upstream/2026-07-22-generator-passed-to-function-loses-state.md`
  — handing a generator to a plain `fn` and resuming it there works for exactly
  one tick, then dies with `Invalid generator state: RestoreState: state buffer
  index 0 >= buffer len 0`. **New 2026-07-22: this one is nondeterministic.**
  The identical source on the identical binary flips between that failure and a
  second, quieter wrong behaviour where every resume "succeeds" and fires **zero**
  host calls — 12 vs 13 out of 25 runs for the `where I: Iterator<f32>` spelling,
  8 vs 17 out of 25 for the unconstrained `fn drive<T>(g: T)` spelling. A
  coin-flip between two wrong behaviours smells like uninitialized or dangling
  sub-context state, not a deterministic codegen mistake. (Contrast: the
  struct-of-enums program under Positive findings is 25/25 identical, so the
  nondeterminism is specific to the generator-into-`fn` path, not the harness.)
- `docs/noiser-bugs-upstream/2026-07-22-generic-protocol-bounds-fail-to-parse.md`
  — inline generic bounds: parameterized protocols (`<I: Iterator<f32>>`) don't
  parse, and bare-protocol bounds (`<T: Foo>`) ignore user `impl` blocks. The
  `where`-clause spelling works in both cases and is the workaround.

The common shape: move the generic/`Maybe` work into a plain `fn` (bound with
`where`, taking the generator **by value**) and the compiler is happy — but the
generator's coroutine state then does not survive the second resume, so there is
currently no composition path that works for more than one tick.

### 4. Corelib: `vec_new` is a builtin, but importing it from `core::vec` fails

`vec_new` is a compiler builtin and is callable **without any import** — it just
needs its type argument:

```noiser
@fn.report: fn(x: f32) -> void;
pub gen fn brain(e: i32) -> i32 { loop { var v = vec_new::<f32>(); v.push(3.0); @fn.report(v[0]); yield 0; } }
0.0
```

Omitting the type argument gives a good error
(`vec_new() requires a type argument, e.g. vec_new::<f32>()`), which is what
makes the builtin discoverable. But the natural next step fails:

```noiser
import { vec_new } from core::vec;
```

```
symbol 'vec_new' not found in module 'core::vec'
  (available: any, map, last, min, first, swap, reverse, max, filter, max_f32,
   is_empty, max_i32, all, min_i32, min_f32, contains, fold, index_of)
```

(the `available:` list is emitted in hash order — it varies run to run.)

Meanwhile `core/vec.noiser` itself calls `vec_new::<U>()` at lines 118 and 127,
unimported — because it is a builtin, not a member of the module it appears to
belong to. Its own header comment (line 3) says as much. Nothing in the docs
tells a caller that `vec_new`/`push`/`pop`/`len` are builtins living outside
`core::vec` while `map`/`filter`/`fold` are module members; the error message is
the only signal, and it reads as "this symbol doesn't exist". Suggest either
re-exporting the builtins from `core::vec` or having the diagnostic say
"`vec_new` is a builtin — no import required".

### 5. Minor

- Expected script runtime errors (e.g. division by zero surfaced through
  `Resume`) are logged by the wrapper at `spdlog` **error** level before the
  host sees them — a host that handles the error gracefully cannot keep its
  stderr clean. Suggest `debug`/`trace`, or a host-installable sink.
- `noiser.cpp` compiles with 31 warnings under clang 22 (missing `panic`
  field initializers for `CompileError{...}` literals, unused
  `tuple_type`/`tuple_ptr`).

## Positive findings worth pinning upstream

- **Generator parameters via `warp_id` work** (`pub gen fn brain(entity: i32)`
  + `Prepare({.warp_id = {id, 0, 0}})` delivers the id) — previously untested
  as an executed path; badlands' entity identity rides on it
  (`noiser_smoke_tests.cpp` pins it). An upstream execution test would guard
  against regressions.
- Host calls (void and value-returning) fire correctly from inside generator
  frames across yields.
- Corelib `import { ... } from core::linalg;` works through plain
  `NoiserProgram::Compile` — the comment at `noiser.cpp:1966` claiming module
  imports fail via `NoOpModuleResolver` is stale (the Rust resolver falls back
  to the embedded corelib).
- **Local multi-file module imports work** via `sampo::noiser::FileModuleResolver`
  (verified 2026-07-22). A `helpers.noiser` dropped in a search path passed to
  the resolver resolves from `import { twice, dist2 } from helpers;` in main;
  its `pub fn`s are callable and produce correct values, and the host `@fn.`
  bindings in the main file are unaffected (`report`/`feed` still bind and fire).
  Without the search path the same program correctly fails with
  `module 'helpers' not found (imported from 'main')`. So brains *can* be split
  across files — worth documenting, since the corelib-fallback behaviour above
  makes it easy to assume only `core::*` is reachable.
- Tuple destructuring `let (a, b, c, d) = @fn.f(e);` works, including with
  host-call sources.
- **Returning enums across a function boundary works in both shapes** — tuple
  *and* struct — even when every field derives from host-call data:
  - `fn decide(...) -> (Goal, Command, i32)` (enums mixed with a scalar)
    round-trips correctly.
  - `fn decide(...) -> Decision` where `struct Decision { goal: Goal, command: Command }`
    **also round-trips correctly as of sha `52174b2`** (re-verified 2026-07-22
    against a `View` assembled from three host calls, with both `match`es taking
    the right arm and output identical to the tuple control — and stable, 25/25
    runs byte-identical, so it is not a flaky pass). This was the
    2026-07-13 corruption bug; it is **fixed** —
    `docs/noiser-bugs-upstream/2026-07-13-struct-of-enums-return-from-perception-corrupts.md`
    is retained with a Resolution section. The tuple form is therefore a plain
    working shape, no longer a workaround, and
    `scripts/brains/warrior.noiser` no longer needs to avoid struct returns.

## API improvement proposals (integration fluidity)

C++-host items, in rough priority order:

1. **`NOISER_NO_JIT` switch.** `noiser.cpp`/`noiser.hpp` reference all 21
   `noiser_jit_*` symbols unconditionally, so a VM-only host must link
   Binaryen or ship stub implementations (badlands:
   `game/src/noiser_jit_stubs.cpp`). A preprocessor guard would remove the
   footgun.
2. **Document the game-host pattern.** The app-space `GameCtx`/`Binding<T>`
   idiom (ctx struct whose impl methods lower to `@fn.` host calls; perception
   pulled via value-returning host calls; one `gen fn` coroutine per entity
   resumed per tick) is proven in `noiser-compiler/tests/weave_binding_vm.rs`
   but documented only in sampo plan docs. A noiser-book cookbook page would
   make it discoverable; badlands (`scripts/brains/warrior.noiser` +
   `game/src/brain.cpp`) is a working reference.
3. **Host-call profiling should be opt-in.** `HostThunkTrampoline` samples
   `high_resolution_clock` per call with the thread-local default ON; hosts
   must know to call `detail::SetHostCallProfiling(false)` (and it living in
   `detail` suggests it isn't meant to be the public switch).
4. **Compiler stack expectation is undocumented for embedders.**
   `noiser_compile*` runs on the caller thread and needs ~64 MiB stack (the
   repo's own `.cargo/config.toml` sets `RUST_MIN_STACK`); badlands compiles
   on a dedicated big-stack pthread (`game/src/brain.cpp`). Either document
   the requirement in the FFI header or spawn internally.
5. **C++23 requirement is undocumented** (`<expected>` in `noiser.hpp`), as is
   the minimal no-CMake TU set (`noiser.cpp` + `noiser_crash.cpp` compile
   standalone — worked well, worth documenting).
6. **Hand-maintained `noiser_vm_ffi.h` (50 fns) risks drift** — cbindgen is
   declared as a build-dependency but nothing generates the header.
7. **Wrapper hard-requires glm + spdlog** — pluggable logging and a
   glm-optional path would shrink the embed footprint.
8. **Generator state snapshot for hot reload.** Recompile + `Prepare` restarts
   brains from the top (fine for loop-shaped brains, lossy for staged
   behaviors). The sparse `SaveState` design looks close to enabling
   serialize/restore of a live generator.

Rust-host items (from the earlier evaluation, still open):

9. One-call generator setup for Rust hosts — the C++ wrapper hides the
   `parse_header_size` → `with_entry_point` → `parse_first_generator` →
   `set_generator_info` → `set_generator_param_count` → `freeze_host_thunks`
   stitch, but a Rust host still hand-assembles it; `CompileResult::into_program()`
   would close the gap.
10. Named generator entry points — `parse_first_generator` limits a file to
    one brain; a `gen fn` name table would allow `warrior`/`coward` variants
    per file.
11. Ship a LICENSE file; align crate versions (`noiser-vm` 0.8.9 vs
    `VERSION` const) and consider tags to pin against.
12. Remove `[profile]` sections from library manifests (every consumer build
    warns) — and `vec.pop()` still leaks heap slots inside generators
    (worked around in shipped scripts).
