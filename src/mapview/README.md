# src/mapview/ — how a map reaches the screen

`badlands_mapview` gets its map through **one interface**, and everything below
it is interchangeable. There is no in-repo generator any more.

```
  STAGE 1                    STAGE 2                        STAGE 3
  tools/protogen/            PatchRequest -> PatchData       this directory
  cached on disk             the ONE frozen interface        a SINK

  coarse sim  ──► world.txt ──► CoarseWorldPatchSource ──┐
  (16 km, ~5 min)  rivers.bin                            │
                                                         ├─► PatchData ─► mapview
  a patch dir  ──────────────► FilePatchSource ──────────┤
                                                         │
  nothing at all ────────────► SyntheticPatchSource ─────┘
```

- **`--synthetic`** — a patch invented analytically: a valley, a river, a lake,
  at a realistic elevation. No simulation, no files, instant and deterministic.
  This is the styling loop, and it is why the rest of this document is no longer
  gated on a 5-minute sim.
- **`--load <coarse-dir>`** — an arbitrary cut out of a cached coarse world, at
  any `--patch-size` and `--patch-res`.
- **`--load <patch-dir>`** — a finished patch read off disk.

Stage 3 cannot tell them apart, and that is enforced rather than hoped for:
`map_view_view.cpp` compiles into a library that links the contract and **not**
the providers, so reaching around the interface fails to link. Full design in
`docs/superpowers/specs/2026-08-02-procgen-stage-split-design.md`.

## Stage 1 — the coarse world

`tools/protogen/` runs particle-based hydraulic erosion over **16 km at 1024²**
(16 m cells), ~5 minutes. Prototype, not in the build; see
`tools/protogen/README.md`. Its output is **self-describing**: `world.txt`
carries resolution, extent, the derived texel size, and the whole-world soil
quantiles used for biome cutoffs; `rivers.bin` carries the river graph extracted
over the whole world and culled by flow.

**No consumer hardcodes the coarse density**, and none takes it as a flag — it
is read from the manifest, which is what lets the cell size change later without
a silent misread.

## Stage 2 — cutting a patch

`CoarseWorldPatchSource` answers a `PatchRequest` (origin, extent, resolution)
against that world.

- **`world_size_m` and `resolution` are independent.** Raising the resolution at
  a fixed extent is a config change, not a mode — every parameter is in world
  metres, so the same request at two resolutions is the same terrain sampled
  more finely.
- **The upscale invents nothing.** A 2 km patch at 2048² is 128×128 source cells
  inflated 256×; there is no detail below the coarse cell size by construction.
  It is a low-frequency BASE for a later relief pass, not finished terrain.
- **Catmull-Rom to refine, area-average to coarsen.** Lanczos-3 does not
  reproduce a linear ramp (~6 cm error), and since a planar hillside *is* a
  linear ramp, that error is periodic with the source grid and shows up as
  corduroy in the shading — hillshade is a derivative, so it amplifies what looks
  negligible in the heightfield. In the other direction a reconstruction filter
  run backwards aliases, so coarsening area-averages instead.
- **There is no integral-cell constraint.** The request is in world metres and
  aligning to the coarse lattice is the provider's problem.
- **Rivers are clipped, rebased to patch-local metres, then culled by length** —
  never re-derived. Extracting from a window is what used to make rivers vanish
  below ~1 km; a 128 m window has no catchment to extract from.

Finding an interesting region is a **separate** job from cutting one:
`tools/protogen/select.cpp` scans a coarse world against the gameplay gates and
prints an origin to pass to `--patch-origin`. It is a discovery tool, run
occasionally, never in the iteration loop.

## The on-disk form

Two directories, one convention. A **coarse world** (stage 1's cached output)
and a **patch** (stage 2's, when it is written down) both use headerless rasters
plus a key/value manifest, so numpy reads them without a parser.

```
worlddir/                              patchdir/
  world.txt    resolution, extent,       map.txt      resolution, world_size_m,
               texel_m, seed, runoff,                 origin_m, source
               steps, soil cutoffs       height.f32   metres — the BED
  <tag>-height.f32  and the other        level.f32    metres — LAKE SURFACE
  <tag>-*.f32       snapshot rasters     biome.u8     mapgen::Biome
  rivers.bin   whole-world graph,        soil.f32     erodible cover
               culled by flow            rivers.bin   clipped graph
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
- **`soil.f32` and `rivers.bin` are optional ON DISK, required by the contract.**
  A directory written before either existed still loads — the field is present
  and correctly sized, just empty. A present-but-malformed file is still an
  error; only absence is tolerated, which is the same forward-compatibility rule
  the manifests apply to unknown keys.
- **Nothing is versioned.** Forward compatibility comes from ignored keys and
  tolerated absence, which has covered every change so far.

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
erosion rather than being imposed on it.

**The quantiles are taken over the WHOLE WORLD, once, and the two resulting
threshold values ride in `world.txt`.** Stage 2 applies them per patch. Taking
them over the patch instead — which the old tool did — makes the same ground
classify differently depending on what you cut, and fails the density- and
resolution-independence guarantees outright. Two floats keep classification
re-tunable in stage 2 without re-running the world.

## Rivers

**Extracted once over the whole coarse world, then clipped per patch.** Stage 1
runs `route_flow` → `accumulate_drainage` → `extract_river_graph` and culls by
FLOW; stage 2 clips the result to the patch rect, rebases it to patch-local
metres, and culls by LENGTH.

That division is the fix for the defect this whole split exists to remove. The
old code extracted the network **from the window**, and a small window has no
catchment to extract from — measured at one origin, only the cut size varying:

| patch | reaches |
|---|---|
| 2048 m | 146 |
| 1024 m | 22 |
| 512 m / 256 m / 128 m | **0** |

At 128 m the window received 0.092 m³/s of boundary inflow against 0.0005 m³/s
of local rain — a 180:1 ratio — and still produced nothing, because the prune
constants are sized for a 1–2 km cut. Boundary inflow (`A_in = Q / runoff`
seeded at the entry cell) existed only to paper over this and is gone with it.

- **Length cannot be culled in stage 1**, because "too short to bother with" is a
  question about a frame and stage 1 has no frame. Flow can, because it is
  physical and patch-independent.
- **A clipped trunk is not a headwater.** Clipping mints `RiverNodeKind::FrameEntry`
  on an inbound crossing — the inbound twin of `Mouth` — and both culls exempt
  it. Without that, a trunk merely *entering* a patch has `in_deg == 0`, reads as
  a stubby headwater, and gets eaten (the same failure once cost a 700 m trunk,
  peak Q 0.7183 → 0.0218 m³/s).
- The frame is still not a defect to fix. Water that leaves genuinely leaves; on
  the parent world it travels on and reaches a lake kilometres away, outside the
  patch.

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

- **A patch carries only as much detail as its coarse world had.** A 2 km cut is
  16,384 source samples at the current 16 m cell, so everything visible is 16 m
  or coarser. The relief pass that would fill the gap — a stateless per-point
  erosion FILTER composed onto the resampled bed, not a second simulation — is
  designed but deliberately not implemented yet (spec §3.4).
- **Screenshot comparison must use a tolerance, never a hash.**
  `badlands_mapview --screenshot` is mildly non-deterministic, and was before
  this work: three runs of one binary gave 54 differing pixels out of 1,474,560
  (0.0037%), max channel delta 2/255.
- **Lake bathymetry is not trustworthy.** Depths reach 250 m in a 2 km box on
  some windows. Harmless visually (past the visibility depth water reads the
  same) but wrong, and it constrains window selection — `--max-lake-depth` exists
  to steer around it.
- **The cluster DAG is resolution-sensitive.** 2048² is 11.9M vertices / 379 MB,
  over WebGPU's 256 MiB default `maxBufferSize`; `GpuContext` requests the
  adapter's own ceiling to cover it. 1024² is 94 MB and loads in 3 s rather
  than 14 s.
