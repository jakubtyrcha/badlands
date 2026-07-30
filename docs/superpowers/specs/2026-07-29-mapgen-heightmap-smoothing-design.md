# Mapgen: heightmap low-pass smoothing

## Goal

The generated heightmap reads as too "wrinkled". Add a low-pass filter over the
final output-resolution heightmap to soften it, with the amount tunable by eye
through the existing `--preview-image-only` loop.

## Where the wrinkle comes from

Measured by comparing the dumped rasters in `mapgen_out/` for seed 2:

- `loop-0010-height.png` (the eroded sim surface, before detail) is already
  fairly smooth. Its artifacts are faceted EDT creases and faint concentric
  terracing on the cone slopes — structure, not wrinkle.
- `08-detail-delta.png` is a dense crinkly carve covering every sloped texel:
  4 octaves from 60 m down to 7.5 m at 2 m amplitude. This dominates the
  wrinkled appearance of `hillshade.png`.

So the high-frequency wrinkle is overwhelmingly `gully_detail_delta`. The user
chose to smooth the **final result** anyway rather than reduce the detail
filter at source, accepting that river valleys and shorelines soften too.

A second, independent contributor was found while investigating this and will
get its own spec: `route_flow` assigns receivers by absolute elevation rather
than by gradient, so 96% of receivers run diagonally and erosion incises along
45° rays. Some of the terrain's directional structure is that artifact rather
than intended relief, so the smoothing constants below must be re-tuned once
that fix lands.

## Decisions taken with the user

- **Smooth the final result**, not the detail filter's source parameters, and
  not the sim surface. One filter, applied once, at output resolution.
- **Separable Gaussian with a strength dial**, not a bilateral / edge-preserving
  filter. Uniform softening is accepted; preserving canyon walls while erasing
  wrinkle was offered and declined as over-complex for the goal.
- **Recompute water depth after smoothing**, preserving the pre-smoothing water
  surface. Holding wet cells fixed, and leaving `water_depth` untouched, were
  both rejected.
- **Knobs live on a new `PostProcessParams` struct** on `MapGenParams`, not on
  `ErosionParams` and not as fixed `constexpr` in `generator.cpp`.

## Interface changes

New `PostProcessParams`, carried by value on `MapGenParams`:

```cpp
struct PostProcessParams {
  float smooth_sigma_m = 0.0f;   // Gaussian sigma in WORLD METERS; <= 0 disables
  float smooth_strength = 1.0f;  // 0 = passthrough, 1 = fully blurred
};

struct MapGenParams {
  uint32_t seed = 1;
  int resolution = 512;
  float world_size_m = 512.0f;
  ErosionParams erosion;
  PostProcessParams post;   // NEW
};
```

Defaults are the disabled state (`smooth_sigma_m = 0`), so adding the struct
changes no output until the tuned values are committed in a follow-up step.

New module `src/mapgen/smooth.{hpp,cpp}`, one pure function:

```cpp
// Separable Gaussian blur of `h`, sigma given in WORLD METERS and converted to
// texels via texel_m, then linearly mixed back toward the input by `strength`.
// Kernel truncated at 3 sigma; edges clamp-extend. Returns h unchanged (bit
// identical) when sigma_m <= 0 or strength <= 0.
Field2D<float> smooth_heightmap(const Field2D<float>& h, float texel_m,
                                float sigma_m, float strength);
```

Sigma is specified in meters, not texels, so the same params give the same
terrain at any `--resolution` — the resolution-independence invariant that the
existing units-guard test pins for `kSlopeMPerM`.

`ErosionParams` and `gully_detail_delta`'s signature are **unchanged**. The
`detail_*` fields arguably belong on `PostProcessParams` too, since they are
also an output-resolution post-process; migrating them is a separable
follow-up, deliberately out of scope here.

## Model

Insertion point is `generator.cpp`, between the detail add and the Lake biome
stamp (today lines 254 and 255):

```
resample h, S, flow, river to output grid          [unchanged]
water_depth = recompute vs output ground           [unchanged, lines 238-247]
heightmap += gully_detail_delta(...)               [unchanged, line 254]

--- NEW ---
surface  = heightmap + water_depth                 (flat inside each lake)
wet      = water_depth > 0
heightmap = smooth_heightmap(heightmap, texel_out,
                             post.smooth_sigma_m, post.smooth_strength)
water_depth[i] = wet[i] ? max(0, surface[i] - heightmap[i]) : 0
--- END NEW ---

stamp Lake where water_depth >= kLakeStampMinDepthM  [MOVED after the above]
```

**Why the surface is captured first.** The renderer derives the water surface as
`heightmap + water_depth`. `gully_detail_delta` already returns zero on wet
cells (`detail_filter.cpp:143`), so lake surfaces are flat today. Blurring the
ground under a lake without recomputing depth would make every lake surface
wrinkled — a visible bug. Capturing `surface` first and re-deriving depth from
it keeps each lake's surface exactly as flat as it was, because `surface` is
constant across a lake by construction. This is the same pattern already used
at `generator.cpp:238-247` for the sim→output resample.

**Accepted consequences:**

- Shorelines recede slightly and shallow lakes get shallower, because blurring
  rounds a concave bowl floor upward. A fringe that dries out is not stamped
  Lake, since the stamp moves after the recompute.
- `a.heightmap` no longer equals bedrock + `a.sediment` at output resolution.
  Nothing consumes that relation today.
- `flow`, `river`, `sediment`, `bedrock` and `biome` are data channels rather
  than geometry and are **not** smoothed.

**Implementation notes.** Two 1D passes over a scratch buffer, tile-parallel via
the existing `parallel_tiles` (pattern of `detail_filter.cpp`). Kernel weights
are normalized to sum to 1 so a constant field is preserved exactly. Edges
clamp-extend; `Field2D` has no wrap semantics anywhere else in mapgen.

## Debug sink

The existing `"final-height"` stage is dumped after the stamp, so it will
already show the smoothed result. To make the change inspectable we need the
**before** image as its own stage:

- `"pre-smooth-height"` — the heightmap after the detail add but before
  smoothing, emitted immediately before the new block. Paired with
  `"final-height"`, this gives a clean before/after in the preview strip.

This needs a one-line change in `outputs.cpp:161`: `pre-smooth-height` must join
`final-height` in the `out_relief` set so it is hillshaded at the output texel
size. Float stages that match no branch fall through to autoscaled grayscale
(`outputs.cpp:170`), which would make a heightmap nearly unreadable.

## Testing

New `src/mapgen/smooth_tests.cpp`, added to the `badlands_erosion_tests` target
alongside `detail_filter_tests.cpp`; `smooth.cpp` is added to
`badlands_mapgen_lib`, `badlands_generator_tests` and `badlands_erosion_tests`.

Unit tests on `smooth_heightmap`:

- `sigma_m <= 0` and `strength == 0` return the input bit-identically.
- A constant field is unchanged (kernel normalization).
- A single-texel spike well away from the borders spreads symmetrically about
  its origin, and the field sum is preserved to within float tolerance. The
  interior placement matters: clamp-extend edges do not conserve sum.
- Blurring is monotone in strength: a mid-strength result lies between the
  input and the full blur, per texel.
- Resolution independence: the same `sigma_m` applied at 256 and at 512 texels
  over the same world extent produces matching profiles when compared in world
  coordinates.

Integration tests in `generator_tests.cpp`:

- With smoothing enabled, every wet cell's water surface
  (`heightmap + water_depth`) is constant within its 4-connected lake
  component, and no `water_depth` is negative.
- With `smooth_sigma_m` explicitly set to 0, `generate_map` reproduces today's
  output exactly. Phrased against an explicit zero rather than "the defaults",
  so it keeps holding once tuned values are committed as the defaults.

## Picking the constants

Rather than guessing, generate seed 2 across a small grid —
`smooth_sigma_m ∈ {1, 2, 4}` × `smooth_strength ∈ {0.5, 1.0}` — and dump each
hillshade into `mapgen_out/sweep/`. The user picks from the images; the winner
is committed as the `PostProcessParams` default in a follow-up commit.

## Deferred

- Migrating the `detail_*` fields from `ErosionParams` to `PostProcessParams`.
- Edge-preserving (bilateral) smoothing, if uniform softening dulls the terrain
  more than the wrinkle removal is worth.
- Re-tuning the constants after the flow-routing fix lands.
