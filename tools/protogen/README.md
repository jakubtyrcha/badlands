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

Taskflow is header-only, so this stays a single standalone TU:

```sh
c++ -O3 -std=c++20 \
  -I src -I third_party/FastNoiseLite -I build/_deps/taskflow-src \
  tools/protogen/protogen.cpp src/core/parallel.cpp -o /tmp/protogen

mkdir -p /tmp/pg && /tmp/protogen --res 512 --world 8192 --steps 3000 \
  --drops 1024 --snapshot-every 750 --out /tmp/pg
python3 tools/protogen/show.py /tmp/pg 512 8192
```

`show.py` writes `-map.png` (hillshade + rivers + lakes) and `-hillshade.png`
per snapshot. `lakes.py` / `lakestats.py` are post-hoc priority-flood analysis.

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
| final relief | 879 m | 806 m |
| lakes at end | **0** | **89** |
| wet at end | 0.00% | 2.52% |

**Lakes persist at 16 km but not at 8 km** — bigger basins outlast the sediment
supply. Earlier notes calling lakes "transient" were reporting a
scale-dependent result as a general one.

## Open

- **Deepest lake reports 203 m** at 16 km, which is far too deep for that
  footprint. Same signature as un-eroded fBm minima surviving as holes rather
  than landforms; lake bathymetry is not trustworthy until chased down.
- The particle pass is serial. Racy in-place writes would parallelise it the way
  the reference does, at the cost of reproducibility.
- No hillslope diffusion, so divides lower only very slowly.
