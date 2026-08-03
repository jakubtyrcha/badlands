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

### 7. Pipeline state is fully determined by the pipeline
No state leaks between draws. If a pipeline does not set something, the backend binds
an explicit default rather than inheriting the last one's.

### 8. Every size, offset and index crossing the API is bounds-checked
In the validation layer, against the actual resource. Dawn did this for us; nothing
does now unless we write it.

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

## Testing

- **`badlands_rhi_tests`** — Null backend, no GPU, runs anywhere. Interface contracts,
  validation checks, resource-state intent.
- **`badlands_rhi_metal_tests`** — the same conformance list against Metal, plus
  readback-based correctness Null cannot assert. **Run it under
  `MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1`**; the layers catch invalid API use that
  a passing test otherwise hides.
- **Assert on values, not images.** Readback plus numeric assertions. No golden-image
  harness — that would mean asserting on shipped data files.
- **A check that only one backend can make must say so.** The `GetCommandLog()`-guarded
  pattern is fine, but anything Null cannot observe needs a Metal-side assertion too,
  or rule 6 goes unenforced.

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
