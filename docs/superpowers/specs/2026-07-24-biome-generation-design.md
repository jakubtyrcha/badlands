# Procedural biome generation (bedrock + quantile cutoffs)

**Date:** 2026-07-24
**Status:** approved

## Goal

Replace the current mapgen with a from-scratch procedural biome generator. The
authored-image map path is deleted outright. Biomes only in this phase — the
heightmap will be **derived from bedrock/biomes later** (erosion/hydraulics is a
future project that also brings Forest/Swamp/Lake/River back).

## Decisions taken (with the user)

- **Full reset of `src/mapgen`:** the noiser fields, voronoi, Whittaker biome
  assignment, height composition, AND the blocks→sections tail are all deleted.
  The section/ledge graph gets rethought later.
- **Viewer stays 3D:** mapview keeps the cluster-LOD terrain + camera, fed a
  flat (all-zero) heightmap with biome colors, so it picks up real heights
  later with no rework.
- **Noiser leaves mapgen only.** Patchgen, noise_texgen and the engine's
  `ScriptTextureProvider` keep it; the submodule stays.
- **Biome enum keeps all 6 values** (Lake, Swamp, Forest, Plains, Hills,
  Mountain); the generator only emits Plains/Hills/Mountain for now. No
  downstream churn; River still fits under the 8-slot shader cap later.
- **Approach: bedrock noise + quantile cutoffs, no CA.** A continuous latent
  "bedrock" field is primary (it is erosion's future input); biomes are a
  classification of it. Hills = the gradation band between the two cutoffs,
  not an explicitly placed biome. A label-CA relaxation pass stays in the back
  pocket if thresholded shapes don't read well.
- **Noise via FastNoiseLite** (MIT, single header, vendored into
  `third_party/`). Seeded, deterministic, has fBm + ridged fractal built in.

## Interface

```cpp
// mapgen/generator.hpp
struct MapGenParams {
  uint32_t seed = 1;
  glm::ivec2 resolution{512, 512};  // texels
  glm::vec2 size_m{512.0f, 512.0f}; // world meters
};

struct MapArtifacts {
  Field2D<float>   bedrock;    // latent field, ~[0,1] — erosion's future input
  Field2D<uint8_t> biome;      // Biome enum values (Plains/Hills/Mountain now)
  Field2D<float>   heightmap;  // world meters — all zeros this phase
};

MapArtifacts generate_map(const MapGenParams& params);  // pure, cannot fail
```

- **No error path.** The old `bool + err` existed only because noiser loaded a
  script file. Pure function of params, returns by value.
- **Noise is sampled in world meters**, so the same (seed, size) at two
  resolutions is the same map, just sharper. Resolution and size are genuinely
  independent inputs.
- **Square texels required:** `size.x/res.x == size.y/res.y`, validated at the
  CLI. The frozen `MapData` contract has one scalar spacing; anisotropic texels
  would need a MapData change, which is out of scope.
- Old `MapgenConfig` + JSON `--config` die. Generation tunables are fixed
  constants in the generator (per the working agreement: no knobs unasked).

## Generator internals

All constants fixed in code; wavelengths in meters.

```
belt    = smoothstep(perlin fBm, wavelength ≈ map extent)   // where mountain belts may exist
base    = perlin fBm, wavelength ≈ 250 m, ~4 octaves        // rolling continental base
ridged  = ridged fractal (1-|perlin|), wavelength ≈ 120 m   // elongated crest lines
bedrock = base + kRidgeWeight · belt · ridged
```

- The ridged term makes thresholded peaks come out as prolonged ridge forms.
- The belt mask keeps mountains to a few belts per map; where belt ≈ 0 the base
  alone rarely crosses the mountain cutoff.
- Layers use distinct seeds derived from `params.seed`; evaluation runs in
  parallel tiles via the existing `parallel.hpp` helper.

Classification — quantiles over the actual bedrock raster (histogram ~1024
bins), so the proportions are structural, not luck:

```
t_hills    = quantile(bedrock, kPlainsFrac ≈ 0.55)
t_mountain = quantile(bedrock, 1 − kMountainFrac ≈ 0.88)
biome(x,y) = bedrock < t_hills ? Plains : bedrock < t_mountain ? Hills : Mountain
```

Hills fall out as the continuous band between the cutoffs — wrapping mountain
belts and capping the higher base swells.

The heightmap output is zeros; bedrock is deliberately not scaled into meters
yet. Deriving height (and hydrology biomes) is the next project.

## Demolition & integration

Deleted from `src/mapgen/`: `fields.*`, `noiser_util.hpp`, `voronoi.*`,
`biome_assign.*`, `heightmap.*`, `sections.*`, `authored_map.*` +
`authored_map_tests.cpp`, `pipeline.*`, `config.*`, `mapgen_constants.hpp`,
plus their tests in `mapgen_tests.cpp`.

Deleted elsewhere: `assets/map/` (both LFS PNGs + `map_meta.json`),
`scripts/mapgen/map_from_render.py`, `scripts/mapgen/fields.noiser`.

Moved, not deleted: `script_eval.{hpp,cpp}`, `patch_eval_tests.cpp`,
`hillshade.*` move to patchgen's corner (its noiser harness); `src/mapgen` ends
up noiser-free. `scripts/mapgen/biomes/` stays.

Survives in `src/mapgen/`: `field2d.hpp`, `parallel.hpp`, `outputs.cpp`
(trimmed: biome/gray writers stay; sections/voronoi/hashed writers go),
`fog_generator.*` untouched.

New: `generator.{hpp,cpp}`, vendored `third_party/FastNoiseLite.h`.

Mapview:

- `--map`, `--map-region`, `--config` go; `--size WxH` (meters) arrives.
- `MakeOneHotMapData` stays; spacing computed from `size_m/resolution` instead
  of the old compile-time 1 m/sample. Terrain renders flat, biome-colored.
- Hover block/section grid overlay and section stats logging are deleted with
  their data. Camera, daylight, fog, screenshot/record stay.
- `--preview-image-only` dumps `bedrock.png`, `biome.png`, `heightmap.png`.
- Fog generator keeps being called: zero emitters until Forest/Swamp return;
  border fog unaffected. **Noted debt:** `BiomeFogParams` assumes 1 m/texel —
  moot now, revisit when vegetation biomes come back.

## Testing

Mechanisms, not sampled outcomes:

- **Determinism:** same `MapGenParams` twice → byte-identical bedrock + biome.
- **Quantile fractions:** a couple of seeds → plains/mountain area fractions
  within a small tolerance of the constants.
- **Classification:** synthetic bedrock raster → exact expected labels.
- **Meters-based sampling:** same seed/size at two resolutions → bedrock agrees
  at coinciding world points.
- No pinning of shapes/seeds; ridge "look" is judged by eye via previews.

## Deferred

- Heightmap derivation from bedrock/biomes; erosion/hydraulics simulation.
- Forest/Swamp/Lake/River biomes (from hydrology); River enum value.
- Section/ledge graph replacement.
- Fog generator's 1 m/texel assumption.
- CA relaxation pass (only if thresholded shapes need it).
