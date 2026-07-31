# mapview: terrain materials + standing water

Status: approved design, ready for a plan.
Scope: `badlands_mapview` looks. Rivers and wave animation are explicitly out.

## Problem

`badlands_mapview` renders the generated map as cluster-LOD terrain whose albedo
is a flat per-vertex biome colour, and it renders no water at all — lakes show up
as dry basins wearing the Lake biome's colour. Two gaps:

1. The PBR material packs under `assets/materials/` are unused by the map tool,
   even though `assets/materials/terrain_biomes.json` and `ResolveBiomePacks`
   already map every biome to a pack.
2. The generator produces real standing water (`water_depth`, `lake_id`,
   `LakeInfo::level_m`) that nothing renders.

## Goals

- Terrain shows one PBR material per biome, blended at biome boundaries.
- Lakes render as still, murky water calibrated to the **Iron Water** palette
  (`/Users/jakub/repos/docs/palettes.html`).
- Water reads as a volume: the coast shows the lake bed, depth carries the colour
  into murky blues.
- The water surface extends far enough under the terrain that vertical wave
  displacement, added later, cannot open a gap at the shoreline.

## Non-goals

- Rivers (`river_class` / `river_depth_m` / `river_flow_dir`).
- Wave animation, foam, shoreline wetness on the terrain.
- Per-biome water palettes (the swamp palettes exist; lakes only for now).
- Removing the now-unused vertex colour from the cluster vertex format.

---

## A. Terrain — biome splat + height-blended layer arrays

### A.1 What exists

`shaders/material/terrain_blend.wesl` already blends three `texture_2d_array`s
(albedo / normal / ARM) by per-layer weights, with planar world-XZ UVs, an iq
de-tiling pass, and skip-zero-weight sampling. It is driven by **vertex**
attributes and is used by `badlands_game`.

`shaders/material/terrain_cluster.wesl` (what mapview uses) samples no textures.
Its only biome signal is a flat per-triangle `meta.x` byte.

### A.2 Decision: weights come from a splat texture, not vertex attributes

The cluster DAG decimates vertices across LOD levels. Vertex-carried blend
weights would therefore have to be averaged by the simplifier, and biome-boundary
resolution would follow the LOD cut. A splat texture sampled in the fragment
stage is LOD-independent and changes neither the cluster vertex format nor the
DAG build.

Cost accepted: ~2 MB VRAM at 512² and one extra texture fetch per fragment.

### A.3 Splat construction (CPU, `MapViewView::Initialize`)

1. Expand `MapArtifacts::biome` into 6 weight fields (one-hot).
2. Blur each field with radius `kBiomeBlendM = 3.0` metres (separable box blur).
   Softens the 1 m-texel staircase into a transition band.
3. Keep the **top 2** weights per texel, zero the rest, renormalize to sum 1.
   Bounds the shader's active-layer count.
4. Pack into two RGBA8 textures at the map's output resolution:
   `splat0.rgba` = slots 0–3, `splat1.rgba` = slots 4–7.
   **Slot index == `mapgen::Biome` enum value**, the same convention
   `terrain_biomes.json` and `LoadTerrainArrays` already use. Slots 6–7 stay zero.
5. Generate a full mip chain (reuse the existing GPU mip path). Without mips the
   weights alias badly when the camera pulls back to `max_height`.

Sampled with a **linear, clamp-to-edge** sampler at `uv = worldXZ / world_size_m`,
with a half-texel inset so edge texels do not bleed.

### A.4 Shared blend module

Extract `tile_variation` and the layer-blend loop from `terrain_blend.wesl` into
`shaders/common/terrain_layers.wesl`. `terrain_blend.wesl` (vertex weights) and
`terrain_cluster.wesl` (splat weights) then differ only in where `w0`/`w1` come
from.

`terrain_cluster.wesl` changes:
- Vertex stage gains a `worldOffsetPos` varying (needed for both the material UV
  and the splat UV).
- Fragment stage samples `splat0`/`splat1`, then calls the shared blend.
- Debug tint modes 1 (position hash) and 2 (LOD level) are unchanged. Mode 0 now
  means *textured* rather than *flat biome colour*.
- The per-vertex colour attribute stays in the vertex layout (removing it would
  churn the DAG build and its tests for no runtime win) but no longer feeds
  albedo.

Bilinear filtering across a biome triple-point can produce up to 4 non-zero slots
even though each texel stores 2. The blend loop therefore culls weights below
`kMinLayerWeight = 1.0/255.0`.

### A.5 Height blending

Linear cross-fades between two ground materials read as a wash. Use the standard
height-lerp (Mishkinis, *Advanced Terrain Texture Splatting*) so the materials
interlock — pebbles poke through mud instead of dissolving into it:

```
ma  = max_i(h_i + w_i) - kHeightBlendDepth
b_i = max(h_i + w_i - ma, 0)
w'_i = b_i / sum(b)
```

`kHeightBlendDepth = 0.2` sets the transition width. Compile-time constant, not a
runtime knob.

**Displacement is packed into the ARM array's unused alpha channel**, not carried
in a fourth array. Sampling order becomes: ARM first (yields `h_i`), recompute
weights, then albedo and normal. That is **the same 6 taps per active layer as
today** with zero extra VRAM and zero extra bindings.

Requires `MaterialLibrary::LoadTerrainArrays` to CPU-merge each pack's `disp` red
channel into its `arm` alpha before upload. Every pack already declares
`"displacement"` in its `material.json`; `LoadPack` simply does not read the key
yet.

The de-tiling crossfade weight stays derived from the albedo difference (as
today), so albedo is sampled and cached in the first pass alongside ARM.

### A.6 Consequence for `badlands_game`

`terrain_blend.wesl` is shared, so the game's terrain gains height blending too.
This is intended, and is verified by a screenshot rather than left to drift.

---

## B. Water geometry — per-lake surface with a buried skirt

### B.1 Decision: build from the generator's lakes, not `MapData`

`MapData::water_level_m()` is a single global scalar. The generator produces
**multiple lakes at different elevations** (`LakeInfo::level_m` per lake, indexed
by `MapArtifacts::lake_id`). Building water off a single plane would be wrong for
every lake but one, so the builder reads the artifacts directly.

River channels carry `lake_id == -1` and are therefore excluded automatically,
which matches the "rivers are out of scope" boundary.

### B.2 The skirt

For each lake `L` at level `level_m`, start from its texel mask and grow outward.
A candidate texel is tested in this order:

1. **Reject** if it lies further than `kMaxSkirtM` from the shoreline.
2. **Reject** if it is not already in `L` and its ground height is **below**
   `level_m`. Without this the skirt can cross a thin ridge and hang as a visible
   sheet over a neighbouring lower basin.
3. **Accept**, and stop growing past it, if its ground height is at or above
   `level_m + kBurialM` — this ring is the buried boundary, so it is included
   rather than excluded.
4. Otherwise **accept** and keep growing outward from it.

Constants: `kBurialM = 0.6` m, `kMaxSkirtM = 6` m. Named in metres because both
are physical: the first is how deep the surface must sit under the terrain, the
second is how far the search may run before giving up on a very flat shore.

Where the cap is hit (a marsh-flat shore), the skirt ends shallower than
`kBurialM`. Accepted: flat shores are exactly where future waves are smallest.

### B.3 Why the overlap is free at render time

The water material renders forward-transparent with `depth_write = false` against
a depth-read-only attachment, but is still depth-**tested**. Terrain drawn in
front of the buried skirt therefore rejects those fragments in hardware. No
stencil, no clipping, no cost — and the mechanism is exactly what keeps vertical
wave displacement from opening a shoreline gap later.

### B.4 Mesh

- 1 m quads (two triangles per texel) — already the tessellation vertex-displaced
  waves will need.
- One merged mesh for all lakes. Per-vertex `Y = level_m` of that vertex's lake,
  so lakes at different elevations coexist in a single draw.
- Vertex layout `kTexturedMesh` (pos / uv / normal / tangent), normal `+Y`,
  `uv = world XZ` — the same layout `BuildLakeMesh` in `game_view.cpp` emits.

### B.5 New module

`src/mapview/lake_surface.{hpp,cpp}` — pure CPU (`mapgen` + `glm`), no engine
dependency, so it is unit-testable in isolation. Mirrors how
`src/game/geometry/water_surface.cpp` is structured.

---

## C. Water volumetricity — a Beer–Lambert medium

### C.1 The model

Single-scattering through a homogeneous medium, evaluated along the view ray:

```
d       = linearZ(sceneDepth) - linearZ(waterSurface)   // optical path, metres
T       = exp(-sigma_e * d)                             // per-channel transmittance
L_in    = scatter_albedo * (skySH(+Y) + shadowedSun)    // in-scattered murk
L_under = bg * T + L_in * (1 - T)
rgb     = mix(L_under, skyReflection, fresnel) + sunSpec
```

`bg` is the already-lit lake bed read from the copied scene colour. The Fresnel
mix (split-sum LUT, `F0 = 0.02`), the prefiltered sky reflection, the shadowed
sun GGX lobe and the PCF shadow lookup are unchanged from the current shader.

### C.2 Why this satisfies both requirements

- **The coast shows the bed.** As `d → 0`, `T → 1` and `L_under → bg` exactly.
  Nothing is added at the waterline, so it is seamless by construction.
- **Depth carries colour into murky blues.** `sigma_e` is per-channel. Red
  extinguishes fastest, so the hue slides toward the `L_in` asymptote as depth
  grows, reaching it at roughly `3 / sigma_e` metres.

Two aesthetic scalars are **deleted** rather than replaced: the `absorption`
knob and the `coastWidth` alpha ramp. The transmittance *is* the shore fade, so
the surface can output `alpha = 1` everywhere.

### C.3 Uniforms

`WaterUniforms` replaces `deepColor` / `shallowColor` / `params.x` (absorption) /
`params.z` (coast width) with:

- `extinction: vec4` — `xyz = sigma_e` in **1/m**, `w` reserved.
- `scatterAlbedo: vec4` — `xyz` = single-scattering albedo (unitless 0–1),
  `w` reserved.

The surviving `params` slots are repacked as `x = refractStrength`,
`y = roughness`, `z`/`w` reserved. `params2` is unchanged: `x = detailStrength`,
`y = blockout flag`.

With the coast ramp gone the surface outputs `alpha = 1` everywhere. The forward
pass blends premultiplied, so `src + dst*(1-alpha)` reduces to exactly the
shader's own colour — and `bg` (the pre-pass scene-colour copy) already carries
the bed, so the waterline stays seamless.

### C.4 Calibration is measured, then fitted

`sigma_e` is stated as a **visibility depth per channel** and derived, never
hand-dialled: `sigma = 3 / d_vis`.

Starting point, to be corrected by measurement: red 1.2 m, green 3.5 m, blue
5.0 m, giving `sigma_e ~ (2.5, 0.86, 0.60)` 1/m.

**The first implementation step measures the real lake depth distribution** —
min / median / max of `LakeInfo::max_depth_m` across several seeds, logged from
mapview. This is a genuine risk to the look: the generator applies a
`lake_freeboard_m = 0.4` and prunes ponds below `min_lake_depth_m = 0.5`, so
lakes may well be 0.5–1.5 m deep. At those depths the visibility figures above
leave the water nearly transparent everywhere.

If measurement says the lakes are shallow, the options are, in order of
preference:

1. Shorten the visibility depths to match the actual bathymetry, accepting a
   narrow shallows band.
2. Raise the freeboard or deepen seeded cavities in the generator (out of scope
   here — a separate change, flagged not made).
3. Accept transparent water.

Picking an unphysical `sigma_e` to compensate for shallow lakes is explicitly not
an option.

`scatter_albedo` is then fitted so the deep asymptote **renders** as Iron Water's
deep-water swatch `#2b3841` after exposure and tonemapping — verified by sampling
a headless screenshot, not by asserting linear values in a unit test.

Reference palette (Iron Water, `docs/palettes.html`):
water `#41505a`, deep water `#2b3841`, cold glint `#6d8994`.

### C.5 Still water

A compile-time `still` shader feature on `water.wesl`:

- Vertex stage: no wave displacement.
- Fragment stage: `N = (0, 1, 0)`, no detail normal.
- Everything else (Fresnel sky reflection, sun glint, shadow, the medium) stays.

Refraction becomes a no-op for free, since the offset is driven by `N.xz`. That
also removes any chance of the bed smearing across the waterline.

Follows the existing `blockout` precedent — a separate factory
(`BuildStillWaterForwardFactory`), not a runtime toggle. The wave path stays
intact and compiled for the game.

A low roughness (~0.05) gives the tight specular glint the palette's
`cold glint #6d8994` describes.

---

## D. Interface changes

Engine-interface changes, approved as part of this design:

1. **`WaterUniforms` layout** — `extinction` / `scatterAlbedo` replace
   `deepColor` / `shallowColor` / absorption / coast width.
   `DefaultWaterParams()` and `BlockoutWaterParams()` are remapped to the new
   semantics. `badlands_water_gpu_test` asserts G-buffer albedo comes from
   `shallowColor`; it moves to `scatterAlbedo`.
2. **`water_material.hpp`** gains `BuildStillWaterForwardFactory()` and a
   `StillLakeWaterParams()` preset (Iron Water calibration). Placed alongside the
   existing presets for consistency with `DefaultWaterParams`.
3. **`MaterialLibrary::LoadTerrainArrays`** merges each pack's displacement into
   the ARM array's alpha. `LoadPack` reads the manifest's `"displacement"` key;
   `LoadTexture2D` gains an "alpha channel sourced from a second file" path,
   following the existing CPU-side `flip_green_dx` precedent.
4. **A generic CPU-bytes → mipped RGBA8 2D texture upload helper** for the splat.
   Engine-side and game-agnostic ("upload these bytes", no notion of biomes).
5. **`ClusterTerrain::Build`** takes the terrain arrays and the splat views.
6. **`MapViewView`** gains a `MaterialLibrary` and a small `SceneGraph`
   (`AddTransparentMeshEntity` requires one) for the water entity.

Game-layer changes:

- `src/mapview/biome_splat.{hpp,cpp}` — the CPU splat builder.
- `src/mapview/lake_surface.{hpp,cpp}` — the CPU water-surface builder.

### D.1 Shared-shader blast radius

`badlands_game` shares both `terrain_blend.wesl` (gains height blending) and
`water.wesl` (gains the new optical model). Both changes are intended. The game
is screenshot-verified as part of this work, not left to drift.

---

## E. Testing

CPU unit tests in `src/mapview/tests/`:

- **Splat**: weights sum to 1 per texel; at most 2 non-zero weights per texel;
  slot index equals the `mapgen::Biome` enum value; a single-biome map produces a
  constant splat.
- **Lake surface**: every emitted vertex outside a lake's own mask has ground
  height `>= level_m` (the no-floating-sheet invariant); every vertex's `Y`
  equals its lake's `level_m`; skirt width never exceeds `kMaxSkirtM`; two lakes
  at different levels produce two distinct `Y` values; a map with no lakes
  produces an empty mesh.

Existing suites:

- `src/engine/tests/terrain_blend_tests.cpp` extended to cover the shared blend
  module and the height-lerp weights.
- `badlands_water_gpu_test` updated for the new uniform names.

Visual verification (headless, `scripts/screenshot.sh`):

- `badlands_mapview` at a low camera height over a lake — shore-to-deep gradient.
- `badlands_mapview` pulled back — biome transitions and splat mip behaviour.
- `badlands_game` — regression check for the two shared shaders.

Deep-water colour is verified by sampling the screenshot's deepest water pixel
and comparing against `#2b3841` within a stated tolerance.

---

## F. Open risk

The `sigma_e` calibration depends on lake bathymetry that has not been measured
yet (see C.4). Measurement is the first implementation step, and its result may
force shorter visibility depths than the starting figures. That is a look
adjustment, not a design change.
