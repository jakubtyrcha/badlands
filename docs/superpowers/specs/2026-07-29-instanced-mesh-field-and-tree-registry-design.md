# Instanced Mesh Field + Tree Registry — Design

**Date:** 2026-07-29
**Status:** Approved (design), pending spec review → plan.

Follow-up to the GPU-driven instanced-rendering core (PRs #36/#38). That work
shipped `GpuInstanceRenderer` (engine-generic GPU frustum-cull + distance-LOD +
prefix-sum compaction + per-bucket indirect draws), proven in isolated GPU
tests, with **no game/app consumer**. This increment builds the first consumer:
a **reusable engine component** that composites a GPU-culled instanced field into
the deferred scene, a **game-side tree field** built from the existing tree
generator + LODs, and a **model-viewer "Multi" mode** that renders a grid of the
selected tree through the real engine path.

This is the previously-deferred **Phase E + F**, re-scoped by the decisions
below. **Map integration remains out of scope.**

---

## Goal

In the model viewer, add a **"Multi"** option to the tree LOD control that
renders the selected catalog tree as a regular **grid** ("cluster") of instances
— each yaw-rotated by a constant increment — through the **actual**
`GpuInstanceRenderer`, with **dynamic per-instance distance LOD** as the camera
orbits/zooms. Bark and leaves render as one instanced model (bark → deferred
G-buffer, leaves → forward-opaque), sharing one transform per tree.

## Decisions (from co-design)

1. **Deliverable = model viewer.** Isolated: no sim, no biome map, no streaming.
   The existing single-tree entries keep their **fixed LOD 0/1/2 radio**
   unchanged; "Multi" is a **4th radio option** on that same control.
2. **"Cells" collapse to a flat grid.** The GPU already frustum-culls +
   LOD-selects every instance, so a coarse CPU cell-cull layer buys nothing
   here. No `InstancedFieldComponent`/ECS cell manager this increment (deferred
   to real map integration).
3. **The driver is a reusable *engine* pattern**, not view-local plumbing: a
   game-agnostic `InstancedMeshField` engine component owns the renderer and is
   driven by `SceneRenderer` at the correct pass boundaries.
4. **Multi-submesh buckets** (the engine change, chosen over separate models): a
   tree is **one model** (one transform row, one compacted slice per LOD bucket)
   with **bark + leaf submeshes** drawn in different passes.
5. **Generalize the renderer API** to `(bucket, submesh)`; update the existing
   engine-test call sites (no dead convenience overloads).
6. **v1 does not cast shadows** from the field. Bark receives the sun shadow via
   deferred lighting; leaves sample the shadow map in the leaf shader. Casting
   (a `PassKind::Shadow` + Pass-0 hook) is a deliberate fast-follow.

## Global constraints (bind every task)

- **Engine stays game-agnostic.** All of §A/§B/§C is `src/engine/` + engine
  shaders. No tree/leaf/foliage/bark vocabulary in engine code, comments, or
  shaders — "instanced meshes", "submeshes", "buckets", "deferred/forward-opaque
  pass". Game vocabulary lives only in `src/game/` + `src/executables/`.
- **`GpuInstanceRenderer`'s cull/prefix-sum/scatter/compaction is unchanged.**
  Only an additive submesh fan-out on the indirect-args + draw side, plus the
  scan writing per-submesh `instanceCount`. `classify`/`scatter` untouched.
- **`num_submeshes` defaults to 1** so the existing behavior is a strict subset
  (args layout, buckets, tests all reduce to today's when `num_submeshes==1`).
- **Non-instanced paths byte-identical.** Deferred/forward/translucency/water,
  and every existing `SceneRenderer` pass, unchanged when `instanced_fields` is
  empty/null.
- **`CullConfigData` stays 144 bytes** (`static_assert`); `num_submeshes` goes in
  the currently-unused `config.counts.w`.
- **Isolated GPU-readback tests** for the engine pieces (Catch2, the existing
  `badlands_gpu_instance_tests` target); a headless smoke/screenshot for the
  viewer path. Do **not** stage `third_party/noiser`.

---

## Architecture

Four pieces, engine → game:

```
[A] GpuInstanceRenderer            (engine, EXTEND)   submesh dimension
[B] InstancedMeshField             (engine, NEW)      reusable pattern: owns [A],
                                                      maps submesh -> pass + material,
                                                      Cull / Draw(PassKind)
[C] SceneContext + SceneRenderer   (engine, EXTEND)   carry + drive [B] at passes
[D] Tree field builder + viewer    (game, NEW)        build [B] from trees;
                                                      "Multi" grid in model viewer
```

Data flow (per frame, Multi mode):

```
view builds InstancedMeshField once (grid instances + bark/leaf LOD meshes + materials)
  -> scene_context.instanced_fields = { &field }
SceneRenderer::Render(camera, ...):
  frame.Begin()
  for f in instanced_fields: f->Cull(frame, camera)         # 3 compute passes, pre-Pass-0
  Pass 0  shadow            (no field draw — v1)
  Pass 1  G-buffer          ... for f: f->Draw(pass, frame, Deferred)      # bark
  Pass 3  deferred lighting (bark receives sun shadow here)
  Pass 3.7 forward-opaque   ... for f: f->Draw(pass, frame, ForwardOpaque) # leaves
  ...tonemap/resolve
```

---

## [A] `GpuInstanceRenderer` — submesh dimension

`src/engine/rendering/gpu_instance_renderer.{hpp,cpp}`,
`shaders/compute/instance_scan.wesl`.

### What stays identical
- `InstanceInput` (transform + bounds_sphere + model_info). One row per tree.
- `bucket = modelId*kMaxLods + lod`. `bucket_count_`, `bucket_base_`,
  `write_cursor_`, `compacted_` — all **per-bucket**, one compacted slice per
  bucket, prefix-summed exactly as today.
- `instance_classify.wesl`, `instance_scatter.wesl` — **no change**.
- The vertex shaders' `compacted[bucketBase[bucketId] + instance_index]` indexing
  (`bucketId` = the bucket, a CPU-known per-draw constant). Both submeshes of a
  bucket pass the same `bucketId` → identical transforms.

### What changes
- **Ctor** gains `uint32_t num_submeshes` (default 1). Store it. Write it into
  `config.counts.w` in `Cull()`.
- **Indirect-args buffer** sized `num_buckets * num_submeshes`
  `IndirectArgsData`. Arg slot for `(bucket, submesh)` =
  `bucket * num_submeshes + submesh`.
- **Mesh storage** `bucket_meshes_` sized `num_buckets * num_submeshes`, indexed
  `(bucket, submesh)`.
- **`SetBucketSubmesh(uint32_t bucket, uint32_t submesh, vb, ib, fmt, index_count)`**
  replaces `SetBucketMesh`. Same targeted-write behavior (write `indexCount@0`
  and `firstIndex/baseVertex/firstInstance@8..20`, **never** touch
  `instanceCount@4`) at the `(bucket, submesh)` arg offset.
- **`Draw`** takes `BucketSubmeshMaterialFn = fn(uint32_t bucket, uint32_t
  submesh) -> RenderingMaterialInstance*`. Iterate all `(bucket, submesh)`; skip
  if no mesh configured (`index_count==0`) or the fn returns nullptr; else
  `BindInstanceData` + set the submesh's vb/ib +
  `DrawIndexedIndirect(args, (bucket*num_submeshes+submesh)*kArgsStride)`.
- **`instance_scan.wesl`**: after computing `cnt = bucketCount[b]`, loop
  `s in 0..numSubmeshes` writing `indirectArgs[b*numSubmeshes + s].instanceCount =
  cnt; .firstInstance = 0`. `numSubmeshes` read from `config.counts.w`.
  (`bucketBase`/`writeCursor` reset stays per-bucket.)
- `CullConfigData` comment updated: `counts.w = numSubmeshes` (was unused).
  `GetArgsBuffer`/readback accessors unchanged in type; callers compute the
  `(bucket, submesh)` offset.

Backward compatibility: with `num_submeshes==1`, arg layout = `num_buckets`
entries (as today), `SetBucketSubmesh(bucket,0,…)` ≡ old `SetBucketMesh`, the
scan loop runs once per bucket. Existing render/cull/LOD/prefix-sum tests updated
only where they name `SetBucketMesh`/`Draw(fn(bucket))`.

---

## [B] `InstancedMeshField` — reusable engine component (NEW)

`src/engine/rendering/instanced_mesh_field.{hpp,cpp}`. Game-agnostic. The
"reusable engine pattern": bundles a `GpuInstanceRenderer` with the
submesh→pass mapping and the per-`(bucket, submesh)` material, exposing exactly
what `SceneRenderer` needs.

```cpp
class InstancedMeshField {
 public:
  enum class PassKind { kDeferred, kForwardOpaque };  // engine passes, no game types

  InstancedMeshField(wgpu::Device, wgpu::Queue, GpuPipelineGenerator&,
                     uint32_t capacity, uint32_t num_models,
                     uint32_t num_submeshes,
                     std::array<float, GpuInstanceRenderer::kMaxLods-1> lod_thresholds);
  bool IsValid() const;

  void UploadInstances(std::span<const GpuInstanceRenderer::InstanceInput>);

  // Configure one (model, lod, submesh): its mesh, which engine pass draws it,
  // and the already-resolved instanced material instance (its bucketId param
  // must equal BucketId(model, lod)). Not owned; must outlive the field's use.
  void SetSubmesh(uint32_t model, uint32_t lod, uint32_t submesh,
                  wgpu::Buffer vb, wgpu::Buffer ib, wgpu::IndexFormat, uint32_t index_count,
                  PassKind, RenderingMaterialInstance* material);

  void Cull(FrameContext&, const Camera&);                 // -> renderer.Cull
  void Draw(RenderPassContext&, FrameContext&, PassKind);  // -> renderer.Draw, filtered
};
```

- `Draw(pass, frame, kind)` calls `renderer_.Draw(pass, frame, fn)` where `fn`
  returns the stored material for `(bucket, submesh)` **iff** that submesh's
  `PassKind == kind`, else `nullptr`. So one `Draw(kDeferred)` in the G-buffer
  pass renders every deferred submesh; one `Draw(kForwardOpaque)` in the
  forward-opaque pass renders every forward submesh.
- The field **does not** resolve materials from a cache itself (keeps it
  game-agnostic) — the caller (the tree builder) hands in resolved
  `RenderingMaterialInstance*`s. The field stores `{PassKind, material}` per
  `(bucket, submesh)` alongside the mesh, and forwards the mesh to
  `renderer_.SetBucketSubmesh`.

## [C] `SceneContext` + `SceneRenderer` — drive the field (EXTEND)

`src/engine/rendering/context/scene_context.hpp`,
`src/engine/rendering/scene_renderer.cpp`.

- **`SceneContext`** gains (mirroring the existing `decals`/`decal_count`
  not-owned pattern, carried here so headless `--screenshot` is covered):
  ```cpp
  // Optional GPU-culled instanced mesh fields, culled before Pass 0 and drawn
  // into the deferred G-buffer + forward-opaque passes. Not owned; must outlive
  // the frame. Null/0 = none.
  InstancedMeshField* const* instanced_fields = nullptr;
  uint32_t instanced_field_count = 0;
  ```
- **`SceneRenderer::Render`** (guarded on `instanced_field_count > 0`):
  - After `frame.Begin()` and **before** Pass 0 opens: `for each field:
    field->Cull(frame, camera)` (the 3 compute passes need the encoder before any
    render pass — see the renderer's sequencing contract).
  - Inside Pass 1 (G-buffer), after `RenderTexturedMeshes`: `for each field:
    field->Draw(gbuffer_pass, frame, kDeferred)`.
  - Inside Pass 3.7 (forward-opaque), after `RenderForwardMeshes`: `for each
    field: field->Draw(forward_pass, frame, kForwardOpaque)`.
  - When empty, every pass is byte-identical to today.

---

## [D] Game side — tree field builder + model-viewer "Multi"

### Tree field builder (`src/game/`)

`src/game/geometry/tree_field.{hpp,cpp}` (name TBD in plan). Given a
`TreeOptions`, an already-created `InstancedMeshField` (1 model, 2 submeshes, 3
LODs), the bark/leaf instanced material factories, a `MaterialInstanceCache`, and
the shared leaf texture/sampler:

- Build the skeleton once (`BuildTreeSkeleton`). For `lod ∈ {0,1,2}`:
  - `bark = GenerateTreeMesh(opts, skeleton)`, `leaf = GenerateLeafMesh(opts,
    skeleton)`; if `lod>0`, `SimplifyMesh(…, kLodRatios[lod])` each (same
    `kLodRatios[]={1.0, 0.5, 0.2}` the viewer already uses).
  - Upload bark & leaf as 11-float `kTexturedMesh` vertex + u32 index buffers.
  - Resolve two instanced material instances with `bucketId = BucketId(0, lod)`:
    - **bark** — `instanced_gbuffer` factory (`MaterialPassType::kDeferred`, GBuffer
      color+depth formats, `cull=None`); params: `tint`=bark color, `params.y`=
      roughness, `bucketId`; 1×1 white albedo texture.
    - **leaf** — `instanced_forward` factory (`kForwardOpaque`, scene HDR color +
      depth formats, `extra_features={"translucency"}`, `cull=None`); params:
      `tint`, `transmission` (from `opts.leaves.transmission_tint/strength`),
      `params.x`=alpha cutoff, `bucketId`; leaf silhouette texture + trilinear
      sampler.
  - `field.SetSubmesh(0, lod, 0=bark, barkMesh, kDeferred, barkMat)`;
    `field.SetSubmesh(0, lod, 1=leaf, leafMesh, kForwardOpaque, leafMat)`.

The builder can produce a field for **any** `TreeCatalog()` preset (the "registry
of trees" aspect); the viewer builds one for the selected preset on demand.

### Grid instances

`N×N` grid (fixed `N`), spacing `S` (fixed), centered on origin. One
`InstanceInput` per tree (`modelId=0`):
- `transform = translate(cell_xz) * rotateY(i * Δyaw) * (display-scale + base-lift)`
  — the same `kTreePreviewHeight` display scale + rest-on-floor lift the single
  tree uses, so each grid tree sits on the floor; `Δyaw` a fixed constant
  increment (the "constant increment rotation around the up axis").
- `bounds_sphere` = world-space center + radius of the scaled tree (for the GPU
  cull).
- `capacity = N*N`.

### Dynamic LOD

`lod_thresholds` (2 values) chosen from the framed camera distance + grid extent
so the near/far halves of the grid land in different LOD bands — multiple LODs
visible at once, shifting live as the user orbits/zooms. Fixed at field build.

### Model-viewer changes (`src/executables/viewer/model_viewer_view.{hpp,cpp}`)

- LOD control gains a **"Multi"** radio beside `0/1/2` (only shown for tree
  entries). State: `lod_mode_ ∈ {Lod0, Lod1, Lod2, Multi}`.
- `RebuildScene`: for a tree entry with `lod_mode_==Multi`, **skip** the
  single-tree `AddMeshEntity`/`AddForwardOpaqueMeshEntity` path; instead build
  the `InstancedMeshField` (grid) via the tree builder, store it as a view
  member, and set `scene_context_.instanced_fields = {&field}` /
  `instanced_field_count = 1`. Frame the orbit on the grid bounds. For
  `Lod0/1/2`, current behavior exactly (and clear `instanced_fields`).
- The field + its GPU buffers are owned by the view, rebuilt when the selection
  or Multi toggles; `instanced_fields` is set each frame in `Update`/`DrawUI`
  alongside the other `scene_context_` writes.

---

## Testing strategy

Engine (`badlands_gpu_instance_tests`, isolated GPU readback):
- **Submesh scan fan-out:** 1 model / 2 submeshes / N instances; after `Cull`,
  read the args buffer and assert **both** submesh slots of each bucket carry the
  same `instanceCount` (= that bucket's survivor count), and non-configured
  slots draw nothing.
- **Shared-slice render:** configure a bucket's two submeshes with **distinct**
  geometry (e.g. a quad at +X vs a quad at −X relative to the instance) and
  distinct colors; render submesh 0 into one target and submesh 1 into another;
  assert both appear at the **same** instance transforms (proves both index one
  compacted slice via the same `bucketId`).
- **Backward-compat:** existing cull/LOD/prefix-sum/render tests updated to the
  `(bucket, submesh)` API with `num_submeshes=1` still pass (no behavioral drift).
- **`InstancedMeshField` drive:** a small end-to-end: build a field with a
  deferred submesh + a forward submesh, `Cull` + `Draw(kDeferred)` into a
  G-buffer-format target and `Draw(kForwardOpaque)` into a color target; read
  back and assert each pass drew only its submesh.

Game/app:
- A headless model-viewer smoke (SIGALRM-bounded) / `--screenshot` in Multi mode
  renders without validation errors and produces a non-empty frame (grid
  visible). Reuse the existing viewer headless harness.

Full `ctest` green; deferred/forward/translucency/water untouched (regression).

## Out of scope (later)

- Shadow **casting** from the field (`PassKind::Shadow` + Pass-0 hook).
- Cells / spatial CPU cell-cull / an ECS `InstancedFieldComponent`; real map
  integration (Forest biome placement via `sim_.BiomeAt`/`GroundAt`).
- Per-instance wind/params, LOD hysteresis, multiple distinct tree models in one
  field, billboard/imposter far-LOD.
