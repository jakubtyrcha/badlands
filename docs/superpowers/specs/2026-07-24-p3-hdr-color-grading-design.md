# Badlands — P3/HDR output pipeline + Oklab color grading

> Design document. Gives the renderer a **Display-P3 / HDR (EDR) output path**,
> adaptive to the display, and adds an **Oklab color-grading pass** to stylize
> the lighting. The scene keeps rendering in linear-sRGB for now; the sRGB→P3
> conversion happens at one point (the final resolve). Grounded in the current
> code and the pinned Dawn/SDL3 sources (paths cited).

## What prompted this

We want to **stylize the lighting** (crush blacks, desaturate midtones while
preserving already-saturated colors and HDR highlights) via a color-grading
pass, and to output in a **wide-gamut P3 / HDR** space rather than plain
SDR-sRGB. Directives settled during design:

- **P3 (or wider/HDR) is the output space even on SDR displays; a tone curve is
  added only for SDR displays.** HDR displays get a linear-EDR passthrough.
- **Scope reduction (user):** keep the scene *lit* in linear-sRGB for now
  ("stick to sRGB albedo for the time being"). Rendering the scene *in* P3
  (converting albedo, sun, SH, IBL, and every material tint to the P3 basis) is
  **deferred future work** — note that when it happens, *all* inputs to a
  lighting product must convert together, since `M·(a·l) ≠ (M·a)·(M·l)` for a
  gamut matrix `M`.

Today the pipeline is entirely linear-sRGB → clamp+sRGB-gamma → 8-bit SDR
surface: scene composited into an `RGBA16Float` HDR buffer
(`scene_renderer.hpp:220`), resolved by `shaders/passes/tonemapping.wesl`, onto
a surface chosen as `capabilities.formats[0]` with **no color-space
configuration** (`gpu_context.cpp:217,227`).

## Feasibility (verified in the pinned deps)

- **`wgpu::SurfaceColorManagement` is hard-rejected by the pinned Dawn**:
  `dawn/native/Surface.cpp:108` returns
  `DAWN_VALIDATION_ERROR("SurfaceColorManagement unsupported.")`. The standard
  WebGPU color-management route is a dead end on this pin (do not bump Dawn
  without approval).
- **The route that works: configure the CAMetalLayer directly.** badlands
  creates the layer itself (`gpu_context.cpp` `CreateSurface`:
  `SDL_Metal_CreateView` → `SDL_Metal_GetLayer` → `WGPUSurfaceSourceMetalLayer`)
  and Dawn's swapchain (`SwapChainMTL.mm::Initialize`) sets only
  `drawableSize/framebufferOnly/device/pixelFormat/opaque/displaySyncEnabled` —
  it never touches `colorspace` or `wantsExtendedDynamicRangeContent`, so
  app-set values persist across Configure/resize.
- **`RGBA16Float` is a supported Metal surface format**
  (`PhysicalDeviceMTL.mm:359`). macOS is also the only platform with a working
  surface path (`CreateSurface`'s `#else` is unimplemented), so the CAMetalLayer
  route covers all real targets.
- **HDR detection**: SDL3 exposes `SDL_PROP_WINDOW_HDR_ENABLED_BOOLEAN` /
  `SDL_PROP_WINDOW_HDR_HEADROOM_FLOAT`; `SDL_EVENT_WINDOW_HDR_STATE_CHANGED`
  fires on changes (v1 decides at startup and ignores the event — ImGui's
  render-target format is fixed at init).
- **Display P3 uses the sRGB transfer curve**, so the existing `linear_to_srgb`
  (`shaders/common/color.wesl`) is the correct SDR-P3 encode.
- **Color helpers already exist**: `shaders/common/colorspace.wesl` has
  `linear_srgb_to_linear_p3`, `linear_srgb_to_oklab`, `oklab_to_linear_srgb`.
- **UI is half-ready**: `ui.wesl` and the ImGui backend already linearize their
  sRGB colors when `output_is_linear == 1`; only the sRGB→P3 primary shift is
  missing.

## Architecture

- **Working buffer stays linear-sRGB** end to end (scene → grade → debug lines).
  No per-pass input conversions.
- **Grade pass** (new): after projected decals, before debug lines — "after
  lighting, before debug overlays." An HDR→HDR fullscreen remap in **sRGB-Oklab**
  (`linear_srgb_to_oklab` → black-crush + midtone-desat → `oklab_to_linear_srgb`),
  using the renderer's existing snapshot-copy pattern (a target can't be sampled
  while bound). Runs only when enabled; default **off** in all apps, enabled by
  `badlands_game`.
- **Resolve** (`tonemapping.wesl`) owns the single sRGB→P3 step, selected by
  `tonemapMode == 2`:
  - **HDR** (float surface + `kCGColorSpaceExtendedLinearDisplayP3` +
    `wantsExtendedDynamicRangeContent`): `linear_srgb_to_linear_p3`, passthrough
    retaining >1.0 (clamp negatives). **No tone curve** — the EDR compositor
    maps to display headroom.
  - **SDR-P3** (`BGRA8Unorm` + `kCGColorSpaceDisplayP3`):
    `linear_srgb_to_linear_p3`, clamp to `[0,1]` (the existing minimal curve; no
    filmic operator unless separately requested) + sRGB-curve encode. *The only
    place a tone curve is applied.*
  - **Fallback** (layer bridge unavailable): today's sRGB clamp+gamma path,
    unchanged.
- **UI / ImGui**: drawn onto the surface after resolve; when the surface is P3
  they apply `linear_srgb_to_linear_p3` after their existing linearize step
  (new `output_is_p3` uniform in their own UBOs).
- **Frame UBO stays size-locked** (`static_assert(sizeof(UniformData)==592)`):
  no new field. `tonemapMode = 2` encodes P3 output; `outputIsLinear`
  distinguishes EDR passthrough vs SDR encode.
- **Screenshots/recordings stay plain sRGB** (the throwaway capture renderer
  never sets P3 mode): P3-encoded pixels in a profile-less PNG would be read as
  sRGB (desaturated) and would break capture diffs. Consequence: the neutral
  substrate is **bit-identical** on the capture path — a strong regression guard.

### Boundaries preserved deliberately

- `SceneContext` colors stay sRGB — no color-space concept leaks into the
  engine interface or the game.
- IBL generation, G-buffer encoding, all material shaders: untouched.
- Debug lines: untouched (they draw into the sRGB working buffer and convert
  with the scene at resolve).

## Components

1. **`metal_layer_color.mm`** (macOS-only ObjC++ bridge):
   `ConfigureMetalLayerColorSpace(void* layer, bool hdr_extended)` — sets
   `colorspace` (+ EDR flag for HDR). Called by `GpuContext::Configure`.
2. **`GpuContext`**: stores the layer pointer; queries HDR at startup; picks
   `RGBA16Float` (HDR) vs `BGRA8Unorm` (SDR); exposes `IsHdr()`/`IsP3()`; logs
   the chosen path.
3. **`SceneRenderer`**: `SetOutputIsP3(bool)` → `tonemap_mode = 2`.
4. **`tonemapping.wesl`**: mode-2 branch as above.
5. **`ui.wesl` + `imgui_impl_wgpu_custom`**: `output_is_p3` uniform + primary
   conversion; flag plumbed from `GpuContext` through `UiRenderer::Prepare` and
   `ImGui_ImplWGPU_InitInfo`.
6. **Color grading**: `ColorGradingConfig` (enabled=false default, six params:
   blackCrushThreshold/Strength, midtoneLuminanceStart/End,
   midtoneDesatStrength, saturationPreservationMask), a `ColorGrading` class
   modeled on `VolumetricFog` (params UBO + fullscreen pipeline), an
   `editor_ui` panel mirroring the fog editor. Shader guards: threshold
   clamped ≥0.01 before the divide; output clamped ≥0 (Oklab edits can push out
   of gamut; negatives must not reach the EDR compositor).

## Phasing

- **Phase 1 — P3/HDR output substrate (neutral):** items 1–5. Correct neutral
  image on SDR and HDR displays; capture path bit-identical; on-screen SDR
  visually unchanged (sRGB→P3→panel round-trips); HDR gains EDR highlights.
- **Phase 2 — Grading stylization + editor:** item 6. Grade in sRGB-Oklab,
  default off, `badlands_game` enables; ImGui sliders.

## Testing

- Phase 1: capture-path screenshots bit-identical pre/post; bounded headless
  smoke run with no Dawn validation errors; log asserts the chosen surface
  path on an HDR Mac (float + EDR) and the SDR fallback.
- Phase 2: `enabled=false` capture bit-identical (no-op guard); a fixed-params
  capture visibly shows crush/desat while saturated colors and >1.0 highlights
  survive (the masks). No pinning of the evolving stylized look — tests target
  the deterministic invariants.

## Out of scope

- Rendering the scene in P3 (all-inputs conversion — future work, see above).
- Native-P3 assets; filmic/AgX/ACES operator; HDR screenshot capture;
  mid-session HDR switching (event logged, decision stays startup-time).
