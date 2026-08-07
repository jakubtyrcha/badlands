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
- **Joints and sockets are ONE attachment namespace, and joints win** (`animation/animation_set.hpp`). `FindAttachment(name)` resolves against both and nothing public can ask which it found — every attachment is `(joint index, offset)`, a joint's offset being identity. That is deliberate: it makes collapsing a prop node into a static socket an import-time size decision rather than a contract. Ids `[0, num_joints)` ARE the joint indices; a colliding socket is dropped by `tools/rigpack` at pack time.
- **`SceneContext::debug_lines` is ONE pointer, so a frame has ONE debug-line buffer.** The HOST owns it, clears it once, lets every overlay append, and points the context at it last. An overlay that owns its own buffer and assigns that pointer silently erases whatever another overlay drew, with no error anywhere — which is exactly what happened when the skeleton overlay joined the navmesh one (`src/game/visual/*_debug_overlay.*`).

## The kTexturedMesh vertex is 12 floats, and the 12th is load-bearing
- **`pos(3) + uv(2) + normal(3) + tangent(4)` = 48 bytes.** `tangent.w` is bitangent handedness; `gbuffer_encode.wesl` computes `B = tangent.w * cross(N, T)`. Three components cannot express it — negating `T` inverts the normal map's U response instead of its V.
- **Never write vertices positionally against a hardcoded stride.** Use `PushVertex` (it takes a `vec4` precisely so a stride change breaks the build) or `kTexturedMeshFloatsPerVertex`. A short write shears the mesh and nothing else reports it.
- **`AddMeshEntity` checks `vertices.size() == vertex_count * stride`** and logs the offending mesh. That check is what caught the floor and both impostor quads when the tangent widened; leave it in.
- **`kCubeMapMesh` stays 9 floats with a 3-component tangent** — it has no UVs, so no handedness to carry.

## USD import (`src/engine/assets/`) — the one target that is NOT badlands_engine
- **`badlands_usd_lib` links tinyusdz and deliberately not SDL3/Dawn/`badlands_engine`**, so `badlands_usd_tests` exercises the parser with no GPU in the link line. Every other "pure CPU" suite here still drags in all of `badlands_engine`; keep this one clean.
- **`usd_scene.hpp` must stay free of tinyusdz AND Dawn types.** `TexturedMeshResult` includes `<dawn/webgpu_cpp.h>` via `mesh_components.hpp`, so the conversion to it lives one layer up in `rendering/geometry/usd_mesh_adapter.cpp` and the Dawn dependency starts there.
- **The tinyusdz pin is an untagged dev commit, on purpose.** No release can read our props: v0.9.4 types MaterialX `subsurface_radius` as `float` where the assets author `color3f` (rejects the whole stage), and v0.9.9-rc7's material converter only accepts `ND_open_pbr_surface_surfaceshader`. See the CMakeLists comment before bumping.
- **A prop's material never comes from its USD.** The files reference `_nor_gl_*.exr`/`_rough_*.exr` that the Poly Haven download does not contain; the real material is the `material.json` pack beside the model, and `UsdMaterialBinding` maps USD material name → pack dir.
- **Tydra supplies tangents; do not write your own.** It computes them when absent and passes authored ones through — these assets author `tangents` as **half4**, which the loader decodes (rejecting the format would discard tangents on exactly the meshes that shipped good ones).

## Verifying rendering work
- Verify through the existing G-buffer debug views and the R/G/B debug materials rather than new tooling.
- Test generators and geometry on small patches; keep structural parameters compile-time and only size runtime.
