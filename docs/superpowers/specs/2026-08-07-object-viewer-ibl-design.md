# Image-based lighting in `badlands_object_viewer`

*2026-08-07*

## The problem

`badlands_object_viewer` is the RHI + render-graph successor to `badlands_viewer`. Its
shading already has an IBL-shaped hole carved out and left open:

- `ShadeStandard(…, ambientSpecular, ambientSH)` in
  `shaders/slang/common/standard_lighting.slang` takes both terms, and its header says
  IBL is "still not done, because these would be changing the maths rather than filling
  in an input".
- `resolve.slang` passes `ambientSpecular = 0`.
- `ResolvePass::SetAmbient()` is wired and never called.

So the work is to fill those inputs. Two things block it, and neither is a detail:

1. **The RHI has no cube textures.** `TextureDesc::array_layers` makes a 2D array and
   nothing else; `TextureViewDesc` has no dimension at all. The Dawn-side IBL
   (`rendering/brdf_lut.hpp`, `prefiltered_cubemap.hpp`, `cubemap_builder.hpp`) is
   unreachable from the RHI by construction.
2. **The viewer's only geometry is a flat 128-triangle plane.** A near-flat surface
   samples one narrow cone of the environment, so a broken prefilter chain would look
   entirely plausible.

This is the FIRST of two specs. The reusable RHI app skeleton, async screenshot readback
and the Windows-facing platform abstraction are the second — except the camera, which
lands here because orbiting is how a correct prefilter is told from a smeared one.

## Decisions

### Cube textures go into the RHI

The alternatives were octahedral 2D maps and a 6-layer 2D array with manual face
selection, both of which avoid touching the RHI. Both were rejected.

A skybox, reflection probes and point-light shadow cubes all need cube textures
eventually, so the gap is paid once rather than once per consumer. Hardware cube
filtering is seamless on Metal and D3D12 with no flag, which makes the prefilter the
textbook one instead of a hand-rolled seam-aware fetch that degrades at high roughness.

The cost is real: `src/engine/rhi/` is under stricter rules, so this arrives with
creation-time refusals, no observable Null/Metal divergence, and conformance coverage on
both backends.

**No sampler flag for seamlessness.** Metal samples `MTLTextureTypeCube` seamlessly with
no toggle, and D3D12 is the same — `GL_TEXTURE_CUBE_MAP_SEAMLESS` is what makes this look
like a knob. Null never samples anything; it is a recording double. A `SamplerDesc` field
here would be advertised-but-unimplemented surface, which rule 4 forbids.

### Two environment sources behind one ingest

A procedural analytic sky is the default; `--env <file.hdr>` loads an equirect HDRI.
Both are one `RadianceFn` — direction to linear RGB — and the cube is filled from it.

**The fill is CPU-side, deliberately.** The SH projection reads the same texels, so the
sky has ONE implementation rather than a Slang copy and a C++ copy that can drift. This
mirrors `CubemapBuilder`, which is proven, and reuses its `FaceUVToDirection` convention
exactly. The 128²×6 evaluation is parallelised with `badlands::ParallelFor`
(`src/core/parallel.hpp`) — the same shared pool `cubemap_builder.cpp` already uses —
because a sun drag re-bakes it.

`.exr` is not included. It is a separate feature with a separate decision.

### The uploaded cube is sun-free

This is one rule with two consequences, and it is the subtlest thing here.

The sun reaches specular through the direct GGX term. If the disc were ALSO baked into
the prefiltered environment it would be counted **twice**, and brightest exactly where a
mirror shows it. So neither the SH projection nor the prefilter ever sees a disc.

The separate Monte-Carlo reason for excluding it from SH — a tiny solid angle blows the
estimate up, which is why `light_environment.hpp` excludes it too — becomes a second
reason for the same rule rather than a special case.

The **background pass** adds the analytic disc, because the sky you *look at* is not a
lighting term. A mirror sphere still shows a sun: via the direct GGX highlight, which is
what supplies it at every roughness. That is standard practice and it is energy-correct.

For `--env`, whatever sun is baked into an HDRI stays baked in, and the analytic disc is
still added by the background. The residual double-count is documented rather than
engineered around.

### Linear roughness parameterization, on both sides

Mip *m* holds roughness *m*/(mips−1), and the resolve samples at roughness×(mips−1). The
invariant is not that the mapping is linear — it is that **the prefilter and the sample
use the same function**, and a test asserts it.

α = roughness² belongs inside the GGX distribution during prefiltering, not in the mip
lookup; sampling at roughness² against a linearly-prefiltered chain reads as everything
being too sharp. Linear also matches `shaders/ibl/prefilter_render.wesl`, so the RHI and
Dawn renderers agree — worth preserving, since `badlands_game` adopts this later.
Changing it is a separate decision affecting both.

**Each mip is re-convolved from the source cube, never downsampled from the mip above.**
Per-face downsampling is what actually produces cross-face seams, and a headless
assertion guards it.

### The chain lives in `src/engine/ibl/`

This diverges from the precedent `material_pack.hpp` states — "kept local to
object_viewer because it has exactly one consumer; the promotion point is the second
one". The divergence is deliberate: IBL is renderer infrastructure rather than a viewer
detail, the Dawn-side engine already carries the copy this replaces, and the RHI cube
support it stands on is engine-level regardless.

### Geometry is a roughness × metallic sphere grid

7 roughness × 2 metallic = 14 spheres, one draw each, on a constant material pack. It is
the standard split-sum validation chart and the only arrangement where a wrong mip
mapping or a wrong F0 lerp is visible rather than plausible.

`DrawInfo` grows `roughness`, `metallic` and an explicit `override_mask`. A sentinel
value ("negative means use the map") would be one value meaning two things — rule 5 — so
the mask is explicit and the plane scene clears it, leaving nothing accepted-and-ignored.

### The resolve splits into two depth-tested draws

`resolve.slang` currently branches on `kVisEmpty` having *already* dispatched the
material path — five texture fetches, gradient reconstruction and a full BRDF — over
every background pixel.

The visbuffer pass already writes a depth buffer, and the graph already has
`DepthReadOnly`. So:

| Pass | Geometry | Depth state (reversed-Z) | Runs where |
|---|---|---|---|
| Visbuffer | sphere grid | write, `Greater` | — |
| Resolve | fullscreen tri, `z = 0` | test, no write, `Less` | geometry was rasterized |
| Background | fullscreen tri, `z = 0` | test, no write, `GreaterEqual` | it was not |

The two draws are disjoint and cover the target exactly once between them. This is the
`src/engine/CLAUDE.md` "Screen-space work is a cost, not a free primitive" section
applied, and that section was written alongside this spec.

**The `kVisEmpty` check stays in the shader as a diagnostic**, emitting a distinctive
value rather than the background colour. Depth makes it unreachable; a visbuffer/depth
desync must be visible rather than silent (rule 1).

**One load-bearing consequence.** The empty branch returns the same dark constant for
*every* debug view today, and `--debug-view triangle_id` has a flat background its oracle
asserts on. So the background pass samples the environment **only in `Lit`** and writes
that same constant for the other nine views. Getting this wrong silently changes what
nine existing headless assertions mean.

### Camera

Left-drag orbits, wheel zooms, WASD/QE pans the orbit target — the model viewer's feel,
so nothing that works today stops working. `OrbitCameraController` moves into a
Dawn-free `badlands_camera` target so RHI apps can link it.

A small adapter converts orbit state into the viewer's existing **camera-OFFSET**
convention; `OrbitCameraController::UpdateCamera` writes a `badlands::Camera`, which does
not use that convention, so the two are not wired together directly. Headless cameras are
untouched, so every existing assertion still holds.

### Debug UI

One "Environment" separator in the EXISTING "Scene lighting" window: an intensity slider
and a one-line source readout. Nothing else — no colour pickers, no toggles, no reload
button, no resolution knob, and no new debug views.

## Verification

Exit status is the assertion; there is no test framework around this app.

- **White furnace, and the assertion is ONE-SIDED.** Constant white environment, sun off,
  albedo 1, AO 1. The real invariant is that the result **never exceeds 1 + ε** at any
  roughness or metallic — a surface cannot emit more than it receives, and a wrong
  split-sum normalisation, a wrong SH scale and a wrong F0 lerp all break it. It is ≈1
  only at low roughness: a correct SINGLE-SCATTER split-sum genuinely loses 10–20% at
  metallic 1 and high roughness (the multi-scatter deficit), so asserting ≈1 there would
  fail against correct code. The high end gets a documented floor, which is a claim about
  the approximation rather than a fudge.
- **BRDF LUT** against a CPU reference of the same integral.
- **Prefilter mip 0 equals the source** for a set of directions, read back from the cube.
- **Cross-face seam** at the highest-roughness mip, guarding per-mip re-convolution.
- **Mip parameterization round-trip** between the prefilter and the resolve.
- **Monotonicity**: peak specular radiance falls as roughness rises across the row.
- **Slang emits `texturecube`, not `texture2d`**, asserted on the generated MSL. Slang's
  `TextureCube` reflection is fine (`KindFromLayout` maps any non-buffer `Resource` to
  `SampledTexture`), but the emitted MSL is the one unproven link in the toolchain.
- **The `.hdr` loader** against a test-generated file, never a shipped asset.
- RHI cube support gets conformance entries on Null and Metal, per rule 9.

Two things no assertion covers, checked by hand: orbiting the grid, and dragging the sun
azimuth so the reflection tracks it.

## Not in scope

- Storage textures and a compute prefilter. The render path suffices, and
  `TextureUsage::Storage` is documented as not-in-MVP.
- `.exr` environments.
- Mesh loading; the grid is generated.
- The app skeleton, screenshot and platform abstraction — the second spec.

## A case that is correct, not broken

If the camera ends up inside a sphere, front faces are near-clipped and back faces are
culled (`CullMode::Back`), so those pixels are **genuinely** empty and the background
covering them is the correct result rather than a fallback.

Depth and the visbuffer cannot disagree about coverage: `VisbufferPass` clears
`kVisEmpty = 0` and depth `0.0` in the SAME render pass that writes both, so "depth > 0"
and "has an id" are the same fact. That equivalence is what makes the depth split above a
faithful replacement for the branch it removes, and the retained `kVisEmpty` diagnostic is
what would report it if a future change broke the pairing.

Near-plane *straddling* is a separate, pre-existing concern belonging to the barycentric
derivation, already covered by `--near-plane-camera`. The orbit control also clamps
`min_distance` to the sphere radius, so the interior is unreachable from the UI anyway.
