# mapview Terrain Materials + Standing Water — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give `badlands_mapview` per-biome PBR terrain materials and still, murky, depth-graded lake water.

**Context / why:** `badlands_mapview` renders the generated map as cluster-LOD terrain whose albedo is a flat per-vertex biome colour, and renders no water at all — lakes read as dry basins. The 21 PBR packs under `assets/materials/` and the biome→pack manifest (`assets/materials/terrain_biomes.json` + `ResolveBiomePacks`) are already in the repo and unused by the map tool, and the generator already produces real standing water (`water_depth`, `lake_id`, `LakeInfo::level_m`) that nothing consumes. Approved design: `docs/superpowers/specs/2026-07-31-mapview-terrain-materials-and-standing-water-design.md`.

**Architecture:** Terrain biome weights come from a CPU-built **splat texture** sampled by world XZ in the fragment stage — LOD-independent, so the cluster DAG and its vertex format are untouched. The existing `terrain_blend.wesl` blend loop is extracted to a shared WESL module, gains height-lerp blending driven by displacement packed into the ARM array's alpha, and is then used by both terrain materials. Water is a per-lake flat mesh with a skirt buried under the terrain, shaded by an upgraded `water.wesl` that replaces its absorption/coast scalars with a per-channel Beer–Lambert medium.

**Tech Stack:** C++20, Dawn/WebGPU, WESL→WGSL (naga reflection via the `wesl` Rust crate), EnTT, glm, Catch2, CMake+Ninja.

## Global Constraints

- Run every command from the repo root — `shaders/` and `assets/` resolve relative to cwd.
- Build with `scripts/build.sh [target …]`; test with `scripts/test.sh [regex]`; screenshot with `scripts/screenshot.sh <app> <out.png> [args…]`.
- Reversed-Z is project-wide: depth clears to `0.0`, opaque compare is `GreaterEqual`. Do not "fix" depth comparisons.
- Pipelines use **explicit reflection-derived** bind-group layouts. A new texture binding must be registered in `src/engine/rendering/material/material_requirements.cpp` or the slot will not resolve.
- Material textures resolve by `param_name == slot_name`.
- No new ImGui panels, sliders, toggles, env-var hooks, or config structs. Every tuning value in this plan is a named compile-time constant or a fixed per-instance preset.
- Engine layers (`src/engine/`, `src/core/`) stay game-agnostic — no biome/lake types.
- Binary assets are git LFS. This plan adds none.
- Commit after each task. Do not bump the pinned Dawn SHA.
- Constants introduced by this plan, verbatim:
  - `kBiomeBlendM = 3.0f` (metres, splat blur radius)
  - `kMinLayerWeight = 1.0 / 255.0` (shader layer cull)
  - `TERRAIN_HEIGHT_BLEND_DEPTH: f32 = 0.2` (height-lerp transition width)
  - `kBurialM = 0.6f`, `kMaxSkirtM = 6.0f` (metres, water skirt)
  - `kSkyBehindDepthM: f32 = 32.0` (water path length when nothing is behind)

---

## File Structure

**Created**
- `src/mapview/biome_splat.hpp` / `.cpp` — CPU biome→splat raster builder. Pure (`mapgen::Field2D` + `<vector>`), no engine deps.
- `src/mapview/tests/biome_splat_tests.cpp` — Catch2, pure CPU.
- `src/mapview/lake_surface.hpp` / `.cpp` — CPU per-lake water surface + skirt builder. Pure (`mapgen` + glm).
- `src/mapview/tests/lake_surface_tests.cpp` — Catch2, pure CPU.
- `shaders/common/terrain_layers.wesl` — the shared layer-blend module.

**Modified**
- `shaders/material/terrain_blend.wesl` — delegates to the shared module.
- `shaders/material/terrain_cluster.wesl` — samples the splat + the shared module.
- `shaders/material/water.wesl` — Beer–Lambert medium + `still` feature.
- `src/engine/rendering/texture_loader.hpp` / `.cpp` — `CopyRedIntoAlpha` helper.
- `src/engine/rendering/material_library.hpp` / `.cpp` — displacement→ARM alpha; `shared_sampler()` accessor.
- `src/engine/rendering/material/material_requirements.cpp` — `terrain_cluster` texture slots.
- `src/engine/rendering/water_material.hpp` / `.cpp` — new uniforms, still factory, presets.
- `src/game/map/cluster_terrain.hpp` / `.cpp` — takes terrain arrays + splat views.
- `src/executables/mapview/map_view_view.hpp` / `.cpp` — MaterialLibrary, splat upload, water entity.
- `src/engine/tests/terrain_blend_tests.cpp` — height-blend GPU case.
- `game/tests/water_gpu_tests.cpp` — new uniform names.
- `CMakeLists.txt` — two new test targets + new mapview sources.

---

### Task 1: Measure lake bathymetry

The spec's σ calibration depends on how deep the generator's lakes actually are. Measure before choosing constants. This task changes no rendering.

**Files:**
- Modify: `src/executables/mapview/map_view_view.cpp` (after `map_ = mapgen::generate_map(params_);`)

**Interfaces:**
- Consumes: `mapgen::MapArtifacts::lakes` (`std::vector<LakeInfo>`, each with `level_m`, `area_m2`, `max_depth_m`, `kind`), `MapArtifacts::water_depth` (`Field2D<float>`).
- Produces: measured numbers recorded in the Task 8 commit message; no code interface.

- [ ] **Step 1: Add the one-shot log**

Insert directly after the `log_step("mg:generate", since(t));` line in `MapViewView::Initialize`:

```cpp
  // Lake bathymetry, logged once: the water material's extinction coefficients
  // are derived from a visibility depth in metres, so the depth distribution
  // the generator actually produces is a load-bearing input, not trivia.
  {
    std::vector<float> depths;
    depths.reserve(map_.lakes.size());
    for (const mapgen::LakeInfo& l : map_.lakes) depths.push_back(l.max_depth_m);
    std::sort(depths.begin(), depths.end());
    int wet = 0;
    for (float d : map_.water_depth.data) {
      if (d > 0.0f) ++wet;
    }
    const float wet_frac =
        map_.water_depth.data.empty()
            ? 0.0f
            : static_cast<float>(wet) /
                  static_cast<float>(map_.water_depth.data.size());
    if (depths.empty()) {
      spdlog::info("lakes: none (wet {:.2f}%)", 100.0f * wet_frac);
    } else {
      spdlog::info(
          "lakes: {}  max_depth_m min/median/max = {:.2f}/{:.2f}/{:.2f}  "
          "wet {:.2f}%",
          depths.size(), depths.front(), depths[depths.size() / 2],
          depths.back(), 100.0f * wet_frac);
    }
  }
```

- [ ] **Step 2: Build**

Run: `scripts/build.sh badlands_mapview`
Expected: `BUILD OK`

- [ ] **Step 3: Measure across seeds**

Run:
```bash
for s in 1 2 3 4 5; do
  perl -e 'alarm 240; exec @ARGV' ./build/badlands_mapview \
    --preview-image-only --seed $s --resolution 512x512 --size 512x512 \
    --out /tmp/mg_$s 2>&1 | grep '^\[.*lakes:'
done
```
Expected: five `lakes: N  max_depth_m min/median/max = …` lines.

- [ ] **Step 4: Record the numbers and pick visibility depths**

Write the five lines into the commit message. Then set the water constants used in Task 7 by this rule:

- Let `D` = the **median** `max_depth_m` across all seeds.
- Blue visibility depth `d_vis.b = 2.0 * D` (the deepest water is roughly at the murk asymptote).
- Green `d_vis.g = 1.4 * D`, red `d_vis.r = 0.5 * D` (red dies first — this is what turns the hue toward blue with depth).
- `sigma_e = vec3(3.0 / d_vis.r, 3.0 / d_vis.g, 3.0 / d_vis.b)`, clamped to a maximum of `8.0` per channel.

If `D < 0.4 m`, stop and report to the user: the spec (§C.4) names this as a real risk, and the fix is a generator change (freeboard / seeded cavity depth) that is out of scope here.

- [ ] **Step 5: Commit**

```bash
git add src/executables/mapview/map_view_view.cpp
git commit -m "feat(mapview): log lake bathymetry once at load

The water material's extinction is derived from a visibility depth in
metres, so the generator's actual lake depth distribution is an input to
the calibration rather than trivia. Measured across seeds 1-5:
<paste the five lines here>"
```

---

### Task 2: Pack displacement into the ARM array's alpha

Height blending needs a per-layer displacement value. Rather than a fourth texture array (extra VRAM, extra binding, extra fetch per layer), fold each pack's `disp` red channel into the unused alpha of its ARM texture. Every pack's `material.json` already declares `"displacement"`; `LoadPack` just never reads it.

**Files:**
- Modify: `src/engine/rendering/texture_loader.hpp`, `src/engine/rendering/texture_loader.cpp`
- Modify: `src/engine/rendering/material_library.cpp` (`LoadPack`, ~line 391)
- Test: `src/engine/tests/texture_array_tests.cpp`

**Interfaces:**
- Consumes: `UploadTexture2DWithMips(device, queue, pipeline_gen, w, h, rgba)` and `ImageGuard` (both `engine/rendering/texture_loader.hpp`); `badlands_image_decode` via the same path `LoadTexture2D` uses.
- Produces: `void CopyRedIntoAlpha(uint8_t* rgba, const uint8_t* src_rgba, size_t pixel_count)` in `namespace badlands`. After this task, the ARM layer of `MaterialLibrary::TerrainArrays` carries displacement in `.a` for every pack that declares one, and `1.0` where it does not.

- [ ] **Step 1: Write the failing test**

Append to `src/engine/tests/texture_array_tests.cpp`:

```cpp
TEST_CASE("CopyRedIntoAlpha moves the source red channel into the destination alpha",
          "[texture]") {
  // 2 pixels: dst is an ARM texel (AO, roughness, metal, unused alpha),
  // src is a displacement texel (grayscale, so R carries the height).
  std::vector<uint8_t> dst = {200, 100, 0, 255,
                              10,  20,  30, 255};
  const std::vector<uint8_t> src = {64, 64, 64, 255,
                                    192, 192, 192, 255};

  CopyRedIntoAlpha(dst.data(), src.data(), 2);

  // RGB is untouched; alpha becomes the source red.
  CHECK(dst[0] == 200);
  CHECK(dst[1] == 100);
  CHECK(dst[2] == 0);
  CHECK(dst[3] == 64);
  CHECK(dst[4] == 10);
  CHECK(dst[5] == 20);
  CHECK(dst[6] == 30);
  CHECK(dst[7] == 192);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `scripts/build.sh badlands_texture_array_tests`
Expected: FAIL — `use of undeclared identifier 'CopyRedIntoAlpha'`

- [ ] **Step 3: Declare and implement the helper**

In `src/engine/rendering/texture_loader.hpp`, after `UploadTexture2DWithMips`'s declaration:

```cpp
// Copies `src_rgba`'s RED channel into `rgba`'s ALPHA channel, in place, for
// `pixel_count` tightly-packed RGBA8 pixels. Both buffers must hold at least
// `pixel_count * 4` bytes and describe the same image size.
//
// This is how a PBR pack's grayscale displacement map rides along in the unused
// alpha of its ARM texture, so height-blended terrain costs no extra texture
// array, binding, or fetch. Pure CPU and side-effect free so it is unit
// testable without a GPU (the same reason LoadTexture2D's DX->GL green flip is
// a CPU step).
void CopyRedIntoAlpha(uint8_t* rgba, const uint8_t* src_rgba,
                      size_t pixel_count);
```

In `src/engine/rendering/texture_loader.cpp`:

```cpp
void CopyRedIntoAlpha(uint8_t* rgba, const uint8_t* src_rgba,
                      size_t pixel_count) {
  for (size_t i = 0; i < pixel_count; ++i) {
    rgba[i * 4 + 3] = src_rgba[i * 4 + 0];
  }
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `scripts/test.sh badlands_texture_array_tests`
Expected: PASS

- [ ] **Step 5: Read the manifest's displacement key in LoadPack**

In `src/engine/rendering/material_library.cpp`, inside `LoadPack`'s manifest parse block, add alongside `arm_rel`:

```cpp
    // Optional: a pack without a displacement map keeps ARM alpha at 255,
    // which the height-lerp reads as a uniform height (i.e. a plain weighted
    // blend for that layer).
    disp_rel = manifest.value("displacement", std::string());
```

declaring `std::string disp_rel;` next to `arm_rel`.

- [ ] **Step 6: Merge displacement into the ARM upload**

Replace the `result.arm = LoadTexture2D(...)` line in `LoadPack` with:

```cpp
  if (disp_rel.empty()) {
    result.arm = LoadTexture2D(device_, queue_, *pipeline_gen_, arm_path);
  } else {
    // Decode ARM and displacement, fold disp.r into arm.a, then upload once so
    // the mip chain is generated from already-merged data (same rationale as
    // LoadTexture2D's CPU green flip: mips must come from final pixels).
    const std::string disp_path = dir + "/" + disp_rel;
    ImageGuard arm_img{badlands_decode_image(arm_path.c_str())};
    ImageGuard disp_img{badlands_decode_image(disp_path.c_str())};
    if (arm_img.image.rgba == nullptr || disp_img.image.rgba == nullptr) {
      spdlog::error("MaterialLibrary: pack '{}' failed to decode arm/disp", dir);
      load_failed_ = true;
      return result;
    }
    if (arm_img.image.width != disp_img.image.width ||
        arm_img.image.height != disp_img.image.height) {
      spdlog::error(
          "MaterialLibrary: pack '{}' arm {}x{} != displacement {}x{}", dir,
          arm_img.image.width, arm_img.image.height, disp_img.image.width,
          disp_img.image.height);
      load_failed_ = true;
      return result;
    }
    const size_t pixels = static_cast<size_t>(arm_img.image.width) *
                          static_cast<size_t>(arm_img.image.height);
    CopyRedIntoAlpha(arm_img.image.rgba, disp_img.image.rgba, pixels);
    result.arm = UploadTexture2DWithMips(device_, queue_, *pipeline_gen_,
                                         arm_img.image.width,
                                         arm_img.image.height,
                                         arm_img.image.rgba);
  }
```

`badlands_decode_image` (declared in `src/crates/assets/include/badlands_assets.h`, auto-detects JPEG vs PNG) and `ImageGuard` are exactly what `LoadTexture2D` uses — see `texture_loader.cpp:88`. `BadlandsImage` is `{uint8_t* rgba; uint32_t width; uint32_t height;}` and `rgba` is NULL on failure. `material_library.cpp` already includes the assets header for its JPEG path; add `engine/rendering/texture_loader.hpp` if it is not already included.

- [ ] **Step 7: Build and run the whole suite**

Run: `scripts/build.sh && scripts/test.sh`
Expected: `BUILD OK`, no new failures. `badlands_game_tests` is seed-flaky by prior history — if it fails, re-run with `--rng-seed` before assuming this change caused it.

- [ ] **Step 8: Commit**

```bash
git add src/engine/rendering/texture_loader.hpp src/engine/rendering/texture_loader.cpp \
        src/engine/rendering/material_library.cpp src/engine/tests/texture_array_tests.cpp
git commit -m "feat(engine): carry pack displacement in the ARM array's alpha

Height-blended terrain needs a per-layer height. Folding each pack's disp
red into its ARM alpha costs no extra texture array, binding or fetch --
every pack's material.json already declares the key, LoadPack just never
read it."
```

---

### Task 3: Shared terrain layer module with height blending

Extract the blend loop so both terrain materials share one implementation, and switch it from a linear cross-fade to a height-lerp so materials interlock instead of dissolving.

**Files:**
- Create: `shaders/common/terrain_layers.wesl`
- Modify: `shaders/material/terrain_blend.wesl`
- Test: `src/engine/tests/terrain_blend_tests.cpp`

**Interfaces:**
- Consumes: nothing from earlier tasks except ARM alpha carrying displacement (Task 2).
- Produces (WESL, importable as `package::common::terrain_layers`):
  - `struct BlendedSurface { albedo: vec3<f32>, tangent_normal: vec3<f32>, ao: f32, roughness: f32 }`
  - `fn sampleTerrainLayers(albedo_array: texture_2d_array<f32>, normal_array: texture_2d_array<f32>, arm_array: texture_2d_array<f32>, samp: sampler, uv: vec2<f32>, w0: vec4<f32>, w1: vec4<f32>, dx: vec2<f32>, dy: vec2<f32>) -> BlendedSurface`
  - `fn terrainPlanarUv(world_offset_pos: vec3<f32>) -> vec2<f32>`
  - `fn terrainPlanarTbn(n: vec3<f32>) -> mat3x3<f32>`
  - `const TERRAIN_MATERIAL_SCALE: f32 = 2.5;`

- [ ] **Step 1: Write the failing GPU test**

Append to `src/engine/tests/terrain_blend_tests.cpp`. It reuses the file's existing `PushTerrainVertex` helper and whatever quad/readback scaffolding the neighbouring `TEST_CASE`s use — copy that scaffolding, changing only the arrays and the assertion:

```cpp
TEST_CASE("terrain blend is height-lerped: equal weights, higher disp wins",
          "[terrain][blend]") {
  // Two 1x1 layers at EQUAL vertex weight (0.5/0.5). Layer 0 is red with a low
  // displacement (arm.a = 32), layer 1 is green with a high one (arm.a = 224).
  // A linear blend would return the midpoint (~50% red, ~50% green); the
  // height-lerp must return essentially pure GREEN, because layer 1 stands
  // proud of layer 0 by far more than TERRAIN_HEIGHT_BLEND_DEPTH.
  const uint8_t albedo[] = {255, 0, 0, 255,   // layer 0: red
                            0, 255, 0, 255};  // layer 1: green
  const uint8_t normal[] = {128, 128, 255, 255, 128, 128, 255, 255};
  const uint8_t arm[] = {255, 255, 0, 32,     // layer 0: disp 32/255
                         255, 255, 0, 224};   // layer 1: disp 224/255

  // ... build the arrays with CreateSolidColorArray(device, queue, X, 2),
  // bind them as albedo_array / normal_array / arm_array overrides, draw a
  // kTerrainBlend quad whose every vertex has layer_indices {0,1,0,0} and
  // blend_weights {0.5, 0.5, 0, 0}, and read back GBufferDebugMode::Albedo
  // exactly as the existing cases in this file do.

  const Rgba centre = /* readback pixel at the quad centre */;
  CHECK(centre.g > 200);
  CHECK(centre.r < 40);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `scripts/build.sh badlands_terrain_blend_tests && scripts/test.sh badlands_terrain_blend_tests "[blend]"`
Expected: FAIL — the linear blend returns roughly equal red and green, so `centre.r < 40` fails.

(If the target name differs, find it with `grep -n terrain_blend_tests CMakeLists.txt`.)

- [ ] **Step 3: Create the shared module**

Create `shaders/common/terrain_layers.wesl`. Move `TERRAIN_MATERIAL_SCALE`, `TERRAIN_BREAK_TILING`, `TILING_VARIATION_TILES`, `hash21`, `variation_noise`, `TileVariation`, `tile_variation`, `sample_layer`, `BlendedSurface`, `planar_uv`, and `planar_tbn` **verbatim** out of `shaders/material/terrain_blend.wesl`, renaming only `planar_uv` → `terrainPlanarUv` and `planar_tbn` → `terrainPlanarTbn`. Then replace `sample_blended` with the height-lerp version:

```wgsl
// Height-lerp transition width (Mishkinis, "Advanced Terrain Texture
// Splatting"). Smaller = a harder, more interlocked seam; larger = closer to a
// plain weighted blend. Structural, so compile-time: it is a property of how
// the packs are authored, not a runtime knob.
const TERRAIN_HEIGHT_BLEND_DEPTH: f32 = 0.2;

// Layers below this contribute nothing visible. Bilinear filtering of a splat
// that stores 2 weights per texel can light up 4 slots at a biome triple point,
// so the cull is what keeps the fetch count bounded.
const TERRAIN_MIN_LAYER_WEIGHT: f32 = 0.00392;  // 1/255

fn sampleTerrainLayers(albedo_array: texture_2d_array<f32>,
                       normal_array: texture_2d_array<f32>,
                       arm_array: texture_2d_array<f32>,
                       samp: sampler,
                       uv: vec2<f32>, w0: vec4<f32>, w1: vec4<f32>,
                       dx: vec2<f32>, dy: vec2<f32>) -> BlendedSurface {
    let max_albedo = textureNumLayers(albedo_array) - 1u;
    let max_normal = textureNumLayers(normal_array) - 1u;
    let max_arm = textureNumLayers(arm_array) - 1u;
    let var_ = tile_variation(uv);

    // Pass 1: per active layer, resolve the de-tiling crossfade from ALBEDO
    // (so all three arrays stay on the same virtual copy) and read that
    // layer's height out of ARM alpha. Albedo and ARM are cached so pass 2
    // costs no re-fetch -- total stays 6 taps per active layer, the same as
    // the pre-height-blend blend.
    var w_in: array<f32, 8>;
    var blend_c: array<f32, 8>;
    var albedo_c: array<vec3<f32>, 8>;
    var arm_c: array<vec2<f32>, 8>;
    var h_c: array<f32, 8>;
    var ma = -1e9;
    for (var i = 0u; i < 8u; i = i + 1u) {
        var w = 0.0;
        if (i < 4u) { w = w0[i]; } else { w = w1[i - 4u]; }
        if (w <= TERRAIN_MIN_LAYER_WEIGHT) {
            w_in[i] = 0.0;
            continue;
        }
        w_in[i] = w;
        let li_a = min(i, max_albedo);
        var blend = 0.0;
        var col = vec3<f32>(0.0);
        if (TERRAIN_BREAK_TILING) {
            let ca = textureSampleGrad(albedo_array, samp, uv + var_.offa, li_a, dx, dy).rgb;
            let cb = textureSampleGrad(albedo_array, samp, uv + var_.offb, li_a, dx, dy).rgb;
            let d = ca - cb;
            blend = smoothstep(0.2, 0.8, var_.blend - 0.1 * (d.x + d.y + d.z));
            col = mix(ca, cb, blend);
        } else {
            col = textureSampleGrad(albedo_array, samp, uv, li_a, dx, dy).rgb;
        }
        blend_c[i] = blend;
        albedo_c[i] = col;

        let a = sample_layer(arm_array, uv, min(i, max_arm), dx, dy, var_, blend);
        arm_c[i] = vec2<f32>(a.r, a.g);
        h_c[i] = a.a;
        ma = max(ma, a.a + w);
    }

    // Pass 2: height-lerped weights, then the one remaining fetch (normal).
    ma = ma - TERRAIN_HEIGHT_BLEND_DEPTH;
    var albedo = vec3<f32>(0.0);
    var tn = vec3<f32>(0.0);
    var arm = vec2<f32>(0.0);
    var wsum = 0.0;
    for (var i = 0u; i < 8u; i = i + 1u) {
        if (w_in[i] <= 0.0) { continue; }
        let b = max(h_c[i] + w_in[i] - ma, 0.0);
        if (b <= 0.0) { continue; }
        albedo = albedo + b * albedo_c[i];
        arm = arm + b * arm_c[i];
        let n = sample_layer(normal_array, uv, min(i, max_normal), dx, dy,
                             var_, blend_c[i]).rgb * 2.0 - 1.0;
        tn = tn + b * n;
        wsum = wsum + b;
    }

    let inv = 1.0 / max(wsum, 1e-4);
    var out: BlendedSurface;
    out.albedo = albedo * inv;
    out.ao = arm.x * inv;
    out.roughness = arm.y * inv;
    let tn_avg = tn * inv;
    if (dot(tn_avg, tn_avg) < 1e-8) {
        out.tangent_normal = vec3<f32>(0.0, 0.0, 1.0);
    } else {
        out.tangent_normal = normalize(tn_avg);
    }
    return out;
}
```

- [ ] **Step 4: Point terrain_blend.wesl at the module**

Delete the moved code from `shaders/material/terrain_blend.wesl` and add, next to its other imports:

```wgsl
@if(!shadow_pass)
import package::common::terrain_layers::{sampleTerrainLayers, terrainPlanarUv, terrainPlanarTbn};
```

Then in its `fs_gbuffer`, replace the three call sites:

```wgsl
    let uv = terrainPlanarUv(input.worldOffsetPos);
    let dx = dpdx(uv);
    let dy = dpdy(uv);
    let surf = sampleTerrainLayers(albedo_array, normal_array, arm_array,
                                   terrain_sampler, uv, input.w0, input.w1,
                                   dx, dy);
    let TBN = terrainPlanarTbn(input.worldNormal);
```

Leave the vertex stage, the weight-scatter loop, and the G-buffer writes untouched.

- [ ] **Step 5: Run the test to verify it passes**

Run: `scripts/build.sh && scripts/test.sh badlands_terrain_blend_tests`
Expected: PASS, including the two pre-existing cases (per-vertex RGB blend, and the missing-override default array).

- [ ] **Step 6: Screenshot badlands_game for the shared-shader change**

Run: `scripts/screenshot.sh badlands_game /tmp/game_heightblend.png`
Expected: renders; terrain biome transitions now interlock rather than cross-fade. Inspect the PNG — a wash-out or black terrain means the height-lerp went wrong.

- [ ] **Step 7: Commit**

```bash
git add shaders/common/terrain_layers.wesl shaders/material/terrain_blend.wesl \
        src/engine/tests/terrain_blend_tests.cpp
git commit -m "feat(shaders): shared terrain layer blend, height-lerped

Extracts terrain_blend's blend loop into common/terrain_layers.wesl so the
cluster terrain can reuse it, and switches the cross-fade to a height-lerp
driven by the displacement now carried in ARM alpha -- materials interlock
instead of dissolving into each other. Fetch count per active layer is
unchanged (albedo and ARM are cached between the two passes)."
```

---

### Task 4: Biome splat builder (CPU)

**Files:**
- Create: `src/mapview/biome_splat.hpp`, `src/mapview/biome_splat.cpp`
- Test: `src/mapview/tests/biome_splat_tests.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `mapgen::Field2D<uint8_t>` (`src/mapgen/field2d.hpp`), `mapgen::Biome` / `mapgen::kBiomeCount` (`src/mapgen/biomes.hpp`, `Lake=0, Swamp, Forest, Plains, Hills, Mountain`).
- Produces:
  ```cpp
  struct BiomeSplat {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> slots0;  // RGBA8, weights for slots 0..3
    std::vector<uint8_t> slots1;  // RGBA8, weights for slots 4..7
    bool empty() const { return width <= 0 || height <= 0; }
  };
  BiomeSplat BuildBiomeSplat(const mapgen::Field2D<uint8_t>& biome, float texel_m);
  ```

- [ ] **Step 1: Write the failing tests**

Create `src/mapview/tests/biome_splat_tests.cpp`:

```cpp
// Pure-CPU tests for the biome -> splat raster the cluster terrain samples.
// The invariants that matter are the ones a wrong splat would silently break:
// slot index == Biome enum value (or every biome wears the wrong texture),
// weights sum to full (or the blend darkens), and at most two layers per texel
// (or the fragment shader's fetch count is unbounded).

#include <catch_amalgamated.hpp>

#include "mapgen/biomes.hpp"
#include "mapview/biome_splat.hpp"

using namespace badlands;

namespace {

mapgen::Field2D<uint8_t> UniformBiome(int w, int h, mapgen::Biome b) {
  mapgen::Field2D<uint8_t> f(w, h, static_cast<uint8_t>(b));
  return f;
}

// Slot weight (0..255) for `slot` at texel (x, y).
int SlotAt(const BiomeSplat& s, int x, int y, int slot) {
  const size_t i = (static_cast<size_t>(y) * s.width + x) * 4;
  return slot < 4 ? s.slots0[i + slot] : s.slots1[i + (slot - 4)];
}

int SlotSum(const BiomeSplat& s, int x, int y) {
  int sum = 0;
  for (int k = 0; k < 8; ++k) sum += SlotAt(s, x, y, k);
  return sum;
}

int NonZeroSlots(const BiomeSplat& s, int x, int y) {
  int n = 0;
  for (int k = 0; k < 8; ++k) {
    if (SlotAt(s, x, y, k) > 0) ++n;
  }
  return n;
}

}  // namespace

TEST_CASE("an empty biome field yields an empty splat", "[splat]") {
  const BiomeSplat s = BuildBiomeSplat(mapgen::Field2D<uint8_t>{}, 1.0f);
  CHECK(s.empty());
}

TEST_CASE("slot index is the Biome enum value", "[splat]") {
  // Lake is enum 0 -> slot 0; Mountain is enum 5 -> slot 5 (i.e. slots1.g).
  const BiomeSplat lake =
      BuildBiomeSplat(UniformBiome(16, 16, mapgen::Biome::Lake), 1.0f);
  CHECK(SlotAt(lake, 8, 8, 0) == 255);
  CHECK(SlotAt(lake, 8, 8, 5) == 0);

  const BiomeSplat mountain =
      BuildBiomeSplat(UniformBiome(16, 16, mapgen::Biome::Mountain), 1.0f);
  CHECK(SlotAt(mountain, 8, 8, 5) == 255);
  CHECK(SlotAt(mountain, 8, 8, 0) == 0);
}

TEST_CASE("weights sum to full at every texel", "[splat]") {
  mapgen::Field2D<uint8_t> b(32, 32, static_cast<uint8_t>(mapgen::Biome::Plains));
  for (int y = 0; y < 32; ++y) {
    for (int x = 16; x < 32; ++x) {
      b.at(x, y) = static_cast<uint8_t>(mapgen::Biome::Forest);
    }
  }
  const BiomeSplat s = BuildBiomeSplat(b, 1.0f);
  for (int y = 0; y < 32; ++y) {
    for (int x = 0; x < 32; ++x) {
      // Rounding to bytes costs at most a couple of units.
      CHECK(SlotSum(s, x, y) >= 253);
      CHECK(SlotSum(s, x, y) <= 257);
    }
  }
}

TEST_CASE("at most two layers are non-zero per texel", "[splat]") {
  // Four biomes meeting at one corner is the worst case for the top-2 cull.
  mapgen::Field2D<uint8_t> b(32, 32, 0);
  for (int y = 0; y < 32; ++y) {
    for (int x = 0; x < 32; ++x) {
      const bool right = x >= 16;
      const bool bottom = y >= 16;
      mapgen::Biome v = mapgen::Biome::Plains;
      if (right && !bottom) v = mapgen::Biome::Forest;
      if (!right && bottom) v = mapgen::Biome::Hills;
      if (right && bottom) v = mapgen::Biome::Mountain;
      b.at(x, y) = static_cast<uint8_t>(v);
    }
  }
  const BiomeSplat s = BuildBiomeSplat(b, 1.0f);
  for (int y = 0; y < 32; ++y) {
    for (int x = 0; x < 32; ++x) {
      CHECK(NonZeroSlots(s, x, y) <= 2);
    }
  }
}

TEST_CASE("a boundary blends and the interior stays pure", "[splat]") {
  mapgen::Field2D<uint8_t> b(32, 32, static_cast<uint8_t>(mapgen::Biome::Plains));
  for (int y = 0; y < 32; ++y) {
    for (int x = 16; x < 32; ++x) {
      b.at(x, y) = static_cast<uint8_t>(mapgen::Biome::Forest);
    }
  }
  const BiomeSplat s = BuildBiomeSplat(b, 1.0f);  // 1 m texels, 3 m blur

  const int plains = static_cast<int>(mapgen::Biome::Plains);
  const int forest = static_cast<int>(mapgen::Biome::Forest);

  // On the seam both are present.
  CHECK(SlotAt(s, 16, 16, plains) > 0);
  CHECK(SlotAt(s, 16, 16, forest) > 0);
  // Well inside each half, only one is.
  CHECK(SlotAt(s, 2, 16, plains) == 255);
  CHECK(SlotAt(s, 29, 16, forest) == 255);
}
```

- [ ] **Step 2: Add the CMake target**

In `CMakeLists.txt`, next to `badlands_biome_manifest_tests`:

```cmake
# badlands_biome_splat_tests: pure-CPU tests for the biome -> splat raster the
# cluster terrain samples (slot index == Biome enum, weights sum to full, at
# most two layers per texel).
add_executable(badlands_biome_splat_tests
    src/mapview/tests/biome_splat_tests.cpp
    src/mapview/biome_splat.cpp
    third_party/catch2/extras/catch_amalgamated.cpp
)
target_include_directories(badlands_biome_splat_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/glm
    ${CMAKE_SOURCE_DIR}/third_party/catch2/extras
)
add_test(NAME badlands_biome_splat_tests COMMAND badlands_biome_splat_tests WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
```

- [ ] **Step 3: Run to verify it fails**

Run: `scripts/build.sh badlands_biome_splat_tests`
Expected: FAIL — `mapview/biome_splat.hpp` does not exist.

- [ ] **Step 4: Write the header**

Create `src/mapview/biome_splat.hpp`:

```cpp
#pragma once

// Builds the biome-weight SPLAT raster the cluster terrain samples by world XZ
// to pick its per-biome materials.
//
// Why a texture and not vertex attributes: the cluster-LOD DAG decimates
// vertices, so vertex-carried blend weights would have to be averaged by the
// simplifier and biome-boundary resolution would follow the LOD cut. A splat is
// LOD-independent -- the coarsest cluster still gets full-resolution biome
// detail -- and it leaves the cluster vertex format and DAG build untouched.
//
// Layout matches terrain_layers.wesl's 8 dense weight slots: slot index IS the
// mapgen::Biome enum value, which is also the texture-array layer index (the
// same convention assets/materials/terrain_biomes.json and LoadTerrainArrays
// use). Slots 6-7 are always zero today.
//
// Pure CPU (mapgen + <vector>), so it is unit-testable without a GPU.

#include <cstdint>
#include <vector>

#include "mapgen/field2d.hpp"

namespace badlands {

// Blur radius applied to the one-hot biome weights before the top-2 cull, in
// world metres. Softens the raster's one-texel staircase into a transition band
// wide enough to read as a blend rather than a jagged edge.
inline constexpr float kBiomeBlendM = 3.0f;

// Two RGBA8 rasters holding 8 biome weight slots, sized to the input field.
// slots0 = slots 0..3, slots1 = slots 4..7; both are tightly packed RGBA8 and
// `width * height * 4` bytes long. Weights at a texel sum to 255 (+/- rounding).
struct BiomeSplat {
  int width = 0;
  int height = 0;
  std::vector<uint8_t> slots0;
  std::vector<uint8_t> slots1;

  bool empty() const { return width <= 0 || height <= 0; }
};

// Builds the splat from a per-texel biome raster (values are mapgen::Biome enum
// values). `texel_m` is the world size of one texel, which is what turns
// kBiomeBlendM into a pixel radius. An empty field returns an empty splat.
//
// Only the two strongest weights per texel survive: the fragment shader's cost
// is proportional to the number of non-zero layers, and bilinear filtering of a
// 2-weight texel can already produce 4 at a biome triple point.
BiomeSplat BuildBiomeSplat(const mapgen::Field2D<uint8_t>& biome, float texel_m);

}  // namespace badlands
```

- [ ] **Step 5: Write the implementation**

Create `src/mapview/biome_splat.cpp`:

```cpp
#include "mapview/biome_splat.hpp"

#include <algorithm>
#include <cmath>

#include "mapgen/biomes.hpp"

namespace badlands {

namespace {

constexpr int kSlots = 8;

// Separable box blur of one weight plane, radius `r` texels, edges clamped.
void BlurPlane(std::vector<float>& plane, int w, int h, int r,
               std::vector<float>& scratch) {
  if (r <= 0) return;
  const float inv = 1.0f / static_cast<float>(2 * r + 1);
  scratch.assign(plane.size(), 0.0f);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      float sum = 0.0f;
      for (int k = -r; k <= r; ++k) {
        const int sx = std::clamp(x + k, 0, w - 1);
        sum += plane[static_cast<size_t>(y) * w + sx];
      }
      scratch[static_cast<size_t>(y) * w + x] = sum * inv;
    }
  }
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      float sum = 0.0f;
      for (int k = -r; k <= r; ++k) {
        const int sy = std::clamp(y + k, 0, h - 1);
        sum += scratch[static_cast<size_t>(sy) * w + x];
      }
      plane[static_cast<size_t>(y) * w + x] = sum * inv;
    }
  }
}

}  // namespace

BiomeSplat BuildBiomeSplat(const mapgen::Field2D<uint8_t>& biome,
                           float texel_m) {
  BiomeSplat out;
  const int w = biome.width, h = biome.height;
  if (w <= 0 || h <= 0) return out;

  // One plane per biome, one-hot, then blurred.
  const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h);
  std::vector<std::vector<float>> planes(mapgen::kBiomeCount,
                                         std::vector<float>(n, 0.0f));
  for (size_t i = 0; i < n; ++i) {
    const int b = std::min<int>(biome.data[i], mapgen::kBiomeCount - 1);
    planes[b][i] = 1.0f;
  }

  const int radius =
      texel_m > 0.0f ? static_cast<int>(std::lround(kBiomeBlendM / texel_m)) : 0;
  std::vector<float> scratch;
  for (auto& p : planes) BlurPlane(p, w, h, radius, scratch);

  out.width = w;
  out.height = h;
  out.slots0.assign(n * 4, 0);
  out.slots1.assign(n * 4, 0);

  for (size_t i = 0; i < n; ++i) {
    // Top 2 by weight. Ties break toward the lower slot, which is stable.
    int best = 0, second = -1;
    for (int b = 1; b < mapgen::kBiomeCount; ++b) {
      if (planes[b][i] > planes[best][i]) best = b;
    }
    for (int b = 0; b < mapgen::kBiomeCount; ++b) {
      if (b == best) continue;
      if (second < 0 || planes[b][i] > planes[second][i]) second = b;
    }
    const float w0 = planes[best][i];
    const float w1 = (second >= 0) ? planes[second][i] : 0.0f;
    const float sum = w0 + w1;
    if (sum <= 0.0f) continue;

    // Quantize so the pair sums to exactly 255 -- a short sum would darken the
    // blend, since the shader normalizes by the weights it actually receives.
    const uint8_t q0 = static_cast<uint8_t>(
        std::clamp<int>(static_cast<int>(std::lround(255.0f * w0 / sum)), 0, 255));
    const uint8_t q1 = static_cast<uint8_t>(255 - q0);

    auto put = [&](int slot, uint8_t v) {
      if (v == 0) return;
      if (slot < 4) {
        out.slots0[i * 4 + slot] = v;
      } else {
        out.slots1[i * 4 + (slot - 4)] = v;
      }
    };
    put(best, q0);
    if (second >= 0) put(second, q1);
  }

  return out;
}

}  // namespace badlands
```

- [ ] **Step 6: Run the tests**

Run: `scripts/test.sh badlands_biome_splat_tests`
Expected: PASS (all five cases)

- [ ] **Step 7: Commit**

```bash
git add src/mapview/biome_splat.hpp src/mapview/biome_splat.cpp \
        src/mapview/tests/biome_splat_tests.cpp CMakeLists.txt
git commit -m "feat(mapview): biome splat raster for terrain materials

Blurs the one-hot biome raster, keeps the top two weights per texel and
packs them into 8 slots across two RGBA8 planes -- slot index is the Biome
enum value, which is also the terrain array layer index. Sampled by world
XZ, so biome detail is independent of the cluster LOD cut."
```

---

### Task 5: Textured cluster terrain

Wire the arrays + splat into `terrain_cluster.wesl` and mapview.

**Files:**
- Modify: `shaders/material/terrain_cluster.wesl`
- Modify: `src/engine/rendering/material/material_requirements.cpp`
- Modify: `src/engine/rendering/material_library.hpp` (add `shared_sampler()`)
- Modify: `src/game/map/cluster_terrain.hpp`, `src/game/map/cluster_terrain.cpp`
- Modify: `src/executables/mapview/map_view_view.hpp`, `.cpp`
- Modify: `CMakeLists.txt` (add `src/mapview/biome_splat.cpp` to `badlands_mapview`)

**Interfaces:**
- Consumes: `sampleTerrainLayers` / `terrainPlanarUv` / `terrainPlanarTbn` (Task 3); `BuildBiomeSplat` / `BiomeSplat` (Task 4); `ResolveBiomePacks(manifest_path, out_pack_dirs)` (`src/mapview/biome_manifest.hpp`); `MaterialLibrary::LoadTerrainArrays`, `MaterialLibrary::TerrainArrays`; `UploadTexture2DWithMips`.
- Produces: `ClusterTerrain::Build(map, ctx, registry, model, params, arrays, splat0_view, splat1_view, splat_sampler, splat_uv)` — new trailing parameters, all required.

- [ ] **Step 1: Add the sampler accessor**

In `src/engine/rendering/material_library.hpp`, in the public section:

```cpp
  // The library's shared trilinear + 16x-aniso REPEAT sampler. Exposed because
  // a caller that binds the terrain arrays itself (the cluster terrain builds
  // its own material factory) must use this one -- the material factory's
  // default sampler has mipmapFilter=Nearest and would defeat every pack's mip
  // chain. Valid after Initialize().
  wgpu::Sampler shared_sampler() const { return sampler_; }
```

- [ ] **Step 2: Register the cluster material's texture slots**

In `src/engine/rendering/material/material_requirements.cpp`, after the `terrain_blend` registration:

```cpp
  // terrain_cluster.wesl - the same three layer arrays as terrain_blend (shared
  // sampler binding 2), plus two RGBA8 biome-weight splat planes on their own
  // CLAMP sampler (binding 7). The splat carries normalized weights, so a
  // repeat sampler would wrap the map's far edge onto its near one.
  MaterialRequirements terrain_cluster_reqs{
      .shader_name = "terrain_cluster",
      .textures = {
          {.slot_name = "albedo_array",
           .texture_binding = 1,
           .sampler_binding = 2,
           .default_texture = "white"},
          {.slot_name = "normal_array",
           .texture_binding = 3,
           .sampler_binding = 2,
           .default_texture = "flat_normal"},
          {.slot_name = "arm_array",
           .texture_binding = 4,
           .sampler_binding = 2,
           .default_texture = "default_arm"},
          // The splat is always bound in practice; the k2D "white" default only
          // guards a programming error, and renders a neutral multi-layer
          // average rather than failing bind-group validation.
          {.slot_name = "biome_splat0",
           .texture_binding = 5,
           .sampler_binding = 7,
           .default_texture = "white"},
          {.slot_name = "biome_splat1",
           .texture_binding = 6,
           .sampler_binding = 7,
           .default_texture = "white"},
      }};
  RegisterMaterial("terrain_cluster", terrain_cluster_reqs,
                   terrain_cluster_reqs);
```

- [ ] **Step 3: Texture the cluster shader**

In `shaders/material/terrain_cluster.wesl`:

Add to the non-shadow imports:
```wgsl
@if(!shadow_pass)
import package::common::terrain_layers::{sampleTerrainLayers, terrainPlanarUv, terrainPlanarTbn};
```

Add the bindings and extend the object UBO:
```wgsl
@group(0) @binding(1) var albedo_array: texture_2d_array<f32>;
@group(0) @binding(2) var terrain_sampler: sampler;
@group(0) @binding(3) var normal_array: texture_2d_array<f32>;
@group(0) @binding(4) var arm_array: texture_2d_array<f32>;
@group(0) @binding(5) var biome_splat0: texture_2d<f32>;
@group(0) @binding(6) var biome_splat1: texture_2d<f32>;
@group(0) @binding(7) var splat_sampler: sampler;

struct TerrainClusterUniforms {
    modelMatrix: mat4x4<f32>,
    debug_params: vec4<f32>,  // x = debug source mode (0 shade / 1 hash / 2 lod)
    // World XZ -> splat UV: uv = worldXZ * splat_uv.xy + splat_uv.zw. The scale
    // is inset by half a texel on each side so the outermost sample lands on a
    // texel CENTRE rather than half-way off the raster.
    splat_uv: vec4<f32>,
}
```

Add `@location(3) worldOffsetPos: vec3<f32>` to `VertexOutput` and set it in `vs_main`:
```wgsl
    output.worldOffsetPos = world_offset_pos;
```

Replace the `albedo` derivation in `fs_gbuffer`'s mode-0 path:
```wgsl
    let world = input.worldOffsetPos + frame.cameraWorldPos.xyz;
    let suv = world.xz * object.splat_uv.xy + object.splat_uv.zw;
    let w0 = textureSample(biome_splat0, splat_sampler, suv);
    let w1 = textureSample(biome_splat1, splat_sampler, suv);

    let uv = terrainPlanarUv(input.worldOffsetPos);
    let dx = dpdx(uv);
    let dy = dpdy(uv);
    let surf = sampleTerrainLayers(albedo_array, normal_array, arm_array,
                                   terrain_sampler, uv, w0, w1, dx, dy);

    var albedo = surf.albedo;
    let mode = i32(round(object.debug_params.x));
    if (mode == 1) {
        albedo = tint_from_unit(f32(input.packed_meta.y) / 255.0);
    } else if (mode == 2) {
        albedo = tint_from_unit(fract(f32(input.packed_meta.z) * 0.61803399));
    }

    let TBN = terrainPlanarTbn(input.worldNormal);
    output.normals = encodeOctahedron(normalize(TBN * surf.tangent_normal));
    output.albedo = vec4<f32>(albedo, 1.0);
    output.material = vec4<f32>(surf.roughness, 1.0, surf.ao, 0.0);
```

`textureSample` and `dpdx`/`dpdy` must stay in uniform control flow — keep them above any branch, as written.

- [ ] **Step 4: Extend ClusterTerrain::Build**

In `src/game/map/cluster_terrain.hpp`, change the declaration to:

```cpp
  // Build the DAG from `map`, the terrain material factory, and one terrain
  // entity into `registry`. `arrays` supplies the three PBR layer arrays
  // (layer index == biome); `splat0`/`splat1` are the biome-weight planes and
  // `splat_sampler` their CLAMP sampler; `splat_uv` maps world XZ to splat UV
  // as {scale_x, scale_z, bias_u, bias_v}. `array_sampler` must be the
  // MaterialLibrary's shared trilinear+aniso repeat sampler.
  bool Build(const MapData& map, const RenderContext& ctx,
             entt::registry& registry, const glm::mat4& model,
             const TerrainClusterParams& params,
             const MaterialLibrary::TerrainArrays& arrays,
             wgpu::Sampler array_sampler, wgpu::TextureView splat0,
             wgpu::TextureView splat1, wgpu::Sampler splat_sampler,
             glm::vec4 splat_uv);
```

(Include `engine/rendering/material_library.hpp`. Drop the default arguments — mapview is the only caller.)

In `cluster_terrain.cpp`, bind the textures on the `MaterialFactoryComponent`'s params, following `MaterialLibrary::TerrainBlend`'s pattern exactly:

```cpp
  auto bind = [&](const char* slot, wgpu::TextureView view,
                  wgpu::Sampler samp) {
    if (!view) return;
    fmc.params.texture_overrides.push_back(DefaultTextureView{
        .param_name = slot,
        .view = view,
        .sampler = samp,
        .type = TextureType::k2D,
    });
  };
  bind("albedo_array", arrays.albedo.view, array_sampler);
  bind("normal_array", arrays.normal.view, array_sampler);
  bind("arm_array", arrays.arm.view, array_sampler);
  bind("biome_splat0", splat0, splat_sampler);
  bind("biome_splat1", splat1, splat_sampler);
  fmc.params.uniform_overrides["splat_uv"] = splat_uv;
```

Keep the existing `debug_params` override and the `ComputeFactoryConfigHash(fmc)` call **after** these additions.

- [ ] **Step 5: Wire mapview**

In `map_view_view.hpp` add members:
```cpp
  MaterialLibrary matlib_;
  MaterialLibrary::TerrainArrays terrain_arrays_;
  wgpu::TextureView splat0_view_, splat1_view_;
  wgpu::Sampler splat_sampler_;
```
(Include `engine/rendering/material_library.hpp`.)

In `Initialize`, after `terrain_map_` is built and before `cluster_terrain_.Build`:

```cpp
  if (!matlib_.Initialize(ctx.device, ctx.queue, ctx.pipeline_gen)) {
    spdlog::error("MapViewView: MaterialLibrary init failed");
    return false;
  }
  std::vector<std::string> pack_dirs;
  if (!ResolveBiomePacks("assets/materials/terrain_biomes.json", pack_dirs)) {
    spdlog::error("MapViewView: failed to resolve biome packs");
    return false;
  }
  terrain_arrays_ = matlib_.LoadTerrainArrays(pack_dirs);
  if (!matlib_.ok()) {
    spdlog::error("MapViewView: terrain arrays failed to build");
    return false;
  }

  // Biome splat: sampled by world XZ, so it is independent of the LOD cut.
  const BiomeSplat splat = BuildBiomeSplat(
      map_.biome, params_.world_size_m / static_cast<float>(params_.resolution));
  if (splat.empty()) {
    spdlog::error("MapViewView: empty biome splat");
    return false;
  }
  splat0_view_ = UploadTexture2DWithMips(
                     device_, queue_, *ctx.pipeline_gen,
                     static_cast<uint32_t>(splat.width),
                     static_cast<uint32_t>(splat.height), splat.slots0.data())
                     .view;
  splat1_view_ = UploadTexture2DWithMips(
                     device_, queue_, *ctx.pipeline_gen,
                     static_cast<uint32_t>(splat.width),
                     static_cast<uint32_t>(splat.height), splat.slots1.data())
                     .view;
  // Clamp + trilinear: the splat holds normalized weights over the map's own
  // extent, so wrapping would fold the far edge onto the near one.
  wgpu::SamplerDescriptor sd = {};
  sd.minFilter = wgpu::FilterMode::Linear;
  sd.magFilter = wgpu::FilterMode::Linear;
  sd.mipmapFilter = wgpu::MipmapFilterMode::Linear;
  sd.addressModeU = wgpu::AddressMode::ClampToEdge;
  sd.addressModeV = wgpu::AddressMode::ClampToEdge;
  splat_sampler_ = device_.CreateSampler(&sd);

  // world XZ in [0, size] -> texel centres in [0.5/N, 1 - 0.5/N].
  const float inv_n = 1.0f / static_cast<float>(splat.width);
  const glm::vec4 splat_uv((1.0f - inv_n) / params_.world_size_m,
                           (1.0f - inv_n) / params_.world_size_m,
                           0.5f * inv_n, 0.5f * inv_n);
```

Then pass the new arguments to `cluster_terrain_.Build(...)`. Add the includes for `mapview/biome_manifest.hpp`, `mapview/biome_splat.hpp`, and `engine/rendering/texture_loader.hpp`.

**Note:** the splat views must be kept alive by the view — `LoadedTexture::view` keeps its texture alive, so holding the views is sufficient.

- [ ] **Step 6: Add the new source to the mapview target**

In `CMakeLists.txt`, add `src/mapview/biome_splat.cpp` to `add_executable(badlands_mapview ...)` alongside `src/mapview/biome_manifest.cpp`.

- [ ] **Step 7: Build and screenshot**

Run:
```bash
scripts/build.sh badlands_mapview
scripts/screenshot.sh badlands_mapview /tmp/mv_near.png --seed 2 --camera-height 30
scripts/screenshot.sh badlands_mapview /tmp/mv_far.png --seed 2 --camera-height 400
```
Expected: `BUILD OK`; the near shot shows real rock/mud/forest-floor detail with interlocking biome transitions; the far shot shows biome regions without shimmer (mips working). Read both PNGs and confirm. A uniformly gray terrain means the array overrides did not resolve — check the `material_requirements.cpp` bindings first.

- [ ] **Step 8: Confirm the debug tint modes still work**

Run: `scripts/screenshot.sh badlands_mapview /tmp/mv_lod.png --seed 2 --camera-height 120 --lod-tint 2`
Expected: LOD-level tinting, unchanged from before this task.

- [ ] **Step 9: Commit**

```bash
git add shaders/material/terrain_cluster.wesl \
        src/engine/rendering/material/material_requirements.cpp \
        src/engine/rendering/material_library.hpp \
        src/game/map/cluster_terrain.hpp src/game/map/cluster_terrain.cpp \
        src/executables/mapview/map_view_view.hpp \
        src/executables/mapview/map_view_view.cpp CMakeLists.txt
git commit -m "feat(mapview): PBR material per biome on the cluster terrain

Samples the biome splat by world XZ and blends the layer arrays through
the shared terrain_layers module, so the coarsest LOD cluster still gets
full-resolution biome detail. Vertex colour stays in the format but no
longer feeds albedo."
```

---

### Task 6: Lake surface builder (CPU)

**Files:**
- Create: `src/mapview/lake_surface.hpp`, `src/mapview/lake_surface.cpp`
- Test: `src/mapview/tests/lake_surface_tests.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `mapgen::MapArtifacts` — specifically `heightmap` (`Field2D<float>`), `lake_id` (`Field2D<int32_t>`, `-1` where dry), and `lakes` (`std::vector<LakeInfo>` with `level_m`).
- Produces: `std::vector<glm::vec3> BuildLakeSurfaceTriangles(const mapgen::MapArtifacts& art, float world_size_m)` — a flat triangle soup in map-local world coordinates, three vertices per triangle, wound CCW seen from +Y.

- [ ] **Step 1: Write the failing tests**

Create `src/mapview/tests/lake_surface_tests.cpp`:

```cpp
// Pure-CPU tests for the lake water surface + its buried skirt.
//
// The skirt is the whole point: the water plane is extended under the terrain
// so that vertical wave displacement (added later) cannot open a gap at the
// shoreline. Two ways to get that wrong are load-bearing here -- the skirt
// stopping before it is buried, and the skirt spilling over a ridge into a
// lower basin where it would hang in the air as a visible sheet.

#include <catch_amalgamated.hpp>

#include <algorithm>
#include <cmath>
#include <set>

#include "mapgen/generator.hpp"
#include "mapview/lake_surface.hpp"

using namespace badlands;

namespace {

// A flat map at height `ground`, with a rectangular lake carved to `bed`.
mapgen::MapArtifacts MakeMap(int n, float ground, int lx0, int lz0, int lx1,
                             int lz1, float bed, float level) {
  mapgen::MapArtifacts a;
  a.heightmap = mapgen::Field2D<float>(n, n, ground);
  a.lake_id = mapgen::Field2D<int32_t>(n, n, -1);
  for (int z = lz0; z < lz1; ++z) {
    for (int x = lx0; x < lx1; ++x) {
      a.heightmap.at(x, z) = bed;
      a.lake_id.at(x, z) = 0;
    }
  }
  mapgen::LakeInfo l;
  l.level_m = level;
  a.lakes.push_back(l);
  return a;
}

std::set<int> DistinctY(const std::vector<glm::vec3>& tris) {
  std::set<int> ys;
  for (const glm::vec3& v : tris) ys.insert(static_cast<int>(std::lround(v.y * 100.0f)));
  return ys;
}

}  // namespace

TEST_CASE("a map with no lakes yields no water", "[water]") {
  mapgen::MapArtifacts a;
  a.heightmap = mapgen::Field2D<float>(8, 8, 10.0f);
  a.lake_id = mapgen::Field2D<int32_t>(8, 8, -1);
  CHECK(BuildLakeSurfaceTriangles(a, 8.0f).empty());
}

TEST_CASE("every water vertex sits at its lake's level", "[water]") {
  // 1 m texels. Bank at 12 m, well above the 10 m level, so the skirt is
  // buried after a single ring.
  const mapgen::MapArtifacts a = MakeMap(16, 12.0f, 6, 6, 10, 10, 8.0f, 10.0f);
  const std::vector<glm::vec3> tris = BuildLakeSurfaceTriangles(a, 16.0f);
  REQUIRE(!tris.empty());
  for (const glm::vec3& v : tris) CHECK(v.y == Approx(10.0f));
}

TEST_CASE("the surface overlaps the shore", "[water]") {
  // The lake spans x in [6, 10). The emitted surface must reach further --
  // that overlap is what a later wave displacement hides its edge in.
  const mapgen::MapArtifacts a = MakeMap(16, 12.0f, 6, 6, 10, 10, 8.0f, 10.0f);
  const std::vector<glm::vec3> tris = BuildLakeSurfaceTriangles(a, 16.0f);
  float min_x = 1e9f, max_x = -1e9f;
  for (const glm::vec3& v : tris) {
    min_x = std::min(min_x, v.x);
    max_x = std::max(max_x, v.x);
  }
  CHECK(min_x < 6.0f);
  CHECK(max_x > 10.0f);
}

TEST_CASE("the skirt never runs past its cap", "[water]") {
  // A dead-flat shore exactly AT the water level never reaches burial depth,
  // so the cap is the only thing stopping the search.
  const mapgen::MapArtifacts a = MakeMap(64, 10.0f, 28, 28, 36, 36, 8.0f, 10.0f);
  const std::vector<glm::vec3> tris = BuildLakeSurfaceTriangles(a, 64.0f);
  float min_x = 1e9f, max_x = -1e9f;
  for (const glm::vec3& v : tris) {
    min_x = std::min(min_x, v.x);
    max_x = std::max(max_x, v.x);
  }
  // Lake spans [28, 36); the skirt may add at most kMaxSkirtM on each side.
  CHECK(min_x >= 28.0f - kMaxSkirtM - 1.0f);
  CHECK(max_x <= 36.0f + kMaxSkirtM + 1.0f);
}

TEST_CASE("the skirt does not spill into a lower dry basin", "[water]") {
  // Lake at level 10 on the left, a 1-texel ridge at 11 m, then dry ground at
  // 5 m -- well BELOW the water level. Water reaching the low ground would
  // hang in the air.
  mapgen::MapArtifacts a = MakeMap(24, 11.0f, 4, 4, 10, 20, 8.0f, 10.0f);
  for (int z = 0; z < 24; ++z) {
    for (int x = 12; x < 24; ++x) a.heightmap.at(x, z) = 5.0f;
  }
  const std::vector<glm::vec3> tris = BuildLakeSurfaceTriangles(a, 24.0f);
  float max_x = -1e9f;
  for (const glm::vec3& v : tris) max_x = std::max(max_x, v.x);
  CHECK(max_x <= 12.0f);
}

TEST_CASE("two lakes keep their own levels", "[water]") {
  mapgen::MapArtifacts a = MakeMap(32, 20.0f, 4, 4, 8, 8, 8.0f, 10.0f);
  // Second lake, far from the first, at a different level.
  for (int z = 20; z < 24; ++z) {
    for (int x = 20; x < 24; ++x) {
      a.heightmap.at(x, z) = 12.0f;
      a.lake_id.at(x, z) = 1;
    }
  }
  mapgen::LakeInfo second;
  second.level_m = 15.0f;
  a.lakes.push_back(second);

  const std::set<int> ys = DistinctY(BuildLakeSurfaceTriangles(a, 32.0f));
  CHECK(ys.size() == 2);
  CHECK(ys.count(1000) == 1);  // 10.00 m
  CHECK(ys.count(1500) == 1);  // 15.00 m
}
```

- [ ] **Step 2: Add the CMake target**

```cmake
# badlands_lake_surface_tests: pure-CPU tests for the per-lake water surface and
# its buried skirt (the overlap that keeps future wave displacement from opening
# a shoreline gap).
add_executable(badlands_lake_surface_tests
    src/mapview/tests/lake_surface_tests.cpp
    src/mapview/lake_surface.cpp
    third_party/catch2/extras/catch_amalgamated.cpp
)
target_include_directories(badlands_lake_surface_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/glm
    ${CMAKE_SOURCE_DIR}/third_party/catch2/extras
)
add_test(NAME badlands_lake_surface_tests COMMAND badlands_lake_surface_tests WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
```

If linking pulls in more of `badlands_mapgen_lib` than expected, link `badlands_mapgen_lib` rather than adding sources.

- [ ] **Step 3: Run to verify it fails**

Run: `scripts/build.sh badlands_lake_surface_tests`
Expected: FAIL — `mapview/lake_surface.hpp` does not exist.

- [ ] **Step 4: Write the header**

Create `src/mapview/lake_surface.hpp`:

```cpp
#pragma once

// Builds the standing-water surface for every lake the generator produced.
//
// Read straight from MapArtifacts rather than from the frozen MapData contract:
// MapData carries ONE global water_level_m, but the generator ponds multiple
// lakes at DIFFERENT elevations (LakeInfo::level_m per lake, indexed by
// lake_id). A single plane would be wrong for every lake but one.
//
// River channels carry lake_id == -1 and are therefore excluded -- rivers are
// out of scope for this pass.
//
// The emitted surface deliberately extends PAST each shoreline and under the
// terrain (the "skirt"). Water is depth-tested but does not write depth, so the
// terrain in front rejects the buried fragments for free; the overlap is what
// keeps a later vertical wave displacement from opening a gap at the waterline.
//
// Pure CPU (mapgen + glm), so it is unit-testable without the engine.

#include <vector>

#include <glm/glm.hpp>

#include "mapgen/generator.hpp"

namespace badlands {

// How far above a lake's level the terrain must rise before the skirt stops
// growing. The outermost ring is buried by at least this much, so a wave
// trough of up to this amplitude still has terrain in front of it.
inline constexpr float kBurialM = 0.6f;

// Hard cap on how far the skirt may run from the shoreline. A dead-flat shore
// never reaches kBurialM; the cap stops the search from flooding the map. Such
// shores are also where waves are smallest, so the shortfall is benign.
inline constexpr float kMaxSkirtM = 6.0f;

// Flat, lattice-aligned water triangles for every lake in `art`, in map-local
// world coordinates, wound CCW seen from +Y. Three vertices per triangle; each
// vertex's Y is its own lake's level_m, so lakes at different elevations can
// share one mesh. Texel (x, z) spans world [x*s, (x+1)*s] on both axes, where
// s = world_size_m / heightmap.width -- the same cell lattice the terrain mesh
// uses. Returns empty when there are no lakes.
std::vector<glm::vec3> BuildLakeSurfaceTriangles(const mapgen::MapArtifacts& art,
                                                 float world_size_m);

}  // namespace badlands
```

- [ ] **Step 5: Write the implementation**

Create `src/mapview/lake_surface.cpp`:

```cpp
#include "mapview/lake_surface.hpp"

#include <array>
#include <cstdint>
#include <deque>
#include <vector>

namespace badlands {

namespace {

constexpr std::array<glm::ivec2, 4> kNeighbours = {
    glm::ivec2{1, 0}, glm::ivec2{-1, 0}, glm::ivec2{0, 1}, glm::ivec2{0, -1}};

}  // namespace

std::vector<glm::vec3> BuildLakeSurfaceTriangles(const mapgen::MapArtifacts& art,
                                                 float world_size_m) {
  std::vector<glm::vec3> tris;
  const int w = art.lake_id.width, h = art.lake_id.height;
  if (w <= 0 || h <= 0 || art.lakes.empty()) return tris;
  if (art.heightmap.width != w || art.heightmap.height != h) return tris;
  const float s = world_size_m / static_cast<float>(w);
  if (s <= 0.0f) return tris;

  // owner[i] = which lake covers texel i (-1 = none). Seeded with the lake
  // texels themselves, then grown outward by a multi-source BFS so each skirt
  // texel is claimed by its NEAREST lake -- two lakes at different levels
  // cannot both claim the same ground.
  std::vector<int32_t> owner(static_cast<size_t>(w) * h, -1);
  std::vector<float> dist(static_cast<size_t>(w) * h, 0.0f);
  std::deque<int> queue;
  for (int i = 0; i < w * h; ++i) {
    const int32_t id = art.lake_id.data[i];
    if (id >= 0 && id < static_cast<int32_t>(art.lakes.size())) {
      owner[i] = id;
      queue.push_back(i);
    }
  }

  while (!queue.empty()) {
    const int i = queue.front();
    queue.pop_front();
    const int32_t id = owner[i];
    const float level = art.lakes[id].level_m;
    const int x = i % w, z = i / w;

    // Rule 3: this texel is the buried boundary -- keep it, expand no further.
    // Lake texels themselves always expand (their bed is below the level).
    if (art.lake_id.data[i] < 0 &&
        art.heightmap.data[i] >= level + kBurialM) {
      continue;
    }

    for (const glm::ivec2& d : kNeighbours) {
      const int nx = x + d.x, nz = z + d.y;
      if (nx < 0 || nz < 0 || nx >= w || nz >= h) continue;
      const int ni = nz * w + nx;
      if (owner[ni] >= 0) continue;               // already water or claimed
      const float nd = dist[i] + s;
      if (nd > kMaxSkirtM) continue;              // rule 1: past the cap
      if (art.heightmap.data[ni] < level) continue;  // rule 2: lower ground
      owner[ni] = id;
      dist[ni] = nd;
      queue.push_back(ni);
    }
  }

  // Two triangles per covered texel, on the terrain's own cell lattice.
  for (int z = 0; z < h; ++z) {
    for (int x = 0; x < w; ++x) {
      const int i = z * w + x;
      if (owner[i] < 0) continue;
      const float y = art.lakes[owner[i]].level_m;
      const float x0 = static_cast<float>(x) * s, x1 = x0 + s;
      const float z0 = static_cast<float>(z) * s, z1 = z0 + s;
      const glm::vec3 a(x0, y, z0), b(x1, y, z0), c(x1, y, z1), d(x0, y, z1);
      // CCW seen from +Y.
      tris.push_back(a); tris.push_back(d); tris.push_back(c);
      tris.push_back(a); tris.push_back(c); tris.push_back(b);
    }
  }
  return tris;
}

}  // namespace badlands
```

- [ ] **Step 6: Run the tests**

Run: `scripts/test.sh badlands_lake_surface_tests`
Expected: PASS (all six cases)

- [ ] **Step 7: Commit**

```bash
git add src/mapview/lake_surface.hpp src/mapview/lake_surface.cpp \
        src/mapview/tests/lake_surface_tests.cpp CMakeLists.txt
git commit -m "feat(mapview): per-lake water surface with a buried skirt

Built from lake_id + LakeInfo::level_m rather than MapData's single global
level, so lakes at different elevations each get their own plane. A
multi-source BFS grows each surface outward until the terrain buries it by
kBurialM, refusing any texel whose ground sits below the water level so the
skirt cannot hang over a neighbouring lower basin."
```

---

### Task 7: Beer–Lambert water + still variant

**Files:**
- Modify: `shaders/material/water.wesl`
- Modify: `src/engine/rendering/water_material.hpp`, `.cpp`
- Modify: `game/tests/water_gpu_tests.cpp`

**Interfaces:**
- Consumes: the measured `sigma_e` from Task 1 Step 4.
- Produces:
  - `std::unique_ptr<MaterialInstanceFactory> BuildStillWaterForwardFactory(wgpu::Device, wgpu::Queue, GpuPipelineGenerator*)`
  - `InstanceParams StillLakeWaterParams()`
  - `WaterUniforms` fields `extinction` (xyz = 1/m) and `scatterAlbedo` (xyz), replacing `deepColor` / `shallowColor` / `params.x` / `params.z`.

- [ ] **Step 1: Rewrite the uniforms and the medium in water.wesl**

Replace the `WaterUniforms` struct with:

```wgsl
struct WaterUniforms {
    modelMatrix: mat4x4<f32>,
    time: f32,
    _pad0: f32,
    _pad1: f32,
    _pad2: f32,
    // Per-channel extinction coefficient, 1/m. Derived from a visibility depth:
    // sigma = 3 / d_vis, so the bed has faded by ~95% at d_vis metres. Red is
    // largest, which is what turns the water toward blue as it deepens.
    extinction: vec4<f32>,
    // Single-scattering albedo (0..1): the fraction of extinguished light that
    // is re-scattered toward the viewer rather than absorbed. This is what the
    // deep water asymptotes to, lit by the sky + shadowed sun.
    scatterAlbedo: vec4<f32>,
    params: vec4<f32>,   // x=refractStrength, y=roughness, z/w reserved
    params2: vec4<f32>,  // x=detailStrength, y=blockout flag
}
```

Add near the top:

```wgsl
// Optical path length used when nothing is behind the water (sky at the far
// edge of the map). Deep enough that transmittance is negligible for any
// plausible extinction, so the surface reads as open water rather than glass.
const kSkyBehindDepthM: f32 = 32.0;
```

In the `@if(transparent)` fragment, replace the depth/absorption/coast block through to `transmitted` with:

```wgsl
    let pixel = vec2<i32>(input.position.xy);
    let sceneDepthRaw = textureLoad(sceneDepth, pixel, 0);
    let waterZ = reconstructLinearZ(input.position.z);
    var pathM = kSkyBehindDepthM;
    if (isValidDepth(sceneDepthRaw)) {
        pathM = max(reconstructLinearZ(sceneDepthRaw) - waterZ, 0.0);
    }
    // Beer-Lambert transmittance along the view ray through the medium.
    let T = exp(-object.extinction.xyz * pathM);
    // How "not transparent" the water is here; drives the refraction ramp so a
    // shallow shoreline does not smear the bed across the waterline.
    let opacity = 1.0 - T.g;

    let screenUV = input.position.xy / frame.screenSize;
    let refrUV = clamp(screenUV + N.xz * object.params.x * opacity,
                       vec2<f32>(0.0), vec2<f32>(1.0));
    let offDepthRaw = textureLoad(sceneDepth, vec2<i32>(refrUV * frame.screenSize), 0);
    var bgUV = refrUV;
    if (isValidDepth(offDepthRaw) && reconstructLinearZ(offDepthRaw) < waterZ) {
        bgUV = screenUV;
    }
    let bg = textureSampleLevel(sceneColor, sceneColorSampler, bgUV, 0.0).rgb;

    let L = normalize(frame.sunDirection.xyz);
    let NdotL = max(dot(N, L), 0.0);
    let worldPos = input.worldOffsetPos + frame.cameraWorldPos.xyz;
    let shadow = sampleShadowMapPCF(worldPos, N, NdotL, shadowMap, shadowSampler);

    // Light available inside the medium: sky (SH, unshadowed) + shadowed sun.
    let envLight = evaluateAmbientSHL2(N, frame.ambientSH)
                   + NdotL * frame.sunColor.rgb * shadow;
    let L_in = object.scatterAlbedo.xyz * envLight;

    // Single-scatter through a homogeneous medium: the bed attenuates, the murk
    // accumulates. At pathM = 0 this is exactly bg, which is why the shoreline
    // needs no separate coast fade.
    let transmitted = bg * T + L_in * (vec3<f32>(1.0) - T);
```

Delete the old `scatter` term, the `coast` variable, and `absorb`. Change the return to:

```wgsl
    let rgb = mix(transmitted, reflColor, fresnel) + sunSpec;
    return vec4<f32>(rgb, 1.0);  // premultiplied; the medium IS the shore fade
```

Keep `roughness` reading from `object.params.w` → change it to `object.params.y` everywhere it appears (three sites: `reflColor` mip selection, `brdf` lookup, `evaluateSpecularGgxBrdf`).

In the `!transparent` G-buffer fragment, change `output.albedo` to `vec4<f32>(object.scatterAlbedo.rgb, 1.0)` and `output.material.x` to `object.params.y`.

- [ ] **Step 2: Add the `still` feature**

Every `@if` in `shaders/` today is at **item level** — a declaration, a binding, a struct, or a whole function. There is no statement-level precedent, so do not write `@if` inside a function body. Split at function granularity, exactly as the file already does for `shadow_pass`.

Add a still vertex entry and gate the existing one:

```wgsl
@if(!shadow_pass && !still)
@vertex
fn vs_main(input: VertexInput) -> VertexOutput {
    // ... the existing wave-displacing body, unchanged ...
}

// Still water: the surface holds its rest plane, so the vertex stage is the
// plain transform. worldXZ is still carried so the fragment stage keeps one
// VertexOutput shape across both variants.
@if(!shadow_pass && still)
@vertex
fn vs_main(input: VertexInput) -> VertexOutput {
    var output: VertexOutput;
    let base = (object.modelMatrix * vec4<f32>(input.pos, 1.0)).xyz;
    output.position = worldCameraOffsetedSpaceToClipSpace(base);
    output.worldOffsetPos = base;
    output.worldXZ = base.xz + frame.cameraWorldPos.xz;
    return output;
}
```

Then factor the shading normal into two gated helpers and call the one that survives compilation:

```wgsl
// Wave normal + high-frequency detail, in world space.
@if(transparent && !still)
fn waterShadingNormal(worldXZ: vec2<f32>) -> vec3<f32> {
    if (object.params2.y > 0.5) {
        return vec3<f32>(0.0, 1.0, 0.0);  // blockout: dead-flat
    }
    let Nwave = waterWaveNormal(worldXZ, object.time);
    let ts = waterDetailNormalTS(worldXZ, object.time, object.params2.x);
    let T = normalize(cross(vec3<f32>(0.0, 0.0, 1.0), Nwave));
    let B = cross(Nwave, T);
    return normalize(T * ts.x + B * ts.y + Nwave * ts.z);
}

// Standing water: a dead-flat surface. Refraction's offset is N.xz, so it
// vanishes here for free -- no bed smearing across the waterline.
@if(transparent && still)
fn waterShadingNormal(worldXZ: vec2<f32>) -> vec3<f32> {
    return vec3<f32>(0.0, 1.0, 0.0);
}
```

The transparent fragment then opens with:

```wgsl
    let N = waterShadingNormal(input.worldXZ);
```

The `waterDetailNormalTS` import must also be gated `@if(transparent && !still)`, or the still pipeline pulls in an unused module.

**Leave the `@if(shadow_pass)` entry and `waterWorldOffset` completely alone.** Water is a forward material: `kForwardTransparent` registers only `RenderPassType::kForward` (`standard_material_factory.cpp:361`), so `BuildStillWaterForwardFactory` — which declares `supported_pass_types = {kForwardTransparent}` — compiles exactly **one** pipeline, with features `{"transparent", "still"}`. No shadow pipeline exists for it, so `still` and `shadow_pass` are never both defined. The shadow entry in `water.wesl` exists purely so the **kDeferred** factory's `{kGBuffer, kShadow}` variant compiles, and that factory does not carry `still`.

The two `vs_main` gates above stay as written (`!shadow_pass && !still` / `!shadow_pass && still`): the wave-displacing entry is shared with the kDeferred G-buffer factory, where `still` is absent. Fragment-side helpers gate on `transparent && [!]still`, since forward-transparent is the only variant `still` ever reaches.

- [ ] **Step 3: Add the still factory and presets**

In `water_material.hpp`, after `BuildWaterBlockoutForwardFactory`:

```cpp
// Still (standing) water: the forward-transparent surface with the "still"
// shader feature compiled in -- no vertex wave displacement, a dead-flat +Y
// shading normal, and no detail perturbation. Everything else (the Beer-Lambert
// medium, the Fresnel sky reflection, the sun glint, shadowing) is the same
// shader. A separate factory rather than a runtime flag, matching the blockout
// water's precedent; the wave path stays compiled for the game.
std::unique_ptr<MaterialInstanceFactory> BuildStillWaterForwardFactory(
    wgpu::Device device, wgpu::Queue queue, GpuPipelineGenerator* pipeline_gen);

// Murky lake water, calibrated to the Iron Water palette (docs/palettes.html:
// water #41505a, deep water #2b3841, cold glint #6d8994). Extinction is stated
// as a visibility depth in metres and converted (sigma = 3 / d_vis); the
// scattering albedo is fitted so the deep asymptote RENDERS as the deep-water
// swatch after exposure and tonemapping. Pair with
// BuildStillWaterForwardFactory.
InstanceParams StillLakeWaterParams();
```

In `water_material.cpp`:

```cpp
std::unique_ptr<MaterialInstanceFactory> BuildStillWaterForwardFactory(
    wgpu::Device device, wgpu::Queue queue, GpuPipelineGenerator* pipeline_gen) {
  FactoryDescriptor desc = BaseWaterDescriptor();
  desc.supported_pass_types = {MaterialPassType::kForwardTransparent};
  desc.color_formats = {SceneRenderer::kAccumulationFormat};  // HDR
  desc.depth_write = false;
  desc.extra_features = {"still"};
  return BuildMaterialInstanceFactory(desc, device, queue, pipeline_gen);
}

InstanceParams StillLakeWaterParams() {
  InstanceParams params;
  params.uniform_overrides = {
      // sigma = 3 / d_vis; see the header. <-- REPLACE with the values derived
      // in Task 1 Step 4 from the measured median lake depth.
      {"extinction", glm::vec4(2.50f, 0.86f, 0.60f, 0.0f)},
      // Fitted in Task 8 so the deep asymptote renders as #2b3841.
      {"scatterAlbedo", glm::vec4(0.09f, 0.13f, 0.16f, 0.0f)},
      // x=refractStrength (still water has a flat normal, so this is inert),
      // y=roughness (low -> the tight "cold glint").
      {"params", glm::vec4(0.0f, 0.05f, 0.0f, 0.0f)},
      {"params2", glm::vec4(0.0f, 0.0f, 0.0f, 0.0f)},
      {"time", 0.0f},
  };
  return params;
}
```

Remap the two existing presets to the new fields, preserving their current look as closely as the new model allows:

```cpp
InstanceParams DefaultWaterParams() {
  InstanceParams params;
  params.uniform_overrides = {
      // The old absorption scalar was 0.12 against a mixed deep/shallow tint;
      // as a medium that is a ~25 m visibility depth, tinted blue-green.
      {"extinction", glm::vec4(0.40f, 0.16f, 0.12f, 0.0f)},
      {"scatterAlbedo", glm::vec4(0.15f, 0.35f, 0.42f, 0.0f)},
      {"params", glm::vec4(0.03f, 0.06f, 0.0f, 0.0f)},  // refract, roughness
      {"params2", glm::vec4(0.15f, 0.0f, 0.0f, 0.0f)},  // x=detailStrength
      {"time", 0.0f},
  };
  return params;
}

InstanceParams BlockoutWaterParams() {
  InstanceParams params;
  params.uniform_overrides = {
      // Strong extinction so the greybox lake reads as tinted water over the
      // light debug bed while still showing a gradient in the first metres.
      {"extinction", glm::vec4(3.0f, 2.2f, 1.8f, 0.0f)},
      {"scatterAlbedo", glm::vec4(0.019f, 0.065f, 0.091f, 0.0f)},  // #264653
      {"params", glm::vec4(0.0f, 1.0f, 0.0f, 0.0f)},  // no refraction, matte
      {"params2", glm::vec4(0.0f, 1.0f, 0.0f, 0.0f)},  // y=1 -> flat, no IBL
      {"time", 0.0f},
  };
  return params;
}
```

- [ ] **Step 4: Update the GPU cross-check test**

In `game/tests/water_gpu_tests.cpp`, replace the explicit uniform block (~line 436) with:

```cpp
  InstanceParams wp;
  wp.uniform_overrides = {
      {"extinction", glm::vec4(0.0f, 0.0f, 0.0f, 0.0f)},  // T == 1: bg passes through
      {"scatterAlbedo", glm::vec4(0.0f, 0.0f, 1.0f, 0.0f)},
      {"params", glm::vec4(0.4f, 0.9f, 0.0f, 0.0f)},  // refract=0.4, roughness=0.9
      {"params2", glm::vec4(0.6f, 0.0f, 0.0f, 0.0f)},
      {"time", 3.0f},
  };
```

The zero extinction preserves the old test's intent exactly (it set absorption to 0 so the refracted background came through unmodified). Fix any other references the compiler flags.

- [ ] **Step 5: Build and run the water suites**

Run: `scripts/build.sh && scripts/test.sh badlands_water`
Expected: `badlands_water_tests` and `badlands_water_gpu_test` both PASS. The wave normal cross-check is unaffected — the `still` feature is not compiled into the G-buffer factory.

- [ ] **Step 6: Screenshot badlands_game for the shared-shader change**

Run: `scripts/screenshot.sh badlands_game /tmp/game_water.png`
Expected: the game's lake still reads as water with a depth gradient. A flat opaque slab means the extinction remap is far too strong; a fully transparent one means it is too weak.

- [ ] **Step 7: Commit**

```bash
git add shaders/material/water.wesl src/engine/rendering/water_material.hpp \
        src/engine/rendering/water_material.cpp game/tests/water_gpu_tests.cpp
git commit -m "feat(engine): water is a Beer-Lambert medium, plus a still variant

Replaces the absorption scalar and the coast-width alpha ramp with a
per-channel extinction in 1/m and a single-scattering albedo. The coast
showing the bed and the hue sliding toward blue with depth both fall out of
the model rather than being dialled in, and the transmittance IS the shore
fade so the surface can output alpha 1.

The 'still' shader feature drops wave displacement and pins the shading
normal to +Y, keeping reflection, glint and the medium -- standing water
without the wave path being removed."
```

---

### Task 8: Water in mapview + calibration

**Files:**
- Modify: `src/executables/mapview/map_view_view.hpp`, `.cpp`
- Modify: `src/engine/rendering/water_material.cpp` (final calibrated constants)
- Modify: `CMakeLists.txt` (add `src/mapview/lake_surface.cpp` to `badlands_mapview`)

**Interfaces:**
- Consumes: `BuildLakeSurfaceTriangles` (Task 6), `BuildStillWaterForwardFactory` / `StillLakeWaterParams` (Task 7), `AddTransparentMeshEntity(scene, name, mesh, factory, params, transform)` (`engine/rendering/scene_build.hpp`), `PushVertex` (`engine/rendering/geometry/mesh_builder_utils.hpp`), `ComputeLocalAabbFromVertices` + `kTexturedMeshFloatsPerVertex` (`engine/rendering/geometry/textured_mesh_builders.hpp`).
- Produces: nothing consumed downstream.

- [ ] **Step 1: Add the members**

In `map_view_view.hpp`:

```cpp
  // Water: a tiny SceneGraph purely because AddTransparentMeshEntity needs one
  // (the cluster terrain is created directly in the registry -- it is a raw
  // indexed mesh, not a MeshAttachment).
  SceneGraph scene_;
  std::unique_ptr<MaterialInstanceFactory> water_factory_;
```
Include `engine/scene/scene_graph.hpp`, `engine/rendering/scene_build.hpp`, `engine/rendering/water_material.hpp`.

- [ ] **Step 2: Build the water entity**

In `Initialize`, after the cluster terrain build:

```cpp
  // Still lake water. The surface deliberately overlaps the shore and runs
  // under the terrain; water tests depth without writing it, so the buried ring
  // is rejected in hardware (and is what keeps a later wave displacement from
  // opening a gap at the waterline).
  water_factory_ =
      BuildStillWaterForwardFactory(ctx.device, ctx.queue, ctx.pipeline_gen);
  if (!water_factory_) {
    spdlog::error("MapViewView: water factory build failed");
    return false;
  }
  t = clock::now();
  const std::vector<glm::vec3> water_tris =
      BuildLakeSurfaceTriangles(map_, params_.world_size_m);
  if (!water_tris.empty()) {
    TexturedMeshResult lake;
    lake.mesh.geometry_type = GeometryType::kTexturedMesh;
    auto& v = lake.mesh.vertices;
    v.reserve(water_tris.size() * kTexturedMeshFloatsPerVertex);
    for (const glm::vec3& p : water_tris) {
      PushVertex(v, p, glm::vec2(p.x, p.z), glm::vec3(0.0f, 1.0f, 0.0f),
                 glm::vec3(1.0f, 0.0f, 0.0f));
    }
    lake.mesh.vertex_count =
        static_cast<uint32_t>(v.size() / kTexturedMeshFloatsPerVertex);
    lake.local_bounds =
        ComputeLocalAabbFromVertices(v, kTexturedMeshFloatsPerVertex);
    AddTransparentMeshEntity(scene_, "lake_water", std::move(lake),
                             water_factory_.get(), StillLakeWaterParams());
  }
  spdlog::info("water: {} triangles from {} lakes", water_tris.size() / 3,
               map_.lakes.size());
  log_step("water", since(t));
```

- [ ] **Step 3: Sync the scene graph each frame**

Mapview mirrors daylight into `scene_context_` today; `SceneGraph::SyncToRegistry` writes SceneGraph's own sun/ambient over it, so seed them first (this is exactly the ordering `game_view.cpp:556-561` documents). At the end of `ApplyDaylight()`:

```cpp
  scene_.SetSunDirection(scene_context_.sun_direction);
  scene_.SetSunColor(scene_context_.sun_color);
  scene_.SetAmbientSH(scene_context_.ambient_sh);
```

and at the end of `Update()`:

```cpp
  scene_.SyncToRegistry(registry_, scene_context_);
```

Check `game_view.cpp:1180` for the exact call site ordering relative to the LOD update and copy it.

- [ ] **Step 4: Add the source and build**

Add `src/mapview/lake_surface.cpp` to `add_executable(badlands_mapview ...)`.

Run: `scripts/build.sh badlands_mapview`
Expected: `BUILD OK`

- [ ] **Step 5: First look**

Run: `scripts/screenshot.sh badlands_mapview /tmp/water_near.png --seed 2 --camera-height 25`
Expected: water visible over the western lake; the shoreline shows the bed and grades into murk. Read the PNG.

If the water is invisible: check that `--seed 2` actually has a lake near the camera focus (the map centre) — try seeds 1–5, or raise `--camera-height` to find one.

- [ ] **Step 6: Calibrate the deep-water colour**

Sample the deepest water pixel and compare with the Iron Water swatch:

```bash
python3 - <<'PY'
from PIL import Image
import numpy as np
im = np.asarray(Image.open('/tmp/water_near.png').convert('RGB')).astype(int)
target = (0x2b, 0x38, 0x41)
# Bluest, darkest pixels are the deep-water asymptote.
mask = (im[:,:,2] > im[:,:,0]) & (im.sum(2) < 200)
if mask.sum() == 0:
    print('no deep-water pixels found -- water too bright or absent')
else:
    px = im[mask]
    med = np.median(px, axis=0).astype(int)
    print('deep median rgb', tuple(med), 'target', target,
          'delta', tuple(med - np.array(target)))
PY
```

Adjust `scatterAlbedo` in `StillLakeWaterParams()` and re-screenshot until each channel is within ±12 of the target. Scale the whole vector to move brightness; shift channels relative to each other to move hue. Do **not** touch `extinction` to fix colour — it is the measured visibility depth, and changing it changes how fast the bed disappears, not what colour the deep water is.

- [ ] **Step 7: Confirm the shore reads correctly**

Run: `scripts/screenshot.sh badlands_mapview /tmp/water_shore.png --seed 2 --camera-height 15`
Expected: at the waterline the lake bed's texture is visible through the water and fades over the first metres. A hard edge at the shoreline means the transmittance path length is wrong; a bed visible everywhere means extinction is too low for the measured depths (revisit Task 1 Step 4).

- [ ] **Step 8: Full suite**

Run: `scripts/test.sh`
Expected: no new failures. `badlands_game_tests` is seed-flaky by prior history — re-run with `--rng-seed` before blaming this change.

- [ ] **Step 9: Commit**

```bash
git add src/executables/mapview/map_view_view.hpp \
        src/executables/mapview/map_view_view.cpp \
        src/engine/rendering/water_material.cpp CMakeLists.txt
git commit -m "feat(mapview): render still lake water

Wires the per-lake surface into a forward-transparent still-water entity and
calibrates the medium against the Iron Water palette: extinction from the
measured lake depths, scattering albedo fitted so the deep asymptote renders
as #2b3841."
```

---

### Task 9: Documentation

**Files:**
- Modify: `CLAUDE.md`

- [ ] **Step 1: Update the mapview description**

In `CLAUDE.md`'s "Build & run" section, the `badlands_mapview` paragraph currently says it renders "biome-colored terrain". Change that to state it renders per-biome PBR materials (blended from a biome splat) and still lake water.

- [ ] **Step 2: Add a convention note**

In the "Non-obvious conventions" list, add:

```markdown
- **Terrain layer blending is height-lerped, and displacement rides in ARM alpha.** `shaders/common/terrain_layers.wesl` is shared by `terrain_blend` (vertex weights) and `terrain_cluster` (splat texture); `LoadTerrainArrays` folds each pack's `disp` red into its `arm` alpha, so height blending costs no extra array or fetch.
- **Water is a Beer-Lambert medium, not a tint.** `water.wesl` takes a per-channel extinction in 1/m plus a scattering albedo; the shore fade and the depth hue shift both fall out of transmittance, so there is no coast-width or absorption knob. The `still` shader feature is standing water (no waves, flat normal).
```

- [ ] **Step 3: Commit**

```bash
git add CLAUDE.md
git commit -m "docs: mapview renders textured terrain + still water"
```

---

## Verification

End-to-end, from a clean checkout of the branch:

```bash
scripts/build.sh                       # BUILD OK
scripts/test.sh                        # full ctest summary
scripts/test.sh badlands_biome_splat_tests
scripts/test.sh badlands_lake_surface_tests
scripts/test.sh badlands_terrain_blend_tests
scripts/test.sh badlands_water

# mapview: near shot (materials + water), far shot (mips), tint mode intact
scripts/screenshot.sh badlands_mapview /tmp/v_near.png  --seed 2 --camera-height 25
scripts/screenshot.sh badlands_mapview /tmp/v_far.png   --seed 2 --camera-height 400
scripts/screenshot.sh badlands_mapview /tmp/v_lod.png   --seed 2 --camera-height 120 --lod-tint 2

# the two shared shaders must not have broken the game
scripts/screenshot.sh badlands_game /tmp/v_game.png
```

Read all four PNGs and confirm:
1. Terrain shows distinct per-biome PBR materials with interlocking transitions, not flat colours.
2. Biome regions are stable and shimmer-free when zoomed out.
3. `--lod-tint 2` still tints by LOD level.
4. Lake water is murky, shows the bed at the shoreline, and darkens toward blue with depth.
5. The lake has no gap or hard seam at the waterline.
6. `badlands_game` still renders its terrain and lake correctly.

**Known pre-existing flake:** `badlands_game_tests` fails or hangs seed-dependently, unrelated to this work. Re-run with `--rng-seed <n>` before treating it as a regression.

---

## Self-Review Notes

- **Spec coverage:** A.3 → Task 4; A.4 → Tasks 3 and 5; A.5 → Tasks 2 and 3; A.6 → Task 3 Step 6; B.2/B.4/B.5 → Task 6; B.3 → verified by Task 8 Step 7; C.1–C.3 → Task 7; C.4 → Tasks 1 and 8 Step 6; C.5 → Task 7 Step 2; D.1–D.6 → Tasks 2, 5, 7, 8; E → Tasks 2, 3, 4, 6 tests + the Verification section; F (out of scope) → nothing added for rivers, waves, swamp palettes, or vertex-colour removal.
- **Spec item D.4 (a generic CPU-bytes→texture helper) needs no new code**: `UploadTexture2DWithMips` already exists in `engine/rendering/texture_loader.hpp` and is used directly in Task 5.
- **Both originally-uncertain points are now resolved against the code**: the decode entry point is `badlands_decode_image` returning `BadlandsImage{rgba, width, height}` (`texture_loader.cpp:88`), and WESL `@if` is item-level only across all 126 uses in `shaders/`, so Task 7 Step 2 splits at function granularity rather than inside a body.
- **Task ordering is a dependency chain**, not a preference: 2 → 3 (height blending needs ARM alpha), 3 + 4 → 5 (the cluster shader needs both the module and the splat), 1 → 7 (extinction needs the measured depths), 6 + 7 → 8. Tasks 4 and 6 are pure CPU and can be done in either order relative to 2/3.
