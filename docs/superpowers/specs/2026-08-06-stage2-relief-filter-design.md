# Stage-2 relief filter — upscaler + high-frequency detailer (v1)

Date: 2026-08-06. Status: approved (brainstormed in-session; approach 1 of 3).
Parent: `2026-08-02-procgen-stage-split-design.md` §3.4, which designed this slot and
deliberately deferred it.

## 1. Purpose and assumptions

Stage 2 (`PatchRequest → PatchData`) today only Catmull-Rom-resamples the coarse
world: nothing smaller than one 16 m source cell exists in a patch. This feature adds
the deferred relief pass — a **stateless per-point erosion filter** after runevision's
"Fast and Gorgeous Erosion Filter" — so a patch gains pleasant high-frequency detail.
Assumptions fixed by the user:

- Large- and mid-frequency erosion, water flow, and biomes are **already baked into
  the stage-1 input**. Stage 2 adds detail; it re-simulates nothing.
- Stage 2 is FAST and optimizes FIDELITY.
- Stage-1 water quality is currently unsatisfying; that must not block stage-2 work —
  inputs may be adjusted artificially while stage 1 improves.

## 2. Scope (v1)

IN: heightmap upscale (existing resample) + octaved gully/ridge detail on dry land,
biome-styled, soil/slope-faded, deterministic, CPU.
OUT (explicitly deferred): river-edge carving, shoreline de-staircasing, synthetic
bathymetry (v1 only MASKS the filter off water); stage-2-owned geometry/Nanite pages
(acknowledged as the target architecture, separate sub-project); WESL/GPU mirror;
Swamp/Forest styles (stage 1 emits Plains/Hills/Mountain/Lake only).

## 3. Architecture

- New TU pair `src/mapgen/relief_filter.{hpp,cpp}` + shared header
  `src/mapgen/cubic_sample.hpp` (Catmull-Rom point sampling with analytic gradient,
  extracted from `coarse_world_patch_source.cpp`'s anonymous namespace). Engine-free;
  `badlands_mapgen_lib` still links nothing; bare-buildable rule holds.
- Public shape: `ReliefSample sample_relief_delta(const ReliefContext&, dvec2
  world_pos_m)` returning `{delta_m, grad}` — a pure function of (seed, world
  position); plus `apply_relief(...)`, the raster driver, tiled with
  `mapgen/parallel.hpp`'s `parallel_tiles`.
- `ReliefContext` carries const refs to the coarse bed/soil/biome/water-depth fields,
  `src_texel_m`, the world `seed` (from `CoarseManifest`), and fixed per-biome
  constants. No debug knobs, no config plumbing.
- Composition point: `CoarseWorldPatchSource::Fetch`, after the height/soil resample
  and **before** `ReconstructWater` — the water rebuild, `derive_water`, biome
  classification and river clip all see the detailed bed. `PatchData` is unchanged.
- **The filter returns a DELTA added onto `ResampleField`'s raster.** Filter-off is
  bit-identical to today's output; the separable raster path remains the single
  source of the base surface.

## 4. The filter

- 4–5 octaves of gradient-oriented gully stripes (extruded cos/sin pairs), wavelength
  halving per octave, evaluated on world-anchored rotated cell grids with per-cell
  pivots hashed from (seed, cell coords) — stateless, chunk-independent, so adjacent
  patches agree on shared borders by construction.
- Crisping per the article: straight gullies (sign-of-derivative), normalized cos/sin
  pairs, stacked fading — each octave sees the base gradient plus the analytic
  gradients of all previous octaves; nothing is finite-differenced.
- Base value+gradient come from `cubic_sample` — same node registration and
  clamped-border renormalization as `ResampleField`'s `CubicTaps`.
- **Fade target** (replaces the article's fragile altitude fade): f(soil depth,
  slope) with per-biome constant tables. Thin soil + steep ⇒ crisp dissected rock;
  deep flat soil ⇒ filter fades out. Stage-1 measurement backing this: bare/thin
  soil averages 31.6° slope vs 7.1° for deep soil.
- **Water mask:** bilinear-sampled coarse water depth > 0 ⇒ strength 0, smoothstep
  feather over a fixed shoreline distance in metres. `level`, lake extents, and
  bathymetry are never touched.
- **Octave band fixed in world metres**, inside the gap between the source Nyquist
  (2·16 m) and the output Nyquist: λ ≈ 24 m down to ~3 m, an octave active only when
  λ ≥ 2·out_texel_m. Resolution stays a pure config change (coarser requests reveal
  fewer octaves; Box/Crop branches get ~nothing); detail never re-states frequencies
  stage 1 already owns.
- All amplitudes and wavelengths are metres. No unitless 0..1 intensities.

## 5. Testing

New Catch2 target `badlands_relief_tests` (pattern: `badlands_patch_tests`), fixtures
hand-written, never simulated:

- Determinism (same ctx+pos ⇒ identical), overlap agreement between two requests at
  identical world positions (the seam guarantee), resolution cutoff (16 m texels ⇒
  ~zero delta), water mask exactness + monotone feather, fade physics (deep-flat ⇒ ~0,
  thin-steep ⇒ bounded nonzero), knob liveness (seed and every biome-table entry
  provably change output).
- Existing pure-resample assertions (e.g. the ramp NODE-registration test) get
  deep-soil fixtures so the fade zeroes the filter there, with a comment.

## 6. Validation & perf

- Iterate constants against the 8 shared cutouts (`~/repos/badlands-map-cutouts`,
  origins in `areas.txt`) at 256 m / 256–1024 px; A/B against the pre-filter dirs;
  mapview `--load` screenshots for 3D judgement.
- Perf: `apply_relief` timed at 2048², serial vs `parallel_tiles`, single reported
  number. Target: interactive patch extraction stays interactive.
