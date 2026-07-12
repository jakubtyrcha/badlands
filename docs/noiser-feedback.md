# Noiser feedback from the badlands integration

Collected while embedding noiser (submodule @ `960f1cc6`, 2026-07-12) as the
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

### 2. `vecN` as a host-function *argument* is rejected with a confusing error

```noiser
@fn.f: fn(v: vec4) -> f32;
```

fails with `@fn.f argument `v` (position 0): expected Unknown, got
HomogTuple { element_type: F32, size: 4 }`. Not an ICE, but "expected
Unknown" suggests the host-fn parameter type never resolved; if vec args are
unsupported by design, the diagnostic should say so.

### 3. Minor

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
- Tuple destructuring `let (a, b, c, d) = @fn.f(e);` works, including with
  host-call sources.

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
