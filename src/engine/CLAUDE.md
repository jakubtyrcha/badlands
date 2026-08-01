# src/engine/ — the renderer and app shell

Ported from the sibling project `sampo` (`sampo::` → `badlands::`): rendering,
GPU/pipeline/reflection/frame infra, data-driven material system, scene graph +
scene renderer, `Camera`, the SDL app shell.

- **No game logic and no game types here, ever.** The interface is general and stable; get user approval before changing it.
- **When porting from sampo, port sampo's tests too.** Don't write bespoke replacements for tests that already exist upstream.

## Pipeline shape
`SceneRenderer` is a deferred renderer: G-buffer geometry pass → deferred lighting
(sun + SH ambient, GTAO, contact shadows) → fog → projected decals → color grading →
tonemap, with a forward-transparent path for water and a debug pass that can replace
tonemap with a G-buffer channel view.

Data flow: WESL (Rust) → WGSL → Dawn pipeline + reflection → material instance (bind
group) → scene-graph node → passes → SDL3. JPEG (Rust `assets`) → Dawn texture → GPU
mips → sampled.

## Invariants (read the cited code before touching these)
- **Shader reflection is naga-in-Rust, not tint.** `shader_reflection.cpp` / `gpu_pipeline_generator.cpp` call the `wesl` crate's `wgsl_reflect*`. Pipelines use **explicit reflection-derived** bind-group layouts (not Dawn AUTO); build bind groups via `CreateBindGroup(device, pipeline, group, entries)`.
- **Reversed-Z end to end:** depth clears to `0.0` (far); opaque depth-compare `GreaterEqual` (`Less` only for the shadow pass); `GLM_FORCE_DEPTH_ZERO_TO_ONE` is set project-wide; `Camera::GetProj` maps near→1, far→0.
- **`static_assert(sizeof(UniformData)==592)` must hold** and must match `shaders/common/frame.wesl`.
- **The per-object uniform buffer is the `group==1` UBO, not `uniform_buffers[0]`** — reflection returns *all* UBOs including the group-0 `frame` UBO, whose emission order varies by shader.
- **Material textures resolve by `param_name == slot_name`** (e.g. `textured_mesh`'s albedo slot is `"mesh_texture"`). `InstanceParams.texture_overrides` carry their own sampler; the factory's default sampler uses `mipmapFilter=Nearest`, so supply a trilinear+aniso sampler when you want the mip chain used.
- **`SceneGraph::SyncToRegistry` starts with `registry.clear()`.** An app that creates entities directly in its registry (mapview: the cluster terrain, the lake water) cannot also drive a SceneGraph over that same registry — the sync would wipe them every frame.

## Verifying rendering work
- Verify through the existing G-buffer debug views and the R/G/B debug materials rather than new tooling.
- Test generators and geometry on small patches; keep structural parameters compile-time and only size runtime.
