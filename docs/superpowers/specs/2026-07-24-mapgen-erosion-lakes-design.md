# Mapgen step 2: erosion + lakes

**Date:** 2026-07-24
**Status:** approved

## Goal

Resculpt the cone-field heightmap into eroded terrain and prove the
cavity→lake pipeline end to end: a two-layer (bedrock + sediment) stream-power
erosion sim, pre-seeded lake basins that flood to **per-lake water levels**,
and a runevision-style analytic gully filter for fine detail. Produces the
hydrology artifacts (`flow`, `sediment`, `water_depth`) that the next step
uses to place Swamp/Forest/streams.

Reference: "Fast and Gorgeous Erosion Filter" (runevision, 2026-03) — used as
the **detail** tier only; it is a stateless directional-noise filter and
produces no hydrology, so mass movement and water need the sim tier.

## Decisions taken with the user

- **Scope:** erosion + lakes now (Lake stamped into the biome map); swamp/
  forest/streams next step, consuming this step's artifacts.
- **Budget:** a few seconds at 512² sim, CPU-only (`--preview-image-only`
  stays pure CPU). Serial-correct first; parallelize only if the budget is
  actually threatened.
- **Grids:** ONE sim resolution and ONE output resolution, both square with a
  single world-scale factor, decoupled from each other and from world size.
  A future game map can raise `sim_resolution` for finer landforms (accepted:
  changing sim res changes landforms).
- **Customization surface:** an `ErosionParams` struct with tuned defaults, on
  `MapGenParams`. No UI, no CLI flags. Sub-knobs stay `constexpr`.
- **Lake seeding:** bottom `lake_frac` quantile of the bedrock latent field
  marks basin regions (mirrors the mountain quantile machinery); cavities
  carve there.
- **Erosion model:** implicit stream power on the drainage tree
  (Braun–Willett) + G-term deposition (Yuan et al.) — chosen over droplet
  particles (no hydrology outputs, weak control) and shallow-water pipes
  (CFL-limited, tuning-fragile; revisit for the hydraulics follow-up).
- **Inspectability:** every pipeline step dumps a visual (debug sink below).
- **No global water level.** Each lake has its own spill elevation. Height 0
  is merely the plains' starting datum. This **retires** the "0 m is the
  water datum" wording of the 2026-07-24 heightmap spec.

## Interface changes

`MapGenParams` collapses to square/scalar (breaking; mapview CLI keeps
`WxH` parsing but rejects non-square for now):

```cpp
struct MapGenParams {
  uint32_t seed = 1;
  int resolution = 512;        // output grid (texels, square)
  float world_size_m = 512.0f; // world extent (square)
  ErosionParams erosion;
};
```

`MapArtifacts` (all fields on the output grid):

```cpp
struct MapArtifacts {
  Field2D<float> bedrock;     // latent field (unchanged)
  Field2D<uint8_t> biome;     // now includes Lake
  Field2D<float> heightmap;   // eroded + detailed ground surface (m)
  Field2D<float> water_depth; // standing water (m), 0 = dry; surface = heightmap + water_depth
  Field2D<float> flow;        // drainage area (m²) — moisture/stream input for the next step
  Field2D<float> sediment;    // sediment thickness (m) — future swamp/soil signal
};
```

`generate_map(params, MapDebugSink* sink = nullptr)` — see Debug sink.

New files: `hydrology.{hpp,cpp}` (priority-flood, receivers, accumulation —
reused by the hydraulics follow-up), `erosion.{hpp,cpp}` (sim loop, layers,
cavities), `detail_filter.{hpp,cpp}` (gully filter). `generator.cpp`
orchestrates; `outputs.cpp` implements the PNG sink.

## Model

State per sim cell: bedrock surface `B` (m), sediment thickness `S ≥ 0` (m),
ground `h = B + S`. Water is never state — re-derived from `h` each iteration.

**Grid & boundary.** Sim grid covers the world rect expanded by `kPadTexels`
(16); bedrock noise is defined everywhere in world space, and the padded EDT
also un-clamps cone heights near map edges. Border cells are pinned base
level (never erode, all flow exits there); the pad is cropped at resample so
the pinned ring never reaches the output.

**Pipeline (sim grid):**

```
bedrock latent → quantile biomes → cone relief B0        [existing math, sim-grid sampling]
→ carve cavities   bottom lake_frac quantile → smooth bowls into B (≤ lake_depth_m)
→ sediment init    S0 = initial_sediment_m · taper(dist-to-plains, sediment_taper_m)
                      + fBm(sediment_noise_m, sediment_noise_wavelength_m); 0 in cavities
→ SIM LOOP × iterations:
    1 route    priority-flood from borders over h (Barnes 2014): D8 receiver per
               cell, pop order = topological order, ε-gradient across flats and
               flooded interiors, in_lake flags + local water level.
               Ties break on (elevation, linear index) — deterministic.
    2 drain    reverse pop order, uniform rain → drainage area A (m²)
    3 incise   implicit stream power (Braun–Willett), slope exponent n fixed at 1
               (closed form; n≠1 needs a per-cell Newton iteration — deferred), per cell
               in pop order:  F = K·A^m·dt/d,  h' = (h + F·h_rcv')/(1+F)
               K = k_sediment while S>0 else k_bedrock; eroded depth consumes S
               first, excess rescaled by k_bedrock/k_sediment before cutting B.
               Unconditionally stable; h' is a weighted average → a cell never
               erodes below its receiver. in_lake cells see slope ≈ ε (no floor
               incision); outlet sills incise with the full lake catchment's A —
               lakes can slowly breach (headroom controlled by lake_depth_m).
    4 deposit  G-term (Yuan et al. 2019): ∂h/∂t += (G/A)·q_s, q_s = upstream
               net-erosion flux; one explicit downstream flux pass per sim
               iteration (donors-before-receivers); the outer iteration loop
               provides the relaxation. Flux entering an in_lake cell deposits
               up to the local water level (deltas), overflow continues to
               the outlet. Border flux exits the map (mass deliberately not
               conserved).
    5 diffuse  ∂h/∂t = D·∇²h, explicit, sub-stepped to D·dt_sub/Δx² ≤ 0.25.
               Removals draw S before B; additions credit S.
→ finalize   final flood → per-lake spill level; W = level − h inside; prune
             lakes with area < min_lake_area_m2 or max depth < min_lake_depth_m
```

**Output (output grid, pad cropped):** bilinear-resample `h`, add the gully
detail filter, resample `W`/`A`/`S`; classify biomes from output-res bedrock
as today, then stamp Lake where `W > 0` (recomputed against the detailed
heightmap so shorelines stay consistent).

**Why plains gain relief:** deposition builds fans where mountain streams
debouch; large through-flowing rivers gently incise the sediment blanket;
sediment noise + diffusion add micro-relief; the detail filter textures
slopes. Plains stay plains-like — gently sculpted.

**Time.** `dt` is a nominal unit; only K·dt, D·dt, G matter. iterations/dt =
terrain age; k_* = how carved; G = valley-fill vs canyon export; D = soft vs
crisp.

## Detail filter (runevision adaptation)

Per output texel: base = bilinear sim `h`; downhill direction from finite-
difference gradient of the base; add N octaves of oriented gully noise.

- **v1 core:** cell-rotated stripe patterns aligned to the local gradient
  (Worley-style pivot grid), octave stacking with stacked fading (small
  gullies fade on ridges/creases of larger ones), normalized gully magnitudes,
  slope-masked amplitude with the ease-out curve `1 − (1 − slope)²`.
- **Skipped in v1** (addable later without interface change): drainage-streak
  map, analytic output derivatives, straight-gully sign trick.
- **Shipped deviation:** amplitude fades to zero within `kShoreFadeDistM`
  (3 m) of standing water, measured as HORIZONTAL distance-to-water (via the
  mask EDT) — not, as originally spec'd, by elevation above the local water
  surface. An elevation-aware clamp is a recorded follow-up: until it lands,
  steep banks near shore can carve below the adjacent lake surface.
- Pure function of (world position, seed, base gradient): deterministic,
  tile-parallel, world-metric wavelengths.

## Debug sink — every step inspectable

`generate_map` takes an optional observer; the generator itself does no I/O:

```cpp
struct MapDebugSink {
  // stage: e.g. "bedrock", "cone", "cavities", "sediment-init",
  //        "loop-height", "loop-flow", "loop-sediment", "loop-lakes",
  //        "water", "detail-delta", "final-height", "biome"
  // (the PNG sink renders hillshade for *-height stages; the generator
  // itself dumps raw fields — rendering choices live in outputs.cpp)
  virtual void dump(std::string_view stage, int sequence,
                    const Field2D<float>& field) = 0;
  virtual void dump(std::string_view stage, int sequence,
                    const Field2D<uint8_t>& mask) = 0;  // biome / lake masks
};
```

`outputs.cpp` implements it to write numbered PNGs (`00-bedrock.png` …,
loop frames `loop-0010-flow.png` …); `--preview-image-only` wires it up
always-on. Loop frames every `dump_every` iterations (hillshade(h), log₂ A,
S, lake mask) — the sim is watchable as a film strip. The detail stage dumps
its **delta raster** (final − resampled base) so the filter's contribution is
visible in isolation. Null sink = zero overhead.

## ErosionParams (defaults are starting points; tuned via the preview loop)

```cpp
struct ErosionParams {
  int sim_resolution = 512;          // sim grid (texels, square, excl. pad)
  int iterations = 80;
  float dt = 1.0f;                   // nominal time unit
  float m = 0.5f;                    // stream-power area exponent (n is fixed at 1)
  float k_sediment = 5e-3f;          // erodibility, sediment
  float k_bedrock = 5e-4f;           // erodibility, bedrock
  float deposition_g = 1.0f;         // 0 = export all, ↑ = refill valleys
  float diffusion = 0.02f;           // D (m²/dt)
  float initial_sediment_m = 4.0f;
  float sediment_taper_m = 60.0f;    // taper distance over dist-to-plains
  float sediment_noise_m = 1.0f;     // fBm amplitude on S0
  float sediment_noise_wavelength_m = 40.0f;
  float lake_frac = 0.03f;           // bottom bedrock quantile → basins
  float lake_depth_m = 12.0f;        // max cavity depth
  float min_lake_area_m2 = 400.0f;   // prune smaller
  float min_lake_depth_m = 0.5f;     // prune shallower
  int dump_every = 10;               // loop dump cadence (0 = off)
  int detail_octaves = 4;
  float detail_wavelength_m = 60.0f; // largest octave
  float detail_amplitude_m = 2.0f;   // first-octave amplitude; worst-case
                                      // total carve ≈ 1.875x with 4 octaves
                                      // at 0.5 persistence
};
```

Sub-knobs (pad, shore fade, ε, gully weight, lacunarity, tie-break order)
stay `constexpr`.

## Performance

Per iteration: priority-flood O(n log n), everything else O(n). 512² × 80
iterations ≈ low seconds single-threaded — within budget. `parallel_tiles`
only where free (per-cell passes, detail filter tiles).

## Testing

Structural invariants only (no seed/order-dependent pinning), Catch2 next to
`generator_tests.cpp`:

- **Determinism:** `generate_map` twice → byte-identical artifacts (extends
  the existing test).
- **Routing:** tilted plane → receivers strictly downhill, column drainage
  area = texel count; flat plate → ε-drainage reaches border everywhere;
  synthetic bowl → correct spill level, per-lake water level uniform
  (max − min < ε).
- **Incision:** 1D chain vs tiny-timestep explicit reference (tolerance);
  property: no cell erodes below its receiver.
- **Layers:** sediment strips before bedrock; closed case: Σ Δ(B+S) +
  exported border flux ≈ 0.
- **Deposition:** flux into a flooded cell raises S at most to water level.
- **Cavities:** mask coverage ≈ lake_frac; depth ≤ lake_depth_m.
- **Lakes:** pruning respects min area/depth; W ≥ 0; every Lake biome texel
  has W > 0.
- **Detail filter:** |delta| ≤ octave amplitude sum; exactly 0 under water;
  deterministic.
- **Deliberately not pinned:** cross-resolution sim equality, lake counts per
  seed, perf timings (logged, not asserted).

## Deferred

- Swamp/Forest/stream placement from `flow`/`sediment`/`water_depth`.
- Hydraulics (shallow-water tier for local water effects).
- Detail-filter refinements: straight gullies, drainage streaks.
- Non-square maps/grids; GPU port; sim-grid parallelism beyond per-cell passes.

## v1.1 addendum (2026-07-24, post-preview review with the user)

Preview judging surfaced two lake defects (random puddle blobs; diagonal
hatched shallow lakes) and one design gap (dead-flat plains give drainage no
opinion). Decisions taken with the user:

### Plains drainage relief (replaces "plains sit flat at 0")

New base-height term applied EVERYWHERE (continuity — no seam at biome
boundaries):

```
relief += kPlainsReliefM · smoothstep(t_lake, t_hills, bedrock)   // 2 m
```

`t_lake` = the cavity quantile cutoff, `t_hills` = the plains/hills cutoff
(both on the sim grid). Rationale: cavities are carved at bedrock minima, so
the bedrock field is a ready-made potential whose valleys lead to the lakes —
plains gradients now point organically toward the cavities, and rivers + the
gully detail filter follow. Smoothstep has zero slope at both cutoffs (smooth
blend); above `t_hills` it saturates (+2 m constant), so hills/mountains
shift without a seam. Sediment fBm drops to a texture role
(`sediment_noise_m` 1.0 → 0.3).

### Capped micro-fill (init conditioning)

After sediment init, route the init surface once; every closed-depression
component with max fill depth ≤ `kMicroFillCapM` (0.75 m) that does not touch
the cavity mask is raised to its spill level (ε-tilted), credited to `S`.
Deeper depressions (real basins) and all seeded cavities stay untouched.
Kills noise-scale puddles at the source while preserving intentional lakes.

### Lake deposition spreading (fixes the diagonal hatching)

`deposit()` no longer drops flux at the lake entry cell (which built
1-texel delta stripes along D8 receiver chains). Per flooded component:
inflow is collected, then poured bottom-up (deepest cells first, level-set
fill) up to the water level; leftover volume continues from the component's
outlet down the receiver chain under the normal dry deposition rule.
Conservation (deposited + exported = eroded) is preserved.

### Lake stamp threshold

Biome Lake is stamped where `water_depth ≥ kLakeStampMinDepthM` (0.3 m), was
`> 0`. Artifacts keep full fidelity; the consistency invariant becomes
`Lake ⇔ W ≥ threshold`.

### Iterations / perf (user question, recorded)

Loop cost is linear in `iterations` (~21 ms/iter at -O2, ~190 ms/iter in the
Debug build, 512² sim). `iterations × dt` is the age product; the implicit
solver is unconditionally stable, so 40 × dt=2 ≈ 80 × dt=1 in total erosion
with coarser routing adaptation. Tuning previews compare both; the default
stays 80 × 1 pending user judging. Release-build/parallelism work: parked by
user decision.

## v1.2 addendum (2026-07-25, second preview review with the user)

### Conical lake bottoms (user-directed)

Cavity carve depth becomes an inverted cone mirroring the mountain relief at
one third its slope: `depth = (kSlopeMPerM / 3) · EDT(distance to nearest
non-basin texel)`, replacing the bedrock-value bowl `lake_depth_m · u²`.
`lake_depth_m` is retired (depth now scales with basin size, uncapped — the
same constant-slope philosophy as the mountains). Deeper centers also buy
silt headroom (v1.1 pour was filling shallow bowls to extinction: seed 1 lost
4 of 6 lakes, seed 3 all of them).

### Height-above-water detail fade (fixes the shoreline rim)

Preview transect evidence: the distance-based shore band (kShoreFadeDistM)
left a zero-carve shelf 0.03–0.27 m proud of carved surroundings — a visible
ring in hillshade around every lake. The fade becomes elevation-based, per
the originally-recorded follow-up: `fade = smoothstep(0, kShoreFadeHeightM
(2 m), ground − nearest_water_surface)`, with nearest_water_surface from a
bounded multi-source BFS (radius kShoreBfsTexels = 16) seeded at wet cells
carrying their surface elevation; beyond the radius fade = 1. Smooth, no
ring, grades detail to local base level, and cannot carve below an adjacent
lake surface. Distance-EDT shore fade is removed.

### Iteration default (user-approved)

`iterations` 80 → 40, `dt` 1.0 → 2.0 (same age product; A/B measured
visually indistinguishable: max heightmap delta 28/255, water mask 1 texel).
A 40/20/10 ladder (dt 2/4/8) is generated for judging whether to go lower.
