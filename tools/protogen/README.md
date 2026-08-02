# protogen — worldgen prototype (throwaway)

Standalone exploration of a replacement for `src/mapgen`'s cone-field relief.
**Not wired into the build.** No CMake target, no tests, not called by anything.

Committed only so it survives; it is expected to be deleted or rewritten.

## What it is

Particle-based hydraulic erosion after Nick McDonald's SimpleHydrology and
["Meandering Rivers in Particle-Based Hydraulic Erosion Simulations"][ref],
plus standing water, a lake water balance, and sediment dispersion in lakes.

[ref]: https://nickmcd.me/2023/12/12/meandering-rivers-in-particle-based-hydraulic-erosion-simulations/

## Build and run

Taskflow is header-only, so this is still a small, hand-listed set of TUs, no
CMake. It now also pulls in the OUTPUT BOUNDARY (world.txt's manifest, the
river extraction pipeline, rivers.bin's binary graph dump) straight from
`src/mapgen/` -- see "Self-describing output" below:

```sh
c++ -O3 -std=c++23 \
  -I src -I third_party/FastNoiseLite -I third_party/glm -I build/_deps/taskflow-src \
  tools/protogen/protogen.cpp src/core/parallel.cpp \
  src/mapgen/hydrology.cpp src/mapgen/river_graph.cpp src/mapgen/river_prune.cpp \
  src/mapgen/coarse_io.cpp src/mapgen/river_io.cpp \
  -o /tmp/protogen

mkdir -p /tmp/pg && /tmp/protogen --res 512 --world 8192 --steps 3000 \
  --drops 1024 --snapshot-every 750 --out /tmp/pg
python3 tools/protogen/show.py /tmp/pg
```

`show.py` writes `-map.png` (hillshade + rivers + lakes) and `-hillshade.png`
per snapshot. `lakes.py` / `lakestats.py` are post-hoc priority-flood analysis.
All four now read resolution and world size from `world.txt` -- see below --
instead of taking them as positional args: `show.py`/`soil.py` take the dump
directory directly, `lakes.py`/`lakestats.py` take one `.f32` file and find
`world.txt` next to it. The old explicit `<n> <world_m>` form still works as
an override (needed for a `window.cpp` output dir, which has no `world.txt`
of its own -- see its "Viewing it in mapview" section below).

## Self-describing output

A dump used to be headerless rasters plus whatever `--res`/`--world` a
consumer happened to pass on argv -- so its 16 m cell size existed nowhere in
the data, and a wrong flag silently reinterpreted the bytes. protogen now
writes two more things next to the `.f32` rasters, once the sim finishes:

- **`world.txt`** -- a `src/mapgen/coarse_io.hpp` manifest: resolution, world
  size (and the texel size derived from them, for readers that would rather
  not divide), seed, runoff, steps, and the whole-world DRY-soil quantiles
  (`soil_cut_mountain_m` / `soil_cut_hills_m`, the same fractions
  `window.cpp`'s `ClassifyBiomes` cuts with) -- so a patch cut later from
  anywhere in the world classifies biomes the same way regardless of what was
  cut.
- **`rivers.bin`** -- a `src/mapgen/river_io.hpp` binary dump of the
  WHOLE-WORLD `RiverGraph`: `route_flow` -> `accumulate_drainage` ->
  `extract_river_graph` -> `prune_river_graph_by_width` ONLY. Not the length
  prune -- a branch's length is only meaningful relative to a frame, and
  there is none yet at whole-world scale; `window_rivers.cpp` applies that one
  per patch, later. Binary because text would be ~35 MB at 16 km.

```sh
# Re-run ONLY the extraction + serialization above against an existing dump,
# skipping the several-minute re-sim -- so an extraction bug costs seconds.
# Calls the exact same code path a normal run does at the end, so the two
# cannot diverge.
/tmp/protogen --extract-rivers /tmp/pg
```

The duplication between `src/mapgen/*`'s `Field2D<T>` and protogen's own flat
`std::vector<float>` fields (see `Grid` in protogen.cpp) is deliberate: a tiny
adapter converts one to the other at the output boundary rather than protogen
adopting `Field2D` internally, so a change to either representation cannot
ripple into the other.

## Findings so far — read before changing anything

- **The erosion law is the reference's and must stay.** Swapping it for stream
  power (`E = K·A^m·S^n`) on the theory that it was "the same law
  re-parameterised" was wrong — one is transport-limited, the other
  detachment-limited — and produced blurry featureless terrain.
- **The particle pass cannot be parallelised by deferring writes.** Particles
  interact through the terrain within a step: the first across a steep cell
  lowers it so the next erodes less. Measured overlap is median 1 visit/cell but
  **max 638**, concentrated in exactly the channels that carve the terrain, so a
  deferred merge over-erodes them catastrophically (heights reached 1e36). The
  reference parallelises only because its writes are racy *and* in place.
  Grid passes (EMA) are true Jacobi updates and are parallel.
- **The cascade must run per particle, in place.** It is an erosional transport
  mechanism, not a smoothing filter, and it is self-limiting only when applied
  in place. As a gentle global pass it cost 44 m of relief and all the fine
  valley structure.
- **Lake deposition deposits the flux DIVERGENCE, not the flux.** `qs` is a
  rate; dumping a fraction of it per step is dimensionally the wrong quantity
  and diverged to 2e5 m of relief.
- **Sediment displaces water.** Deposition must decrement `water`, or the bed
  rises while the cell still reports itself as lake bottom until the next
  rebuild.
- **Lake inflow must be read at the shoreline.** Discharge is not accumulated on
  water cells, so `Qm3s` inside a lake decays to zero under the EMA and every
  lake starves itself out of existence.

- **Lake deposition must be cut off after a few in-lake steps.** Every particle
  steers at the same spill cell, so shedding all the way there put the thickest
  deposit at the OUTLET. 4 steps is right: 2 is too short to deposit near the
  inlet, none reproduces the defect.
- **Rebuild cadence must be in the converged regime.** `lake_interval` sets how
  stale the hypsometry is, so a coarse value keeps a silted-up basin looking
  alive. Intervals 5/10/25 agree (0% wet); 50 and 100 report 2.66%/2.25%. The
  old default of 50 was in the lagging regime.

- **The substrate is a two-layer one: bedrock + soil.** The surface is
  `bedrock + soil`; erosion takes soil at full rate, then bites bedrock at
  `bedrock_erodibility` (0.1, the k_bedrock/k_sediment ratio from
  `src/mapgen/erosion.hpp`). **The transport law is untouched** -- `c_eq` and the
  reference's deposition step are verbatim; the substrate changes only how much
  of the requested cut is YIELDED, and the particle's load is scaled by the same
  factor so bedrock cannot invent mass. Deposition is never resisted and always
  lands on soil.
- **Soil starts as a layer WITHIN the initial surface**, not on top of it
  (`bedrock = height - initial_soil_m`), so step 0 is bit-identical to a
  no-substrate run and the A/B stays honest.

## Verification

`protogen --test` — 23 assertions on 32–64 cell grids at the production 16 m
cell size, under a second. Every bug above has one. Absolute numbers check
against analytic answers: mass conserves to 0.16%, discharge matches
`runoff × area`, lake volume equals basin capacity, lake surfaces level to
0.0000 m.

**The single most productive diagnostic in this work: a real logic change that
produces bit-identical output.** It caught two masked parameters (the lobe-length
clamp hiding `settling_velocity`, the wander cap hiding jet turbulence) and
disproved two wrong diagnoses of the cadence bug. `knob liveness` in the suite
automates it.

## Full-map runs

| | 8 km (512²) | 16 km (1024²) |
|---|---|---|
| steps × drops | 3000 × 1024 | 3000 × 4096 |
| runtime | 51 s | 5 min |
| final relief | 879 m | 808 m |
| lakes at end | **0** | **37** |
| wet at end | 0.00% | 3.20% |
| deepest lake | — | 308 m |

The 16 km column is run **M16b**, taken after the `max_age` sediment-sink fix.
The pre-fix numbers (89 lakes, 2.52% wet, 203 m deepest) are what PR #50's body
cites; the fix merged fewer, larger lakes into one dominant central basin with a
feeding chain from the north.

**Lakes persist at 16 km but not at 8 km** — bigger basins outlast the sediment
supply. Earlier notes calling lakes "transient" were reporting a
scale-dependent result as a general one.

## Phase 2a — gameplay window (`window.cpp`)

Picks a 1 km window out of a finished world and resamples it to the gameplay
grid. Separate standalone TU, stdlib only:

```sh
c++ -O3 -std=c++20 tools/protogen/window.cpp -o /tmp/protogen_window
/tmp/protogen_window --in /tmp/pg --tag 3000-step --res 1024 --world 16384 \
                     --out /tmp/win --kernel-compare
python3 tools/protogen/show.py /tmp/win 2048 1024
/tmp/protogen_window --test      # 17 assertions, instant
```

Geometry is forced, not chosen: 2048 texels x 0.5 m is 1024 m, which is exactly
64 source cells at 16 m. A non-integer cell count is rejected rather than
rounded.

Four gates, all satisfiable together — 449 of 58081 windows pass on M16b, 17
distinct locations after overlap suppression:

| criterion | test | passes alone |
|---|---|---|
| lake | largest connected component >= 16 cells at depth > 0.5 m | 8594 |
| river | >= 40 off-lake cells at Q >= 0.02 m^3/s | 23919 |
| mountains | window relief >= 150 m | 37979 |
| plains | >= 50% of the central 512 m is **dry** and slope < 5 deg | 15922 |

- **Plains must exclude water.** A lake surface is flat, so a bare slope test
  scored lakes as plains and put them at the top of the ranking (97% "plain",
  mostly lake). Excluding wet cells drops the passing set 520 -> 464.
- **"Mountains" is local relief**, since the sim emits no biome classification.
- Candidates are ranked by plain fraction — the scarcest resource in this world
  (map-wide median slope is 10.5 deg) — then overlap-suppressed, since windows
  one stride apart are the same place scored twice.

### The upscale invents nothing

32x from a 64x64 patch: every output texel is interpolated, and there is no
detail below 16 m by construction. This is the LOW-FREQUENCY BASE for a later
detail pass, not finished terrain.

**Catmull-Rom, not Lanczos-3.** Two independent reasons, both measured:

- Lanczos-3 **does not reproduce a linear ramp** — its weights sum to 1 only to
  5.7e-3 and its first moment is off by 6.5e-2, giving a **6 cm** reconstruction
  error. A planar hillside *is* a linear ramp, so that error is periodic with the
  16 m source grid: it is the corduroy ripple visible in an upscaled hillshade.
  Every cubic reproduces linear to machine epsilon.
- Hillshade is a **derivative**, so ringing that looks negligible in height
  (Lanczos overshoots 1.93 m on 160 m of relief) is obvious in the shading — and
  normals are what the renderer consumes.

On a hard 0..100 step Lanczos swings **-26.3 to +126.3**; B-spline stays exactly
in range (all weights positive) and is the `--kernel bspline` escape hatch, at
the cost of smoothing. `--kernel-compare` prints the trade on the chosen window.

### Water is rebuilt, never resampled

Depth has a hard shoreline step, and Lanczos on it went to **-2.01 m**. Instead
the bed is resampled, each lake's surface elevation is carried as the constant it
physically is, and depth is re-derived as `surface - bed`.

- **The bed keeps the den.** `height` is the lake BED, so a basin stays a basin.
- **The waterline is flood-filled at OUTPUT resolution**, so it lands on the
  resampled bed's contour. Measured shoreline wobble on a cone: **0.006 texels**,
  against 8 texels (one source cell) for a nearest-neighbour mask.
- **The flood is bounded to 2 source cells around the sim's footprint.** The
  sim's water field is NOT hydrostatically consistent: it ponds puddles sitting
  above their own surroundings. One measured case — a 19-cell pond perched at
  425.62 m with 2682 connected cells below it — drowned 66% of the window when
  the flood was unbounded. The sim decides a lake's EXTENT; the resample may only
  refine its BOUNDARY.

### Viewing it in mapview

The window tool also writes the load set `src/mapgen/map_io.hpp` reads:

```sh
./build/badlands_mapview --load /tmp/win     # resolution/size come from map.txt
```

Two things had to change on the engine side before a 2048² map would draw, both
worth knowing because neither failed loudly:

- **The lake surface was two triangles per TEXEL.** At 2048² that is 1.38M
  triangles for a surface that is flat. Now merged into per-row runs — identical
  geometry, 1382006 → 4198 triangles.
- **WebGPU's default `maxBufferSize` is 256 MiB regardless of hardware.** An
  11.9M-vertex cluster DAG needs 379 MB, and going over does not fail at the
  allocation: the buffer comes back invalid, poisons the command buffer, and the
  frame renders **black** with only a validation message. `GpuContext` now
  requests the adapter's own ceiling (4 GiB here).

Biomes are cut on ELEVATION quantiles over the dry texels (mapgen's 55%/12%
fractions), wet → `Lake`. Deliberately a placeholder: in an eroded world slope
reads as "mountain" better than elevation, and the honest source is what the sim
produced (slope, discharge, sediment). One function, `ClassifyBiomes`.

## Open

- **Deepest lake reports 308 m** at 16 km (M16b), which is far too deep for that
  footprint. It lands directly on phase 2a: the rank-0 window carries a 72.6 m
  lake in a 1 km box, and `--max-lake-depth` exists only to steer around it. Same signature as un-eroded fBm minima surviving as holes rather
  than landforms; lake bathymetry is not trustworthy until chased down.
- The particle pass is serial. Racy in-place writes would parallelise it the way
  the reference does, at the cost of reproducibility.
- No hillslope diffusion, so divides lower only very slowly.
- **The sim ponds single-cell puddles.** The rank-3 window holds 10 water
  components, 6 of them 1–3 cells. Each becomes a flat margin-bounded patch at
  output resolution, which is what the blocky specks along a valley are. They are
  faithful to the data, not a resample artefact; culling them is a decision about
  the sim's output, so `window.cpp` does not silently filter them.
- **Phase 2b (the detail pass) does not exist.** The 2048^2 is a smooth base
  surface, by design and by construction.
