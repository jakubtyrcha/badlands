# src/mapgen/ — the map data contract, raster I/O, and the river chain

**There is no generator here any more.** The fBm bedrock field, the stream-power
erosion sim, the smoothing and canal passes and the preview-PNG dump were all
deleted; the name is now historical. What remains is the vocabulary every map
source speaks, the on-disk form, and the river network algebra.

- **`PatchRequest` → `PatchData` (`patch_data.hpp`, `patch_source.hpp`) is the
  one frozen interface here.** Three providers implement it: `SyntheticPatchSource`
  (analytic, no sim and no files — the styling fixture), `FilePatchSource` (a
  patch directory), `CoarseWorldPatchSource` (an arbitrary cut out of a cached
  coarse world). Adding a fourth should require touching nothing downstream.
- **`erosion.hpp` is only `ErosionParams`** now — a channel-hydraulics params
  block. `MapArtifacts`, `MapGenParams` and `generator.hpp` are gone.
- **This directory is not a pipeline stage.** All three procgen stages sit on
  it. See `docs/superpowers/specs/2026-08-02-procgen-stage-split-design.md`.
- **`badlands_mapgen_lib` LINKS NOTHING, and that is load-bearing.** Four test
  targets link it precisely because it drags in no Dawn/engine; they used to
  hand-compile these TUs to avoid exactly that. Do not add a link here without
  checking what it costs them.
- **Every `src/mapgen/*.cpp` that `tools/protogen/` compiles must stay
  buildable with bare `c++ -std=c++23 -I src -I third_party/glm`** — no
  engine, no spdlog, no taskflow. protogen is a standalone TU with its own
  hand-written build command (`tools/protogen/README.md`), not CMake; a TU it
  pulls in (currently `hydrology.cpp`, `river_graph.cpp`, `river_prune.cpp`,
  `coarse_io.cpp`, `river_io.cpp`) that starts needing something heavier
  breaks that command silently until someone tries it. Verify a new one with
  `c++ -std=c++23 -I src -I third_party/glm -fsyntax-only <file>.cpp` before
  adding it to the list.
- **Everything is in world METRES, never texels.** The same `(seed, size_m)` at
  two resolutions is the same map, just sharper. Keep every new parameter
  resolution-independent for the same reason.
- **Tests sit next to their sources** (`river_graph_tests.cpp`, …) and build into
  per-topic targets (`badlands_river_tests`, `badlands_patch_tests`,
  `badlands_patch_io_tests`, `badlands_artifact_io_tests`, …).
  Sources are listed explicitly in the root `CMakeLists.txt`, so a new test file
  that isn't added there compiles into nothing and silently never runs.
- **`badlands_game` does not use this path.** The game builds terrain from
  `src/game/map/symbolic_map_generator.*`, a hand-authored greybox map.

## The river chain, in order
`hydrology` (D8 routing + drainage) → `river_graph` (extraction, Manning
hydraulics) → `river_prune` (stage 1 culls by FLOW) → `river_clip` (stage 2 clips
to the patch rect, minting `FrameEntry`) → `river_prune` again (stage 2 culls by
LENGTH) → `river_arcs` (biarc refit,
carries curvature) → `river_carve` (the sub-texel cavity, sampled by the terrain
DAG through `TerrainDetailField`). `river_carve` is stage-3 by caller but library
by nature — it is a pure function of a graph and a heightfield.

## Running the tool
```sh
# a patch invented analytically -- no sim, no files. The styling loop.
./build/badlands_mapview --synthetic --patch-size 128 --patch-res 128

# a cut out of a cached coarse world (world.txt), at any size and resolution
./build/badlands_mapview --load <coarse-dir> --patch-size 128 --patch-res 128 \
                         --patch-origin 1696,2560

./build/badlands_mapview --load <patch-dir>  # a finished patch (map.txt)
./build/badlands_mapview --test-map          # the synthetic 128 m forest fixture
```

**The stage-2 loop is two commands, and they share one `Fetch`.** Export the
windows offline, then open the same origin in 3D:
```sh
./build/badlands_patch_export --load <coarse-dir> --windows <list.txt> \
    --patch-size 256 --patch-res 256 --out <out-dir> --dump-patch
./build/badlands_mapview --load <coarse-dir> --patch-size 256 --patch-res 256 \
    --patch-origin 5568,2176
```
- A window loaded as a coarse world and as a `--dump-patch` directory renders
  **byte-identically** in mapview — measured, and the reason stage 2 and stage 3
  need no separate fold.
- `badlands_patch_export`'s height PNGs autoscale **per image** and record the
  range in a sidecar; a shared range costs 7x the precision here. Flags and the
  decode formulas are in `src/executables/patch_export/README.md`.
- Exactly one of `--synthetic` / `--load` / `--test-map` is required. `--load`
  detects which kind of directory it was handed: `world.txt` means a coarse
  world, `map.txt` means a finished patch.
- **`--patch-size` and `--patch-res` are independent.** Raising the resolution
  at a fixed extent is how you get finer relief; it is a config change, not a
  mode.
- `--seed N` seeds foliage placement and the test map, not terrain.
- `--lod-tint N`: 0 shaded, 1 per-triangle position hash, 2 LOD level.
  `--serial-build` builds the cluster DAG single-threaded as a perf A/B — the
  DAG is bit-identical either way.

## Reviewing generated output
- Open **every** artifact before reaching a verdict on a map change; a single
  hillshade is not evidence.
- Present results as file paths or a table, not a viewer.
- **Compare screenshots with a tolerance, never a hash.** `badlands_mapview
  --screenshot` is mildly non-deterministic (measured: 54 of 1 474 560 px, max
  channel delta 2/255, across runs of one binary).
