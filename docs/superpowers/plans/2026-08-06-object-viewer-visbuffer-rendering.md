# Plan — object_viewer stage 4: visbuffer, resolve, lighting, debug views

Spec: `docs/superpowers/specs/2026-08-06-object-viewer-visbuffer-rendering-design.md` (approved).
Binding rules: `src/engine/rhi/CLAUDE.md` (4, 6, 9, 10, 13) and the root `CLAUDE.md` UI rule.

Five sub-stages. **Each one lands green on its own**, with its own test and its own red
proof, so a failure names the stage that caused it rather than the last one to compile.

---

## 4a — Output colour space: the RHI swapchain, and one shared transform

The only stage that touches the RHI, done first so everything after it emits pixels the
right way from the start rather than being retrofitted.

1. `rhi_types.hpp` — `enum class ColorSpace { Srgb, DisplayP3, ExtendedLinearDisplayP3 }`,
   `SwapchainDesc::color_space` (default `Srgb`, so every existing call site is untouched).
2. `src/engine/rhi/metal/metal_color_space.mm` — the layer tagging. Ported from
   `src/engine/rendering/metal_layer_color.mm`, which currently sits with the Dawn code;
   the RHI cannot depend on that target. **The Dawn copy stays** — `badlands_game` still
   uses it, and deleting it is a separate change.
3. Metal `CreateSwapchain` — tag, then enforce the safety rule: **a float surface that
   failed to tag drops to `BGRA8Unorm` and reconfigures once**, logged. Linear values into
   a nil-colorspace layer have no defined transfer, so presenting untagged is silent
   corruption, not a degraded look.
4. Null — carries the field and reports it back through the swapchain, so a conformance
   test can assert the two backends agree about what is constructible (rule 13).
5. `shaders/slang/common/output_transform.slang` — `SrgbToLinear`, `LinearToSrgb`,
   `LinearSrgbToLinearP3` (matrix ported from `shaders/common/colorspace.wesl`), and
   `EncodeOutput(float3 ldr, uint mode)`.
6. Wire the two existing producers: `LinePass`'s frame uniform and
   `ImGui_ImplRHI_InitInfo::output_mode`.

**Tests**
- Conformance, both backends: the new field round-trips; an unsupported combination is
  refused at creation time, not at present.
- **The two-sink test.** Render `--scene lines` into an `RGBA8Unorm` sink and an
  `RGBA16Float` sink; the float texel must equal the 8-bit one put through decode +
  primaries. Needs no HDR display and no swapchain.
- The same two-sink comparison for a fixed ImGui rect (`imgui_rhi_tests`).
- The untagged-float refusal, asserted on its log/fallback rather than on a picture.

**Red proofs**
- Drop the decode from the float path → the two-sink test goes red.
- Hard-code `output_mode = Srgb8` inside `imgui_impl_rhi` → the ImGui half goes red.
- Let a failed tag keep the float format → the refusal test goes red.

---

## 4b — Depth attachments in the render graph

Stages 0–3 listed this as out of scope; `ResourceState::DepthWrite`/`DepthRead` already
exist in the RHI, so this is the graph catching up.

1. `RasterPassBuilder::DepthTarget(handle, LoadOp, StoreOp, float clear_depth)`.
2. `Compile()` derives `DepthWrite` for the writing pass and `ShaderRead` for any later
   pass that samples it.
3. **Refusals, logged:** a depth-format resource used as a colour target, a colour-format
   resource used as a depth target, and two depth targets on one pass.

**Tests** — `render_graph_tests`, Null backend, no GPU, inside a validation scope: the
derived transitions, and each of the three refusals.

**Red proof** — emit `RenderTarget` instead of `DepthWrite` → the validation scope goes
dirty. This is the property the graph was built early to make checkable, and it costs no
GPU to check.

---

## 4c — The plane and the visibility buffer

Asserted **without** the resolve, by reading the `R32Uint` texture back directly. A stage
that could only be checked through the next stage is not a stage.

1. `src/executables/object_viewer/plane_mesh.{hpp,cpp}` — 8 × 8 quads, ±5 m, UV ×4, the
   three-`float4` vertex. Pure CPU.
2. `shaders/slang/object_viewer/visbuffer.slang` — vertex pulls by `SV_VertexID`; fragment
   writes `(draw_slot << 24) | (SV_PrimitiveID + 1)`.
3. `VisbufferPass` — `R32Uint` (clear 0) + `Depth32Float` (clear **0.0**, `GreaterEqual`),
   through the graph, plus the `DrawInfo` and vertex/index storage buffers.
4. `--scene plane` with a perspective camera; `lines` and `grid` keep their ortho camera.

**Tests**
- CPU unit tests on the mesh: triangle count, winding, UV range, tangent orthonormality.
- Visbuffer readback: the centre pixel's packed value equals the CPU ray/plane triangle
  index + 1; the background is exactly `0`; every non-zero value decodes to draw 0.
- Depth readback: monotonic front-to-back, and a nearer pixel holds the **larger** value.

**Red proof** — clear depth to `1.0` → the plane vanishes under `GreaterEqual` and both
readback tests go red. That is the reversed-Z convention asserted rather than commented.

---

## 4d — The resolve: barycentrics, gradients, material, shading

The stage the rest exists for.

1. `src/executables/object_viewer/material_pack.{hpp,cpp}` — `material.json` via
   `nlohmann_json`, decode via `badlands_decode_image`, CPU box-filtered mips, upload via
   `ITexture::Write(mip, layer, …)`. Albedo as `RGBA8UnormSrgb`; normal/ARM/displacement
   linear. Default pack `aerial_rocks_01_1k`.
2. `shaders/slang/common/{brdf,sh_lighting,standard_lighting}.slang` — **ported verbatim**
   from the WESL originals. Inputs patched only: `shadow = 1.0`, `ambientSpecular = 0`,
   sun from `object_viewer`'s frame uniform.
3. `shaders/slang/object_viewer/resolve.slang` — fullscreen; unpack → `DrawInfo` → indices
   → vertices → **barycentrics by edge functions + `1/w`** → **`∂λ/∂x`, `∂λ/∂y` from the
   same edge coefficients** → interpolate uv/normal/tangent and carry `∂uv/∂x`, `∂uv/∂y` →
   `SampleGrad` → tangent-space normal → `shadeStandard` → branch on `debug_view` →
   `EncodeOutput`.
4. `--debug-view <name>` on the CLI, one name per enumerator.

**Tests** — the full §9 oracle table. The `lit` case is a CPU port of `shadeStandard` +
`brdf`, which **doubles as the proof the Slang port matches the WESL original** — the only
check that the port did not quietly change the shading.

**Red proofs**
- Swap two barycentric components → the vertex/centroid test goes red.
- Substitute `ddx`/`ddy` for the analytic gradients → the grazing-angle mip test goes red.
- Evaluate the ported SH with `lab_common`'s convolved form → the `lit` oracle goes red.
- Drop the sRGB→P3 matrix → `albedo` goes red while the four scalar previews stay green,
  because the matrix is identity on D65 neutrals.

---

## 4e — The two windows, and the one that shrinks

**Bound by the root `CLAUDE.md` rule: nothing in a UI without explicit approval.** These
are the approved elements and there are no others.

1. `Graphics debug` — one radio group of ten: `Lit`; `Triangle ID`, `Barycentric`, `Depth`;
   `Albedo`, `Normal`, `Roughness`, `Metallic`, `Displacement`, `AO`.
2. `Scene lighting` — `azimuth`, `elevation` (degrees), `color` (`ColorEdit3`), `intensity`.
3. The existing `object_viewer` window: **camera readout and zoom slider removed**, one
   frame-timing line kept.

**Tests** — the widgets are not testable, but the two things behind them are, and both are
pure CPU:
- `(azimuth, elevation) → direction` round-trips, and elevation 90° points straight up.
- **Every enumerator in the UI radio group has a `--debug-view` name and a distinct shader
  constant**, asserted by iterating the enum. This is what stops a UI-only mode from
  existing that no headless assertion ever covers.

---

## Verification

- `scripts/build.sh` → `BUILD OK`; `scripts/test.sh` → every suite.
- Metal suites under `MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1`.
- `ctest -L display` — `rhi_lab_windowed_resize` and the `object_viewer` escape self-test.
- Both RHI suites clean under AddressSanitizer.
- `badlands_object_viewer --headless --scene plane --debug-view <each>` exits 0.
- `rhi_lab` renders unchanged — it shares the swapchain that 4a modifies.
- CMake still configures with no Slang SDK present.

## Out of scope, stated so it is not assumed

SSAO, shadow maps, contact shadows, IBL specular, transparency, a second material, LOD,
meshlets, transient-resource aliasing, a topological sort in the graph, and any port of
`SceneRenderer`. Metallic and displacement are **preview-only** — the ported `shadeStandard`
is dielectric-only (`F0` fixed at 0.04) and one material has no height-blend, so neither
channel drives anything. `badlands_viewer` is untouched.
