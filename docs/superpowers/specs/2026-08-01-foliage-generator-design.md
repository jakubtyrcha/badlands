# Foliage Generator — Forest Plopper on Biome + Heightmap — Design

**Date:** 2026-08-01
**Status:** Approved (design + plan), implementation in progress.

## Goal

The volumetric-foliage work shipped everything needed to *draw* a tree well — a tree
generator, per-model runtime LOD chains, voxel-crown L0..L3, and a GPU-driven instanced
field with frustum cull, distance LOD and shadow casting — but nothing decides *where*
trees go. `BuildTreeField` renders a 16x16 grid of one preset in the model viewer, and
that is its only consumer.

This adds the missing half: a **reusable, game-agnostic foliage generator** that takes a
biome lookup and a heightmap and produces per-cell tree instance lists, plus the render
path that turns those into a drawn forest.

It is a game world component, not a mapview feature. mapview is only the test harness: it
loads a synthetic 128 m map that bypasses procgen, so placement can be judged without
erosion noise in the way.

Outcome: `./build/badlands_mapview --test-map` shows a clumped forest with a ragged edge,
bushes and saplings on the outside, mature canopy inside, all GPU-culled and LOD'd — and
the placement half is unit-tested with no GPU.

## Decisions (from co-design)

1. **Whole-map batch generation**, output bucketed into 32 m cells
   (`kFoliageCellSizeM`, compile-time) so cells can be coarse-culled on the CPU by their
   bounds before the existing GPU-driven per-instance cull runs.
2. **Jittered grid + multi-class rejection** as the sampling core. Layers run in priority
   order; `hash(seed, layer, gx, gz)` drives everything. No global active list, no RNG
   state, deterministic and seam-free.
3. **Clumping via `forest_weight x fBm`.** Uniform blue noise reads as a lawn. The density
   field, not the sampler, is what makes a forest look grown rather than planted.
4. **The maturity gradient is an EDT depth field in metres**, not the biome blend. A forest
   edge is a light/microclimate gradient with a real length scale (~10-30 m in temperate
   forest); the biome blend has no physical length — its width is an artifact of raster
   resolution (`MapData` slices are one-hot per texel) and `kBiomeBlendM = 3.0`. Driving
   edge structure off the blend would give a 1-3 m ring on real maps.
5. **Three layers:** canopy, saplings, bushes. No deadwood, no grass, no ground cover.
6. **~28 models via per-layer species sets**, not a species x tier x variant cross-product.
   Nothing is generated that no layer uses.
7. **Forest type is a C++ struct table**, not JSON. The plopper carries zero per-species
   logic, which is what "data-driven" has to mean here; a parser, a schema and an error
   path for one forest type would be cost without benefit.
8. **Placement and mapview rendering ship together** as one increment.

## Global constraints

- **`src/foliage/` is game-agnostic.** It knows layers, models, coverage and depth. It
  never says "tree", never includes `mapgen::Biome`, and never sees `TreeOptions`.
- **No engine changes.** `InstancedMeshField` already takes `num_models` + per-model
  `ModelLod`, and `SceneContext::instanced_fields` already carries fields to the renderer.
  The only generalization is game-side: `TreeField` from 1 model to N.
- **Pure CPU and unit-testable.** The placement half links no engine, no Dawn, no EnTT.

## Architecture

```
src/foliage/            NEW  pure CPU, game-agnostic
  |- TerrainQuery       abstract input: HeightAt / CoverageAt / WaterLevelM
  |- ForestType         data table: models + layers + depth trapezoids
  |- depth_field        coverage raster -> EDT -> noise-warped depth-into-forest
  \- scatter            the sampler -> FoliageField (32 m cells of FoliageInstance)

src/game/visual/        game content + render adapter
  |- forest_catalog     ForestType for badlands: model i -> TreeCatalog preset + seed
  |- map_data_query     MapData + Biome::Forest -> TerrainQuery
  \- tree_field         generalized to N models

src/game/map/           forest_test_map_generator: 128 m, perlin height, gaussian blobs

src/executables/mapview --test-map: build map -> place -> render, per-cell CPU cull
```

### The decoupling seam

```cpp
struct TerrainQuery {
  virtual float HeightAt(float x, float z) const = 0;    // ground, metres
  virtual float CoverageAt(float x, float z) const = 0;  // this forest's biome weight [0,1]
  virtual float WaterLevelM() const = 0;
};
```

Slope is derived inside the library by central differences on `HeightAt`, so the query
stays three functions. `CoverageAt` is per-forest-type — the game-side adapter binds
`Biome::Forest` to it — which is what keeps `mapgen::Biome` out of the library entirely.

### Data model

```cpp
struct DepthCurve { float rise_start, rise_end, fall_start, fall_end; };  // metres
                                                    // fall_end = FLT_MAX -> plateau
struct FoliageModel {
  float radius_m, height_m;      // spacing footprint; height for cell Y bounds
  glm::vec2 scale_range;
  float weight;                  // selection weight within its layer
  DepthCurve depth;              // species mix shifts with depth
};
struct FoliageLayer {
  float grid_m, radius_m, max_slope_deg;
  DepthCurve density;                    // controls HOW MANY
  float edge_scale, edge_scale_depth_m;  // young-at-the-edge ramp
  uint16_t first_model, model_count;
};
```

Model curves control *which*; the layer curve controls *how many*. Species mix shifting
with depth then falls out of model selection with no separate concept.

### Output

```cpp
inline constexpr float kFoliageCellSizeM = 32.0f;
struct FoliageInstance { glm::vec3 position; float yaw, scale; uint16_t model, layer; };
struct CellYBounds { float min_y, max_y; };
struct FoliageField {
  glm::vec2 origin_m; int cells_x, cells_z;
  std::vector<std::vector<FoliageInstance>> cells;  // row-major, dense
  std::vector<CellYBounds> cell_y;
};
```

Cell XZ bounds are implicit from the cell index, so only min/max Y is stored — which is
also what keeps engine's `Aabb` out of the library. Cell Y comes from each instance's
ground height expanded by `model.height_m * scale`.

## The generator

- **Depth field.** Sample `CoverageAt` onto a 1 m raster, threshold at 0.5, run the exact
  EDT (`mapgen::distance_to_mask`) on the inverted mask -> depth-into-forest in metres,
  0 outside. Then perturb with fBm (amp ~4 m, wavelength ~12 m, clamped >= 0): a raw EDT
  gives a smooth offset curve, i.e. a suspiciously tidy tree line.
- **Density.** Per layer, `layer.density(depth) * clump(p)`, where `clump` is fBm remapped
  through a lo/hi window so glades open up rather than merely thinning.
- **Sampler.** Layers in declared order (canopy -> sapling -> bush), canopy claiming space
  first. Per layer, walk a jittered grid at `grid_m`; one PCG-style integer hash per grid
  cell yields the jittered position, every accept-roll, the model pick, yaw and scale.
- **Accept tests, in cost order:** `coverage > 0` -> density roll -> slope and water
  clearance -> spacing.
- **Spacing** uses a uniform hash grid; reject when an accepted instance is within
  `max(r_i, r_j)`. That is the multi-class rule: bushes pack tightly among themselves but
  still respect a canopy tree's footprint.
- **Scale** is `lerp(scale_range, hash01)` times an edge ramp climbing from `edge_scale` at
  depth 0 to 1.0 at `edge_scale_depth_m`.
- Instances stay **upright** — trees grow vertical, they do not align to the ground normal.
- Iteration is strictly ordered and single-threaded, which is what makes the output
  byte-identical run to run.

## Content

28 models, per-layer species sets:

| Layer   | Models                                      | Target height |
| ------- | ------------------------------------------- | ------------- |
| Canopy  | Oak/Pine/Ash/Aspen **(large)** x 4 seeds = 16 | 18-26 m     |
| Sapling | Oak/Pine/Ash/Aspen **(small)** x 2 seeds = 8  | 4-5 m       |
| Bush    | Bush 1 x 2, Bush 2, Bush 3 = 4                | 1.2-1.6 m   |

A variant is the same preset with a different `TreeOptions::seed`, which changes the
skeleton (`BuildTreeSkeleton` reads it) and therefore the mesh.

Per-model depth curves give the canopy species mix a real gradient, and it happens to be
ecologically right: **aspen is a pioneer**, so it takes the edge; pine takes the interior;
oak and ash are broad.

## Per-model LOD scaling

`kFoliageVoxelWorldSizes` and the Multi-mode LOD thresholds were tuned for an 8 m preview
tree (`kFoliagePreviewHeight`). A 22 m oak using those world-space cell sizes would get
~20x the tets. Both scale by the model's own height ratio:

```
r                = target_height_m / kFoliagePreviewHeight
cell_native[lod] = kFoliageVoxelWorldSizes[lod] * bark_height_native / kFoliagePreviewHeight
threshold[lod]   = kMultiLodThresholds[lod] * r
```

This is the correct generalization because LOD is a screen-space budget: scaling the cell
size with the tree fixes the crown's *relative* resolution, and scaling the threshold fixes
the on-screen tet size. It also leaves `cell_native` **identical** to what the viewer
already passes, so the Phase-6 pine dead-zone retune and `leaf_voxelizer_tests`' sane-band
coverage still hold unchanged.

## Out of scope

- **Real generated maps.** `classify_biomes` emits only Plains/Hills/Mountain, so
  `Biome::Forest` coverage is 0 on a procedural map and the field comes back empty. That is
  the honest behaviour; forests reach real maps when mapgen learns to emit Forest, which is
  its own piece of work. No Plains stand-in.
- Grass and ground cover, deadwood, streaming/on-demand regeneration, and any consumer
  outside mapview (the game still renders a static demo town; terrain is not ported).

## Testing

Testing targets the **underlying methods**, each in isolation: `DepthCurve` evaluation,
the integer hash's determinism/uniformity, the clump remap, the depth field against an
analytic disc, slope derivation on a known ramp, the spacing grid against brute force, and
cell-index round-tripping. End-to-end assertions over a scripted `TerrainQuery` stub then
cover determinism, spacing, depth ordering (bush < sapling < canopy), terrain rejection,
coverage and bucketing.

The visual result — clumped rather than carpeted, ragged tree line, readable edge band — is
a judgement made from headless screenshots, not an assertion.
