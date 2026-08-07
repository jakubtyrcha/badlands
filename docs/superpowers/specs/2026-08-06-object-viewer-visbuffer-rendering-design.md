# object_viewer stage 4 — visibility buffer, material resolve, lighting, debug views

Status: **SPEC — awaiting approval.** Nothing here is built yet.
Predecessor: `2026-08-06-render-graph-and-object-viewer-design.md` (stages 0–3).

---

## 0. Ideas considered, and what was rejected

| Idea | Verdict |
|---|---|
| **Visbuffer + barycentric resolve** (Burns/Hunt): raster writes `(draw, primitive)`, the resolve refetches the triangle and derives everything else | **Taken.** Barycentrics are not a preview, they are the mechanism — §4. |
| **Thin G-buffer** (albedo / normal / ARM targets) | Rejected. Simpler, but it is not a visbuffer. |
| **`SV_Barycentrics` written to a second attachment** | Rejected. Then the raster produces them and the visbuffer stops being a visbuffer. |
| **Debug views as one uniform-branched resolve** vs N fragment entry points | **One branched resolve.** Lit and debug then read identical material code, which is the only way a debug view can be trusted to describe the lit view. |
| **Real PBR pack** vs a procedural checker | **Real pack.** Roughness/metallic/displacement/AO previews of a constant assert nothing. |
| **New GGX BRDF** vs **port the engine's** | **Port.** Ruled by the user: take the shading from the existing shader, patch only the inputs. See §5. |
| **Meshlets / cluster DAG here** | Rejected. `rhi_lab` already proves that path; this stage is about the resolve. |

---

## 1. Scope

**In:** a tessellated debug plane; a visibility-buffer raster pass with depth; a resolve
that recovers barycentrics *and their screen-space gradients* and interpolates attributes;
one PBR pack sampled through the RHI; the engine's existing shading, ported; ten debug
views; two new ImGui windows; an output path correct on both an 8-bit and an EDR float
sink; headless pixel assertions for all of it.

**Out, stated so it is not assumed:** SSAO, shadow maps, contact shadows, IBL specular,
transparency, more than one material, LOD, meshlets, the `SceneRenderer` port.
`badlands_viewer` is untouched.

---

## 2. Geometry — `--scene plane`

- A tessellated quad on y = 0: **8 × 8 quads = 128 triangles**, spanning ±5 m, UV tiling 4×.
  Tessellation and span are `constexpr`; only the scene selector is runtime.
- **128 triangles, not 2.** A two-triangle quad makes the triangle-ID preview a two-colour
  image and the barycentric preview two gradients — neither can distinguish a correct
  resolve from a plausible one.
- Vertex layout, **three `float4`s (48 bytes)** so the CPU and MSL agree. A `float3` pads to
  16 bytes in MSL, which shears every vertex read:
  ```
  float4 pos_nx;   // xyz position,  w normal.x
  float4 nyz_uv;   // xy normal.yz,  zw uv
  float4 tangent;  // xyz tangent,   w bitangent sign
  ```
- Vertices and 32-bit indices live in `StructuredBuffer`s. **The RHI has no vertex buffers
  by design**; the raster pulls by `SV_VertexID`, exactly as `LinePass` does.
- **Camera:** `--scene plane` is perspective. `lines` and `grid` stay orthographic — the
  line scene's assertion is a closed form that only holds for an unrotated ortho camera.

---

## 3. The visibility buffer

- **`R32Uint`** cleared to `0` = "nothing here", plus a **`Depth32Float`** attachment.
- **Reversed-Z throughout:** near → 1, far → 0, depth clears to `0.0`, compare `GreaterEqual`.
- Packing (**ruled**): **`(draw_slot << 24) | ((primitive_id + 1) & 0xFFFFFF)`** — 8 bits of
  draw (255), 24 bits of primitive (16.7 M). The `+1` keeps `0` reserved for "empty".
- **Masked in the shader, refused on the CPU.** Unmasked, a primitive past 2²⁴−1 overflows
  into the draw bits and the resolve fetches an out-of-bounds `DrawInfo`. A mask alone
  contains that but makes it *silent*, which rule 1 forbids — so the CPU, which knows the
  triangle count when it builds `DrawInfo`, refuses the mesh with a log. The mask is the
  memory-safety backstop; the refusal is the error report.
- A **`DrawInfo` storage buffer** — `{ first_index, first_vertex, material_slot }` — with one
  live entry. Not speculative: the resolve reads `first_index` to locate the triangle's
  three indices. With one draw the value is 0, but the fetch is real code on the real path.

---

## 4. The resolve — barycentrics are the mechanism

Yes, barycentrics are required, and they carry more weight than the preview suggests.
**Without them the visbuffer produces nothing but a triangle ID:** every interpolated
attribute — uv, normal, tangent — exists only because the resolve reconstructs them.

One fullscreen raster pass, per pixel:

1. Load `packed`. If `0`, emit the background and stop.
2. Unpack draw and primitive; fetch `DrawInfo`, then three indices, then three vertices.
3. Transform the three positions by `viewProj` → three clip positions.
4. **Derive perspective-correct barycentrics** at this pixel's NDC, by edge functions over
   the projected triangle followed by the `1/w` correction.
5. **Derive their screen-space gradients from the same edge functions** — `∂λ/∂x`, `∂λ/∂y`
   fall out of the edge coefficients already computed in step 4, so this is roughly fifteen
   extra lines, not a second derivation.
6. Interpolate uv, normal, tangent; carry `∂uv/∂x`, `∂uv/∂y` through the same weights.
7. Sample albedo / normal / ARM / displacement with **`SampleGrad`** using those gradients.
8. Build the shading normal from the interpolated tangent frame and the normal map
   (`normal_format: "dx"` in the manifests — green is **not** flipped).
9. Shade (§5), branch on `debug_view`, apply the output path (§6).

**Analytic gradients, not `ddx`/`ddy`.** Screen-space derivatives of a resolved attribute
are wrong in every quad that straddles a triangle edge, which on a 128-triangle plane is a
mip seam along every internal edge. Since step 5 is nearly free once step 4 exists, taking
the correct path costs almost nothing — and it is what makes the resolve usable for real
meshes rather than only for a flat test plane.

**The known failure mode: a triangle straddling the near plane.** The resolve refetches the
*original, unclipped* vertices, so a vertex behind the eye has `w ≤ 0` and the 2D
screen-space edge functions of steps 4–5 are not merely imprecise, they are meaningless.
This is reachable in this stage rather than hypothetical — WASD can put the camera inside a
ground-plane triangle. It gets its own oracle case, and if that case fails the fix is to
reformulate the derivation as a world-space ray/triangle intersection against the
reconstructed view ray. **Clamping `w` to an epsilon is not the fix**; it converts a visible
failure into a plausible-looking wrong answer.

---

## 5. Shading — ported, not written

**Ruled: take the shading from the existing shader and patch only the inputs.** Ported to
Slang verbatim:

| Source | What comes across |
|---|---|
| `shaders/common/brdf.wesl` | `evaluateEonRetroBrdf` (EON Oren-Nayar + Callisto retroreflection), `evaluateSpecularGgxBrdf`, `evaluateLagardeFresnel` |
| `shaders/common/sh_lighting.wesl` | `evaluateAmbientSHL2` |
| `shaders/common/standard_lighting.wesl` | `shadeStandard` and `computeSpecularOcclusion` |

The patched inputs, and nothing else:
- `albedoLinear`, `N`, `V`, `roughness`, `ao` come from the resolve instead of the G-buffer.
- `shadow = 1.0` — no shadow maps this stage.
- `ambientSpecular = float3(0)` — no IBL this stage.
- `frame.sunDirection` / `frame.sunColor` come from `object_viewer`'s own frame uniform,
  driven by the `Scene lighting` window.

### Two consequences worth stating rather than discovering later

- **Metallic is preview-only.** `shadeStandard` uses a fixed `F0_DIELECTRIC = 0.04` and
  never reads a metallic channel; `material_library.hpp` says the same on the CPU side
  ("metallic is currently unused"). Feeding metallic into `F0` would be *changing the
  shading math*, not patching its inputs, so it is out — the `Metallic` debug view shows the
  authored channel, and that channel drives nothing. **Displacement is preview-only for the
  same reason:** with one material there is no height-blend to consume it and no parallax.
- **The SH convention must come across with the evaluator.** The engine's
  `evaluateAmbientSHL2` evaluates the *raw* basis because the diffuse convolution is baked
  into the coefficients at projection time; `lab_common.slang`'s `EvaluateSH` applies the
  `c1…c5` convolution inline instead. Mixing one's coefficients with the other's evaluator
  silently double-applies or drops the convolution. `object_viewer` takes the engine's pair.

---

## 6. Output — LDR data onto an EDR surface

**Ruled: go EDR now**, matching what the Dawn path already does on an HDR display.

### 6.1 What the Dawn path actually does — and why one flag is not enough

`gpu_context.cpp:273`–`330` plus `tonemapping.wesl` mode 2 do **three** things, not one:
1. Pick `RGBA16Float` when the display reports HDR.
2. Tag the `CAMetalLayer` **Display P3** — *extended-linear* P3 for the float surface,
   sRGB-encoded P3 for an 8-bit one (`ConfigureMetalLayerColorSpace`, already in
   `src/engine/rendering/metal_layer_color.mm`).
3. Convert **sRGB primaries → P3 primaries** (`linear_srgb_to_linear_p3`) before writing.

So the transform is `(linear vs encoded) × (sRGB vs P3 primaries)`, and a lone
`output_is_linear` gets the gamut wrong. There is also a safety rule to carry across:
**a float surface must never stay untagged** — linear values into a nil-colorspace layer
have no defined transfer — so a failed tag drops to `BGRA8Unorm` and reconfigures once
(`ResolveSurfaceFormatAfterTagging`).

### 6.2 The single function every producer uses

```hlsl
// `linear_display` is display-referred and LINEAR, in sRGB primaries.
float4 EncodeOutput(float3 linear_display, uint output_mode);
//   Srgb8                : no primaries change; encode with the sRGB curve
//   DisplayP3_8          : sRGB->P3 -> clamp -> encode (P3 uses the sRGB curve)
//   ExtendedLinearP3_16  : sRGB->P3 -> write linear, unclamped
```
Used by **all three** producers, or each is wrong on a float sink:
- the resolve — the tonemapped lit view *and* all nine debug views;
- `imgui_impl_rhi` — ImGui's vertex colours and atlas are sRGB
  (`imgui_impl_wgpu_custom.cpp:82` already does this for Dawn);
- `LinePass` — its colours are authored in the same space.

**The input is LINEAR.** An earlier draft of this spec took sRGB-encoded input, which made
every producer encode only for `EncodeOutput` to decode again on the next line. That
round-trip does no work, and it invites a real double-decode the moment one half of it is
forgotten — the albedo path is one missing re-encode away from it, since the
`RGBA8UnormSrgb` sampler has already decoded in hardware. Everything upstream is linear
already: albedo from the sampler, and the tonemap's output once it drops its `pow(1/2.2)`.

**Debug views still emit their raw value as the display code value**, so every material
preview is an exact number rather than a picture — but that claim is now spelled once, as
`SrgbToLinear(raw)`, at the point the claim is made. "Interpret this data byte as a screen
code value" becomes a deliberate, named act rather than a side effect of the contract.

### 6.3 Ruled: tag Display P3 in **both** cases

`object_viewer` matches `badlands_game` on the same display — extended-linear P3 on the
float surface, sRGB-encoded P3 on the 8-bit one. The sRGB→P3 matrix is therefore on **both**
paths, and `Srgb8` (untagged) survives only as the fallback a failed tag drops to.

**One consequence that makes the tests easier than it looks: sRGB and Display P3 share the
D65 white point, so the primaries matrix maps neutrals to themselves exactly.** The four
scalar previews (roughness, metallic, AO, displacement) are greyscale and still read back
as their source bytes. Only the chromatic previews — albedo, the encoded normal, the
triangle-ID hash, the barycentric triple — need the matrix in their oracle. A test that
forgets it will pass on the scalars and fail on the chromatics, which is the right shape.

---

## 7. Interfaces this stage changes (approval needed)

1. **`rhi::SwapchainDesc::color_space`** — new enum
   `ColorSpace { Srgb, DisplayP3, ExtendedLinearDisplayP3 }`, plus Metal layer tagging and
   the untagged-float refusal. `metal_layer_color.mm` moves or is duplicated into the RHI;
   it currently sits under `src/engine/rendering/` with the Dawn code.
2. **`graph::RasterPassBuilder::DepthTarget(handle, LoadOp, StoreOp, float clear_depth)`** —
   the graph has no depth attachment; stages 0–3 listed it as out of scope.
   `ResourceState::DepthWrite`/`DepthRead` already exist in the RHI, so this is the graph
   catching up rather than a new RHI concept.
3. **`ImGui_ImplRHI_InitInfo::output_mode`** and the same on `LinePass`'s frame uniform.
4. **A Dawn-free PBR pack loader.** `MaterialLibrary` takes a `wgpu::Device` and cannot
   serve. Proposed **`src/executables/object_viewer/material_pack.{hpp,cpp}`**: parse
   `material.json` with `nlohmann_json`, decode with `badlands_decode_image` (already
   linked), box-filter the mip chain on the CPU, upload via
   `ITexture::Write(mip, layer, …)`. Kept local to `object_viewer` because it has exactly
   one consumer; the promotion point is the second one.
5. **Slang ports** of `brdf`, `sh_lighting` and `shadeStandard` under
   `shaders/slang/common/`, so the next RHI pass shares them rather than re-porting.

Default pack: **`aerial_rocks_01_1k`** — it has all four maps.

---

## 8. The two ImGui windows, and the third that survives

### `Graphics debug`
One radio group, ten entries, exactly as requested:

| Group | Entries |
|---|---|
| — | `Lit` (default; not a debug view) |
| Visbuffer | `Triangle ID`, `Barycentric`, `Depth` |
| Material | `Albedo`, `Normal`, `Roughness`, `Metallic`, `Displacement`, `AO` |

- `Triangle ID` — the primitive index hashed to colour, the `0.5 + 0.5·cos(2π(id·k + phase))`
  `rhi_lab` already uses.
- `Barycentric` — rgb = the three weights.
- `Depth` — view-space distance normalized between near and far, **white = near**. Raw
  reversed-Z is not shown: under perspective it clusters against 1.0 and reads as flat white.
- `Normal` — the world-space *shading* normal (normal map applied), remapped `n·0.5 + 0.5`.
- The four scalar channels are greyscale.

### `Scene lighting`
Directional light only: `azimuth` and `elevation` in degrees, `color` (`ColorEdit3`), and
`intensity`.

### The existing `object_viewer` window — **ruled: fps only**
The camera-position readout and the zoom slider are removed. One frame-timing line stays.

### The rule this stage is bound by
`CLAUDE.md`: **never add anything to a UI without explicit approval.** These windows hold
the elements listed above and no others — no stats block, no pack selector, no tonemap knob,
no gizmo toggles.

### Passes — **ruled: plane only**
Three passes: visbuffer raster → resolve → ImGui. No grid or axes over the lit plane, so no
line pixel can land on a texel a material preview is asserting.

---

## 9. Testing — a CPU oracle per view

Headless, `--scene plane --debug-view <name>`; exit status is the assertion, per the existing
`object_viewer` contract. Every case compares against a CPU evaluation of the same rule —
the oracle pattern that caught the real defects in the blend and splat work.

| Case | Oracle |
|---|---|
| `triangle-id` | A CPU ray/plane intersection names the triangle under the centre pixel; the hash of that index must equal the rendered colour. |
| `barycentric` | At a projected vertex the weights are a basis vector; at a projected centroid, ≈(⅓,⅓,⅓). |
| **uv gradients** | At a known grazing angle, the resolve's chosen mip must equal a CPU evaluation of the same LOD formula — this is what separates analytic gradients from `ddx`/`ddy`. |
| `depth` | A nearer pixel has a **larger raw** value (reversed-Z); the normalized preview is monotonic front-to-back. |
| `albedo` | Equals the CPU-decoded source texel at the CPU-computed UV, **through the sRGB→P3 matrix** (chromatic — the matrix is not identity here). |
| `roughness` / `metallic` / `ao` / `displacement` | Equal the corresponding CPU-decoded bytes **unchanged** — greyscale, and D65 neutrals survive the matrix exactly. |
| `normal` | A flat plane with a flat normal map ⇒ world normal (0,1,0) ⇒ `(0.5, 1.0, 0.5)` put through the output transform. |
| `lit` | A CPU port of `shadeStandard` + `brdf` on the CPU-sampled texels, ±1 LSB. **This doubles as the proof the Slang port matches the WESL original.** |
| **output path** | Render the identical frame into an `RGBA8Unorm` sink and an `RGBA16Float` sink; the float texel must equal the 8-bit one put through the same decode + primaries transform. One test, no HDR display, and it pins the rule for the resolve, ImGui and the lines at once. |
| swapchain tagging | A failed tag must drop a float surface to `BGRA8Unorm` rather than present untagged linear. |
| graph depth | Null backend, inside a validation scope: depth transitions to `DepthWrite` for the raster and `ShaderRead` for the resolve. |

**Red proofs (rule 10 — each must fail before its fix lands):**
- Drop the decode from the float-sink path → the two-sink test goes red.
- Swap two barycentric components → the vertex/centroid test goes red.
- Substitute `ddx`/`ddy` for the analytic gradients → the grazing-angle mip test goes red.
- Clear depth to `1.0` instead of `0.0` → the reversed-Z test goes red and the plane vanishes.
- Hard-code `output_mode = Srgb8` in `imgui_impl_rhi` → the ImGui half of the two-sink test
  goes red.
- Evaluate the ported SH with `lab_common`'s convolved form → the `lit` oracle goes red.
- Drop the sRGB→P3 matrix → the `albedo` oracle goes red while the scalar previews stay
  green, because the matrix is identity on neutrals.

---

## 10. Open questions

None. All rulings are recorded above: EDR now with an RHI swapchain colour space; P3 tagged
on both surfaces; `(draw_slot << 24) | (primitive + 1)`; fps-only in the surviving window;
plane without the grid; shading ported from the WESL originals with inputs patched.
