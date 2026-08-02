# Volumetric Octahedral Impostors as Foliage LOD4

## Context

The voxel-crown chain stops at L3 because it has to. `foliage_voxel_config.hpp` records
that an L4 at a 5.0 m cell was tried and dropped: the tets' `overscale` makes the crown
*volume* visibly larger than the tree it stands for, so an L4 tree reads as bigger than
the L3 it replaces. Coarsening a voxelization past a point stops being a cheaper tree and
starts being a different, wrong one.

An impostor breaks that: cost stops scaling with crown complexity at all, because the
crown becomes two triangles. This adds LOD4 as a **volumetric billboard** — one that
writes albedo, world normal and translucency into the G-buffer and is lit by the deferred
pass exactly like the voxel crowns are, rather than a flat unlit card.

## What already exists (and shortens this a lot)

- **The foliage shading model is already in the G-buffer.** `packVoxelFoliageGBuffer`
  writes `material.a = kShadingModelFoliage`, and `deferred_lighting.wesl` branches on it
  to add `evaluateTranslucency`. The impostor reuses this verbatim — **no new shading
  model, no G-buffer format change.**
- **A LOD slot is free.** `GpuInstanceRenderer::kMaxLods = 8` and only 4 are used; LOD
  count is runtime per model (`ModelLod`), and each `(model, lod, submesh)` carries its
  own vertex/index buffers *and its own material*. An impostor level is therefore just
  another LOD whose submesh happens to be a quad with a different material — **no change
  to the instancing core.**
- **`ColorRenderTarget`** (color + depth, readback-capable) is the bake target, and this
  codebase already renders offscreen in anger (`decal_pass_tests`, `terrain_blend_tests`,
  `mip_generator_render.wesl`).
- **`BuildLeafMipChainRgba8` already does Castano alpha-coverage preservation**, which is
  precisely the machinery alpha-cutout impostor mips need to not dissolve with distance.

## Decisions taken

| Question | Decision |
|---|---|
| View set | **Octahedral hemisphere, 4x4 = 16 views**, camera-facing quad, blend the 3 nearest cells |
| Bake timing | **At load, on the GPU**, in the same step that builds the voxel LOD chain |
| Leaf cards | **Converted to deferred alpha-test in this work**, sharing the cutout plumbing |

Octahedral over a yaw ring, **decided against a stated requirement rather than against the
current code**. `GameCameraController` today documents `pitch_deg` as a "FIXED down-tilt"
and says "the tilt and compass heading never change", which would make a 6-view yaw ring
exact and about 3x cheaper in bake, memory and per-pixel fetches. That guarantee is not
one to build on: **camera rotation is planned.** An impostor scheme is baked into content
and into a material's whole sampling structure, so it is the wrong thing to have to redo
when the camera opens up — the ~3x is the price of not making LOD4 the blocker.

It also pays for itself immediately, in shadows: the yaw ring could not have cast a
correct shadow at LOD4 (see below), so the cheaper option was never as cheap as it looked.

## Architecture

```
src/game/visual/
  impostor_atlas.{hpp,cpp}   NEW  the baked product: 2 texture_2d_arrays + tile layout
  impostor_baker.{hpp,cpp}   NEW  renders N models x 16 ortho views into the atlas
  octahedral.hpp             NEW  hemi-oct encode/decode + cell/barycentric lookup (pure)
  tree_field.cpp             MOD  LOD chain gains an impostor level; leaf cards go deferred
  forest_renderer.cpp        MOD  bake after the meshes, before upload

shaders/game/
  foliage_impostor.wesl      NEW  the volumetric billboard material (gbuffer + shadow)
  impostor_bake.wesl         NEW  MRT bake shader (albedo+alpha, normal+translucency)
  foliage_cutout.wesl        NEW  alpha-tested deferred material for the leaf cards
```

**Nothing under `src/engine/` changes.** The baker is game-side because the bake shader has
to know what a tree *is* (solid bark, tinted leaf cards with a transmission term), and
`shaders/CLAUDE.md` already reserves `shaders/game/` for exactly that. It drives the bake
through engine primitives (`ColorRenderTarget`, `GpuPipelineGenerator`, the material
factories) rather than adding an engine facility. **Step 0 exists to prove that before
anything is built on it.**

## The atlas

**One `texture_2d_array` layer per MODEL**, each layer a 4x4 grid of 128x128 view tiles
(512x512 per layer). Two arrays:

| Array | Contents |
|---|---|
| `impostor_albedo` | `rgb` = albedo, `a` = coverage (the alpha-test channel) |
| `impostor_surface` | `rg` = octahedral-encoded **tree-local** normal, `b` = translucency strength, `a` = AO |

Layer-per-model, not view-per-layer: `maxTextureArrayLayers` is 256, so 16 views x 28
models would sit at the limit and a bigger forest would simply fail to build. Per-model
layers scale with the content instead, and keep the atlas maths to a tile offset.

Normals are baked in the tree's **local** space, so the runtime rotates them by the
instance's yaw — a 2D rotation of `n.xz`, since instances only ever rotate about Y.

Budget at 16 models: 2 arrays x 16 layers x 512^2 x 4 B = 33.5 MB, ~45 MB with mips.

**Tile bleed is the trap.** Mip sampling across tile borders pulls in the neighbouring
view. Inset the sampled uv by half a texel at the tile's mip level and cap the mip chain
so the coarsest mip still resolves a tile. This is the kind of thing that looks fine in a
screenshot and shows up as haloing only at distance, so Step 2's test asserts it directly.

## Octahedral mapping

Hemisphere (never viewed from below, and a full sphere would waste half the atlas):

```wgsl
fn hemiOctEncode(d: vec3<f32>) -> vec2<f32> {      // d normalized, d.y >= 0
    let n = d / (abs(d.x) + abs(d.y) + abs(d.z));
    return vec2<f32>(n.x + n.z, n.z - n.x) * 0.5 + 0.5;
}
fn hemiOctDecode(uv: vec2<f32>) -> vec3<f32> {
    let f = uv * 2.0 - 1.0;
    let p = vec3<f32>((f.x + f.y) * 0.5, 0.0, (f.y - f.x) * 0.5);
    return normalize(vec3<f32>(p.x, 1.0 - abs(p.x) - abs(p.z), p.z));
}
```

The bake takes tile `(i, j)` -> `uv = (i + 0.5) / 4` -> `hemiOctDecode` -> the ortho eye
direction. The runtime takes the view direction in tree-local space -> `hemiOctEncode` ->
the uv whose surrounding views it blends. **Encode and decode must round-trip**, and that
is a pure function with a checkable answer — pinned in Step 1 before any GPU work.

**Views sit at TILE CENTRES, not grid vertices** (corrected in Step 1; the draft's
`uv * 3` assumed vertices). Under this mapping the square's entire boundary is the
horizon, so a 4x4 *vertex* grid would put 12 of its 16 views at zero elevation — angles a
50-58 degree camera never looks from — leaving 4 for everything it does. Centres put every
view strictly above the horizon, elevations ~18 to ~72 degrees. The runtime lattice is
therefore `uv * 4 - 0.5`, and directions outside the outermost centres clamp onto them.

**Blending three views.** The lattice coordinate lands inside one quad of centres; its
diagonal splits it into two triangles, and the containing triangle's three corners get
barycentric weights. Six texture fetches per pixel (3 views x 2 arrays), affordable
*specifically* because LOD4 only ever covers a few dozen pixels.

Not "the three nearest views" — also corrected in Step 1. Near a quad corner the excluded
corner can be closer than one of the three chosen, so a nearest-3 rule is a different (and
worse) function: it flips discontinuously as the query crosses a bisector, and a
discontinuous blend is the popping the impostor exists to remove. Triangle membership is
what makes it continuous, and continuity is what the test asserts.

Alpha is blended with the same weights and *then* cutout. Cross-fading three cutout
silhouettes ghosts slightly at the boundary; at LOD4 range that is sub-pixel, and the
alternative (nearest cell only) pops. `kImpostorBlendCells` makes it a one-constant
retreat if the ghosting reads worse than the pop.

## Shadows — the one genuinely new problem

`CullShadow` culls against the **light's** frustum but selects LOD from the **camera's**
position, deliberately: "shadow geometry must match the LOD the camera renders". So an L4
instance casts its shadow *from the impostor quad*. A camera-facing quad lit from the side
casts a sliver, and a distant forest's shadows would visibly collapse.

**Octahedral solves this for free, and it is the strongest argument for the choice.** The
shadow variant billboards toward the **light** instead of the camera and samples the
octahedral view for the **light** direction — the same atlas, a different lookup, a
correctly tree-shaped shadow. Legal within the existing contract: a shadow material may
not declare `@group(2)`, but the atlas lives in `@group(0)` with the material's own
params.

This needs a fragment stage on the shadow variant (to `discard`), where `voxel_foliage`'s
shadow variant today is depth-only with no fragment output. **Step 0 confirmed a fragment
stage with zero color targets compiles and discards correctly on this Dawn build**, so the
"no shadow at L4" fallback is off the table — good, since a distant forest quietly losing
its shadows would have been the most visible compromise in this plan.

---

## Step 0 — Spike — DONE, 2026-08-01

Ran as a throwaway GPU test (`[impostor-spike]`, since deleted) plus one question settled
by reading. All three unknowns resolved; **the plan stands and needs no engine change at
all.**

1. **Game-side offscreen MRT works, no engine change.** A `shaders/game/` shader compiled
   through `GpuPipelineGenerator` with two `RGBA8Unorm` targets, rendered, and both
   attachments read back with their own distinct constants (so they were genuinely
   separate, not aliased). `gpu_pipeline_generator.cpp` is explicitly "MRT-ready" — it
   builds one `ColorTargetState` per entry in `RenderTargetFormats`. **The baker stays
   game-side.**
2. **A fragment stage with ZERO color targets compiles and discards.** Passing an empty
   `RenderTargetFormats` yields `targetCount = 0` with the fragment stage still attached.
   A depth-only pass whose fragment stage discards its right half wrote 0.5 on the left
   and left the 1.0 clear on the right. **The light-facing cutout shadow is viable; the
   "no shadow at L4" fallback is not needed.**
3. **Array binding needs NO engine change — the engine already has an idiom for exactly
   this, and it is the caller's job.** `standard_material_factory.cpp` picks its
   unbound-slot default dimensionality per **GeometryType**, not per slot, and says so
   outright:

   > this is per-GEOMETRY, so it cannot describe a material whose slots mix
   > dimensionalities. kTerrainCluster is exactly that [...] Its array slots are
   > therefore REQUIRED to be bound by the caller; `ClusterTerrain::Build` enforces that
   > rather than letting a wrong-dimension default reach Dawn.

   The impostor material is the same shape as `terrain_cluster` (two arrays, plus 2D
   slots), so it takes the same route: **always supply both arrays via
   `InstanceParams.texture_overrides`, and check for null views at the top of the build
   with an error that names the missing one** — `ClusterTerrain::Build` (cluster_terrain
   .cpp:38-53) is the reference. One subtlety worth copying: `DefaultTextureView::type` on
   an *override* only filters default-view recipes, so `TextureType::k2D` is correct even
   for an array view.

   This is strictly better than the engine change first drafted here: the failure mode is
   a named error at build time rather than a neutral-gray render that hides a missing
   binding, and it keeps `src/engine/` untouched.

### Observation, NOT part of this work: `TextureRequirement::forced_type` is dead

It is declared in `material_requirements.hpp` with a comment describing exactly the
per-slot override that would solve the above — and **nothing anywhere reads it**. The
factory branches on `expected_type` (geometry-derived) and never consults `req.forced_type`.

Flagged because it is an active trap rather than harmless dead code: it reads as the
supported way to type a slot, so the obvious "fix" is to populate it — which compiles,
looks right in review, and does nothing at all. Worth deleting or wiring up, but on its
own, not smuggled into this feature.

## Step 1 — `octahedral.hpp` (pure, no GPU) — DONE, 2026-08-01

`src/game/visual/octahedral.hpp` + `game/tests/octahedral_tests.cpp` (8 cases in
`badlands_tree_tests`, no device). Two corrections to this plan came out of it — views at
tile centres rather than vertices, and triangle membership rather than nearest-3 — both
folded into the section above.

Tests, and what each is actually for:
- Encode/decode round-trip over a dense hemisphere sweep.
- The map's anchors (centre = zenith, the four corners = -Z/+X/+Z/-X) — everything
  downstream assumes this, and a change would silently invalidate the tile layout.
- Below-horizon input projects onto the horizon rather than producing a NaN or a flipped
  view; a free camera can dip under a tree on a rise.
- All 16 views are distinct unit directions with `y > 0.05` — the direct assertion of the
  centres-not-vertices decision.
- Weights are non-negative and sum to 1 across a sweep fine enough to land on cell edges
  and the diagonal seam.
- Querying a baked view's own direction returns that view with weight 1 (exact at the
  sample points, or every tree is permanently a blur of neighbours).
- **Continuity**: the full 16-weight vector moves by < 0.1 in L1 per small angular step.
  This is the property the whole scheme exists for and the one nearest-3 would fail.
- Reconstruction accuracy, with a **derived** bound: 16 views over a hemisphere is
  0.393 sr each, whose cone half-angle is 20.4 deg, so ~20 deg is this grid's resolution
  and nothing can beat it. Measured worst case **18.4 deg** — the blend is already at the
  grid's limit. Guarded at 22 deg as a regression catch, with a lower bound of 1 deg so
  the test cannot pass by never leaving a baked view.

## Step 2 — `impostor_atlas.{hpp,cpp}` + tile layout — DONE, 2026-08-01

`src/game/visual/impostor_atlas.{hpp,cpp}` + `game/tests/impostor_atlas_tests.cpp`
(6 cases). Layout arithmetic is free functions with no GPU types, so baker and material
share one source of truth and it is checkable without a device.

`BindImpostorAtlas` is the `terrain_cluster` contract from Step 0 made unforgettable: it
pushes both arrays as instance overrides and refuses an unbuilt atlas by name, rather than
letting a geometry-derived 2D default reach Dawn as a dimension mismatch at draw time.

**The mip/bleed policy came out differently from the draft, and better.** Insetting by half
a texel *at the coarsest mip* — what "half-texel inset" naively means for a mip-mapped
atlas — would have cost 12.5% of every tile on every side at a 4x4 tile, cropping the tree
at every level to protect the coarsest. Instead `local_uv` in [0,1] maps onto the tile's
**texel centres at the SAMPLED mip**: exactly half a texel at every level, no waste, and
not really an inset at all but the correct mapping from a continuous domain onto T texel
centres. Two consequences worth carrying into Step 4:

- The material must sample with an **explicit LOD**, since the uv range depends on the mip.
  That suits an alpha-cutout impostor anyway — derivative-driven LOD is unreliable at the
  silhouette.
- The sampler uses `mipmapFilter = Nearest`. Trilinear would blend a second level whose
  texel-centre range is different, reintroducing the cross-tile reach the mapping prevents.

Mips stop at a 4x4 tile (6 levels) rather than running to 1x1: past that a tile carries
less than the 3-view blend needs, and each level is another 16 renders per model in the
bake. Layer-per-model was kept over layer-per-(model,view) — the latter makes bleed
structurally impossible but caps the forest at 16 models, which the current pine forest sits
exactly on and the earlier 28-model mixed forest would have blown.

Tests: every tile's uv range lies strictly inside its own pixel rect at every mip; no two
tiles' ranges overlap (all pairs, not just adjacent, so a transposed index fails here);
local uv maps monotonically and `0.5` hits the tile's exact geometric centre (where the
baked trunk axis lands); the range equals the texel-centre arithmetic exactly; tile rects
tile the layer with no gap or overlap; and binding refuses an unbuilt atlas.

## Steps 3 and 4 — DONE, 2026-08-01. Steps 5 and 6 NOT started.

**Step 3 (baker)** — `impostor_baker.{hpp,cpp}` + `shaders/game/impostor_bake.wesl`, with
5 GPU tests. **Step 4 (material + LOD4 slot)** — `shaders/game/foliage_impostor.wesl` plus
`TreeFieldImpostor` in `BuildTreeField`, with 2 more. All 7 pass; 37/37 suites green.

What changed from the plan, and what bit:

- **Coverage preservation is per TILE, not per layer.** A near-overhead view covers a few
  percent of its tile while a side view covers ten times that, so one scale fitted over
  the whole layer is dominated by the dense tiles and lets the sparse ones dissolve — and
  the sparse ones are what a top-down camera looks at most. The Castano kernel was
  extracted from `leaf_texture.cpp` into `game/visual/alpha_coverage.hpp` and is now shared
  rather than reimplemented.
- **A full-layer box downsample is already tile-safe**, so no special filter was needed:
  tiles are power-of-two sized AND aligned, so a 2x2 output texel always reads two inputs
  from the same tile. That holds while the tile stays even, which is exactly why the chain
  stops at 4x4.
- **Two-sided normals are flipped against the VIEW DIRECTION, not `@builtin(front_facing)`.**
  front_facing came out inverted (mean normal dotted -0.9 against its own view) because it
  depends on winding and the framebuffer's Y convention. The bake knows its own view
  direction; that has no convention in between.
- **Group-0 binding 0 is the engine's frame UBO.** Taking it produced a duplicate-binding
  bind-group-layout error at draw time, not a compile error. Material bindings start at 1,
  which is why `instanced_gbuffer.wesl` does too. Slot names are `tex_1`/`tex_2` — the
  reflection-derived naming, same deviation `tree_field.hpp` already documents.
- **WESL's `@if` gates DECLARATIONS, not statements.** A single `vs_main` with `@if`-ed
  lines inside does not parse; the camera-facing and light-facing variants are two whole
  `@if`-gated functions over a shared helper.
- Two test assertions were wrong before the code was: octahedral decode length is a point
  on the OCTAHEDRON (in [1/sqrt(3), 1]), not a unit vector — replaced with a far stronger
  per-tile "mean normal faces its own view" check that catches wrong-space normals, a
  transposed tile index and a lost sign at once; and coverage must be measured at the
  MODEL's own cutoff (Pine's is 0.35, not 0.5).

### Review pass, same day — 7 findings, all real, all fixed

Four were correctness, and **three of the four lived in the shader, where the new GPU
tests could not see them**: those tests read the ATLAS back but never render the impostor
material. That gap is the single most important thing to close when LOD4 is wired into an
app — a full-frame render assertion (writes `kShadingModelFoliage`, normals stable as the
camera orbits, shadow not degenerate) belongs alongside that wiring, not after it.

- **`packVoxelFoliageGBuffer` arguments were swapped.** The signature is
  `(normal, albedo, roughness, ao, translucency)`; the impostor passed translucency into
  `ao` and AO into `translucency`. Since the bake always writes AO = 1, every LOD4 pixel —
  bark included — reported full translucency and blew out under back-lighting. The same
  expression also squared the strength (`params.y * surface.z`) when `surface.b` already
  holds it; it is now passed straight through.
- **The baked normal was decoded in the wrong basis.** The bake stores the tree-local
  CARTESIAN normal, but the fragment stage treated its components as coordinates in the
  quad's right/up/toEye frame. A stationary tree's shading swung as the camera orbited,
  and the gbuffer and shadow variants disagreed about the same texel because they build
  different frames. The vertex stage now passes the instance's rotation columns and the
  fragment applies that rotation. (The old third axis was negated too — moot, but it means
  nothing about the previous version was right.)
- **The mip box filter averaged RGB into cleared black texels**, so silhouette edges lost
  colour and the coverage fit then lifted those half-dark texels back over the cutoff —
  trees fading toward black exactly where the impostor takes over. Now a coverage-weighted
  downsample, with the surface map weighted by the ALBEDO's coverage (its own alpha is AO)
  so encoded normals stop being diluted toward the clear value.
- **The texel-centre inset was computed at a FRACTIONAL mip while the sampler snaps to the
  nearest integer level.** At `mip = k + 0.5` the inset covered only 0.354 texels of the
  level actually fetched — inside the guard band, so the tap reached the neighbouring
  view's tile, defeating the layout's whole no-bleed argument. The mip is now rounded in
  the vertex stage so the inset and the fetch agree on one level.

And three hygiene: `alpha_coverage.hpp` claimed to be the single copy while
`leaf_texture.cpp` still had its own (now genuinely ported onto it, with its tests as the
gate); `FoliageLodThresholdsWithImpostor` was dead code duplicating live arithmetic
(deleted); and the impostor members had been spliced between a comment and the members it
described.

## Steps 5 and 6 — DONE, 2026-08-02, with the app wiring

**Wiring.** `ForestRenderer::Build` now bakes the atlas itself and hands a
`TreeFieldImpostor` to `BuildTreeField`; mapview's call site is unchanged. A failed bake
is deliberately NOT fatal — the forest falls back to the voxel-only chain, which is what
existed before LOD4 and beats refusing to render a forest.

`BakeImpostorAtlas` lost its `wgpu::Instance` parameter: `RenderContext` does not carry
one, and `device.GetAdapter().GetInstance()` gets it without widening an engine struct.

Measured on the test map: 16 models x 16 views, 512 draws, **~1.7 s** added to the forest
build (524 ms -> 2279 ms). That is the real cost of this feature at load and the obvious
thing to attack if it bites — most of it is the 96 per-model readback/upload round trips.

**Step 5 — leaf cards to deferred.** `shaders/game/foliage_cutout.wesl` plus a game-side
factory in the model viewer, replacing `MaterialLibrary::TranslucentFoliage` +
`AddForwardOpaqueMeshEntity`. `TranslucentFoliage` had exactly one production consumer, so
this is viewer-only in reach but removes the last foliage level lit by a different path
than the rest of the chain (no GTAO, no contact shadows, a second lighting model to keep
in step). The shader is deliberately a near-copy of `voxel_foliage`'s non-instanced
variant plus a texture and a discard, sharing the shading model and the per-object UBO
layout so the two cannot drift.

**Step 6 — the rebalance, and it went the opposite way from the plan's guess.** The plan
assumed L4 would need pulling IN to be reachable. An A/B at 400 m camera height
(impostor on vs. threshold pushed out of reach) shows why the answer is stronger than
that: **L3 is a mass of fat blobs and the impostor is a crisp conifer silhouette.** L3's
tets overscale — the exact defect that killed the earlier voxel-L4 attempt — while the
impostor is a picture of the tree and cannot have that error.

So the impostor is better AND cheaper than L3 (2 triangles against L3's 56-428 plus a
256-triangle bark budget), and `kFoliageImpostorThresholdPreviewM` is now
`kFoliageLodThresholdsPreviewM.back()` — the same 70 that starts L3. In the FIELD path L3
is now a zero-width band and the chain runs L0, L1, L2, impostor.

The resolution arithmetic agrees: the impostor's limit is its 128 px tile, so it is exact
once the tree covers no more than that — `27/d * 935 <= 128`, i.e. d >= 197 m for a 27 m
pine, 58 in preview units. That lands just under 70, so by the time L3 would have started
the impostor is already past where its own resolution is the limiting factor.

L3 is kept in the chain rather than deleted because the viewer's manual "Voxel L3" mode
still shows it. **Follow-up worth doing: drop L3 from the FIELD's chain**, which saves a
voxelization and a bark decimation per model at load.

### Follow-ups done same day

**The bake takes the VOXEL L0 crown, not the leaf cards.** The field's own LOD0 is the
voxel crown, so baking cards made the impostor a picture of a tree the chain never draws
and the L2 -> L4 switch changed the tree's appearance rather than only its cost. The bake
shader gained a `voxel_brightness` uniform that swaps albedo between a sampled texture
(bark) and voxel_foliage's vertex-baked `uv.x` term, so the crown reproduces
`voxel_foliage.wesl` exactly. Two things fell out: the per-model leaf TEXTURE is no longer
needed by the baker at all, and `TreeFieldModel::leaf_cards` (added for the old bake) is
gone again. Both bark and crown are opaque now, so the preserved coverage threshold is a
plain 128 silhouette cut rather than a per-model leaf `alpha_cutoff`.

**The viewer's L3 slot is the impostor**, in the render and in the UI radio ("Impostor"
instead of "Voxel L3"). `lod_level_` numbering and `--lod` are unchanged. This needed a
non-instanced variant of `foliage_impostor.wesl`, which surfaced two engine contracts
worth recording:

- **Where a material's params UBO lives is fixed by geometry.** `SelectMaterialParamsBuffer`
  puts an INSTANCED material's constants in a group-0 UBO (group 1 being the instance
  storage) and everything else's in the group-1 per-object UBO. Declaring a group-0 params
  UBO on the non-instanced variant left it unbound and surfaced only as a bind-group
  entry-count mismatch at draw time. Both variants now carry one params block in the right
  group, reached through `matPlacement()`/`matParams()` accessors, with the atlas layer
  folded into `params.w` so the field lists match.
- **A non-instanced `modelMatrix` produces CAMERA-OFFSET space, not world.** Computing
  `to_eye` against `frame.cameraWorldPos` and then projecting with `worldSpaceToClipSpace`
  double-applied the offset and put the quad off-screen -- the level rendered as nothing
  but the bark mesh behind it, which reads as "the impostor is bark-coloured" rather than
  as "the impostor is missing". `eyePos()`/`projectQuad()` are now gated per variant.

The impostor level also stops adding the separate bark entity, since the atlas already
contains the bark.

**Multi mode walks the impostor too.** It was still calling `BuildTreeField` with no
`TreeFieldImpostor`, so the one mode whose whole purpose is showing dynamic LOD stopped a
level short of what the game draws at distance, with the retired L3 as its coarsest level.
Both viewer consumers now share one cached bake (`EnsureImpostorPreview`) -- they bake the
same model at the same preview height, so a second bake was pure duplicate work.

Multi is also the best evidence the normal-rotation fix is right: 256 instances at
different yaws all light consistently, which is exactly what the old view-basis
reconstruction could not have produced.

### Threshold correction, 2026-08-02

`kFoliageImpostorThresholdPreviewM` is back to **130**, not `kFoliageLodThresholdsPreviewM
.back()`. Setting it equal to L2->L3's own value both tripped
GpuInstanceRenderer's strictly-ascending validator (it logs and continues, so L3 was simply
skipped) and put the impostor at 70 preview metres -- 236 m for a 27 m pine, but only 70 m
in the viewer's 8 m-tree Multi grid, which reads as impostors everywhere.

**The Step 6 reasoning was half right and generalized too far.** The 400 m A/B genuinely
showed L3's tets as fat blobs against a clean impostor, and that is still true at 400 m.
What it ignored is the impostor's OTHER error term: tile resolution stops binding at ~58
preview metres, but VIEW COUNT does not. 16 views over a hemisphere are ~20 deg apart, so a
view up to 10 deg off the nearest baked one carries a parallax error of about
0.18 * crown depth -- roughly 19 px for an 8 m tree at 70 m. L3 keeps the 70-130 band.
Raising `kImpostorViewsPerAxis` is what would let the impostor come in earlier.

### Volume: depth + thickness, 2026-08-02

The impostor read flat, and its shadow was a hard cutoff, for two separate reasons: the
bake wrote a constant AO of 1.0, and translucency was a per-model constant rather than a
per-pixel optical path. The surface map is repacked to fix both, with no extra memory:

| channel | was | is |
|---|---|---|
| `rg` | octahedral local normal | unchanged |
| `b` | per-model translucency (redundant -- the uniform carries it) | **depth**, free: it is `@builtin(position).z` under the bake's ortho |
| `a` | AO, hardcoded 1.0 | **thickness** |

**Thickness is additive depth accumulation, not a min/max envelope.** Each fragment
contributes its own depth signed by facing, so a closed solid contributes exit minus enter
and a pile of them contributes the sum -- and the voxel crown is exactly a pile of closed
tets. An envelope would have been simpler but a conifer crown is mostly air, so its
envelope hugely overstates the optical path. Sign comes from `@builtin(front_facing)`
rather than from the normal, because `EmitTetMesh` gives a tet ONE shared normal for all
four faces; a globally inverted winding only flips the total, which the reader removes with
`abs()`.

Two things this needed that are worth remembering:

- **R16Float, and a hand-rolled readback.** The accumulation is signed, so an 8-bit unorm
  target would clamp every negative term to zero and destroy the subtraction; float32
  blending is an optional WebGPU feature, while R16Float is core-blendable. But
  `TextureReadback` decodes only 8-bit and 32-bit-float, so the baker carries a half-float
  decode and a blocking buffer read.
- **Normalize per model against its own p99, not against the ortho span.** The raw sum is
  in units of the bounding SPHERE's diameter and a crown occupies a small fraction of
  that: a sparse pine peaked at ~0.03, i.e. 8 of 255 codes, so the channel was almost
  entirely quantization noise -- the first version of the thickness test caught exactly
  this (p10=0, p90=7). The runtime only reads thickness through shaping constants, so
  "how deep into THIS crown" is both more useful and what survives 8 bits.

At runtime, thin edges transmit and the dense middle does not
(`transmit = strength * exp(-k * thickness)`), and thick interior is occluded
(`ao = 1 - k * thickness`). A new GPU test asserts the channel has real structure --
p90 - p10 spread, most covered texels non-zero, no saturation -- because a constant or
cancelled channel would look like a shading problem rather than a bake one.

### Conservative depth: implemented in Dawn, unreachable from here

Writing `frag_depth` would give the impostor real per-pixel depth -- correct terrain
intersection, an internally-structured cast shadow, and self-shadowing straight through the
existing shadow map with NO engine change (the G-buffer world position becomes correct, so
`deferred_lighting.wesl` needs to know nothing). It costs early-Z. Conservative depth is
the standard escape: promise the hardware the depth only moves one way and early-Z
survives.

Two gates, and the FIRST one is ours:

1. **The `wesl` crate does not parse it.** Spiked with `@builtin(frag_depth, less)` and it
   fails in WESL -> WGSL translation -- "invalid `builtin` attribute, expected 1 argument"
   -- so the shader never reaches Dawn. Note the syntax: Tint takes the depth mode as a
   SECOND ARGUMENT to `@builtin`, not as a separate `@depth(...)` attribute.
2. **Dawn gates it as unsafe-experimental.** `feature_status.cc` files
   `LanguageFeature::kFragmentDepth` under experimental -> `kUnsafeExperimental`, and
   `validator.cc` rejects the depth mode unless `allowed_features` contains it. Reaching it
   means enabling an unsafe-APIs toggle at device creation, which is app-wide, not
   foliage-local.

**Taken: plain `frag_depth` (late-Z), in both variants.** Two things make late-Z less
alarming than it sounds here: the impostor already `discard`s for its alpha cutout, so it
was never a clean early-Z fragment to begin with; and LOD4 is by definition the far,
small-on-screen tier, so the loss lands on the cheapest fragments in the frame.

**The quad stays on the CENTRE plane, and moving it to the near tangent plane was tried
and reverted.** The tangent plane is what conservative depth would want -- every real
surface behind the rasterized depth, so the shader only ever decreases it under reversed-Z
(`less`). But the bake is ORTHOGRAPHIC, and a quad pulled a full radius toward the camera
magnifies the baked image under perspective: half-width r at distance d-r subtends more
than a sphere of radius r at d. The error vanishes as d/r grows and is invisible at LOD4
range, but it is a real silhouette error, and it buys nothing while the promise cannot be
stated. Centre plane keeps the silhouette exact; a fragment is offset by
`radius * (1 - 2 * depth)`, since the bake's depth runs 0 at the near tangent plane and 1
at the far one.

**What the depth write bought, all three from one mechanism:** the impostor intersects
terrain and its neighbours correctly; the cast shadow gains internal structure instead of
being a stamped silhouette (the shadow variant writes depth too, facing the light); and it
SELF-shadows through the existing shadow map with no engine change -- because the
G-buffer position `deferred_lighting.wesl` reconstructs is now the true surface rather
than a plane. That last one retires the "needs a shading-model change" conclusion recorded
below.

**Not done: the light-tile self-shadow lookup.** The design holds -- the atlas is a depth
map from 16 directions, so projecting a pixel into the SUN's tile and comparing depths is a
shadow test needing no `frag_depth` write. The obstacle is that the impostor writes a
G-buffer and the lighting happens later, so the only channel it could fold a directional
occlusion into is AO, which reaches ambient and transmission but NOT the direct sun term.
That is a partial answer at best; a full one wants a shading-model change in
`deferred_lighting.wesl`, i.e. an engine change and a separate decision.

### Second review pass, 2026-08-02 — 6 findings, all real, all fixed

- **The runtime alpha cutoff disagreed with the one the bake preserved coverage at.** The
  bake fits every coarser mip so its coverage at 128 matches mip 0's, but the material was
  handed the model's `leaves.alpha_cutoff` -- 0.35 for pine, i.e. 89. Mip 0 is a binary
  mask so nothing showed up close; from mip 1 down the shader admitted texels the fit had
  pushed just under, so the silhouette GREW with distance. That is the Castano failure
  inverted, and the fit was actively causing it. The viewer hardcoded 0.5 for the same
  material, so the preview and the game drew different silhouettes. Now one exported
  `kImpostorAlphaCutoff` next to the mip constants, read by all three.
- **`foliage_cutout.wesl` mixed world and camera-offset space** building its two-sided
  `to_eye` -- the same mistake the impostor had, left behind in the other shader after I
  fixed it in one. It evaluates to `2*camWorld - objWorld`, which points the right way only
  while the object sits at the world origin, which is exactly where the model viewer's tree
  sits. Any card mesh placed elsewhere would light half its crown inside out.
- **`impostor_baker.hpp` still argued for baking leaf CARDS**, at length, after the
  implementation had deliberately switched to the voxel L0 crown -- so the header made the
  opposite case to the code it documents.
- **Unsigned underflow** in the threshold-count check: `leaf_lod_meshes.size() - 1` wraps
  for an impostor-only model, printing SIZE_MAX and rejecting a legitimate one-level chain.
- **The viewer rebuilt a full TreeFieldModel on every impostor cache hit** (skeleton + four
  voxelizations) only for `EnsureImpostorPreview` to discard it, on every LOD radio change.
- **`src/executables/CLAUDE.md` still documented `--lod 1-4` as voxel L0..L3.**

### Render-path GPU test, 2026-08-02

`game/tests/impostor_render_gpu_tests.cpp`, 6 cases, on an ARTIFICIAL shape: a small
trunk sphere plus an offset crown sphere, hand-assembled into a `TreeFieldModel` with no
skeleton, no voxelization and no leaf texture.

Speed was the lesser reason. A real tree's impostor has no expected value, so every
assertion about it degrades to "looks plausible" -- which is precisely what let six
runtime bugs through four review rounds. A sphere's impostor is analytic from every
direction, which turns "plausible" into "exactly this". The crown is OFFSET from the pivot
because a centred sphere is yaw-invariant and would hide a rotation mistake.

Verified by REINTRODUCING each bug and watching the test fail:

- **The normal test catches the wrong-basis decode.** At a sphere's silhouette centre the
  visible point is the one facing the viewer, so its world normal IS the direction to the
  camera -- exactly, at every azimuth. Read straight off `GBufferDebugMode::Normals`.
- **The depth test catches a plane-depth write.** Neutering `frag_depth` reads 39.59
  against a 38.60 expectation -- exactly one crown radius, which is the whole difference
  between a billboard and a volume.

**Two things it does NOT catch, and the reason is inherent.** Reintroducing the alpha-cutoff
mismatch and the fractional-mip bug both leave the silhouette-drift test passing. A
coverage fit only moves texels whose alpha sits near the threshold; a shape simple enough
to have an analytic silhouette has a one-texel partial rim, while a real crown is stippled
throughout. A ragged 12-blob fixture was tried and was still too solid. Reproducing these
needs a fixture close enough to a real crown that the analytic expectation -- the whole
point of the approach -- is gone.

So the cutoff is defended structurally instead: both call sites read one exported
`kImpostorAlphaCutoff` and cannot disagree without editing it. The fractional mip has no
test and no structural guard; it is a real remaining gap.

**Two G-buffer debug modes were added** (approved engine change):
`GBufferDebugMode::Translucency` (material.g) and `::BakedAo` (material.b), the two
channels that had no view. `Ao` already existed but is the SCREEN-SPACE GTAO field, a
different quantity. With them the argument order in `packVoxelFoliageGBuffer` became
exactly assertable, and reintroducing the swap now fails the test: at the crown's centre
the correct pack reads `g=0.004, b=0.251` and the swapped one `g=0.251, b=0.004`, so
`g < b` at both a thick and a thin sample is the discriminator.

**Two fixture bugs found while writing this, both of which had made a test vacuous:**

- **The fixture was rendering the VOXEL level, not the impostor.** The impostor's cutoff
  scales with model height (`130 * h / 8`), so at `target_height_m = 2.0` it starts at
  32.5 m -- and the material-channel test sat at 25 m. It was reading voxel_foliage's
  constant AO of 1.0 and constant translucency, which is indistinguishable from "the
  impostor writes constants". The fixture now uses a deliberately tiny height so the
  cutoff sits at ~8 m, and `OrbitCamera` REQUIREs every distance clears it. The drift
  test had the same fault in two of its five samples.
- **The rim sample sat on the `kMinAo` floor**, so a rim-vs-centre AO comparison read
  equal and passed vacuously. It now asserts against the floor value directly, from a
  camera close enough that the thickness gradient resolves in screen pixels.

### Impostor read darker than the voxel level, 2026-08-02 — two causes

- **The 3-view blend was not coverage-weighted.** It summed `texel * barycentric weight`,
  so a texel covered in ONE view but empty in the other two came out scaled by that view's
  weight alone -- dark -- while its alpha could still clear the cutoff. Wherever the three
  views disagree about coverage the impostor darkens, which for a stippled crown is most
  of it, and worst on the sparsest species. Now premultiplied: colour weighted by each
  view's own alpha and divided by the total, with the SURFACE map weighted by the albedo's
  alpha (its own alpha is thickness, not coverage, so an empty view would otherwise pull
  the encoded normal toward the 0.5/0.5 clear).

  This is the SAME correction `DownsamplePair` already makes in the bake. Third time this
  session a fix landed in one place and not its twin -- the other two were the
  camera-offset `to_eye` (fixed in the impostor, left in `foliage_cutout`) and this.

- **The AO constants were set against nothing.** A voxel crown writes a baked AO of 1.0
  because it has real geometry for screen-space GTAO to occlude; a two-triangle impostor
  gives GTAO nothing, so its baked AO stands IN for that. The first values (1.6 with a
  0.25 floor) instead added occlusion on top, so a mid-crown pixel sat at 0.25 against the
  voxel level's 1.0 -- a 4x step at the LOD switch, read as "the impostor is darker".
  Now 0.5 with a 0.5 floor, i.e. approximately what GTAO already gives the level it
  replaces. The reference point is in the comment so the next retune has one.

### Still open

- **No render-path test.** The GPU tests read the atlas back; none renders the impostor
  material. Three of the four correctness bugs the review found lived in the shader and
  were invisible to them. A full-frame assertion — writes `kShadingModelFoliage`, shading
  stable as the camera orbits, shadow not degenerate — is the highest-value thing left.
- The ~1.7 s bake at load.
- Removing L3 from the field chain.

## Step 3 — `impostor_baker` + `impostor_bake.wesl`

Renders each model's bark + leaf cards from 16 ortho views into the two arrays. Ortho
extent comes from the model's existing measured bounds, so the tree exactly fills its tile
at every view. Runs in `BuildForestModels`, after the CPU meshes and before placement —
**"along the voxels", though not in the same loop**: voxelization is CPU and parallel while
the bake needs the device, so the bake is a serial GPU pass after the parallel CPU one.

Verification is a readback, not a screenshot:
- Every tile has non-zero coverage (a silhouette that bakes empty is the pine dead-zone
  failure again, in a new place).
- Coverage is plausible per view — within a band, not a fixed number.
- Baked normals are unit length after decode.
- Determinism: same models -> byte-identical atlas.

## Step 4 — `foliage_impostor.wesl` + the LOD4 slot

The material: camera-facing quad in the vertex stage (light-facing in the shadow variant),
octahedral 3-cell blend, alpha cutout, `packVoxelFoliageGBuffer`. Quad extent from the
model's bounds via the existing per-bucket `MaterialParams`. LOD count 4 -> 5; L4 has no
bark submesh at all (the bake includes it), which is legal — `Draw` skips zero-index slots.

`badlands_tree_field_gpu_tests` gains an L4 case: a field at L4 distance draws, writes
`kShadingModelFoliage`, and casts a shadow whose extent is not degenerate.

## Step 5 — Leaf cards to deferred alpha-test

`foliage_cutout.wesl`: albedo texture + alpha test + the foliage shading model, reusing
Step 4's cutout plumbing for both the gbuffer and shadow variants. Switches the model
viewer's "Original" path off `AddForwardOpaqueMeshEntity`.

The Castano coverage-preserving mips already exist; this is what makes them matter.

## Step 6 — Threshold rebalance (tuning, with screenshots)

**The chain's thresholds may be the real lever, not the new level.** Thresholds scale with
tree height, so a 27 m pine only reaches L3 at 236 m and would reach a naively-placed L4
past 400 m — beyond the 128 m test map entirely. LOD4 does no work at all unless the
chain is rebalanced, and how far to pull it in is a visual call, so this step ends with
screenshots at several distances for you rather than an assertion.

## Verification

```sh
scripts/build.sh && scripts/test.sh
scripts/test.sh badlands_tree_tests              # octahedral + atlas layout
scripts/test.sh badlands_tree_field_gpu_tests    # the L4 slot end to end
scripts/screenshot.sh badlands_mapview /tmp/l4_far.png  --test-map --camera-height 300
scripts/screenshot.sh badlands_viewer  /tmp/l4_tree.png --generator 5 --lod 5
```

## Risks

- **Octahedral costs ~3x a yaw ring, and is paying for a camera that does not exist yet.**
  Accepted deliberately: camera rotation is planned, and this is content-and-material
  shaped, i.e. expensive to redo later. Not a risk to mitigate, a price to remember when
  reading the bake times.
- **Cutout ghosting from the 3-cell blend.** Mitigated by distance, retreatable to nearest-
  cell via one constant.
- **Tile bleed under mips** — the failure that hides until it is far away. Step 2 asserts
  the layout; the sampler policy is the part a test cannot fully cover.
- **The shadow fragment stage** is the plan's one hard external dependency. Step 0 settles
  it before Step 4 depends on it.
- **A quad cannot self-shadow or intersect terrain plausibly.** At LOD4 range trees are
  small enough that this is invisible, but it puts a floor under how near L4 can be pulled
  in Step 6 — which interacts directly with the previous risk about thresholds.
