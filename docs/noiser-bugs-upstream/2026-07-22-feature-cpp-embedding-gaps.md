# C++ embedding gaps: six changes that would remove badlands' host-side workarounds

**Date**: 2026-07-22
**Version (noiser sha)**: `52174b2c9e517d9daa2ad6f11fb4e264fd5fec0d`
**Kind**: feature request (C++ host / build integration)
**Status**: new

These are not language bugs — they are integration gaps in the `sampo::noiser`
C++ wrapper and its build. Each one currently costs badlands a concrete
workaround, cited below by file. They are ordered by how much code they would
delete from the host.

Context: badlands embeds noiser as its game-AI scripting layer. One
`pub gen fn brain(entity: i32)` coroutine runs per entity, resumed once per
30 Hz tick; perception and actions cross as `@fn.` host calls
(`game/src/brain.cpp`, `scripts/brains/hero.noiser`). Backend is
`NoiserBackend::kVM` exclusively — badlands never uses the JIT.

---

## 1. `NOISER_NO_JIT` build switch

### Current behaviour
`noiser.cpp` / `noiser.hpp` reference every `noiser_jit_*` symbol
unconditionally, with no preprocessor guard. The JIT implementation lives in the
`noiser-wasm` crate, which pulls in Binaryen. A VM-only host must therefore
either link Binaryen it will never call, or supply stubs.

badlands ships **`game/src/noiser_jit_stubs.cpp`** — 24 stubbed `noiser_jit_*`
symbols whose only purpose is to satisfy the linker. Its header comment:

> `noiser.cpp` references every `noiser_jit_*` symbol unconditionally (no
> `#ifdef` guard), but the JIT implementation lives in the noiser-wasm crate,
> which drags in Binaryen. Badlands only ever uses `NoiserBackend::kVM`, so
> these stubs satisfy the linker and are never called at runtime.

### Proposed behaviour
A `NOISER_NO_JIT` macro that compiles out every JIT declaration, definition and
call site. With it defined:
- `noiser.hpp` declares no `noiser_jit_*` symbol.
- `NoiserBackend::kJit` either disappears from the enum or is accepted and
  fails at runtime with a clear diagnostic (`"built with NOISER_NO_JIT"`).
- A translation unit compiling `noiser.cpp` + `noiser_crash.cpp` links against
  the VM staticlib alone.

### Verification
Build a TU that only calls `NoiserProgram::Compile` + `Prepare` + `Resume`,
linking **only** the VM staticlib, with no stub file present:

```cpp
#define NOISER_NO_JIT
#include <noiser.hpp>
int main() {
  auto p = sampo::noiser::NoiserProgram::Compile("pub fn main() -> f32 { 1.0 }");
  return p.has_value() ? 0 : 1;
}
```

Acceptance: links and returns 0 with no `noiser_jit_*` symbol referenced
(`nm -u` on the object file shows none). Today this fails at link time unless
the 24 stubs are supplied.

---

## 2. Compiler stack requirement is undocumented and unhandled

### Current behaviour
`noiser_compile*` recurses deeply and runs on the **caller's** thread. The
noiser repo's own `.cargo/config.toml` sets `RUST_MIN_STACK` to 64 MiB, but an
embedder gets no signal — the default 8 MiB thread stack produces a stack
overflow (a hard crash, not a `CompileError`).

badlands works around this in **`game/src/brain.cpp`** with
`compile_big_stack()`: a dedicated `pthread` created with
`pthread_attr_setstacksize(&attr, 64u << 20)` purely to host the compile call,
joined immediately after. Every compile path in the game pays this detour, and
the scratch harness used to file today's bug reports had to replicate it.

### Proposed behaviour
Either of:
1. **Spawn internally** — `NoiserProgram::Compile` runs the compiler on its own
   adequately-sized thread and joins. Callers stop caring. (Preferred: it makes
   the requirement unrepresentable rather than merely documented.)
2. **Document + assert** — state the requirement in the FFI header and in the
   C++ wrapper's doc comment, and detect an insufficient stack up front,
   returning a `CompileError` instead of overflowing.

### Verification
```cpp
// On a default-stack thread (~8 MiB on macOS/Linux), compile a deeply nested
// expression. Must return a CompileError or succeed — must NOT crash.
std::thread t([]{
  std::string src = "pub fn main() -> f32 { ";
  for (int i = 0; i < 200; ++i) src += "(";
  src += "1.0";
  for (int i = 0; i < 200; ++i) src += ")";
  src += " }";
  auto p = sampo::noiser::NoiserProgram::Compile(src);
  // acceptance: this line is REACHED (no stack overflow)
});
t.join();
```

---

## 3. Host-call profiling is on by default, and the switch lives in `detail::`

### Current behaviour
`HostThunkTrampoline` samples `std::chrono::high_resolution_clock` on **every**
host call, and the thread-local default is ON. For badlands this is squarely in
the hot path: every entity's brain makes several perception calls per tick at
30 Hz, so the sampling cost scales with population.

The opt-out is `sampo::noiser::detail::SetHostCallProfiling(false)`. badlands
calls it exactly once (`game/src/game.cpp`, inside a `std::call_once`), and the
callsite carries this comment:

> One-time noiser runtime configuration. The profiling switch is thread-local
> and defaults to ON, and upstream has no public API for it yet — this is the
> only `detail::` callsite.

Two problems: the default, and that the only remedy is reaching into `detail::`,
which by convention is not public API. It is also **thread-local**, so a host
that compiles or resumes on more than one thread must remember to disable it on
each — an easy and silent performance regression.

### Proposed behaviour
- Default **off**.
- A public, non-`detail::` API to enable it — ideally per-program or per-context
  rather than thread-local, e.g. `program.SetHostCallProfiling(bool)` or a field
  on `NoiserInput`.
- If it must stay thread-local, say so in the public documentation.

### Verification
Acceptance: a program making 10^6 host calls shows no `high_resolution_clock`
sampling in a profile without opting in, and `sampo::noiser::detail::` is not
named anywhere in the host's source.

---

## 4. Generator state snapshot/restore (hot reload)

### Current behaviour
Recompiling and re-`Prepare`-ing restarts every generator from the top. In
badlands, `game_reload_script` (`game/src/game.cpp`) creates a fresh
`BrainRuntime` and then re-spawns every entity's brain from scratch, discarding
each coroutine's accumulated state.

That is tolerable for loop-shaped brains that recompute everything each tick
(today's `hero.noiser`), and lossy for anything staged. It directly penalises
the sequential behaviours badlands wants next — a taxman midway through his
round, a peasant walking to work — because reloading a script to tune one
unrelated constant teleports every such entity back to step zero.

`SaveState`'s sparse design in the VM looks close to enabling this already.

### Proposed behaviour
`ExecutionContext` gains serialise/restore:
```cpp
std::vector<uint8_t> ExecutionContext::SaveState() const;
bool ExecutionContext::RestoreState(std::span<const uint8_t>);   // false on shape mismatch
```
Restoring into a context prepared from a **recompiled** program should either
succeed (when the generator's state shape is unchanged) or fail cleanly so the
host can fall back to a cold restart. A shape-hash accompanying the blob would
let hosts decide without trial-and-error.

### Verification
```
1. Prepare generator, Resume 5 times.
2. blob = SaveState().
3. Recompile the identical source; Prepare a fresh context; RestoreState(blob).
4. Resume once.
Acceptance: step 4 yields exactly what a 6th Resume of the original would have.
Then repeat with a source edit that changes the generator's locals:
RestoreState must return false rather than corrupt the context.
```

---

## 5. `noiser_vm_ffi.h` is hand-maintained despite cbindgen being a dependency

### Current behaviour
`third_party/noiser/noiser-vm/src/noiser_vm_ffi.h` declares **54** exported
functions and is maintained by hand. `noiser-vm/Cargo.toml` declares
`cbindgen = "0.26"` as a build-dependency, but nothing in the build actually
generates the header from the Rust source. Any signature that drifts is caught
at link time at best, and at runtime as memory corruption at worst.

### Proposed behaviour
Generate the header in `build.rs` via the already-declared cbindgen dependency,
and either commit the generated artefact with a "do not edit" banner or emit it
into `OUT_DIR` and have consumers include from there. Add a CI check that the
committed header matches a fresh generation.

### Verification
Acceptance: deliberately change a `#[no_mangle] extern "C"` signature in
`noiser-vm`; the build either regenerates the header or fails a CI diff check.
Today the stale declaration survives silently.

---

## 6. The C++ wrapper hard-requires glm (and spdlog)

### Current behaviour
`noiser.hpp` includes `<glm/glm.hpp>` at line 11, so **every** consumer of the
public header needs glm on its include path, whether or not it uses vector
types. `NoiserInput` is itself defined in terms of glm (`glm::ivec3 warp_id`),
so the dependency is in the API, not just an implementation detail. The
implementation additionally logs through spdlog.

badlands happens to use both already, so this costs it nothing today — it is
raised because it inflates the minimum embed footprint for hosts that do not,
and because it interacts with item 3 below.

### Proposed behaviour
- Express `NoiserInput` in plain types (`int32_t warp_id[3]`) or a
  noiser-owned tiny struct, with a glm interop header offered opt-in behind
  `NOISER_USE_GLM`.
- Make logging pluggable: a host-installable sink, defaulting to no-op. This
  also fixes the related complaint that **expected** script runtime errors
  (e.g. division by zero surfaced through `Resume`) are logged at spdlog
  **error** level before the host ever sees them, so a host that handles the
  error gracefully still cannot keep its stderr clean. badlands hits this on
  every deliberate downgrade-path test.

### Verification
Acceptance: a TU including `<noiser.hpp>` compiles with neither glm nor spdlog
on the include path, and a host that installs a null log sink produces no
output when a script divides by zero and the host handles the resulting
`CompileError`.

---

## Related

- `docs/noiser-feedback.md` — the running integration log (items 1, 3, 4, 5, 6,
  7, 8 correspond to sections 1, 3, 2, 6, 5, 6, 4 above)
- `game/src/noiser_jit_stubs.cpp` — the 24-symbol workaround for item 1
- `game/src/brain.cpp` — `compile_big_stack()`, the workaround for item 2
- `game/src/game.cpp` — the sole `detail::SetHostCallProfiling` callsite (item 3)
  and `game_reload_script` (item 4)
