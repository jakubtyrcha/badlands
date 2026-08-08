# src/engine/rhi/ — the render hardware interface

Foundational. Everything the renderer will become sits on this, so the standard is
higher than for the code above it: a defect here is silent, backend-specific, and
expensive to fix once call sites multiply.

Design decisions and their rationale live in
`docs/superpowers/specs/2026-08-03-rhi-slang-exploration.md` (D1-D11 + three probe
results). Read it before changing the shape of anything here.

## Layering

- **The RHI links no backend SDK.** `badlands_rhi` deliberately does not link `webgpu`;
  a `wgpu::`, `MTL*` or `D3D12*` type in the interface headers must be a link or
  compile error, not a code-review catch.
- **Dependencies run one way.** `badlands_slang` depends on the RHI; the RHI never
  invokes a shader compiler and never sees Slang. The render graph will sit above,
  never beside.
- **Backends implement, they do not extend.** A backend may not add public surface the
  interface does not declare.

## The rules

These are not style preferences. Each one is here because its absence produced a
confirmed defect.

### 1. No silent error handling
Every failure path logs. An early `return` on a bad argument, a null resource or an
unsupported combination must say what it refused and why. A caller must never have to
infer failure from a wrong image.

### 2. No empty fallbacks without a diagnostic
Falling back to a plausible-looking default is worse than failing, because it hides.
If a lookup misses, log and refuse — do not substitute index 0, a whole-resource view,
or a default-constructed entry. Severity follows consequence: `spdlog::error` when the
result will be wrong, `warn` when it will be merely suboptimal.

### 3. Detect and refuse, never detect and continue
If a check fires, the unsafe operation does not run. A validation layer that reports a
problem and then performs it anyway has converted a diagnosable mistake into undefined
behaviour, which is strictly worse than not checking.

### 4. No advertised-but-unimplemented surface
A descriptor field that is accepted and ignored is a trap with a delayed fuse. Either
implement it or reject it loudly at the call. Interface surface that exists "for later"
must be unreachable, not silently inert.

### 5. One value must not mean two things
An API that returns the same value for "fine" and "not checked" cannot be used safely.
Prefer a type that makes the ambiguity unrepresentable over a comment explaining it.

### 6. Backends must not diverge observably
Null and Metal must behave identically for everything the interface documents. A
behavioural difference is a bug in whichever one is wrong — and, separately, a gap in
the conformance list, which should have caught it. Fix both.

**Prefer one implementation over two that agree.** Logic duplicated per backend is how
they drift; `ResolveBindingTable` and `ResolveViewDesc` exist because "resolve, retain,
refuse" written twice had already produced a divergence nothing caught.

### 7. Pipeline state is fully determined by the pipeline
No state leaks between draws. If a pipeline does not set something, the backend binds
an explicit default rather than inheriting the last one's.

### 8. Every size, offset and index crossing the API is bounds-checked
Against the actual resource, at the layer rule 13 assigns it to. Dawn did this for us;
nothing does now unless we write it.

**Always by subtraction, never by addition.** `offset + count > size` is unsigned
arithmetic that wraps, so a huge count sums to something small and passes the very
check it exists to fail. Write `count > size - offset`, having first established that
`offset <= size`. This has been got wrong three times.

### 13. Creation-time refusal is not validation
The two are different layers and the distinction decides where every new check goes:

| | Where it lives | Compiled out? |
|---|---|---|
| **Can this object exist at all?** | Backend, via the shared resolver | **No** — it is a constructor precondition |
| **Is this call valid against live state?** | Validation decorator | Yes (rule 12) |

An object that cannot be encoded must not be constructed, in release builds as much as
in debug ones — so `CreateBindingTable` and `CreateView` return null rather than leaving
the decorator to notice, because the decorator is not there in release. Per-call checks
(copy extents, index ranges, resource state, pass ordering) are the decorator's, because
they are per-call and must not cost anything in a shipping build.

Corollary: **resolve once, at creation.** `ResolveBindingTable` turns slots into
target-native indices when the table is built, so the record path is an indexed walk
that cannot encounter an unresolvable slot — there is no later point where a backend
must choose between guessing an index and dropping a binding. That is also what makes
rule 11 hold without effort.

### 9. All functionality has unit tests
Every feature is tested at RHI level, on **every** backend, through the shared
conformance list in `src/engine/tests/rhi_conformance.hpp`. "Tested on Null only" is
not tested — that is exactly how rule 6 gets violated unnoticed.

### 10. Red before green
A fix lands with a test that failed before it and passes after. If the defect cannot be
made to fail a test, say so explicitly in the commit rather than skipping the test.

### 11. Nothing allocates in the record path
Per-draw and per-pass recording must not allocate, hash a string, or copy a
`shared_ptr`. Names resolve to indices once, at compile or resolve time — never at
record time (D9). Virtual dispatch at pass granularity is fine and deliberate (D5); at
per-parameter granularity it is not.

### 12. Validation is opt-in and must compile out
It exists to be always-on during development and absent from profiling and release
builds. If it cannot be compiled out, it is not validation — it is overhead.

## Invariants (read the cited code before touching these)

- **Reversed-Z end to end.** Depth clears to `0.0` (far), opaque compares
  `GreaterEqual`. `GLM_FORCE_DEPTH_ZERO_TO_ONE` is `PUBLIC` on `badlands_rhi` because
  any consumer using glm needs the matching convention — without it every fragment
  silently fails the depth test.
- **`slot` is the engine's identifier; `location.index` is the target's.** They are not
  interchangeable: Slang numbers bindings per category, so a constant buffer, a texture
  and a sampler can all report index 0. See `rhi_pipeline.hpp`.
- **Per-stage binding visibility is not derivable.** Slang's `ProgramLayout` reports
  every module global regardless of entry point, so `ReflectedBinding::visibility` is
  always `All` and the Metal backend binds to every stage. Correct, slightly wasteful;
  narrowing needs a different derivation, not a different struct.
- **A texture's dimension is STATED, and a cube's six faces are spelled differently on each side.** `TextureDesc::dimension` replaced the old "`array_layers > 1` means array" rule, because a cube and a six-layer array are not the same texture. The RHI keeps `array_layers == 6` for a cube — view ranges and `Write`'s `layer` all address faces 0..5 — but **Metal's `MTLTextureTypeCube` requires `arrayLength == 1`** and gives it six slices implicitly (`arrayLength` on a cube counts *cubes*, which is `MTLTextureTypeCubeArray`). Passing 6 trips a descriptor assertion rather than returning nil, so no downstream null check can catch it. `TextureViewDimension::Auto` resolves to the texture's own dimension in `ResolveViewDesc`, exactly as a `mip_count` of 0 resolves to a real count — a view descriptor handed back by `GetDesc()` never carries a sentinel.
- **Readback is AWAITABLE, and `IBuffer::Read` is not.** `Read` is a memcpy out of live memory that does NOT wait for the GPU — every caller pairs it with `WaitIdle`, stalling the whole device to synchronise one copy. `IRhiDevice::ReadTexture` **takes an `ITextureView`** — the same currency `BindingEntry::texture_view` takes, so the object you bind to a shader is the object you read back — and returns an `ITextureReadback` that waits on exactly its own copy (`Wait`, finite timeout) or notifies (`OnComplete`). A `(texture, mip, layer)` triple here would be a second spelling of `TextureViewDesc` with its own bounds checks to keep in agreement; the view's range is already resolved and validated by `ResolveViewDesc`, so the only extra refusal is a view covering more than ONE subresource, which has no single answer. Nothing refuses for not being finished: both targets hand back a real completion object — Metal a command buffer you can `waitUntilCompleted`, DX12 a fence you `SetEventOnCompletion` and then `WaitForSingleObject` — so the interface exposes one instead of a flag to spin on. **The Metal handler must be attached BEFORE commit**, which is why `ReadTexture` takes the encoder and is documented as pre-submit. `ReadbackCompletion` (`rhi_common.cpp`) is shared: "signal, wake the waiter, run the callback once" written twice would drift, and the drift would be a race.
- **Resource-state transitions are checked, not executed, on Metal.** Metal auto-tracks
  hazards, so it can never reveal a missing declaration — the validation decorator
  checks the declared intent as bookkeeping over the command stream, with no GPU. This
  is what the eventual DX12 backend will lean on, so do not "simplify" it away because
  Metal ignores it.
- **Three lifetimes, not one.** (a) The C++ object — a binding table shares ownership
  of everything it references, and a texture view outlives `Destroy()` on its texture
  (the handle is released, the object is not). (b) The GPU timeline — a submitted
  command buffer keeps its resources alive until it retires; **Metal provides this
  natively, so Metal can never reveal a backend that fails to, and DX12 must implement
  it.** (c) Frame pacing and deferred deletion — deliberately absent, because "a frame"
  is defined by present cadence and there is no swapchain yet. Do not conflate them.
- **Shader source is target-native by contract.** The RHI never compiles anything; a
  shared test needs per-backend source (see `MinimalComputeSource` in
  `rhi_conformance.hpp`), which is also where the HLSL arm goes.
- **A shader may be split per target where Slang is wrong, and only there.** Slang 2026.14.1
  emits the 32-bit atomic intrinsic for Metal, so 64-bit atomic min/max has to be hand-written
  MSL; HLSL is correct. Each split names the Slang version that forced it, so it can be
  retired. Full account in the spec's "Toolchain gap" section.

## Testing

- **`badlands_rhi_tests`** — Null backend, no GPU, runs anywhere. Interface contracts,
  validation checks, resource-state intent.
- **`badlands_rhi_metal_tests`** — the same conformance list against Metal, plus
  readback-based correctness Null cannot assert. **Run it under
  `MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1`**; the layers catch invalid API use that
  a passing test otherwise hides.
- **`badlands_splat_tests`** — the Dreams-style splat chain (SDF → points → indirect
  dispatch → 64-bit-atomic visibility buffer → resolve), asserted on pixels. Needs both a
  Slang SDK and a Metal device, which is why it is its own target.
- **Assert on values, not images.** Readback plus numeric assertions. No golden-image
  harness — that would mean asserting on shipped data files.
- **A check that only one backend can make must say so.** The `GetCommandLog()`-guarded
  pattern is fine, but anything Null cannot observe needs a Metal-side assertion too,
  or rule 6 goes unenforced.

## Tests owed when DX12 lands

Rule 10 says an untestable fix is declared, not skipped. These are the declared
ones — each ships working and unasserted because nothing on this platform can
reach its failure path. **Write them as part of the DX12 backend, not after.**

| Owed test | Blocked on | Where |
|---|---|---|
| A faulted command buffer is reported, not silently dropped | Provoking a GPU fault (shader trap, timeout, page fault) | `ReportIfFailed`, `metal_rhi.mm` |
| An unhandled backend refuses instead of returning empty shader source | A third `BackendKind` existing, which makes the switch non-exhaustive | `UnhandledBackend`, `rhi_conformance.hpp` |

A refusal that cannot be reached from the public API is not automatically
untestable — a test-local fake resource is usually enough, and that is how
`RetainBindingResources`' two refusals are covered (`rhi_tests.cpp`). Exhaust
that option before adding a row here.

## Code review must validate these

A review of this directory is not complete without checking, explicitly:

1. Does every new failure path log? (rules 1, 2)
2. Does any check report and then proceed anyway? (rule 3)
3. Is any new descriptor field accepted and ignored? (rule 4)
4. Can Null and Metal disagree about anything new, and would the conformance list see
   it? (rules 6, 9)
5. Are new sizes/offsets/indices bounds-checked? (rule 8)
6. Does the record path allocate or touch a string? (rule 11)
7. Did the fix land with a test that failed first? (rule 10)
