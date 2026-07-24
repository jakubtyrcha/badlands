# High-level heightmap from distance-to-plains

**Date:** 2026-07-24
**Status:** approved

## Goal

Fill `MapArtifacts::heightmap` (all zeros since the biome-generation phase) with
a first, high-level relief: the initial landmass + ridge structure that later
detail, erosion, and water simulation will refine.

## Method (user-specified)

For each texel, compute the **exact Euclidean distance to the nearest Plains
texel** over the biome map, then map distance linearly to height:

```
d(x,y)    = EDT(biome == Plains), in WORLD METERS (per-axis texel scaling)
height    = kSlopeMPerM · d
```

**Units are world-metric, not texel-metric.** `d` is Euclidean distance in
world meters (the transform runs on the texel grid but each axis is scaled by
that axis's texel size in meters), and `kSlopeMPerM` is height meters per
meter of horizontal WORLD distance. Regenerating the same map at a different
resolution must not change the terrain's slopes or peak heights beyond
discretization error — a texel-unit slope would double all heights at half
resolution, which is exactly the bug class the units-guard test below pins.

- Plains stay flat at exactly 0 m (their distance is 0). 0 m is the water
  datum, per the established convention.
- The farthest texel from any plains is the highest point; elongated mountain
  belts get a crest along their medial axis — the ridge structure emerges from
  the distance field itself.
- Decisions taken with the user:
  - **Constant slope** (not normalized-to-peak): bigger landmasses get
    proportionally taller peaks; nothing rescales with map extent or seed.
  - **Linear profile** (no shaping exponent): uniform flank steepness is fine
    for this phase; erosion resculpts later (YAGNI).
- Named implication (accepted): height is purely distance-driven, so a large
  hills-only region can outrank a thin mountain ridge. Hills-only blobs become
  modest domes.

## Algorithm

Exact EDT via **Felzenszwalb–Huttenlocher** (O(n), two separable 1D
lower-envelope passes over squared distances: rows, then columns). Chosen over
chamfer (directional error → faceted flanks) and BFS/Dijkstra (non-Euclidean
octagonal artifacts). Deterministic; parallel per line on the existing worker
helper. Handles per-axis texel size, so distance is in world meters without
assuming square texels at the generator level.

## Interfaces

No public interface changes. `MapArtifacts` is unchanged — `heightmap` simply
stops being zeros. Internals exposed for unit testing (pattern of
`compute_cutoffs`):

```cpp
// generator.hpp
// Exact Euclidean distance (world meters) from each texel to the nearest
// texel whose biome == Plains. All-plains -> all zeros. Exposed for tests.
Field2D<float> distance_to_plains(const Field2D<uint8_t>& biome,
                                  glm::vec2 texel_m);
```

`generate_map` calls it after `classify_biomes` and writes
`heightmap = kSlopeMPerM * d`. `kSlopeMPerM` is a fixed constexpr (shipped:
0.75 per user direction — a texel 200 m inside a mountain complex peaks at
~150 m; dramatic first-pass relief, to be resculpted by erosion).

## Integration

- Mapview needs no changes to render relief: the cluster terrain, raycast
  picking, and fog base heights already consume `heightmap`.
- **Preview/judging tool:** `hillshade.{hpp,cpp}` moves back from
  `src/executables/patchgen/` into `src/mapgen/` (generic heightmap relief
  shading; patchgen keeps working since it links `badlands_mapgen_lib`), and
  `write_preview_images` adds `hillshade.png` — grayscale heightmaps are
  nearly unreadable for judging ridge structure; hillshade is the by-eye tool
  for the slope tuning loop.

## Edge cases

- All-plains map → heightmap all zeros.
- No-plains map: unreachable (quantile cutoffs guarantee ~55% plains); the
  EDT still degrades gracefully (infinite distance clamps are avoided by
  seeding from the actual plains set; if empty, return all zeros).
- Map borders are NOT treated as plains — distance is to actual plains texels
  only.

## Testing

- **EDT oracle:** `distance_to_plains` vs a brute-force O(n²) scan on small
  synthetic grids, compared in WORLD METERS, including a non-square-texel
  case — exact equality (power-of-two texel values make every double
  operation exact, so both sides reduce to the same float).
- **Units guard (world-metric slope):** the same world-space plains layout
  sampled at two resolutions yields distances that agree at coinciding world
  points within one coarse texel (boundary discretization); a texel-unit
  implementation would disagree by the resolution ratio and fail loudly.
- **Analytic cone:** a single plains texel yields the exact radial distance
  field.
- **Plains pinned to zero:** every Plains texel's height is exactly 0.
- **Determinism test updated:** it currently asserts heightmap == all zeros;
  it changes to asserting the two runs' heightmaps are byte-identical.
- **Deliberately NOT pinned:** cross-resolution heightmap equality (the
  discretized plains set differs per resolution; only bedrock keeps the exact
  world-sampling invariant).

## Deferred

- Profile shaping (convex exponent), detail noise on flanks.
- Erosion/hydraulics; water bodies below the 0 m datum; Forest/Swamp/Lake/River.
- Any biome-aware height modulation beyond the plains mask.
