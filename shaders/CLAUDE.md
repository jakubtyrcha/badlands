# shaders/ — WESL sources

`.wesl` is compiled to WGSL by the Rust `wesl` crate, which also produces the
reflection the pipeline generator consumes. Sources are grouped by role: `common/`
(shared includes), `passes/`, `material/`, `geometry/`, `compute/`, `ibl/`, `ui/`,
`game/` (game-specific shaders — keep these out of `common/`), and `tests/`.

- **`shaders/common/frame.wesl` is the group-0 frame UBO** and must stay in sync with the C++ `UniformData` (`static_assert(sizeof(UniformData)==592)`). Changing one without the other is a silent corruption, not a compile error.
- **Reversed-Z is assumed everywhere:** near→1, far→0, depth clears to `0.0`, opaque compare `GreaterEqual`. Only the shadow pass uses `Less`.
- **Terrain layer blending is height-lerped, and displacement rides in ARM alpha.** `common/terrain_layers.wesl` is shared by `terrain_blend` (weights per VERTEX) and `terrain_cluster` (weights from a biome SPLAT texture sampled in world XZ, so they survive LOD decimation); `LoadTerrainArrays` folds each pack's `disp` red into its `arm` alpha, so height blending costs no extra array, binding, or fetch.
- **Water is a Beer-Lambert medium, not a tint.** `water.wesl` takes a per-channel extinction in 1/m plus a scattering albedo; the shore fade and the depth hue shift both fall out of transmittance, so there is no coast-width or absorption knob and the surface outputs alpha 1.
- **State extinction as a visibility depth and convert:** `sigma = 3 / d_vis`.
- **The `still` shader feature is standing water** (no waves, flat normal). It only ever reaches the forward-transparent variant, since `kForwardTransparent` registers no shadow pass.
