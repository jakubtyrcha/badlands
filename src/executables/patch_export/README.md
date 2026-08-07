# badlands_patch_export — the stage-2 iteration loop, offline half

Cuts windows out of a cached stage-1 world and writes them as images, through the
**same** `mapgen::CoarseWorldPatchSource::Fetch` `badlands_mapview` renders from.
One window, one stage-2 path, two ways to look at it.

```sh
# the whole set, plus mapview-loadable patch dirs
./build/badlands_patch_export --load build/worlds/w16-s1 \
    --windows build/worlds/w16-s1/windows.txt \
    --patch-size 256 --patch-res 256 --out /tmp/px --dump-patch

# the same window in 3D
./build/badlands_mapview --load build/worlds/w16-s1 \
    --patch-size 256 --patch-res 256 --patch-origin 5568,2176

# ...or straight off a dumped dir
./build/badlands_mapview --load /tmp/px/gorge
```

`--load` here takes **only** a coarse world (`world.txt`). A finished patch
directory is what this tool produces, not what it reads.

## Output, per window

| File | What |
|---|---|
| `<name>-height.png` | RGBA8. `R = G` = height code; `B = min(255, height + water)`; `A = 255` |
| `<name>-biome.png` | RGBA8 through `kBiomePalette`, the palette the 3D terrain's vertex colours use |
| `<name>-hillshade.png` | RGBA8. NW sun at 45°, flat water tinted by depth, river centrelines |
| `<name>-export.txt` | the sidecar: how the codes decode back to metres |
| `<name>/` | with `--dump-patch`: the patch_io directory (`mapview --load`-able, shareable) |

**Decode height:** `metres = height_range_m.lo + (R / 255) * (hi - lo)`, with the
range from the sidecar. **Decode water depth:** `metres = ((B - R) / 255) *
water_max_m`, exact wherever `R + water` did not saturate.

## Two range rules, and they differ on purpose

- **Height autoscales per image.** Sharing one range across a run sounds better
  and measures worse: over the eight w16-s1 windows the union spans 279 m, so a
  code step is 1.09 m, while the stage-2 relief being judged averages 0.116 m per
  texel — the entire signal disappears under one step. One window spans ~41 m, so
  a step is 0.16 m. Pass `--height-range LO,HI` when you want windows comparable
  instead of detailed; the run prints the union to hand back to it, and the
  sidecar always records what was used.
- **Water uses a fixed 8 m full scale.** Blue has to mean the same depth in every
  image ever exported, so it is never derived from a run's own maximum.

## Window lists

`--windows FILE` reads `name x y` per line (origin in world metres), `#`
comments and blank lines skipped. A malformed line is an error, not a skip — a
silently dropped window is a window you think you looked at.

```
# name        x      y
gorge       5568   2176
delta       8768  13568
```

`--window NAME=X,Y` is the same thing inline and repeatable; a bare
`--patch-origin X,Y` does one window called `patch`.

## Reading the printed table

Columns are name, origin, fetch ms, height min/max, wet %, and river node/edge
counts. **`fetch_ms` is only meaningful against a Release build** — the default
CMake configuration here is Debug, where a 256² fetch runs ~100 ms against a few
in an optimised build.

## Layering

The encoders (`src/mapgen/patch_export.hpp`) return pixel buffers and never write
a file; the PNG calls live in this tool's `main.cpp` alone. That is what keeps the
`assets` crate and the engine out of `src/mapgen/` — the mistake `outputs.cpp` and
`hillshade.cpp` were deleted for — and it is why `badlands_patch_export_tests` can
link `badlands_patch_providers` by itself. It also leaves room for a headless-GPU
renderer later: that replaces one encode function and touches nothing else.
