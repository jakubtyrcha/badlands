# src/mapview/ — how a map reaches the screen

`badlands_mapview` has **two ways** to get a map, and they are not variants of one
pipeline. They come from different generators and meet only at `MapArtifacts`.

```
  (A) generated          MapGenParams ──► mapgen::generate_map ──┐
                         (seed, size, erosion)                   │
                                                                 ├─► MapArtifacts ─► mapview
  (B) loaded    protogen ─► window.cpp ─► rasters ─► load_map ────┘
                (16 km sim)  (2 km cut)   on disk
```

- **(A) `--seed/--resolution/--size`** — the in-repo generator: bedrock fBm →
  quantile-cut biomes → stream-power erosion + lakes. Documented in
  `src/mapgen/CLAUDE.md`.
- **(B) `--load DIR`** — a window cut out of a world simulated *outside* this
  process. This is the direction the map work is moving, and it is where the
  terrain in recent screenshots comes from.

## The loaded path, end to end

**Phase 1 — geological simulation.** `tools/protogen/` runs particle-based
hydraulic erosion over **16 km at 1024²** (16 m cells), ~5 minutes. It produces
height, standing water, discharge and **soil** (erodible cover over bedrock).
Prototype, not in the build; see `tools/protogen/README.md`.

**Phase 2a — the gameplay window.** `tools/protogen/window.cpp` picks a square
out of that world and resamples it to the gameplay grid.

- Geometry is forced, not chosen: `out_res × out_texel_m` must be a whole number
  of source cells. 2048 × 1 m = 2048 m = exactly 128 cells at 16 m.
- **The upscale invents nothing.** A 2 km window is 128×128 source cells
  inflating to 2048² — **256 output texels per source sample**, so there is no
  detail below 16 m by construction. It is a low-frequency BASE for a later
  detail pass, not finished terrain.
- **Catmull-Rom, not Lanczos-3.** Lanczos does not reproduce a linear ramp (~6 cm
  error), and since a planar hillside *is* a linear ramp, that error is periodic
  with the source grid and shows up as corduroy in the shading. Hillshade is a
  derivative, so it amplifies what looks negligible in the heightfield.
- **Pin a window with `--origin-cell X,Y`, not `--rank N`.** Ranking is for
  discovery: the ordinal points somewhere else the moment the map or a gate
  changes. Ranked output prints the `--origin-cell` needed to pin it.

**Phase 2b — load and render.** `mapgen::load_map` (`src/mapgen/map_io.hpp`)
reads the directory into `MapArtifacts`; everything downstream is the generated
path's code unchanged.

## The on-disk form

```
mapdir/
  map.txt      manifest: resolution, world_size_m, source provenance
  height.f32   float32 metres — the BED, never the water surface
  level.f32    float32 metres — LAKE SURFACE elevation
  biome.u8     uint8, mapgen::Biome
  soil.f32     float32 metres of erodible cover (optional)
  inflows.txt  rivers crossing the boundary: texel_x texel_y discharge_m3_s
```

- **Water rides in the LEVEL raster, not a depth field.** `depth = max(0, level −
  height)`, and a dry texel stores `level == height`. No sentinel, no mask, and a
  lake surface is exactly flat by construction — the level is the per-lake
  constant, so no amount of bed detail can tilt it.
- **The bed keeps the den.** `height` is the lake bed, so a basin stays a basin
  through the resample.
- **The rasters are headerless**, so the manifest's element count is the only
  thing between a wrong resolution and a silently reinterpreted map. A mismatch
  is an error, never a guess.
- `--resolution`/`--size` are read from `map.txt`; passing them is unnecessary.

## Biomes come from the substrate

Not from elevation. protogen tracks bedrock and soil separately, and biomes are
cut on **quantiles of the dry soil distribution** — thinnest 12% to Mountain, the
next 33% to Hills — with wet texels going to Lake.

This is a measurement, not an aesthetic choice. On the 16 km run, from a uniform
4 m start, 40% of the map strips to bare rock, and the cover lands where the
physics puts it:

| soil | share | mean slope |
|---|---|---|
| bare/thin (< 0.5 m) | 53.6% | **31.6°** |
| deep (> 4 m) | 29.1% | **7.1°** |

A 4.4× slope separation, so "mountain = close to bedrock" falls out of the
erosion rather than being imposed on it. Quantiles rather than absolute depths
keep the split structural across windows.

## Rivers

`src/mapgen/window_rivers.hpp` routes the loaded window and extracts its network
(`route_flow` → `accumulate_drainage` → `extract_river_graph`).

- **A window is a cutout, so it needs boundary inflow.** Rivers cross into it
  carrying catchment it cannot see; each crossing converts to the upstream area
  it implies (`A_in = Q / runoff`) and is seeded at the entry cell. Staying in
  AREA units lets it ride the existing accumulation untouched.
- **A window is not automatically a catchment**, and this decides whether rivers
  reach the lake at all. Measured as the fraction of cells whose flow terminates
  in a lake rather than at the frame: one 2 km window scored **50%** and its
  rivers ran off the edge, while another scored **93%** and carried a 4.2 m trunk
  into the lake. The terrain is identical in kind; only the cut differs.
- The frame is not a defect to fix. Water that leaves genuinely leaves — on the
  parent map it travels on and reaches a lake kilometres away, outside the
  window. Padding the routing does **not** change this (measured: identical), and
  a window that drains outward will never show rivers reaching its own lake.

### From graph to geometry: arcs, a carve, and water in the cavity

Every reach is refitted as a chain of **circular arcs** (`src/mapgen/river_arcs.hpp`);
the arcs drive a **carve** into the terrain (`src/mapgen/river_carve.hpp`) and a
**water surface** inside it (`src/mapview/river_surface.hpp`). Three files because
the curve is not the cavity and the cavity is not the water — a flow field or a
bank spline would want the same curve and neither of the meshes.

- **Arcs, not a spline.** A meander *is* an arc — curvature is the quantity river
  geometry is written in — so the representation stores what you want to read.
  Offsetting to a bank is closed-form (same arc at `r ∓ w/2`), and arc length is
  exact, so parameterising by distance along the river is free. Biarc spans grow
  greedily under a 0.5-texel tolerance; fit quality is logged at load.
- **The carve is physical, not authored:** `cavity = 1.390·d_flow + 0.45·w^0.6`.
  Bankfull (the 1–2 yr flood, ~3× mean, `d ~ Q^0.3` ⟹ ×1.39) plus a bank cut
  whose exponent falls out of hydraulic geometry (`d ~ Q^0.3, w ~ Q^0.5` ⟹
  `d ~ w^0.6`); `k_bank` is the single free coefficient. The bed elevation comes
  from the CENTRELINE (running downstream minimum), never from local terrain, so
  a carved channel cannot run uphill. Compact support: outside the corridor the
  carved field returns the base surface **bitwise**.
- **The corridor is locally refined, not baked.** A 0.34 m-deep, 0.52 m-wide
  median cavity is sub-texel on the 1 m lattice, so the cluster DAG subdivides
  corridor quads 8× (0.125 m) through a *generic* per-quad detail field
  (`TerrainDetailField` — the DAG knows nothing about rivers; the adapter in
  `map_view_view.cpp` is the only place the two meet). Fan seams keep plain
  terrain bit-identical; measured on W7 at 3 m/yr: 35,722 corridor texels
  (0.85% of the map), DAG 31 s / 18.9M verts vs 14 s / 11.9M plain.
- **The water sits IN the cavity** at `bed + d_flow`, per cross-section, at TRUE
  width — no minimum-width floor anymore; the cavity, not a drawn stripe, is
  what makes a 0.5 m brook visible. It uses the lakes' still-water material;
  its extinction is calibrated to 2.5–10 m visibility, so brooks render nearly
  clear (accepted — the den does the work).
- The old debug-line layer and the drawn-to-width debug ribbon are both gone;
  what the lines carried and the carve does not is Strahler order.

## Known limits

- **The 2 km window is 16,384 source samples.** Everything visible is 16 m or
  coarser; the detail pass that would fix this does not exist yet.
- **Lake bathymetry is not trustworthy.** Depths reach 250 m in a 2 km box on
  some windows. Harmless visually (past the visibility depth water reads the
  same) but wrong, and it constrains window selection — `--max-lake-depth` exists
  to steer around it.
- **The cluster DAG is resolution-sensitive.** 2048² is 11.9M vertices / 379 MB,
  over WebGPU's 256 MiB default `maxBufferSize`; `GpuContext` requests the
  adapter's own ceiling to cover it. 1024² is 94 MB and loads in 3 s rather
  than 14 s.
