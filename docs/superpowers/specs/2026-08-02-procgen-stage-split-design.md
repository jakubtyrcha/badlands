# Procgen stage split — design

Splits map generation into three stages with one frozen interface between them,
so the geological simulation, the coarse→detailed extraction, and the rendering
sink can be worked on in isolation. The immediate goal is a fast styling loop —
materials, foliage, water, relief detail — on a small patch, without the
5-minute world simulation in the way.

## What is already settled

Decided in the design conversation, with the measurements that decided it.

| | value | why |
|---|---|---|
| stage names | `coarse-hydraulic-erosion-sim` → `detailed-patch-extraction` → `map-detailing` | stage 2 is not a sim; the name must not claim it is |
| the one new interface | `PatchRequest` → `PatchData` | stage 3 is a sink, not an outward interface |
| river graph extraction | moves to **stage 1** | fixes a measured defect, see §5 |
| relief detail method | erosion **filter**, not simulation | "less about sim, more about control" |
| first relief implementation | bicubic B-spline upsample | Catmull-Clark on a regular quad grid *is* bicubic B-spline |
| sub-texel geometry | stays in **stage 3** | a 0.34 m cavity is not representable on a 1 m lattice |
| patch size / resolution | **fully configurable, independent** | 128² is an iteration size; the game uses 2048² |
| coarse density | **read from the data** | must never be hardcoded, in any consumer |
| where stage 2 lives | `src/mapgen/`, namespace `badlands::mapgen` | no new module; the layering is enforced by CMake targets instead (§3.6) |
| the water block | carried in `PatchData`, not derived per consumer | the contract says a patch HAS water; how a provider gets it is its own business (§3.1) |
| river culling | **flow** in stage 1, **frame + length** in stage 2 | length relative to a frame is meaningless without a frame (§2.2) |
| biome cutoffs | two floats in the coarse manifest | per-patch quantiles make the same ground classify differently (§2.3) |
| patch coordinates | **patch-local**, `origin_m` echoed as provenance | every stage-3 consumer assumes a zero-based lattice; 16 km offsets cost float precision |

Three findings that shaped this and should not be re-litigated:

- **The coarse sim's job is structural variety, not fidelity.** It exists so that
  a patch containing a lake, a river or a valley *exists to be picked*. Data
  outside the eventual region of interest is scaffolding. Fidelity inside the
  patch is stage 2's problem.
- **The erosion filter is a function, not a stage.** It is non-iterative,
  evaluates each point independently, needs only a height plus its gradient,
  chunks trivially, and returns analytical derivatives. That is the same shape
  as `RiverCarve::HeightAt`, which the cluster DAG already consumes through
  `TerrainDetailField`.
- **Gullies and channels are different processes at different scales.** The flow
  graph is solved on the coarse terrain, so gullies the filter invents are not
  in the drainage solution. That is correct — trunk channels come from the sim,
  hillslope dissection comes from the filter — but the filter must never be
  allowed to dam a channel.

## 1. The three stages

```
STAGE 1  coarse-hydraulic-erosion-sim         tools/protogen/
         CACHED ON DISK. The expensive artifact (~5.2 min, §5).
         Owns: relief, soil, standing water + lake levels,
               and the river graph.

              │  self-describing coarse artifact
              ▼

STAGE 2  detailed-patch-extraction            the one frozen interface
         PatchRequest  in   (origin, world_size_m, resolution)
         PatchData     out  (height, level, biome, soil, rivers, …)
         Implementation is free: stateful, cached, streaming, lazy.

              │  PatchData
              ▼

STAGE 3  map-detailing                        mapview + game, EXISTING CODE
         A SINK. Populates the game world: cluster-LOD terrain DAG,
         biome splat + terrain materials, lake surfaces, river
         corridor carve + channel water, foliage.
         (navmesh: out of scope for this design)
```

## 2. Stage 1 — coarse-hydraulic-erosion-sim

Conceptually fixed by this design. Its internals — the particle pass, the lake
water balance, the substrate model — are explicitly **not** in scope; they are
revisited separately once the stages are decoupled.

Two changes are required of it, both at its output boundary.

### 2.1 The coarse artifact must be self-describing

Today protogen writes only headerless `.f32` rasters. Geometry reaches every
consumer through command-line flags — `window.cpp` takes `--res 1024 --world
16384`, `show.py` takes them as argv — so **the 16 m cell size exists nowhere in
the data**. It is folded into every command line by hand, and a wrong flag
silently reinterprets the bytes.

A manifest alongside the rasters carries resolution, world size, the derived
texel size, and the sim's own params and seed. This is the rule the patch format
already follows: the manifest is what makes headerless rasters safe, because
element counts are checked against it and a mismatch is an error rather than a
guess.

**No consumer may hardcode the coarse density, and no consumer may take it as a
flag.** It is read from the artifact.

### 2.2 River graph extraction moves here

The network is extracted where the catchment exists — on the whole coarse world
— and travels with the artifact. Stage 2 **clips** it; it never re-derives it.

This is a defect fix, not a preference. See §5.

**Two culls, in two stages, and the split is not arbitrary.** Stage 1 culls by
**flow** — a physical, patch-independent threshold that means the same thing
everywhere. It cannot cull by length, because "is this branch too short to
bother with" is a question about a frame, and stage 1 has no frame. Stage 2
therefore clips to the patch and *then* culls by length, so stage 3 receives
only channel worth drawing.

**That ordering exposed a latent bug, and closing it is part of this design.**
Clipping minted an inbound frame crossing as `RiverNodeKind::Source`; both prune
passes re-derive `Source` from `in_deg == 0`; and the length cull removes only
headwater chains. So a trunk that merely *enters* a patch read as a headwater
and was deleted whenever its in-frame stretch fell below the threshold — the
same failure the prune already documented one level down (a 700 m trunk eaten
fragment by fragment, peak Q 0.7183 → 0.0218 m³/s).

The fix is a new node kind, **`RiverNodeKind::FrameEntry`** — the inbound twin
of the existing `Mouth` — minted by the clip and exempted by the culls. Two
things are worth recording because neither was obvious:

- The exemption lists alone are **not** sufficient. They govern how surviving
  nodes are re-labelled; the cull's own trigger is purely topological, so it
  also needs an explicit `FrameEntry` guard or the fix is cosmetic.
- The degenerate branch matters more than the interpolated one. The crossing
  solver rejects `t >= 1`, so a sample landing *exactly* on the frame falls
  through to a fallback path — which is precisely what a lattice-aligned channel
  does routinely. That fallback was the likeliest route to the bug, not the
  least likely.

### 2.3 Biome cutoffs are whole-world, and live in the manifest

Biomes are cut on **quantiles of the soil distribution**, and the old tool took
those quantiles *over the window it was cutting*. That makes the same ground
classify differently depending on what you cut — which fails the density- and
resolution-independence guarantees in §8 outright, not marginally.

Stage 1 therefore computes the quantiles once over the whole world and records
the two **threshold values** in its manifest. Stage 2 applies them per patch.
Two floats, not a raster: classification stays re-tunable in stage 2 without
re-running or re-post-processing the world, and the same ground classifies the
same way regardless of the cut.

## 3. Stage 2 — detailed-patch-extraction

The one frozen interface in this design. It is a boundary, **not an algorithm**:
what crosses it is specified, how it is produced is not.

### 3.1 The contract

```
PatchRequest
  origin_m          corner of the region, in the coarse world's frame
  world_size_m      extent of the region
  resolution        texels per side
                    ( texel_m = world_size_m / resolution, derived )

PatchData
  texel_m
  origin_m          echoed for provenance only; the lattice is zero-based
  height            f32 metres — the BED, never the water surface
  level             f32 metres — lake surface elevation, flat per lake
  water_depth       f32 metres
  lake_id           i32, index into `lakes`, -1 where dry
  lakes             per-lake records
  biome             u8, mapgen::Biome
  soil              f32 metres of erodible cover
  rivers            RiverGraph, clipped + culled, PATCH-LOCAL metres
  elevation_range   min/max metres
```

- **The water block is carried, not derived by each consumer.** An earlier draft
  had `PatchData` hold `level` alone, with a free function producing depth /
  lake ids / lake records — on the grounds that a provider which *cannot* write
  a derived field cannot write it inconsistently. That was reasoning from what
  today's providers happen to do. The contract says a patch HAS water; whether a
  given provider derives it or authors it outright is that provider's business,
  and a future one may well have better information than a re-derivation can
  recover.
- **Coordinates are patch-local**: texel (0, 0) at world (0, 0), spanning
  `[0, world_size_m]`. Forced from two directions — every stage-3 consumer
  already assumes a zero-based lattice, and absolute coordinates at a 16 km
  offset spend float precision on an offset the patch cannot use. `origin_m`
  rides along as provenance and nothing transforms by it.

- **`depth = max(0, level − height)`**, with a dry texel storing `level ==
  height`. No sentinel, no mask, and a lake surface is exactly flat by
  construction because the level is the per-lake constant.
- **The bed keeps the den.** `height` is the lake bed, so a basin survives any
  resample.
- **`soil` is required, not optional.** It is the erosion filter's fade target.
  The reference technique fades on altitude and admits that is not robust; soil
  depth is the physically honest signal — thin soil means bare rock means little
  gullying. Stage 1 already measures a 4.4× slope separation between bare/thin
  soil (53.6% of the map, mean slope 31.6°) and deep soil (29.1%, 7.1°), so this
  is a measurement we have rather than an invented 0..1 scalar.
- **`rivers` carries the graph, not the arc chains.** Arc chains are a
  deterministic function of `(graph, tolerance)`; carrying both creates two
  truths that can drift. Curvature is still available to every consumer —
  `RiverArc` stores it signed and exact (0 means straight) — via one
  `build_river_arcs` call, measured at 10 ms.
- **`elevation_range` is load-bearing, not diagnostic.** Stage 3 needs it to
  frame a camera. See §5.

### 3.2 Providers

The interface admits several implementations, and stage 3 cannot tell them
apart. That indistinguishability is the test that the boundary is right — the
same trick `--test-map` already plays for the forest.

| provider | what it does | why it exists |
|---|---|---|
| `CoarseWorldPatchSource` | reads the cached coarse artifact, resamples, applies the relief chain, clips the graph | the real pipeline |
| `FilePatchSource` | reads a serialized `PatchData` off disk | regression fixtures; pins terrain while materials change |
| `SyntheticPatchSource` | analytic terrain with a declared river and lake | **styling with no upstream cost at all** |

`SyntheticPatchSource` carries real weight. It removes the 5-minute simulation
*and* the patch-selection problem from the styling loop: you declare "valley
with a 2 m river and a lake" and get it, deterministically. Materials, foliage
and water can therefore be worked on before any of stage 1's open questions are
settled.

### 3.3 Selection is not extraction

Finding an interesting region and extracting a patch from it are separate jobs,
and only the second one is this interface.

- **Selection** — the four gameplay gates (lake, river, mountains, plains) that
  scan a coarse world and rank candidate regions. It stays a **discovery tool**,
  run occasionally, printing an origin to pin. It never runs in the styling loop.
- **Extraction** — `PatchRequest → PatchData`, given an origin someone already
  chose.

`tools/protogen/window.cpp` currently does both in one standalone, stdlib-only
TU that is not in the build. This design splits it: extraction moves behind the
interface and into the build; selection stays a tool.

### 3.4 The relief chain

Inside `CoarseWorldPatchSource`, producing `height` at the requested density:

1. **Resample** the coarse bed to `texel_m`.
2. **Erosion filter** — a stateless per-point function adding gullies and ridges
   aligned to the local gradient, faded by `soil`, and **masked to dry land** so
   it cannot gully a lake floor.

**Step 2 is deliberately not in the first implementation.** Ship step 1 alone,
confirm the stages are decoupled and the loop is fast, then add the filter as a
second op in the same chain. The chain is ordered so that adding it changes no
interface — it is a height in, height out stage behind the same `PatchData`.

The first implementation of step 1 is a **bicubic B-spline**. On a regular quad
lattice with no extraordinary vertices — which a heightfield is — Catmull-Clark
subdivision *is* uniform bicubic B-spline subdivision, and its limit surface is
the bicubic B-spline surface. `window.cpp` already ships `--kernel bspline`
(all-positive weights, zero ringing, at the cost of smoothing) and
`--kernel-compare` to print the trade, so this is testable before it is written.

The channel carve is **not** in this chain. It is sub-texel and belongs to stage
3 (§4).

### 3.5 Resampling rules

- **Both directions.** Today the resample only ever upsamples. Once coarse
  density is a variable, a request can land at ratio 1 (pass through) or below 1
  (downsample), and a downsample wants area-averaging — a reconstruction filter
  run backwards aliases.
- **Water is rebuilt, never resampled.** Depth has a hard shoreline step and a
  ringing kernel on it goes negative (measured with Lanczos-3: −2.01 m). The bed
  is resampled, each lake's surface elevation is carried as the constant it
  physically is, and depth is re-derived.
- **No integral-cell constraint.** The current rule — the patch must be a whole
  number of coarse cells — exists because one shared tap table needs a periodic
  source/output relationship. That is an implementation artifact and becomes a
  trap the moment coarse density is free (a 12.5 m cell makes a 128 m patch
  non-integral and gets rejected for no real reason). **The region is specified
  in world metres; aligning to the coarse lattice is the provider's problem, not
  the caller's.** Per-texel taps cost nothing.
- The honest half of the old rule survives: geometry is never silently
  reinterpreted, and a contradiction is an error.

### 3.6 The layering is enforced by CMake, not by convention

Stage 2 lives in `src/mapgen/` under the existing namespace — no new module, no
second namespace. That directory is not a stage; it is the shared vocabulary
(`Field2D`, `Biome`, `LakeInfo`, the river algebra) that all three stages sit
on, and every type the contract needs already lives there.

The cost of that choice is that nothing in the *tree* records which file belongs
to which stage. So the boundary is drawn in the build instead:

```
badlands_mapgen_lib        vocabulary + river algebra + artifact I/O   links nothing
badlands_patch_lib         patch_data, patch_source  — THE CONTRACT    -> mapgen_lib
badlands_patch_providers   the three providers + patch I/O             -> patch_lib
badlands_mapview_view      map_view_view.cpp + stage-3 render code     -> patch_lib
badlands_mapview           main_mapview.cpp only          -> mapview_view + providers
```

**`map_view_view.cpp` compiles into a library that does not link the
providers.** If stage 3 ever reaches around the interface to a concrete source,
it fails to link. Only the app's `main` — the one named selection boundary, the
same shape `main_ai_sandbox.cpp` uses to pick a `SandboxMode` — sees them.

That `badlands_mapgen_lib` links **nothing** is load-bearing too: four test
targets link it precisely because it drags in no Dawn, and they previously
hand-compiled its TUs to avoid exactly that.

## 4. Stage 3 — map-detailing

A **sink**. It takes `PatchData` and populates the game world; it exposes no
further interface. This is existing code in `src/mapview/`, `src/game/` and
`src/executables/mapview/`, and this design does not redesign it.

It owns everything sub-texel, because that is what cannot be baked onto the
patch lattice:

- **The river corridor and carve.** The median cavity is 0.34 m deep and 0.52 m
  wide — sub-texel on a 1 m grid — so it stays an analytic field sampled by the
  DAG's `TerrainDetailField` hook at 0.125 m. It cannot be baked at production
  size: a 2048 m patch at 0.125 m is 16384² = 1.07 GB.
- **The carve reads the pre-filter base surface for its centreline.** Its bed is
  a running downstream minimum, which is what guarantees a channel cannot run
  uphill; letting it read invented gullies would let that guarantee wander. The
  cavity is subtracted from the composed surface, so the carve still wins
  locally and the filter can never dam a channel.
- Water surfaces, terrain materials and the biome splat, foliage placement.

Sub-metre relief on hillsides is **not** a separate mode. It is `resolution`
raised against the same `world_size_m`.

## 5. What this fixes, measured

**Rivers vanish below ~1 km patches.** Same world, same origin, only the cut
size varies:

| patch | reaches | channel | corridor texels |
|---|---|---|---|
| 2048 m | 146 | 4965 m | 17 783 |
| 1024 m | 22 | 714 m | 2 334 |
| 512 m | 4 (zero-discharge lake connectors) | 0 m | **0** |
| 256 m | 0 | 0 m | **0** |
| 128 m | 0 | 0 m | **0** |

Reproduced at a second origin. The graph is extracted *from the window*, and a
128 m window has no catchment: 0.092 m³/s arrives as boundary inflow against
0.0005 m³/s of local rain, a 180:1 ratio, while the prune constants
(`kMinRiverBranchM = 32 m`, `min_channel_area_m2 = 1500 m²`) are sized for a
1–2 km cut. Extracting in stage 1 and clipping in stage 2 makes rivers appear at
any patch size by construction. **A 128 m styling patch is impossible without
this move.**

**A small patch cannot be framed by the camera.** `map_view_view.cpp` pins
`focus.y = 0` with a fixed 50° pitch, so the view ray crosses the terrain
elevation band at a constant `cot(50°) × elevation ≈ 0.84 × elevation` from the
patch centre, *independent of `--camera-height`*. For terrain at ~440 m that is
371 m, so any patch narrower than ~740 m is never intersected. Measured: 128 m,
256 m and 512 m render pure black at every camera height tried (42, 200, 460,
480, 520); 1024 m and 2048 m render fine. `elevation_range` in `PatchData` is
what lets stage 3 focus on the terrain instead of on y = 0.

**Patch selection and patch size stop being the same thing.** The four candidate
gates are unsatisfiable at 128 m — 0 of 65 025 windows pass, because a lake of
≥16 cells cannot exist in an 8×8-cell window. Structure is found at 1–2 km with
the existing gates; the region of interest is then cut inside it at whatever
size is wanted.

**Iteration cost is almost entirely stage 1.** Measured on this machine:

| step | time |
|---|---|
| coarse sim, 3000 steps @ 1024²/16 km | **~5.2 min** (particle pass 91%, serial) |
| patch cut | 0.05 s |
| C++ edit → rebuild `badlands_mapview` | ~3 s |
| load + headless screenshot | 128 m 0.32 s · 512 m 0.84 s · 1024 m 2.6 s · 2048 m 10.6 s |

Caching the coarse artifact and generating patches on demand removes the only
slow step from every loop that is not a stage-1 change. `SyntheticPatchSource`
removes it from styling loops entirely.

## 6. Rules that hold across every boundary

- **Every parameter is in world metres, never texels.** This is already the
  repo's rule — noise is sampled in world metres so the same seed and size at two
  resolutions is the same map, only sharper — and it is what makes 128² → 256² a
  pure config change. The erosion filter's octave scales, the arc-fit tolerance
  and the biome blur all have to obey it, or the resolution knob silently
  changes the terrain.
- **`world_size_m` and `resolution` are independent.** Neither implies the
  other, and neither is a constant anywhere.
- **Coarse density is read from the data**, in every consumer, always.
- **FFI-style honesty at the seam:** a mismatch between a manifest and its
  rasters is an error, never a guess.

## 7. Known limits

- **`resolution` is free up to about 2048².** A 2048² patch already builds an
  11.9M-vertex DAG at 379 MB and needed `GpuContext` to request the adapter's
  real `maxBufferSize` over WebGPU's 256 MiB default; 4096² is roughly 4× that
  and becomes a GPU-memory conversation.
- **`badlands_mapview --screenshot` is mildly non-deterministic**, and was
  before this work: three runs of one binary gave 54 differing pixels out of
  1 474 560 (0.0037%), max channel delta 2/255. Screenshot comparison must use a
  tolerance, never a hash.
- **Lake bathymetry is not trustworthy** — depths reach 250 m in a 2 km box on
  some windows. A stage-1 concern, explicitly out of scope here.
- Navmesh population from `PatchData` is out of scope.

## 8. Testing

**No test runs the coarse simulation.** Not once, not a short one. Stage 1's
physics is covered separately and already is, by protogen's own in-binary suite
(27 assertions on 32–64 cell grids, sub-second, outside ctest). Everything here
builds a **synthetic coarse artifact** — analytic bed, hand-written manifest,
hand-built graph — into a temp directory. Coarse fixtures at 32²/64², patches at
64²/128². Nothing needs to be big to be correct.

The two independence tests below are the ones that catch a hardcoded density,
and **neither means anything unless `world_size_m` is held FIXED** across the
comparison. Vary exactly one thing.

- **Provider indistinguishability.** Stage 3 renders a `SyntheticPatchSource`
  patch and a `FilePatchSource` patch through the identical path. If it needs to
  know which it got, the interface is wrong.
- **Resolution independence.** The same `(origin, world_size_m)` at two
  resolutions produces the same terrain, sampled more finely — checked by
  comparing the coarser output against a downsample of the finer one, within the
  resample's own error.
- **Density independence.** A coarse artifact resampled to the same target
  density from two different coarse resolutions agrees within the resample's
  error. This is the test that catches a hardcoded 16.
- **Round-trip.** `PatchData → serialize → deserialize` is exact, and a manifest
  that contradicts its rasters is rejected rather than guessed.
- **Water invariants survive resampling in both directions.** Depth never goes
  negative, a lake surface stays exactly flat, a basin stays a basin.
- **Rivers at every patch size.** A patch containing a channel yields a non-empty
  clipped graph at 128 m, 512 m and 2048 m — the direct regression pin for §5.
