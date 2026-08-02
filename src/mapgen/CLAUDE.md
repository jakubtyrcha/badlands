# src/mapgen/ — the map data contract, raster I/O, and the river chain

**There is no generator here any more.** The fBm bedrock field, the stream-power
erosion sim, the smoothing and canal passes and the preview-PNG dump were all
deleted; the name is now historical. What remains is the vocabulary every map
source speaks, the on-disk form, and the river network algebra.

- **`MapArtifacts` (`generator.hpp`) is a plain struct, not the output of a
  function.** Two things fill it today: `map_io.cpp` (rasters off disk, written
  by `tools/protogen/`) and the `--test-map` forest fixture. `erosion.hpp` is
  likewise only `ErosionParams` + `LakeInfo` — a params block and a record.
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
  per-topic targets (`badlands_river_tests`, `badlands_map_io_tests`, …).
  Sources are listed explicitly in the root `CMakeLists.txt`, so a new test file
  that isn't added there compiles into nothing and silently never runs.
- **`badlands_game` does not use this path.** The game builds terrain from
  `src/game/map/symbolic_map_generator.*`, a hand-authored greybox map.

## The river chain, in order
`hydrology` (D8 routing + drainage) → `river_graph` (extraction, Manning
hydraulics) → `window_rivers` (window clip + prune) → `river_arcs` (biarc refit,
carries curvature) → `river_carve` (the sub-texel cavity, sampled by the terrain
DAG through `TerrainDetailField`). `river_carve` is stage-3 by caller but library
by nature — it is a pure function of a graph and a heightfield.

## Running the tool
```sh
./build/badlands_mapview --load <dir>     # rasters on disk; geometry from map.txt
./build/badlands_mapview --test-map       # the synthetic 128 m forest fixture
```
- Exactly one of `--load` / `--test-map` is required. Resolution and world size
  come from the manifest — there are no `--resolution` / `--size` flags.
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
