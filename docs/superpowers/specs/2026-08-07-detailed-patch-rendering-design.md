# Detailed patch rendering: loading real LiDAR into mapview

Date: 2026-08-07

## Why

Procgen stages 1 and 2 are set aside. Stage 2's detail output is unsatisfactory, and an ML
upscaler is being built to replace it (`/Users/jakub/repos/terrain-net`). Meanwhile the
real-time rendering path has nothing good to render against — a 2 km cut of the 16 m coarse
world carries no detail below 16 m by construction, so every rendering-quality question is
being asked of terrain that cannot answer it.

terrain-net's Phase 0 (`terrain-fetch`) already pulls real bare-earth LiDAR at 1 m for named
British and European areas, plus co-registered ESA WorldCover land cover. Its heightfields are
the shape the ML model is expected to produce. Rendering them now unblocks rendering-quality
work without waiting for either the sim or the model.

**This spec covers the data contract and the loader.** Foliage placement and water rendering
are the named follow-ups.

## The reframe

Elevation-derived classification is dead. "Hills" and "Mountain" are recoverable from the
heightmap, so a class raster that carries them carries nothing. What the renderer actually
needs to know is:

- where the water is (lakes, rivers, sea)
- where the marshland is
- what vegetation to place, and where (soil and water availability)
- what rock or ground material to paint

And what it can expect to be given, in production:

- a detailed heightmap (sim coarse + ML)
- a water map (sim)
- a soil level (sim)
- morphology detail baked into the heightmap by the ML — but the *class* is still wanted
  separately, to choose materials and rock props
- a wetland classification, and forest types, to couple with the foliage generator

## Decision 1 — Fields plus one region label

A patch carries **continuous physical fields** that drive *where* things go, and **one
low-frequency categorical label** that picks *which assets* are used. Vegetation cover is a
third, separate raster.

```cpp
struct PatchData {
  // physical, per texel -- WHERE
  Field2D<float>   height;   // m, the bed
  Field2D<float>   level;    // m, water surface
  Field2D<float>   soil;     // m of erodible cover
  RiverGraph       rivers;

  // biological, per texel -- WHAT grows
  Field2D<uint8_t> cover;    // mapgen::Cover

  // geological, per PATCH -- WHICH assets
  TerrainClass     terrain_class;
};
```

The split is not cosmetic. Ground material derived from slope, curvature and soil resolves at
the heightmap's own 1 m, whereas a categorical raster pins every material edge to WorldCover's
10 m staircase. It also frees `Cover` from the shader's 8-slot budget, because `Cover` no
longer indexes those slots.

`terrain_class` is per patch today because that is what terrain-net emits — one label per area
in `manifest.json`. It becomes a raster when `detail_class.tif` ships.

## Decision 2 — Lakes yes, rivers deferred, soil approximated

- **Standing water** is derived: `route_flow`'s priority-flood gives `in_lake` and
  `water_level`; a connected component survives only if it overlaps `Cover::Water`.
  The veto is load-bearing. Real 1 m data has drystone walls and field banks that are genuine
  relief and genuinely dam water, so unvetoed flood-fill ponds where no lake exists.
- **Rivers are not derived.** `accumulate_drainage` and `extract_river_graph` are not called.
  A 1024 m window has no catchment worth extracting from — measured previously at 0 reaches
  below a 512 m cut — so this is a patch-size problem, not a code problem.
- **Soil is approximated**, from slope, calibrated to the sim's own two measured points
  (deep soil ≈ 4 m at 7.1° mean slope, bare/thin < 0.5 m at 31.6°). Explicitly `TEMPORARY`:
  the sim's raster replaces it wholesale, and when it does it replaces *slope* as an input to
  the same consumer rather than changing that consumer.

## Decision 3 — Cover comes from raw land cover, not the material raster

terrain-net emits both `material.r8` and (as of this work) `landcover.r8`. The material raster
is strictly lossier: it collapses shrub, grass, crop and moss into one class, and it bakes in
a `slope > 45° → rock` rule at the source resolution. Both are right for shading and wrong for
placing vegetation. We carry land cover and apply our own slope rule at 1 m.

`mapgen::Cover` is badlands' own vocabulary, not ESA's — the provider translates. Production
will not use WorldCover at all, and a contract that embedded ESA's numbering would have to be
renegotiated the moment it arrives.

## Decision 4 — Gameplay map data is separate from visual map data

`mapgen::Biome` is a **gameplay** vocabulary, not a rendering one:

```
game/src/movement.h:34    bool is_walkable(mapgen::Biome biome);
game/src/nav_world.h:33   float biome_move_cost(mapgen::Biome biome);
game/src/heroes.h:37      mapgen::Biome biome_at(const BadlandsGame&, glm::vec2);
badlands_sim.hpp:1271     int32_t BiomeAt(float, float) const;   // frozen across the C ABI
```

133 references across 50 files, reaching movement cost, navigation, critter habitat, exploration
and animal spawning. It is not touched.

The two vocabularies would otherwise collide in `MapData`, whose one-hot slices are indexed *by
the class value* — so `slice(k)` would mean `Biome` in `badlands_game` and `Cover` in
`badlands_mapview`. Instead:

- **`VisualMapData`** — height, cover slices, terrain class. The render path's only view.
- **`MapData`** — stays the gameplay type (renamed `GameplayMapData` when `badlands_game` is
  next touched, not in this work).
- **`TerrainLattice`** — the cluster DAG builder stops taking a whole map. It needs only nodes,
  spacing, a height accessor and one class byte per node (vertex colour plus the `biome_id`
  debug meta), so both map types can feed it and nothing downstream needs a vocabulary.

## Decision 5 — Nodata is a decoupled prepass

```cpp
Field2D<uint8_t> fill_nodata(Field2D<float>& field, float sentinel);
```

Multi-source BFS outward from the valid/invalid frontier, replacing each sentinel with the
nearest valid sample and returning a mask of what it touched. It knows nothing about patches,
cover or terrain-net; the **caller** decides that filled texels become `Cover::Unknown`. That
is what "decoupled" buys: the simplest fill that terminates, replaceable without touching a
consumer.

## Decision 6 — The ground-material palette stays open

`shaders/common/terrain_layers.wesl` is not touched. Its 8 weight slots, RGBA8x2 planes and
world-XZ sampling are LOD-independent and correct. Only the CPU function that fills them
changes, from one-hot-by-biome to weights-derived-from-fields:

```cpp
BuildGroundSplat(const VisualMapData&) -> { weights, pack list }
```

The slot meanings (bare rock, scree, stony ground, turf, heath, peat, silt, forest floor) and
the pack palette are **provisional**, pending work on alternating brushes for material
variation. This is a named seam, not a frozen decision — nothing about it enters the contract.

## The data inventory

What the renderer needs, where it comes from in production, and what stands in today.

| channel | production source | today | quality |
|---|---|---|---|
| detailed height, 1 m | sim coarse + ML | `height.r32`, real LiDAR | excellent |
| coarse height, 16 m | sim | `height_16m.tif`, avgpool16 of the above | exact; same code the trainer uses |
| water — lakes | sim | derived: priority-flood, vetoed by observed water | good enough; the veto is what makes it so |
| water — rivers, sea | sim | nothing | deferred |
| **soil depth** | sim | nothing | approximated from slope; `TEMPORARY` |
| wetness / marsh | sim wetness + peat maps | `Cover::Wetland` (WorldCover class 90) | poor — ~39 % user's accuracy, "weak hint only" per terrain-net |
| vegetation cover | foliage generator | `landcover.r8` | real, but 10 m blocky |
| **forest type** (broadleaf/conifer) | foliage generator | nothing | Copernicus HRL Forest Type is the open 10 m source |
| terrain class | ML conditioning | one label per area in `manifest.json` | a patch scalar, not a raster |
| rock/soil/grass split | derived | derived at 1 m from slope + curvature + soil | replaces `material.r8`'s pre-baked 45° rule |

### Gaps worth closing, in order

1. **Ground material packs.** `assets/materials/` has 37 packs, ~14 rock/gravel/mud, the rest
   bark, brick and plank — and **no grass, heath, peat or snow**. The sample patches are ~90 %
   grass and heath, so vegetation renders as rock until this is fixed. The existing packs are
   Poly Haven CC0 names and all four are available there. Highest value per unit effort, and it
   is asset work, not code.
2. **Forest type**, to make foliage species meaningful rather than uniform.
3. **Soil depth**, which only the sim can supply, and which is the most orthogonal of the
   placement signals — it is what separates a mantled slope from an exposed one at a fixed
   gradient.
4. **Per-texel detail class**, planned in terrain-net.

## Out of scope

Foliage placement · water rendering · rivers · the brush and palette work · migrating
`badlands_game` to `VisualMapData` · anything in procgen stages 1 and 2.
