# src/mapgen/ — procedural map generation

A continuous "bedrock" latent field (low-frequency fBm + belt-masked ridged fractal)
classified into biomes by quantile cutoffs, then stream-power erosion, hydrology and
lakes. Consumed by `badlands_mapview` (`src/mapview/` turns it into cluster-LOD terrain
with one PBR material per biome, blended from a biome splat texture, plus still lake
water).

- **`generator.cpp` and the sim passes (`erosion`, `hydrology`, `smooth`) are pure functions of their params** — no I/O, no failure path. The I/O lives in `outputs.cpp` and `hillshade.cpp`, which write the preview PNGs; keep it there.
- **Noise is sampled in world METERS**, so the same `(seed, size_m)` at two resolutions is the same map, just sharper. Keep any new parameter resolution-independent for the same reason.
- **Tests sit next to their sources** (`generator_tests.cpp`, `erosion_tests.cpp`, …) and build into per-topic targets (`badlands_generator_tests`, `badlands_erosion_tests`, …). Sources are listed explicitly in the root `CMakeLists.txt`, so a new test file that isn't added there never runs.
- **`badlands_game` does not use this pipeline.** The game builds terrain from `src/game/map/symbolic_map_generator.*`, a fixed hand-authored greybox map; wiring mapgen into the game is unfinished work, not an existing path.

## Running the tool
```sh
./build/badlands_mapview --seed 2 --resolution 500x500 --size 500x500   # view it
./build/badlands_mapview --preview-image-only --out mapgen_out          # dump PNGs
```
- `--preview-image-only` runs the generator on the CPU (no window, no GPU) and dumps a numbered PNG per pipeline stage plus the preview rasters (bedrock/biome/heightmap/hillshade/flow/water/sediment), then exits.
- `--resolution WxH` is map texels and `--size WxH` is world metres; both are square-only (W must equal H).
- `--lod-tint N`: 0 shaded, 1 per-triangle position hash, 2 LOD level. `--serial-build` builds the cluster DAG single-threaded as a perf A/B baseline — the DAG is bit-identical either way.

## Reviewing generated output
- Open **every** artifact before reaching a verdict on a map change; a single hillshade is not evidence.
- Present results as file paths or a table, not a viewer.
