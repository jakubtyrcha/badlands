# RHI + Slang: exploration and decision log

**Status:** pre-plan exploration. No implementation has started.
**Nature:** living document, spanning many sessions. Append decisions; don't rewrite history.

This records why we are replacing Dawn/WebGPU with a native RHI, what we measured,
what we decided, and what is still open. Read it before resuming the effort.

## Motivation

- Dawn/WebGPU blocks two capabilities we want: 64-bit atomics for compute-shader
  rasterization, and mesh shaders.
- Neither can be reached from WGSL. 64-bit atomics do not exist in the language, and
  Tint will not grow them.
- Owning the stack also removes the Dawn version pin as a gate on everything else.

## Decisions

Each entry records what was chosen and why. Supersede by appending, not editing.
**Newest first below D4**, so the most recent thinking is nearest the top.

| | Decision | One line |
|---|---|---|
| D1 | Build a real RHI *and* switch shader language | Both; Dawn/Metal interop kept only as a fallback |
| D2 | Metal and DX12 are the core APIs | RHI deliberately WebGPU-shaped to minimise translation |
| D3 | Hardware floor | Turing/RDNA2+ on PC, Apple8 (M2)+ on macOS |
| D4 | Binding model: descriptor tables | Bindless deferred, deadline = before vis-buffer material resolve |
| D5 | Virtual dispatch | Devirtualization is a bonus, never a plan; enables a validation decorator |
| D6 | Branch-and-swap | No dependency on Slang's WGSL backend; short shader freeze at cutover |
| D7 | A render graph is essential | Auto-binding, auto-barriers, over the RHI |
| D8 | Shared binding resolver | Graph and `MaterialInstance` coexist; only duplication is unified |
| D9 | No strings at record time | Resolve once to a dense index; the current id API is a facade over strings |
| D10 | Visibility-buffer, GPU-driven | Dissolves the geometry-pass problem; renderer rewrite accepted |
| D11 | A new app is the integration testbed | Terrain patch + instanced trees, grown from stage 1 |

Probe findings that touch these decisions are in **Probe findings** below.

### D1 — Build a real RHI *and* switch shader language (2026-08-03)

- Both, not one. The cheap alternative (Dawn/Metal interop via `GetMTLDevice` +
  `SharedTextureMemoryIOSurface`) was considered and rejected as a destination.
- That interop path remains available as a fallback if the RHI stalls.

### D2 — Metal and DX12 are the core APIs (2026-08-03)

- WebGPU is not a shipping target. Dawn survives only as a transitional backend.
- **The RHI deliberately looks like WebGPU**, to minimise translation across ~3,350
  existing call sites. This is a cost decision, not an endorsement of WebGPU's model.
- DX12 is out of scope for the first plan. A stub backend keeps the seam honest.

### D3 — Hardware floor (2026-08-03)

| Platform | Floor | Gets us |
|---|---|---|
| PC | NVIDIA Turing+ / AMD RDNA2+ / Intel Arc | Mesh shaders, 64-bit atomics, DX12 Resource Binding Tier 3 |
| macOS | Apple8 (M2)+ | Mesh shaders, 64-bit atomic min/max, Argument Buffers Tier 2 |

- Steam Hardware Survey (July 2026): ~72.6% of PC is confidently capable, ~84% of the
  classifiable population. Some of the unclassified "Other" is Steam Deck (RDNA2, capable).
- macOS is 2.32% of Steam total, so dropping M1 costs almost nothing measurable.
- **Apple8's 64-bit atomic min/max is exactly the compute-raster primitive** — a packed
  depth+payload `atomicMin`/`atomicMax`. Apple9 is not required.
- Consequence: both motivating features are baseline on every target. No capability
  fallback paths are needed, and bindless is available everywhere.

### D4 — Binding model: descriptor tables, not full bindless (2026-08-03)

- Keep WebGPU's group/binding call shape so call sites port mechanically.
- Define layouts as descriptor tables that a Metal argument buffer or a DX12 descriptor
  heap can back directly.
- Deferred rather than rejected. The reasons are sequencing, not capability:
  - No resource allocator exists; Metal bindless needs heaps plus `useHeap`/residency sets.
  - Bindless removes the error class Dawn validation currently catches.
  - It reshapes the reflection contract and material system rather than re-pointing them.
  - The GPU does not pick materials yet — instance-renderer buckets are CPU-known constants.
- **Build the heap allocator first.** It improves the non-bindless path too, and is the
  real prerequisite for going bindless later.

### D11 — A new app is the integration testbed (2026-08-03)

- Build a new `src/executables/` app that draws a **terrain patch plus instanced trees** on
  the new stack. It is the consumer that stages 1-3 are developed against, not a separate
  later step.
- **`badlands_mapview` already draws exactly this scene** — cluster-LOD terrain via
  `ClusterTerrain` plus an `InstancedMeshField` forest, in ~1,276 LOC. It is both the
  reference implementation and the visual comparison target.
- The new app depends on far less than the game does. `terrain_clusters.hpp` is pure CPU
  with no GPU dependency, so the chain is MapData → cluster DAG → GPU buffers → graph → RHI.
- **It needs none of `SceneGraph`, `MaterialInstance`, `MeshRenderingMaterial`,
  `StandardMaterialFactory` or `SceneRenderer`.** mapview already bypasses the scene graph
  for its terrain, so this is a narrowing of an existing pattern rather than a new one.
- Start it at stage 1 as the vertical slice — a single Slang shader through the RHI — and
  grow it as the graph and vis-buffer land. Infrastructure never sits without a consumer.
- It is also where 64-bit atomics and mesh shaders first get exercised, so the motivating
  features are proven early rather than at the end.

#### Prototype scope: material id + real terrain blending, no lighting

**The prototype resolves triangle/cluster id AND real blended terrain material. No lighting,
no post, no bindless.**

Terrain blending turns out to be the one material path that needs none of the deferred
machinery, because of a decision already made in `material/terrain_cluster.wesl`:

- **Biome weights come from a splat texture sampled in world XZ, explicitly not from vertex
  data** — chosen so boundaries survive LOD decimation. So cluster and LOD are irrelevant to
  the blend.
- Its resources are **7 fixed group-0 bindings**: three `texture_2d_array`s
  (albedo / normal / ARM), two splat textures, two samplers. All static, all declarable as
  graph ports.
- **World position reconstructs from depth**, so the resolve needs no barycentric attribute
  fetch at all. Layer UVs are planar from world position (`terrainPlanarUv`).
- The only attribute genuinely lost is the vertex normal. Deriving a geometric normal from
  depth derivatives is adequate for a material-and-blend view.

Trees carry a small fixed set too — `InstancedMeshField` runs `num_submeshes=2`, so two
material ids over two static resource sets.

**The general rule this establishes:** a small fixed material set needs no bindless.
Material id indexes a statically declared array of resource sets. Bindless is required only
when the material count becomes large or dynamic — which is exactly why D4's deferral holds.

What the prototype **proves**: the RHI, the graph including barrier derivation, Slang shaders
end to end, GPU-driven cluster selection, indirect draw, **and the graph's port model against
real texture resources rather than only render targets**.

What it **defers**: bindless and the heap allocator, lighting, post, and the general material
system. That is the right split — the prototype tests the architecture, and now renders
something worth looking at while doing it.

### D10 — Visibility-buffer, GPU-driven; renderer rewrite accepted (2026-08-03)

Answers the geometry-pass question left open by D7. A renderer rewrite is acceptable — the
current pipeline is a prototype.

- **A visibility buffer dissolves the geometry-pass problem.** The only thing that resists a
  dataflow graph is per-draw material binding; a vis-buffer pass writes
  `{cluster_id, triangle_id}` and binds no material at all.
- So there is no special geometry node kind: **one raster node** with a static port set
  (vertex/index heap, cluster buffer, indirect args, visbuffer target, depth), plus an
  ordinary screen-space resolve node reading a bindless material table.
- This is why the two motivating features cohere — 64-bit atomics for the compute-raster
  path on small triangles, mesh shaders for the hardware path on larger clusters. That is
  Nanite's dual-path design, and the terrain cluster DAG is already built for it.

#### Where the terrain actually stands

The build side is genuine Nanite; the runtime is not.

- DAG, boundary-locked QEM simplification, 13 levels, ~8.2k clusters, ~1.05M tris for a
  512² map. Seamlessness is a build invariant, already pinned by tests.
- **Runtime is CPU**: `SelectClusters` is a flat pass over all clusters, then per-cluster
  `DrawIndexed` over ranges of one shared buffer.
- **Measured: ~0.95-1.0 ms per frame on selection alone, and ~908 draws** for the near
  camera. Roughly 6% of a 16 ms budget spent deciding what to draw.
- The gap to GPU-driven is narrow and specific: move selection to compute, emit indirect
  args, collapse ~908 draws into one `DrawIndexedIndirect`.
- Terrain already has the prerequisites — one shared buffer, per-cluster error and bounding
  sphere, and a cut that is a pure function of camera and τ. **That is the same
  classify → scan → scatter → indirect shape `GpuInstanceRenderer` runs for trees**, so both
  geometry producers converge on one model.

#### Consequences for earlier decisions

- **D4's rationale is superseded.** Bindless was deferred partly because "the GPU doesn't
  pick materials yet"; vis-buffer material resolve *requires* bindless, since selection is
  per-pixel. The deferral stands as sequencing, but its deadline is now known: **before the
  vis-buffer resolve pass.**
- **D8's correctness rider is retired.** Geometry nodes touch no material textures, so there
  is nothing for them to declare. The heap is declared by the resolve node.

#### Known cost

- Material evaluation moves to screen space; vertex attributes need manual barycentric
  interpolation from triangle IDs; G-buffer and deferred lighting get restructured.
- Shadows likely stay depth-only raster, so two geometry paths persist regardless.
- The batch/slot model is still needed transitionally, since not everything converts at once.

### D9 — No strings at record time (2026-08-03)

Rule: **strings are permitted at compile and resolve time, never at record time.**

- Resolve once to an opaque id, then use the id per draw:
  `auto p = GetParamId(pipeline, "Roughness"); SetFloat(p, x);`
- **The id is a resolved dense index into that pipeline's parameter table, not a hash.**
  A raw hash yields no offset and collides silently; hashing is fine as the lookup key
  *during* resolution only.
- The id carries its type (as `MaterialParameterId` already does) so setters can validate
  cheaply, in debug builds at minimum.
- The id belongs to a compiled pipeline or material, **not to the RHI**. Binding indices and
  UBO offsets differ per shader variant, so a free `RHIGetParamUid` has nothing to resolve
  against. The existing `RenderingMaterialInstance::GetParameterId` is the right shape.
- The rule covers the graph too. D7's port-name-to-reflection matching is string work at
  graph compile time, which is fine; record-time port binding must be index-based.

#### Why this is structural, not a signature change

Measured, and worse than it looks:

- `SetParameterByName("modelMatrix", …)` is called **inside the per-entity draw loop** in
  `render_textured_mesh.cpp` and `render_forward.cpp`, once per entity plus once per uniform
  override, every frame.
- **`SetParameter(id, value)` is currently slower than the string path.** It hashes
  `handle_to_name_` to recover the name, then calls `MaterialInstance::SetFloat(name, …)`
  which hashes `constant_map_` again. The handle is a facade over a string-keyed system.
- `GetParameterId` is effectively unused in production — only tests call it.
- `InstanceParams::uniform_overrides` is `unordered_map<std::string, MaterialParameterValue>`,
  so the override *data* is string-keyed and iterated per entity per frame.

So the fix is three changes, not one:

1. The id resolves to a direct location (`{UniformType, index}`), never back to a name.
2. `MaterialInstance`'s name→location map is consumed at instance creation, not per set.
3. `uniform_overrides` becomes a `vector<{ParamId, value}>` resolved at instance creation.

### D8 — Shared binding resolver; graph and MaterialInstance coexist (2026-08-03)

Option C of the three considered. The graph does **not** subsume `MaterialInstance`.

- Extract reflection name→binding resolution into one component that both the graph and
  `MaterialInstance` call. Only the genuinely duplicated logic is unified.
- The graph keeps pass-level binding: render targets, engine globals, group 0, compute I/O.
  Ports are few and resolve at graph compile time.
- `MaterialInstance` keeps per-draw work — UBO packing from reflected offsets, and the
  `Bind`/`BindPerObject`/`BindInstanceData` protocol. Neither is distorted to fit the other.
- Rejected: the graph subsuming materials (A) would need per-draw binding as a first-class
  graph concept — a much larger graph, and a bet that should be deliberate rather than a
  side effect of stage 3.

**Correctness rider, not optional:** geometry nodes must declare their material texture set
to the graph, even though `MaterialInstance` still binds it. Otherwise those textures'
states are invisible to D7's barrier derivation, which on DX12 is a missing transition
rather than an untidiness. One transition before the pass is enough.

### D7 — A render graph is essential to the port, not optional (2026-08-03)

Resolves R3 and the open render-graph question.

The graph must:

1. Automate bindings from shader reflection.
2. Automate barriers and resource-state transitions for DX12.
3. Minimise userspace boilerplate.
4. Make porting between platforms easier.
5. Talk to the GPU only through the RHI.

Layering that follows: **engine passes → render graph → RHI → Metal / DX12.**

- The RHI stays explicit, verbose and WebGPU-shaped (D2). The graph is where terseness
  lives. Do not push convenience into the RHI to satisfy requirement 3.
- The graph becomes the pass-authoring API. Requirement 4 is satisfied because passes never
  see a backend.
- **Requirement 1 also insulates D4.** If passes bind by name through the graph, moving to
  bindless later changes the graph's binding layer rather than every pass — which is what
  makes deferring bindless safe.

#### What sampo's ProcessingGraph does and does not give us

Measured, not assumed. It is a skeleton worth porting, not a solution.

| Requirement | sampo today |
|---|---|
| 1. Reflection-driven binding | **Absent.** Nodes bind manually. |
| 2. Barrier derivation | **Absent.** A `resource_producer` map orders nodes; no usage or state is tracked. |
| 3. Minimal boilerplate | Partial — builder + `NodeHandle` + typed ports help; binding is still hand-written. |
| 4. Platform portability | Partial — the node interface takes `wgpu::Device` and `wgpu::CommandEncoder` directly. |
| 5. Via RHI | **Absent.** It speaks `wgpu::` throughout. |

What it *does* give us, and why it is still worth porting: the DAG skeleton (`ResourceId`,
Kahn topological sort, `TexturePool`, `Compile`/`Flush` watermarks, builder + `NodeHandle`,
single-encoder execution), 2,189 LOC of tests, and ~30 worked node examples.

#### The one extension that unlocks two requirements

- `PortDesc` today is `{name, default_value, required}`. Access is implied only by whether
  a port is an input or an output.
- **Give ports an explicit usage** — sampled, storage-read, storage-write, render-target,
  depth, indirect-arg. That single change feeds both barrier derivation (requirement 2) and
  reflection matching by name (requirement 1).

#### Known gap: geometry passes

- sampo's graph models texture dataflow. Roughly 4-5 of badlands' 12 passes have node
  analogues; shadow, gbuffer, forward, decals and debug lines have none.
- Those need scene traversal, so the graph needs a node kind whose input is a draw list or
  scene view rather than a texture. **This is the main design work beyond porting sampo.**

### D6 — Branch-and-swap; no dependency on Slang's WGSL backend (2026-08-03)

Supersedes R1, which was judged overblown.

- The work happens on a branch. Slang targets MSL only; **Slang's WGSL backend is not used
  and not depended on.**
- `main` keeps WESL → WGSL → Dawn untouched, so game development proceeds there in parallel.
- At cutover, shader development pauses while the remaining shader delta is translated.
  The shader port is small and image-verifiable, so the freeze window is short.

Consequences, which are the reason this is recorded rather than assumed:

- **Dawn does not get an RHI backend.** A Dawn backend would need WGSL, which would
  reintroduce exactly the dependency this decision removes.
- **So there is no A/B backend comparison during the port.** Every ported subsystem is
  verified through Metal alone.
- **The property-based GPU tests are the verification spine.** The 19 suites assert on
  numeric readback properties, not on stored reference images — so they stay meaningful
  oracles across a backend swap.
- Do **not** build a golden-image regression harness. Storing reference PNGs would mean
  asserting on shipped data files, against this project's testing convention. Screenshots
  stay a human-inspection tool.

### D5 — Virtual dispatch, and do not rely on devirtualization (2026-08-03)

- The RHI is a virtual interface. Devirtualization is treated as a bonus, never a plan.
- Whole-program devirtualization only fires with LTO plus `-fwhole-program-vtables` and a
  single implementation linked in. We will link several backends (real + null + validation),
  so it will not fire.
- **This does not matter.** Draw counts here are tens to low hundreds per frame — the
  renderer is GPU-driven with one indirect draw per bucket. A few thousand virtual calls
  per frame is microseconds against a 16 ms budget.
- The decisive argument is different: **a virtual interface lets a validation layer be a
  decorator backend** that forwards to the real one. That is how we replace the Dawn
  validation we are giving up.
- It also lets a null/mock backend coexist with Metal in one test binary.
- If dispatch ever profiles hot, the fix is batching at the graph level, not devirtualization.

## Measured state (as of 2026-08-03)

Re-measure before trusting these; they are a snapshot, not a contract.

### Coupling

- ~3,350 `wgpu::` references over 106 distinct API symbols, across 139 files. There is no
  existing abstraction layer — `wgpu::` *is* the abstraction.
- `game/` (the EnTT simulation) is completely clean. The grep hits there are the English
  word "dawn" in hero-schedule comments.

| Bucket | Share | Fate under the RHI |
|---|---|---|
| Target-neutral (formats, usages, load/store ops, pass descriptors, blend, cull) | 57% | Maps 1:1 to Metal and DX12. Keep verbatim. |
| Opaque handles (`Device`, `Buffer`, `Texture`, `Queue`, `TextureView`) | 32% | Mechanical rename. |
| Binding model (`BindGroup*`, `PipelineLayout`) | 5% | The part worth designing — see D4. |
| Async + validation (`CallbackMode`, `MapAsync`, error scopes) | 4% | Deletes. Metal and DX12 are synchronous here. |

- Roughly 89% of the surface survives a WebGPU-shaped RHI unchanged.
- Bind-group creation is ~35 sites; `SetBindGroup` ~43 sites. The 5% is concentrated.

### Shaders

- 63 `.wesl` files, ~6,760 LOC. 95 imports across 39 files, a shallow graph centred on
  `common/frame` (44 of them).
- ~180 `@if` conditional-compilation sites over 12 feature flags (`shadow_pass`,
  `instanced`, `transparent`, `is_cubemap`, `sphere_mode`, `use_srgb`, `translucency`,
  `still`). These map straight to Slang's preprocessor.
- **The hard WGSL→Slang cases do not occur here:** zero `var<workgroup>`, zero barriers,
  zero push constants, zero `ptr<>`, zero `override` constants.
- Everything else is 1:1 — `textureLoad` ×52, `textureSampleCompare` ×10, `textureStore` ×9,
  `@builtin` ×97, and only 6 atomic sites for Slang's `Atomic<T>`.
- Densest file is `game/foliage_impostor.wesl` at 31 conditionals.

### Tests

- 19 GPU test suites. Device creation already funnels through one helper
  (`test::RequestDevice` in `gpu_test_helpers.hpp`), so that part is a single port.
- Their `wgpu::` usage is dominated by texture/buffer descriptors plus readback — exactly
  the RHI's core surface. The bodies port mechanically once resources and passes exist.
- **Tests using Dawn's validation scope.** 14 sites across `gpu_instance_tests` — 8 via
  `RunCapturingValidationErrors`, 6 raw `PushErrorScope`. Probe C found these are far less
  of a problem than first assumed; see its findings below.

## Probe findings (2026-08-03)

Measured with `tools/slang_probe` against **Slang v2026.14.1**, macOS arm64, M5 Pro.
Reproduce with that directory's README. Three shaders were hand-ported from WESL:
`terrain_cluster` (+ `frame`, `terrain_layers`, `gbuffer_encode`) and `instance_classify`
(+ `instance_common`). All compile clean to both the `metal` and `hlsl` targets.

### Probe A — runtime compilation is viable

| Step | Cost |
|---|---|
| `createGlobalSession` | **60-70 ms, once per process** |
| `createSession` | **~0.01 ms** — effectively free |
| Cold compile, per entry point (load + compose + link + codegen) | 13-44 ms |
| Warm, same session (module cache hit) | 4-27 ms |
| With `.slang-module` precompiled imports | 20-45% off cold, where imports dominate |
| WESL baseline, same shaders | 17-23 ms `terrain_cluster`, 2.5-3.5 ms `instance_classify` |

- **Verdict: keep runtime compilation.** Per-variant cost is tens of milliseconds and is
  already cached by `GpuPipelineGenerator`'s declaration hash, so nothing forces an offline
  pipeline.
- Slang is slower than WESL — roughly 2× on the big shader, 6× on the small one. Both are
  the same order of magnitude, and both are paid once per variant. Note the WESL baseline
  links whatever `build/libwesl_ffi.a` was produced (a debug Rust build), so WESL's real
  margin is *wider* than shown, not narrower.
- Run-to-run variance is high (the WESL baseline itself moved 17→23 ms between runs). Treat
  every number here as indicative, not as a budget.
- **Preprocessor macros express the feature flags cleanly.** `SessionDesc.preprocessorMacros`
  plus `#if`/`#ifdef` covers what WESL's `@if` does; the `SHADOW_PASS` variant compiles and
  reflects correctly. Link-time constants were not needed.
- **A fresh session per macro set costs nothing** (0.01 ms), so the existing per-variant
  cache maps across unchanged — no need for one session to serve many macro sets.

#### Hot-reload: drop the session

- A session caches modules by name and **does not** notice a changed source file.
- A **new** session does, and `createSession` is ~0.009 ms.
- So `InvalidateAll()` must additionally drop the `ISession`. That is a one-line change and
  free at runtime — hot-reload survives intact.

### Probe B — reflection covers the data; the binding model does not survive

**Everything the engine consumes is available**, and the layout is exact:

| Engine field | Slang source | Status |
|---|---|---|
| `ReflectedUniformBuffer` members `{name, offset, size, type}` | `TypeLayoutReflection::getFieldByIndex` + `getOffset(Uniform)` | **Exact.** `frame` reflects to `shadowParams` at 576 + 16 = **592 bytes**, matching `static_assert(sizeof(UniformData)==592)` |
| `ReflectedVertexInput` `{location, name}` | `EntryPointReflection` params + `getOffset(VaryingInput)` | Present, plus semantic names |
| `ReflectedFragmentOutput` `{location, name}` | `getResultVarLayout()` fields | Present |
| compute `workgroup_size[3]` | `EntryPointReflection::getComputeThreadGroupSize` | Present |
| resource kind / shape / access | `TypeLayoutReflection::getResourceShape/getResourceAccess` | Present |
| `ReflectedBinding` **visibility** (per-stage) | `ProgramLayout::getParameterByIndex` | **NOT AVAILABLE this way** — see below |

#### Stage visibility is the one real gap

- `ProgramLayout` reports **every global in the module, regardless of entry point.**
  Measured: `terrain_cluster`'s `vs_main` and `fs_gbuffer` return byte-identical global
  parameter lists, even though the vertex stage samples none of the five textures.
- So composing module + one entry point does **not** prune to what that entry point uses.
  naga's `collect_function_globals` walk, which the engine relies on today, has no
  equivalent on this API path.
- **This is load-bearing on Metal**, where `setVertexBuffer` and `setFragmentBuffer` are
  separate calls, and on D3D12 root-signature visibility flags. Without it, every resource
  binds to every stage — correct but wasteful.
- Three ways out, in preference order: find a per-entry-point used-resource query in
  Slang's reflection API; derive visibility from the generated MSL/HLSL source; or bind to
  all stages and accept the cost. **Resolve before the stage-2 contract is fixed.**

**But the `(group, binding)` model does not:**

- **`[[vk::binding(b, set)]]` is ignored for both the `metal` and `hlsl` targets.** Every
  parameter came back `space=0` with per-category sequential indices, regardless of what was
  requested. WGSL-style unified group/binding pairs simply do not exist here.
- **`ParameterBlock<T>` is the mechanism that does work.** It maps to one Metal argument
  buffer (category `MetalBuffer`) and to one D3D12 register space (category
  `SubElementRegisterSpace`). **That is exactly D4's descriptor-table model**, and it is the
  construct the RHI should be built on.

#### Metal vs D3D12: the divergence is narrow and predictable

Diffing the full reflection dump for the same shaders across both targets:

- **Graphics reflection is byte-identical.** Textures, samplers, constant buffers, vertex
  inputs, fragment outputs, uniform member offsets — no difference at all.
- **Only structured buffers diverge.** Metal puts them all in the buffer category with one
  shared index space (1,2,3,4); D3D12 splits them into `srv` for read-only and `uav` for
  read-write, each with its own space (srv 0,1 / uav 0,1).
- **`ParameterBlock` index assignment order also differs** — Metal assigned material=0,
  object=1, frame=2; HLSL put frame at cbuffer 0 with the blocks at spaces 1 and 2.
- **Slang's category enum aliases across targets** (`MetalBuffer == ConstantBuffer == 2`,
  `MetalTexture == ShaderResource == 3`), so a category value is only meaningful alongside
  the target that produced it.

**Recommendation for the stage-2 reflection contract:** normalize the stable half and keep
the binding half target-specific. Names, member offsets and sizes, type kinds, varyings and
workgroup size are identical across targets and belong in one engine-owned struct. Binding
locations are not, and should be stored per target rather than forced into a single
`(group, binding)` pair. The divergence is small enough to be a tagged variant, not a
per-backend reflection system.

### Probe C — the validation requirement is one query, not a subsystem

**This corrects an earlier assumption in this document.**

- 14 sites use Dawn's validation scope: 8 via `RunCapturingValidationErrors`, 6 raw
  `PushErrorScope`.
- **All 14 assert `NoError`. Not one asserts that an error is raised.**
- So Dawn validation is used as a **correctness oracle for our own calls** — "run this and
  prove the backend found nothing wrong" — not to prove that a guard fires. The earlier
  claim that these tests "prove a guard fires via the backend's complaint" was wrong.
- **The requirement is therefore a single scoped query on the RHI: "did the validation layer
  observe anything during this scope?"** The D5 decorator provides it; 14 test sites consume
  it. That is dramatically smaller than R2 assumed.
- Metal's own layers cover the API-level cases per Apple's documentation — `MTL_DEBUG_LAYER`
  for invalid or missing bindings, index mismatches and use-after-end;
  `MTL_SHADER_VALIDATION` for nil textures, signature mismatches, non-resident resources and
  out-of-bounds access.
- **Deferred: empirically verifying that coverage.** The plan called for a deliberate-error
  Metal program; that is meaningful work now and nearly free once the Metal backend exists.
  Do it then, before relying on the gap analysis.

### Porting gotchas worth knowing before stage 4

- `module frame;` collides with that module's own `frame` global — Slang reports an ambiguous
  reference. WESL has no module-name concept, so this hazard is new.
- `SampleGrad` takes the sampler as its **first** argument, unlike WGSL's
  `textureSampleGrad(t, s, …)` ordering.
- An entry point named `main` is silently renamed to `main_0` on the Metal target.

## Direction

### Shape

- Virtual RHI interface (D5), WebGPU-shaped (D2), descriptor-table binding (D4).
- Backends: `MetalRhi` (real), `Dx12Rhi` (stub), plus a null/mock and a validation
  decorator for tests.
- Dawn stays as a backend during the transition — see the parallel-development risk below.

### Staging

Each stage must be independently verifiable against current renders.

The new app (D11) is the consumer throughout — it starts at stage 1 and grows. Infrastructure
is never built without something drawing through it.

1. **RHI interface + Metal backend + DX12 stub**, with RHI-level tests. The new app draws
   its first triangle from one hand-written Slang shader.
2. **Slang toolchain + reflection contract.** Slang replaces the `wesl` crate's compile and
   naga reflection. Reflection is expressed in engine-owned types, never `wgpu::`.
   This must precede the graph, because the graph's auto-binding consumes it.
3. **Render graph over the RHI.** Port sampo's DAG skeleton and its tests, retarget the node
   interface from `wgpu::` to RHI types, add port usage, reflection-driven binding, and
   barrier derivation. The app's passes move onto it.
4. **GPU-driven terrain + trees, visbuffer with material-id resolve.** Cluster selection
   moves to compute; ~908 draws collapse to one indirect draw. The resolve reconstructs
   world position from depth and runs the real splat-driven terrain blend. This is the
   prototype's finished state.
5. **Materials, bindless and the heap allocator**, then lighting — the deferred half of D11.
6. **Migrate the game onto the stack**, and **DX12**, once the seam and barrier derivation
   have proven themselves.

Stages 1-4 are all one app growing. Nothing from the existing renderer is ported until
stage 6, so `main` stays untouched (D6).

## Risks and open questions

### R1 — Parallel game development — RESOLVED by D6

Judged overblown. Branch-and-swap with a short shader freeze at cutover, and no use of
Slang's WGSL backend. Left here so the reasoning is not re-litigated.

The residual risk is not shaders but **C++ divergence**: game development on `main` touches
`src/game/visual` (12 wgpu-coupled files) and may add render passes that must be ported at
cutover. Keep an eye on how much engine-side surface `main` grows.

### R2 — Losing Dawn's validation — LARGELY RETIRED by probe C

- The *test* half is settled: all 14 validation sites assert `NoError`, so the RHI needs one
  scoped "did the validation layer observe anything?" query, not an error-raising contract.
- What remains is the development-time half: Dawn validation catches mistakes while writing
  code, and Metal's layers are documented to cover the API-level cases but that has not been
  verified here. Verify when the Metal backend lands.

### R3 — Resource state tracking for DX12 — RESOLVED by D7

The render graph derives barriers from per-port usage, rather than the RHI tracking state
internally. Residual risk: the graph must cover *every* GPU access, or the uncovered ones
need hand-written barriers. `gpu_instance_renderer`'s compute→render dependency is the first
case to check, since it currently relies on the encoder handling it implicitly.

### Open questions

- **badlands has no render graph today.** `SceneRenderer::Render` is a fixed imperative
  sequence of ~12 passes (shadow → gbuffer → GTAO → contact → skybox → lighting → forward
  opaque → forward transparent → decals → grade → debug lines → tonemap), each beginning a
  pass inline with a locally-built descriptor. Dependencies are implicit in the ordering.
- `scene_build.hpp` is a scene-composition helper and `scene/scene_graph.*` is the scene
  graph — neither is a render graph.
- **But sampo has one, and it was never ported.** See "sampo's ProcessingGraph" below.
- **This is the same question as R3.** A render graph derives barriers from declared
  resource use; automatic state tracking inside the RHI is the alternative. Pick one before
  DX12, because doing neither means hand-inserting barriers into `scene_renderer.cpp`.

## sampo's ProcessingGraph — the unported prior art

`/Users/jakub/repos/sampo`, `src/image_processing/`, documented in its `docs/GPU_TASK_GRAPH.md`.

- A compiled DAG over GPU textures and CPU images: typed ports, dependency inference from
  port bindings, topological sort via Kahn, `ResourceId`/`ResourceView`/`TextureDesc`.
- ~1,100 LOC for graph + builder, **2,189 LOC of tests**, and ~30 node types.
- `TexturePool` gives transient-resource reuse, keyed by descriptor. Single-submission
  scope, no GPU-timeline tracking.
- `Compile()` / `Flush()` support incremental compilation via watermarks.
- Nodes record into one shared command encoder; the graph submits once and runs
  `OnPostSubmit()` for async readback mapping.

**It is not only an image-processing tool.** The node inventory includes `gtao_node`,
`contact_shadows_node`, `deferred_lighting_node`, `tonemapping_node` and
`convolution_bloom_node` — roughly badlands' post-geometry pipeline, expressed as graph
nodes. badlands re-implemented those inline in `scene_renderer.cpp` instead.

Honest limits:

- **It derives no barriers.** Like badlands, it leans on WebGPU's automatic hazards, so it
  does not answer R3 as-is.
- **It models texture dataflow, not geometry passes.** Roughly 4-5 of badlands' 12 passes
  have direct node analogues; shadow, gbuffer, forward, decals and debug lines have none,
  because they need scene traversal rather than texture inputs.

Why this still matters for R3:

- **The graph already infers reads and writes from port bindings** — today only to order
  nodes. That is exactly the information barrier derivation needs.
- Extending it to emit resource-state transitions is therefore far smaller than building
  state tracking from scratch. The data is present and unused.
- Per this project's convention, porting from sampo means porting sampo's tests too. The
  2,189 LOC test suite comes with it.
- **Per-binding stage visibility** — probe B measured that `ProgramLayout` does not prune
  globals per entry point, so this is a genuine gap rather than an unknown. Pick one of the
  three routes in the probe B findings before the stage-2 contract is fixed.
- **Empirical Metal validation coverage** — deferred from probe C to when the Metal backend
  exists.
- Whether Dawn is deleted at the end or kept indefinitely as a portability backend.

## References

- [Slang supported targets](http://shader-slang.org/slang/user-guide/targets)
- [Slang Metal specifics](http://shader-slang.org/slang/user-guide/metal-target-specific)
- [Metal Feature Set Tables](https://developer.apple.com/metal/Metal-Feature-Set-Tables.pdf)
- [Go bindless with Metal 3 (WWDC22)](https://developer.apple.com/videos/play/wwdc2022/10101/)
- [D3D12 Resource Binding spec](https://microsoft.github.io/DirectX-Specs/d3d/ResourceBinding.html)
- [Steam Hardware Survey](https://store.steampowered.com/hwsurvey)
