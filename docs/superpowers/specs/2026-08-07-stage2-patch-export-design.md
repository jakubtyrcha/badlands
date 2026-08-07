# Stage-2 patch export: a durable offline image loop beside the mapview 3D view

Date: 2026-08-07
Status: approved, implementation on `feat/stage2-patch-export`

## 1. Why

Stage 2 (`PatchRequest → PatchData`) upscales a coarse stage-1 world and adds
high-frequency relief. Looking at its output offline currently means a pile of
throwaway scratchpad drivers — an ad-hoc extraction `.cpp`, a python hillshade
renderer, a debug dumper that `#include`s a production TU. None of them survive a
session, and both stage 1 and stage 2 need many more iterations.

The iteration loop itself has to become durable: **one in-repo command turns a
stage-1 dump plus a list of windows into exported images, and the same window
opens in mapview for a 3D check.**

## 2. What was verified first (and what it removed from scope)

The stated goal was "let mapview operate on the slice, effectively folding stage 2
and stage 3". Measurement shows that fold already exists:

- The same window loaded as a **coarse world** (`--load build/worlds/w16-s1
  --patch-origin 5568,2176 --patch-size 256 --patch-res 256`) and as a **dumped
  patch dir** renders **byte-identically** (1 087 540 B both). `Fetch` → `MapData`
  → cluster DAG → render is one path with two front doors.
- The gorge window's apparent flatness in 3D is real terrain, not a defect: it is
  a 9.5 % ramp plus 3.45 m rms residual over 256 m. The offline hillshade looks
  dramatic only because it shades normals.
- The delta window renders biome, relief and lake water correctly.

**Therefore this work changes no mapview code.** It builds the missing offline
half and pins the encoders with tests.

One artifact was observed and deliberately left alone: small lakes show hard 16 m
staircase shores, the coarse lake footprint showing through stage 2's mask-only
water handling. Making such things visible is what this tool is for; fixing them
is a separate piece of work.

## 3. Exports

Three images plus a sidecar per window.

### 3.1 `<name>-height.png` — height with the watermap layered on top

One RGBA8 PNG, written through the existing `badlands_write_png`:

```
R = G = height code, linear over [range.lo_m, range.hi_m], clamped
B     = clamp(height code + water code, 0, 255), water linear over [0, water_max_m]
A     = 255
```

Dry ground reads as neutral gray; water reads blue in proportion to depth. The
water code is recovered exactly as `B - R`, and metres come back through the
sidecar's range.

**8-bit is a deliberate choice, not an oversight.** A 16-bit encoding was
considered — the assets crate does have `badlands_write_png16` — and rejected:
its writer is single-channel, so water could not share the file, and 0.16 m per
code step over a 256 m window is good enough for a preview. The detailer is meant
to add *larger* features anyway; high frequency is about wavelength, not
amplitude.

**The range is shared across a batch, not per image.** Per-image autoscale
silently makes windows non-comparable, which is the exact failure mode the
deleted `write_gray_png_range` carried a warning about. The default is the union
of every fetched window's `elevation_range`; `--height-range LO,HI` forces it.

### 3.2 `<name>-biome.png` — the biome map

RGBA8 through `mapgen::kBiomePalette` (`src/mapgen/biomes.hpp`), which is the same
palette the cluster terrain's per-vertex colour uses
(`src/game/geometry/terrain_clusters.cpp`). The exported biome map therefore
matches the 3D view byte-for-byte. Out-of-range values clamp to opaque black
rather than indexing past the palette.

### 3.3 `<name>-hillshade.png` — the judgement image

RGBA8: Lambert shading on the analytic height gradient at `patch.texel_m` with a
fixed NW/45° sun and an ambient floor, water tinted by depth, and river
centrelines rasterized from each `RiverEdge::points_m` polyline. This is the
shape the deleted `compute_hillshade` had and what `tools/protogen/show.py`
reproduces today.

It ships because the raw heightmap does not read as terrain — gully and ridge
quality is what these iterations are about, and only a shaded image shows it.

### 3.4 `<name>-export.txt` — the sidecar

`key value` per line, the same form as `map.txt` and `world.txt`: `resolution`,
`world_size_m`, `origin_m`, `texel_m`, `height_range_m LO HI`, `water_max_m`,
`source`. Without it the codes are undecodable back to metres.

## 4. Structure

Two units, split so the dependency stays where it belongs.

### 4.1 `src/mapgen/patch_export.{hpp,cpp}` — pure encoders

Buffer-returning functions only: **no PNG, no file I/O, no `assets` crate, no
engine**.

```cpp
struct ExportRange { float lo_m = 0.0f, hi_m = 0.0f; };

std::vector<uint8_t> encode_height_water_rgba(const Field2D<float>& height,
                                              const Field2D<float>& water_depth,
                                              ExportRange range, float water_max_m);
std::vector<uint8_t> encode_biome_rgba(const Field2D<uint8_t>& biome);
std::vector<uint8_t> encode_hillshade_rgba(const PatchData& patch);
```

This split is the fix for a mistake the build system already records: `outputs.cpp`
and `hillshade.cpp` once made `badlands_mapgen_lib` link `badlands_engine` for
`CpuImage`'s PNG write, and were deleted for it. Keeping encoding pure means the
encoders are unit-testable with no Rust crate and no GPU, and `src/mapgen/` stays
bare-buildable for `tools/protogen/`.

The module belongs in `src/mapgen/` for the same reason `patch_io.cpp` does: it is
another serialization of the frozen contract.

### 4.2 `src/executables/patch_export/main.cpp` → `badlands_patch_export`

The driver, and the only place a concrete `PatchSource` is chosen. Headless — no
SDL, no Dawn.

```
badlands_patch_export --load DIR [--tag NAME]
                      [--patch-size M] [--patch-res N]
                      (--patch-origin X,Y | --window NAME=X,Y ... | --windows FILE)
                      --out DIR
                      [--layers height,biome,hillshade]
                      [--height-range LO,HI] [--dump-patch]
```

Flags that overlap mapview keep mapview's exact names and meanings. Windows come
as repeatable `--window NAME=X,Y` or a `--windows FILE` of `name x y` lines;
`--dump-patch` additionally writes each window as a patch_io directory, which is
the mapview-`--load`-able and shareable form. stdout is one table row per window:
name, origin, fetch ms, height min/max, wet %, river nodes/edges.

PNG writing is confined to this translation unit, which is what keeps
`src/mapgen/` free of the `assets` dependency.

### 4.3 Why the split also serves a headless GPU later

The tool is fetch → encode → write. Only the encode step is presentation, so a
headless-GPU renderer replaces one function behind the same signature and leaves
the CLI, the batch loop and the patch dumping untouched.

## 5. Tests — `badlands_patch_export_tests`

Catch2 amalgamated, linking only `badlands_patch_providers`; hand-written
fixtures, no sim. The encoders having no `assets` dependency is what makes this
target possible, so it is also the test that the split held.

- Height ramps map to linear code ramps in R and G; `lo`→0, `hi`→255;
  out-of-range clamps; a degenerate range yields flat mid-gray, not a divide by
  zero.
- **The blue-channel pin:** a dry texel has `B == R`, a wet one has `B > R`, and
  `B - R` recovers the water code exactly.
- Water saturation: a depth at `water_max_m` saturates blue without wrapping;
  `water_max_m <= 0` leaves `B == R` everywhere.
- Biome: each `Biome` maps to its palette entry byte-for-byte with alpha 255; an
  out-of-range value does not read past the palette.
- Hillshade: a flat field is uniform; on a ramp the away-facing slope is darker
  than the sun-facing one; a wet texel is water-tinted where its dry neighbour is
  not; a river edge paints its centreline.
- Every buffer is `width * height * 4` bytes.
- Round-trip: `write_patch` output reloaded through `FilePatchSource` matches the
  direct fetch's height field.

## 6. Out of scope

Window *picking* (the eight origins stay hand-authored); `--synthetic` source
support; relief-filter internal debug layers; the staircase lake shores and any
other stage-1/stage-2 water-quality fix; any mapview code change; a headless-GPU
renderer.

## 7. Recorded for the next iteration

**The detailer should add larger features.** High frequency means short
wavelength, not small amplitude, and today's relief filter reads as fine ripples
on a smooth base. That is a `relief_filter.cpp` amplitude/wavelength change, and
it wants this export tool to exist first — judging it is exactly what the tool is
for.
