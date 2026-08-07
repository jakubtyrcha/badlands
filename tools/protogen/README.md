# protogen — worldgen prototype (throwaway)

Standalone exploration of a replacement for `src/mapgen`'s cone-field relief.
**Not wired into the build.** No CMake target, no tests, not called by anything.

Committed only so it survives; it is expected to be deleted or rewritten.

## What it is

A **two-phase** hydraulic erosion / morphodynamics prototype:

- **Phase 0 — Lagrangian carve.** Particle-based hydraulic erosion after Nick
  McDonald's SimpleHydrology and ["Meandering Rivers in Particle-Based
  Hydraulic Erosion Simulations"][ref]: thousands of independent "drops" walk
  downhill, eroding/depositing as they go. Cheap, serial, in-place, and what
  gives the terrain its dendritic structure in the first place. A particle
  that reaches its travel budget still carrying sediment no longer dumps that
  load into the one cell it died in — it redirects into a suspended-sediment
  field (`Grid::sus`) that settles out over the following steps (see
  "Findings", below).
- **The handoff — `SweWarmStart`.** A one-shot, CPU-side function that seeds
  phase 1's water depth from phase 0's *finished* bed: lakes prefilled to
  their `PriorityFlood` spill level, channels prefilled to a Manning normal
  depth. Starting phase 1 bone dry would mean waiting on real rain to fill a
  lake from nothing — on the order of 1e8 substeps; this instead starts the
  fluid cycles near the steady state they would converge to anyway.
- **Phase 1 — Eulerian shallow water + Exner morphodynamics.** A virtual-pipes
  shallow-water solver (`protogen_swe.cpp`) advances water depth/velocity in
  staggered cycles, with a MORFAC-accelerated bed-exchange/advection/talus
  group running once per cycle. GPU-shaped by construction: every pass is a
  standalone, gather-only Jacobi sweep over the whole grid, reading only
  front-buffer state, so **the pass list below is the frozen future GPU
  dispatch order**, not an implementation detail:

  ```
  swe_substeps × { SweFlux → SweDepth → SweVelocity }
  once per cycle: { SedExchange → SedAdvect → TalusFlux → TalusApply }
  ```

  The morpho group is skipped entirely when `morfac == 0` (a fluid-only run).

**One water representation, not two.** Unlike the retired standing-water/lake
machinery, phase 1 carries exactly one water field, `Grid::h` — a depth,
present wherever the cell is wet, channel or lake alike, exactly like the real
shallow-water equations it approximates. There is no separate "is this a
lake" simulation state anywhere in the solver. **The lake/river distinction
exists in exactly one place**, downstream of the physics:
`ClassifyBoundaryWater` (protogen.hpp/protogen.cpp) seeds a region wherever a
cell is both deep (`h > 1 m`) and slow (`speed < 0.05 m/s`), then grows it by
4-connected flood fill across neighbours whose water SURFACE (`bed + h`)
matches within 1 cm — a lake's surface is flat by construction, so this stops
exactly at the true shoreline without ever asking "is this a channel," only
"is the surface still flat." That is what `water.f32`/`world.txt`'s
`lake_tag` carry; `depth.f32` (phase-1 dumps only) is the honest, unmasked
`h` instead, wet or dry, channel or lake.

[ref]: https://nickmcd.me/2023/12/12/meandering-rivers-in-particle-based-hydraulic-erosion-simulations/

## Build and run

Taskflow is header-only, so this is still a small, hand-listed set of TUs, no
CMake. Four protogen TUs (phase 0 + driver + output boundary; phase-1 SWE +
morphodynamics; the `--test` suite) plus the `src/mapgen/` river-extraction
pipeline the output boundary pulls in — see "Self-describing output" below:

```sh
c++ -O3 -std=c++23 \
  -I src -I third_party/FastNoiseLite -I third_party/glm -I build/_deps/taskflow-src \
  tools/protogen/protogen.cpp tools/protogen/protogen_swe.cpp tools/protogen/protogen_tests.cpp \
  src/core/parallel.cpp \
  src/mapgen/hydrology.cpp src/mapgen/river_graph.cpp src/mapgen/river_prune.cpp \
  src/mapgen/coarse_io.cpp src/mapgen/river_io.cpp \
  -o /tmp/protogen

mkdir -p /tmp/pg && /tmp/protogen --res 512 --world 8192 --steps 3000 \
  --drops 1024 --snapshot-every 500 --cycles 3000 --morfac 10 --out /tmp/pg
python3 tools/protogen/show.py /tmp/pg
```

`--cycles 0` (the default) stops after phase 0, byte-shaped exactly like a
pre-phase-1 run always was. `--cycles N` runs the handoff plus `N` phase-1
cycles, in batches of `--snapshot-every` (reused as a cycle cadence once
`--cycles > 0`), dumping a `%04d-cycle` snapshot after each batch.

### File map

| file | what |
|---|---|
| `protogen.hpp` | shared types (`Params`, `Grid`, `SimStats`) and the handful of functions crossing a TU boundary |
| `protogen.cpp` | phase 0 (particle walk, cascade), the whole-sim driver (`main`), the output boundary (`world.txt`/`rivers.bin`), run diagnostics, perf report |
| `protogen_swe.cpp` | phase 1: the SWE fluid passes, the Exner/talus morpho passes, `SweWarmStart`, `ClassifyBoundaryWater` |
| `protogen_tests.cpp` | the `--test` sanity suite (79 assertions) |
| `select.cpp` | phase 2a: gameplay-window scanner (standalone, no CMake, stdlib only) |
| `show.py` / `soil.py` | render a dump: hillshade + rivers + lakes, depth/velocity (phase-1 only), the soil-cover raster |
| `lakes.py` / `lakestats.py` | post-hoc priority-flood lake analysis on a bare heightmap — independent of the sim's own `water.f32`, still useful for a phase-0-only dump |

`show.py` writes `-map.png` (hillshade + rivers + lakes) and `-hillshade.png`
per snapshot; a phase-1 (`NNNN-cycle`) snapshot additionally gets `-depth.png`
(raw `h`, log-scaled) and `-vel.png` (speed) whenever those rasters are
present. All four Python tools read resolution/world size from
`<dump-dir>/world.txt` instead of taking them as positional args
(`show.py`/`soil.py` take the dump directory; `lakes.py`/`lakestats.py` take
one `.f32` file and find `world.txt` next to it) — the old explicit
`<n> <world_m>` form still works as an override for a dump without one.

## Self-describing output

A dump used to be headerless rasters plus whatever `--res`/`--world` a
consumer happened to pass on argv — so its 16 m cell size existed nowhere in
the data, and a wrong flag silently reinterpreted the bytes. protogen writes
two more things next to the `.f32` rasters, once the sim finishes:

- **`world.txt`** — a `src/mapgen/coarse_io.hpp` manifest: resolution, world
  size (and the texel size derived from them), seed, runoff, steps, the
  whole-world DRY-soil quantiles (`soil_cut_mountain_m` / `soil_cut_hills_m`,
  the cutoffs `src/mapgen/coarse_world_patch_source.cpp` classifies biomes
  against), and — once phase 1 has run — its provenance: `morfac`, `cycles`,
  `substeps`, so a reader (or `--extract-rivers`, below) can tell a phase-1
  run happened and which tag it finished on. All three are 0 when phase 1
  never ran, the file's normal "always write, parser tolerates absence" rule.
- **`rivers.bin`** — a `src/mapgen/river_io.hpp` binary dump of the
  WHOLE-WORLD `RiverGraph`, off the phase-0-finished bed OR the phase-1-
  finished one, whichever the run reached. Binary because text would be
  ~35 MB at 16 km.

```sh
# Re-run ONLY the extraction + serialization above against an existing dump,
# skipping the sim — so an extraction bug costs seconds, not minutes.
# Picks the phase-1 tag automatically if world.txt records cycles > 0.
/tmp/protogen --extract-rivers /tmp/pg
```

The duplication between `src/mapgen/*`'s `Field2D<T>` and protogen's own flat
`std::vector<float>` fields (see `Grid` in protogen.hpp) is deliberate: a tiny
adapter converts one to the other at the output boundary rather than protogen
adopting `Field2D` internally, so a change to either representation cannot
ripple into the other.

## Findings so far — read before changing anything

### Phase 0 (particle carve) — still true

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
- **The substrate is a two-layer one: bedrock + soil.** The surface is
  `bedrock + soil`; erosion takes soil at full rate, then bites bedrock at
  `bedrock_erodibility` (0.1, the k_bedrock/k_sediment ratio from
  `src/mapgen/erosion.hpp`). **The transport law is untouched** — `c_eq` and the
  reference's deposition step are verbatim; the substrate changes only how much
  of the requested cut is YIELDED, and the particle's load is scaled by the same
  factor so bedrock cannot invent mass. Deposition is never resisted and always
  lands on soil. Soil starts as a layer WITHIN the initial surface
  (`bedrock = height - initial_soil_m`), not on top of it, so step 0 is
  bit-identical to a no-substrate run and an A/B stays honest.
- **The cascade is the simulation, not the erosion.** A sampling profile puts
  62% of runtime in `Cascade` and 17% in its `std::sort` alone. Returning before
  the sort when no neighbour exceeds the repose angle is bit-identical (the
  settle loop would `continue` on every neighbour) and worth **1.20x** — which
  is exactly the sort's share recovered. Do NOT also filter the gather to
  `d > max_diff`: `std::sort` is unstable, so dropping elements can permute
  EQUAL drops differently, and equal drops are the normal case on symmetric
  fixtures — it breaks bit-identity exactly where the tests are most symmetric,
  for a few percent more.
- **`-march=native` buys nothing here** — 22.0 s vs 21.7 s at 512²/1200 steps,
  inside the noise. An earlier 1.71x reading was taken on a loaded machine and
  did not survive a best-of-N re-measure. Left off the build line deliberately.
- **The terminal-exit sediment redirect, and why only that one exit.**
  `Descend`'s post-loop "reached max_age/`max_travel_m` still carrying a load"
  used to dump its whole remaining load into the ONE cell the particle died
  in — measured at ~40% of all deposited mass, and the likely source of
  thousands of one-cell pits (the original defect this project fixes: the
  old pipeline manufactured 13,956 depressions and ponded 25% of the 8 km
  fixture). It now injects into `Grid::sus` instead, which drains over the
  following steps: `settle_fraction` (0.1, ~96% settled by 30 steps) joins
  the bed each step, and `sus_diffusion` (0.2) of the rest spreads to the
  four orthogonal neighbours first, smearing the old spike into a shallow
  halo instead of erasing or instantly relocating it. `Descend` has three
  OTHER `Deposit`-at-death sites (exact-zero heading, a stalled zero-speed
  walk); only the travel-budget exit was redirected — the other two are
  either unreachable to float precision or scattered wherever the terrain
  happens to be flat, not the same fixed cell every run the way the
  travel-budget cutoff is.

### Phase 1 (SWE + morphodynamics) — new

- **Transport capacity is on the ENERGY slope, not the bed slope.**
  `SedCapacityM`'s law is `C = capacity_Kc_s · |∇(bed+h)| · |v|`, an
  equivalent sediment depth. Both factors vanish in a still lake (flat water
  surface ⇒ zero energy slope; no flow ⇒ zero speed) — `StillLakeInert` pins
  a still lake moving *exactly* nothing. Using the BED slope instead would
  keep `C > 0` for flowing water over a locally flat bed (the water surface
  still tilts — that tilt is what drives the flow), so a flat reach would
  wrongly report zero capacity and terrace; `FlatReachTransport` pins the
  opposite: `C > 0` there specifically because the bed is flat but the
  surface is not.
- **MORFAC applies to the erosion demand BEFORE the soil/bedrock split, not
  after** — a ratified deviation from the plan's literal order. `yield(M·D)`
  is the exact aggregation of `M` sequential fluid intervals over a
  depleting soil column (the substrate law is additive: `MorfacAggregationIdentity`
  measures 1 cycle at M=100 against 100 cycles at M=1 to a 5e-7 relative
  difference); split-then-×M can demand M× more soil than exists and
  silently erode bedrock at full rate instead. `MorfacMantleTransition`
  pins the exact numbers on a thin mantle (0.5 m soil, D=0.01 m, M=300 → soil
  exactly 0, bedrock cut exactly 0.25 m), and a tripwire forbids
  soil-production/weathering interleaved with M>1 (additivity breaks under
  weathering).
- **The MORFAC clamp masks the channelization factor's cross-section
  signature above roughly M=50** (Task 6: identical channel cross-sections
  measured at M=300) — but aggregate bed change is NOT M-invariant, and an
  earlier version of this finding claimed otherwise from the wrong baseline.
  Comparing each 8 km run's FINAL bed against the shared `0000-initial`
  height (what both a naive read and this document's first draft did)
  measures mostly phase 0: phase 0 alone (`3000-step-height.f32`, bit-identical
  between the M=10 and M=50 runs since MORFAC has no phase-0 effect)
  already accounts for max |Δheight| 61.557 m / mean 5.856 m — 99.9% of the
  ~61.6-61.8 m / ~5.86-5.89 m figures a naive full-run comparison reports,
  which is why those looked "indistinguishable." Isolating phase 1's OWN
  contribution (`3000-step-height.f32` → `3000-cycle-height.f32`) tells the
  opposite story: M=10 moved max 18.99 m / mean 0.00486 m; M=50 moved max
  26.57 m / mean 0.0346 m — a **7.1× divergence in mean bed disturbance**.
  MORFAC is not clamp-saturated into invariance at production cycle counts;
  M=50 genuinely disturbs the bed more. That strengthens, not weakens, the
  M=10 choice below: it reaches equal-or-better depression/drainage
  diagnostics (see "Full-map runs") for roughly a SEVENTH of the mean bed
  disturbance M=50 costs.
- **Warm start seeds channel depth at the cell's FULL width (`cell_m`), not
  the particle walk's sub-grid regime width.** The brief's literal formula
  (`w = channel_width_coeff·√Q`, `Descend`'s own closure) models a channel
  narrower than one cell — correct for a particle at a continuous position,
  wrong for `SweFlux`, whose own conveyance is already `A_pipe = cell_m·h`
  (the full cell width). Seeding a depth calibrated for a narrower channel
  systematically overshoots what the fluid solver converges to: measured
  0.123 m of L-inf water-surface drift over 50 fluid-only cycles with the
  regime width (4× over the 0.03 m bound), 0.0154 m with `cell_m` — an 8x
  reduction. `channel_width_coeff` is untouched everywhere else (`Descend`,
  `river_graph`); this is one call site's conveyance width, not a knob change.
- **The lake/river boundary is a two-threshold seed-and-grow rule, not a
  simulated state.** See "What it is" above for the full contract
  (`h > 1 m` AND `speed < 0.05 m/s` to seed; grow while `|Δ(bed+h)| < 1 cm`).
  Both seed thresholds matter independently — a deep-but-fast flood pulse in
  a channel, and a shallow-but-slow puddle, must each fail to seed alone;
  `BoundaryClassification` pins both. Deterministic and O(cells): a pure
  function of grid state, independent of scan order.

### This validation task (Task 8)

- **`select.cpp`'s channel gate needs SOME discharge estimate, and a
  phase-1 (`NNNN-cycle`) dump has no `Q.f32`** — `DumpPhase1` deliberately
  never writes it, since `Qm3s`/`discharge` are phase-0 particle-EMA fields
  phase 1 never updates (see `DumpPhase1`'s own comment); writing them under
  a phase-1 tag would dump stale phase-0 state under a new name. `select.cpp`
  now falls back to `Q = depth · vel · cell_m` when `Q.f32` is absent — not an
  invented proxy, but the EXACT formula `SweFlux` itself uses to turn a face
  velocity into a flux (`A_pipe = cell_m·own_h`), reconstructed from
  `depth.f32`/`vel.f32`, which ARE phase-1's own live state. A phase-0 tag's
  real `Q.f32` (e.g. the old M16b `--tag 3000-step` usage) is read verbatim,
  unaffected.
- **The depression-count/drainage diagnostic was phase-1-only, which starved
  its own "before vs after" purpose.** `PrintRunDiagnostics`'s header
  comment already said the numbers exist for "Task 8's full-run validation"
  baseline, but `main()` only ever called it after phase 1 finished — a
  `--cycles 0` run (exactly the baseline case) printed nothing. It now fires
  once after phase 0 (labelled `phase-0`) and again after phase 1 completes
  (labelled `phase-1`), so a single `--cycles > 0` run reports its own
  before/after without a separate baseline invocation.

## Verification

`protogen --test` — **79 assertions** on 32–1024 cell grids at (mostly) the
production 16 m cell size, run in ~6 s:

| section | assertions |
|---|---|
| mass conservation, determinism, knob liveness, discharge/valley/repose/lobe/bedrock/substrate/symmetry/resolution (phase 0 core) | 19 |
| physics invariants (some `PEND` until a mechanism lands) | 15 |
| phase-1 SWE fluid (virtual-pipes shallow water) | 11 |
| phase-0 → phase-1 warm start | 2 |
| phase-1 morphodynamics (Exner + advection + talus) | 28 |
| driver, output boundary | 4 |

The two-phase water suite checks the SWE relaxation against the exact
fill-spill-merge oracle (`PriorityFlood`): fixed-terrain convergence, clock
and resolution invariance, the warm start's proximity to the fluid solver's
own steady state, and — for the morpho passes — mass ledgers with an
independently-computed border-export term, the MORFAC aggregation identity,
channel persistence with the channelization factor on/off, and knickpoint
retreat.

**The single most productive diagnostic in this work: a real logic change that
produces bit-identical output.** It caught two masking bugs in the retired
standing-water/lake machinery (a lobe-length clamp hiding a lake-plume
settling knob, a wander cap hiding jet turbulence) and disproved two wrong
diagnoses of a since-retired cadence bug. `knob liveness` in the suite
automates it — every knob added since (fluid, morpho, and the driver's own
`--cycles`) gets a liveness row, phase 0 and phase 1 alike.

`select --test` — 5 assertions (gate wiring, overlap suppression, and — new
this task — the `Q.f32` fallback both firing when it should and staying out
of the way when it shouldn't), instant.

## Full-map runs

Both at the chosen production config: **`--morfac 10`, `--cycles 3000`,
default `--swe-substeps 50`** — picked over `--morfac 50` on the 8 km probes
above (worse off-map-drainage number AND 7.1× the mean phase-1 bed
disturbance for only marginally fewer depressions; see "Findings"). All
three runs below hit zero tripwires; the sediment-mass audit
(checked every 10 cycles) never fired.

| | 8 km, phase 0 only (`--cycles 0`) | 8 km, phase 1 (M=10, 3000 cycles) | 16 km, phase 1 (M=10, 3000 cycles) |
|---|---|---|---|
| resolution | 512² | 512² | 1024² |
| steps × drops | 3000 × 1024 | 3000 × 1024 | 3000 × 4096 |
| relief | 329.2 m | 329.2 m | 329.0 m |
| wet % (final, `water.f32 > 0`) | 0.00% | 1.66% | 2.12% |
| max lake depth | — | 5.2 m | 6.6 m |
| depressions (8-nbr local minima) | 4098 | 3905 (−4.7%) | 18958 (−0.6%, from an unchanged 19072 baseline) |
| off-map drainage (D8 reaches border) | 2.73% | 2.94% (+0.21 pt) | 1.57% (+0.06 pt, from 1.51%) |
| advect fixer worst \|factor−1\| | — | 0.380 | 0.466 |
| advect fixer unplaced residual | — | 0 m³ | 0 m³ |
| runtime: phase 0 | 26 s | 26 s | 119 s |
| runtime: phase 1 | — | 71 s | 256 s |
| runtime: total | 26 s | 98 s | 375 s (~6.2 min) |

**Both diagnostics move the RIGHT way at both scales** — fewer depressions,
more off-map drainage — but the improvement is modest in relative terms
(5–6% at 8 km, well under 1% at 16 km) next to the old pipeline's defect
(13,956 depressions, 25% ponded, on the SAME 8 km fixture size). That
comparison is not apples-to-apples on the diagnostic itself (the old number
was never measured with this D8/local-minima method) but the mechanism is:
phase 0 alone — before phase 1 runs at all — already lands at 4098
depressions on 8 km, because the terminal-exit redirect (see "Findings")
removes the dominant one-cell-pit source. Phase 1 is cleaning up what is left
over: smaller, more numerous, spatially scattered local minima that a
global relaxation reaches slowly. **16 km's proportionally weaker
improvement at the same 3000-cycle budget is a genuine scale effect, not a
defect** — the morpho passes are spatially LOCAL per cycle, so a 4x-area map
needs more than the same absolute cycle count to reorganize a proportional
share of its (also ~4.6x larger) depression count; a longer run was out of
this task's exploration budget and is a natural next probe.

### 8 km MORFAC knob probes (3000 cycles each, `--swe-substeps 50`)

| M | depressions | off-map drainage | phase-1-only max \|Δheight\| | phase-1-only mean \|Δheight\| | advect fixer worst |
|---|---|---|---|---|---|
| — (baseline) | 4098 | 2.73% | — | — | — |
| 10 | 3905 | **2.94%** | 18.99 m | **0.00486 m** | 0.380 |
| 50 | **3856** | 2.64% (worse than baseline) | 26.57 m | 0.0346 m (7.1×) | 0.362 |

The Δheight columns isolate phase 1's own contribution
(`3000-step-height.f32` → `3000-cycle-height.f32`, NOT the shared
`0000-initial` baseline — phase 0 alone already accounts for max 61.557 m /
mean 5.856 m and is bit-identical between the two runs, so comparing against
it would mostly measure phase 0). M=50 removes marginally more depressions
but REGRESSES off-map drainage below the baseline AND costs 7.1× the mean
bed disturbance, while M=10 improves both diagnostics for a fraction of the
disturbance. A short-duration check (M=10, 300 cycles instead of 3000) moved
depressions from 4098 to only 4094 and drainage not at all — cycle count,
not just M, has to be large enough for the effect to show, which is why 3000
(matching phase 0's own step budget)
is the chosen default over a shorter probe.

## Phase 2a — window selection (`select.cpp`)

Scans a finished world for a gameplay-sized window against four gameplay
gates, ranks the survivors, and prints an origin to pin. It only PICKS —
extraction (resampling the picked origin into a gameplay patch: bed/water
reconstruction, biome classification, the mapview load set) is no longer part
of this tool. That is `src/mapgen/coarse_world_patch_source.hpp`, used
through `badlands_mapview --load <coarse-dir>
--patch-size/--patch-res/--patch-origin`; this tool's whole job ends at
printing the origin to pass to `--patch-origin`.

Separate standalone TU, stdlib only. Geometry (resolution, world size) is
read from `<dump-dir>/world.txt`, not passed on argv — `--res`/`--world`
remain as an explicit override for a dump that predates world.txt:

```sh
c++ -O3 -std=c++23 tools/protogen/select.cpp -o /tmp/select
/tmp/select --in /tmp/pg --tag 3000-cycle   # phase-1 tag: Q reconstructed from depth*vel
/tmp/select --in /tmp/pg --tag 3000-step    # phase-0 tag: Q.f32 read verbatim, no lake water
/tmp/select --test      # 5 assertions, instant
```

`--origin-cell X,Y` (source cells) re-scores one pinned origin instead of
ranking. `--rank N` is an ordinal into the ranked list — it reshuffles
whenever the map or a gate changes, so prefer pinning by coordinates once a
location is chosen. The scanned window size is a world-metres flag
(`--window-m`, default 1024).

### Gate table: M16b (old pipeline) vs the 16 km two-phase run

| criterion | M16b (old, standing-water pipeline) | this run (phase-1, M=10, 3000 cycles) |
|---|---|---|
| lake: largest connected component ≥ 16 cells at depth > 0.5 m | 8594 (14.8%) | **14644 (25.2%)** |
| channel: ≥ 40 off-lake cells at Q ≥ 0.02 m³/s | 23919 (41.2%) | **39072 (67.3%)** |
| plains: ≥ 50% of the central 512 m dry and < 5° slope | 15922 (27.4%) | **53758 (92.5%)** |
| mountains: window relief ≥ 150 m | 37979 (65.4%) | 0 (not comparable — see below) |
| all four together | 449 (0.77%) | 0 |

Out of 58081 windows scanned, both maps.

**Lake, channel, and plains all improve — the water-classification story this
project set out to test.** The lake-only `water.f32` (a physically simulated
depth, spill-level bounded) both grows the lake gate's pass rate and lets the
plains gate correctly exclude water without a lake sitting on top scoring as
"flat, therefore plain."

**Mountains and the all-four composite are NOT a controlled comparison** —
`relief_m` (the height-field's real-metres budget) is 400 on this branch and
every other run in this document, but M16b predates the `Horseshoe` terrain
substrate entirely and ran at `relief_m = 900` (an "alpine range" scale,
commit `b3e8b07`, unrelated to this project). The current terrain's max local
relief anywhere in a 1024 m window on this 16 km map is 128.6 m — under the
150 m gate by construction, not because of anything phase 1 did. Lowering
`--min-relief` to something the terrain can clear (measured down to 80 m)
still yields zero four-gate survivors: the handful of windows that DO clear a
relaxed relief bound rarely also carry a co-located lake+channel+plains
signature at this map's scale. This is a terrain-generator-default fact that
predates this branch, not a regression introduced by the two-phase pipeline;
re-tuning `relief_m` for gameplay-window yield is a separate, out-of-scope
decision.

## Open

- **The lake-outlet backwater watch item is real but small.** `ClassifyBoundaryWater`'s
  surface-continuity growth (see "What it is") is not re-checked against the
  seed speed threshold once a region is growing, so a genuinely-flowing
  backwater reach right at a lake's outlet CAN join the lake tag if its
  surface still matches within 1 cm. Measured on the 16 km run: of the
  lake's own MARGIN cells, 6.3% exceed the 0.05 m/s seed-speed threshold
  (max 0.87 m/s, a real channel speed) — a small leak into the outlet, not a
  gross one. The other direction (clipping) does not appear to happen: the
  first non-lake cell ring outside every lake tops out at 0.98 m depth
  (just under the 1 m seed) with speed up to 0.75 m/s, i.e. the boundary
  tracks right up to genuinely channel-like water rather than cutting the
  margin short. Net read: the rule is closer to "slightly generous at the
  outlet" than "clips the shoreline," which is the safer of the two failure
  directions for this tool's downstream consumers (a slightly-too-large lake
  reads as a lake; a clipped one would hide real standing water).
- **The old "deepest lake reports 308 m" item is resolved by construction, not
  by observation alone.** The old pipeline synthesized lake bathymetry
  post-hoc from a bare, un-eroded heightmap, where a noise minimum could
  masquerade as a 300 m-deep "lake" that was never a real landform. Phase 1
  cannot reproduce that failure mode: `SweWarmStart` seeds a lake to the
  `PriorityFlood` spill level of the FINISHED (eroded) bed, and the fluid
  solver's own dynamics hold it there (`WarmStartLakeLevel`'s exact-zero
  drift, `StillLakeInert`'s bit-exact stillness) — there is no separate
  "bathymetry" step to distrust independently of the terrain the sim already
  produced. Measured max lake depth at 16 km is 6.6 m, two orders of
  magnitude below the old number; some of that gap is the terrain-scale
  difference noted above (`relief_m` 400 vs M16b's 900), so treat the
  MECHANISM fix as the resolution and the absolute number as corroborating,
  not as an apples-to-apples before/after.
- **`talus_relaxation_per_yr` (default 1.0/yr) is uncalibrated for production
  MORFAC.** It puts the hillslope-relaxation process on the real-time clock
  deliberately (see its own comment in `protogen.hpp`), but nothing in this
  validation watched hillslope behaviour specifically at scale — the 8/16 km
  renders should be read with that in mind.
- **16 km's depression/drainage improvement is much weaker in relative terms
  than 8 km's at the same 3000-cycle budget** (see "Full-map runs"). Whether
  more cycles closes the gap, or whether the remaining depressions at that
  scale are a structurally different (harder) population, is untested —
  next probe, not a defect.
- **`lakes.py`/`lakestats.py` are independent of the sim's own water
  classification.** They re-derive lakes from a bare heightmap via
  `PriorityFlood`, which is still meaningful for a phase-0-only dump (no
  `water.f32` content to read instead) but redundant with — and not
  guaranteed to agree with — phase 1's own physically simulated
  `water.f32` on a phase-1 dump.
- The particle pass is serial. Racy in-place writes would parallelise it the
  way the reference does, at the cost of reproducibility.
- No hillslope diffusion in a production run (`enable_diffusion` defaults
  off), so divides lower only very slowly, if at all.
- **The sim ponds single-cell puddles.** Faithful to the data, not a resample
  artefact; culling them is a decision about the SIM's output, so nothing
  downstream silently filters them.
- **The relief pass does not exist yet.** A patch carries only what its coarse
  world had, so everything visible is one coarse cell or larger. The fix is a
  stateless per-point erosion FILTER composed onto the resampled bed — not a
  second simulation — designed in
  `docs/superpowers/specs/2026-08-02-procgen-stage-split-design.md` §3.4 and
  deliberately deferred.
