# src/executables/ — the apps

Each app owns an `AppView` and builds its scene from the world itself; the app shell
(`engine/app/sdl_viewer_app.hpp`) owns the window, GPU and loop.

| App | What it is |
|---|---|
| `badlands_game` | the game: symbolic-map terrain + biome PBR packs, town, heroes |
| `badlands_ai_sandbox` | the AI harness, driven by a `--mode` |
| `badlands_viewer` | model/LOD viewer, plus the character/skeleton viewer (`--character`) |
| `badlands_mapview` | the map tool (see `src/mapgen/CLAUDE.md`) |
| `badlands_patch_export` | the map tool's headless half: coarse-world windows → height/biome/hillshade PNGs |
| `badlands_rhi_lab` | the RHI/Slang/visibility-buffer MVP — PNG by default, `--window` for a live camera |
| `badlands_object_viewer` | the RHI + render-graph successor to `badlands_viewer` |

## `badlands_patch_export` has no `AppView`, and no GPU at all
It links `badlands_patch_providers` plus the `assets` crate for the PNG write —
no engine, no SDL, no Dawn. It lives here because it is an app with CLI flags,
not because it renders. It is the offline half of the map tool: the same
`CoarseWorldPatchSource::Fetch` mapview uses, written to images instead of a
swapchain, so a stage-2 change can be judged without a display. See
`patch_export/README.md`.

## The RHI apps are not `AppView`s, and they share a shell
`badlands_rhi_lab` and `badlands_object_viewer` build on `badlands_rhi` (+
`badlands_slang`, + `badlands_render_graph`), not `badlands_engine`. That is deliberate,
not temporary: `SdlViewerApp` is Dawn all the way through (`RenderContext` hands views a
`wgpu::Device`), so the RHI-era app shell is a separate decision.

The Slang SDK both need is a required prerequisite — `scripts/fetch_slang.sh`.

- **The window, swapchain, resize coalescing and frame pacing live in
  `engine/app/rhi_app_shell.hpp` (`badlands_rhi_app`), shared by both.** Everything in
  it was learned the hard way once — macOS focus-on-show, click-through, pixels vs
  points, coalesced resize, the per-frame autorelease pool — and a second copy is a
  second copy of every one of those bugs. Add an app by supplying callbacks, not by
  forking the loop.
- **`OnFrameBegin` fires after `BeginFrame`, so a SKIPPED frame still recycles its
  allocator slot.** Doing that work in `OnRender` leaks slots on a minimized window.
- **`badlands_rhi_app` is separate from `badlands_rhi` because the RHI must never link
  SDL** — it runs headless, and a swapchain is only one of its consumers.

## `badlands_object_viewer` runs the SAME graph headless and windowed
Only the imported output texture differs: an acquired drawable, or a plain texture that
gets read back. There is no headless *mode* to rot — it is the real path with a different
sink, which is the whole of the graph's sink abstraction.
```sh
./build/badlands_object_viewer --headless --out /tmp/ov.png   # asserts every texel
./build/badlands_object_viewer                                # live, Esc to quit
```
The headless run **verifies its own pixels and exits non-zero on mismatch** — exit status
is the assertion, since there is no test framework around it. Writing a PNG and exiting 0
would pass just as well against a graph that recorded no pass at all.

- **`--scene clear|lines|grid|plane`, and each scene carries its OWN assertion.** "Every
  texel is the clear colour" and "a segment covers these texels" are different claims; a run
  that could not say which it checked would be checking neither.
- **Every pass draws into an offscreen SCENE target in encoded sRGB; ONE output pass
  converts to the surface.** Alpha blending happens in the target's space, so ImGui drawn
  straight into an extended-linear EDR surface blends in linear and washes out every
  translucent panel. The engine reached the same conclusion in `common/ui_composite.wesl`.
- **`--present srgb|p3|edr`** picks the surface's colour space (`edr` also makes the sink
  `RGBA16Float`, which is how the extended-range path is reachable with no HDR display).
  Windowed asks for P3 and upgrades to EDR when the display reports HDR.
- **`--debug-view <name>` covers all ten views the `Graphics debug` window offers**, from one
  table — a test iterates the enum, so a UI-only mode with no headless assertion cannot exist.
- **The headless view oracles use a SYNTHETIC constant pack, not a shipped one.** Constant
  textures make the mip level irrelevant, which confines mip prediction to
  `--self-test-gradients` (two-sided against a checkerboard). An explicit `--pack` is
  honoured and the exact comparisons are skipped, with the reason logged.
- **`--self-test-visbuffer` asserts the R32Uint target directly, with no resolve**;
  `--self-test-output` renders the same frame into an 8-bit and a float sink and requires
  them to agree; `--near-plane-camera` REFUSES unless straddling triangles actually cover
  pixels, so it cannot pass vacuously.
- **The debug UI is Dear ImGui through `imgui_impl_rhi`, not `imgui_impl_metal`.** One
  backend serves Metal, DX12 and Null, and it keeps the RHI seam sealed — a native encoder
  handed out of the RHI is a compile error on purpose. ImGui is added to the graph LAST, so
  the debug UI always sits on top.
- **ImGui gets first refusal on input** (`io.WantCaptureMouse`/`WantCaptureKeyboard`) via
  the shell's `OnEvent` hook, so a drag inside a panel does not also move the camera.
```sh
./build/badlands_rhi_lab --out /tmp/lab.png              # materials + lighting
./build/badlands_rhi_lab --out /tmp/vis.png --debug-vis  # the visibility buffer
./build/badlands_rhi_lab --window                        # live: WASD/QE, right-drag to look
```
`ctest -L display` runs the windowed resize test; it is out of the default suite
because it needs a real display.

## Common flags
Run from the repo root — `shaders/` and `assets/` resolve relative to cwd.
```sh
./build/badlands_game --screenshot out.png   # headless: render one frame to PNG
./build/badlands_game --record frames/       # headless: render a frame sequence
scripts/screenshot.sh badlands_viewer /tmp/t.png --generator 1 --lod 5
```
- **`--lod` means different things for a tree and for a prop, and a prop's chain length is not a constant.**
  - **Trees:** 0 = original cards (deferred alpha-cutout, not forward), 1–3 = voxel LODs L0..L2, **4 = Impostor** (the baked octahedral billboard, which took over the slot voxel L3 used to hold), 5 = Multi (instanced field). Voxel L3 is still BUILT (the field selects it between 70 and 130 preview metres) but is no longer reachable from the manual switch.
  - **Props:** 0 = source mesh, 1..N-1 = triangle LODs, N = Impostor, N+1 = Multi — where **N is derived per model** from its size and triangle count (`src/game/visual/lod_screen_space.hpp`), so boulder_01 gets 4 mesh levels and the war hammer 2. The UI builds its radio list from the model; `RebuildScene` clamps `lod_level_` against that model's own maximum, and a generator change re-clamps.
  - Multi drives the same chain the game would, impostor included: LOD count is per-model and runtime (`GpuInstanceRenderer::ModelLod`), capped by the compile-time `kMaxLods` (8).
- `badlands_game` reads `USE_BLOCKOUT_MODE` (any non-empty value) to render greybox proxies instead of detailed PBR materials.

## Debug overlays are shared, and the buffer belongs to the host
- **`badlands_game` and `badlands_ai_sandbox` both host `NavDebugOverlay` and `SkeletonDebugOverlay`** (`src/game/visual/`), each off by default and toggled in the debug panel. The apps differ only in the ground-height function they supply — flat `0` for the arena, `GroundAt` for the town.
- **Each host owns ONE `DebugLineBuffer` per frame**; overlays append and never assign `SceneContext::debug_lines`. See the invariant in `src/engine/CLAUDE.md` — getting this wrong makes one overlay silently erase another.
- **Skeletons draw OVER the capsules, which stay the character blockout mesh.** A `--screenshot` run cannot show them: `SaveScreenshot` calls `Update(0.0f)`, so the sim never ticks and no character has been projected yet. The overlay's wiring is covered headlessly by `badlands_animation_tests` (`[overlay]`) instead.

## badlands_viewer holds TWO AppViews, chosen at startup
- **`ModelViewerView` is the default; `--character` selects `CharacterViewerView`.** `SdlViewerApp` takes a view FACTORY, not a switchable view, so there is no in-session toggle between foliage and characters.
- **`--clip <logical-name>`, `--anim-time <0..1>` and `--rig <path>` also imply `--character`.** The name is a key in the manifest (`assets/characters/quaternius/clips.json`), e.g. `walk`, not a filename; `--anim-time` pins playback so a `--screenshot` frame is reproducible; `--rig` names a manifest instead of the shipped Quaternius one.
```sh
scripts/screenshot.sh badlands_viewer /tmp/c.png --character --clip walk --anim-time 0.5
scripts/screenshot.sh badlands_viewer /tmp/r.png --rig assets/characters/0ad_biped/rig.json
```
- **Attachment axis triads are always drawn, at every attachment** — which is every joint plus every surviving socket, so the Quaternius rig shows 53 and the 0 A.D. biped 83. No toggle, which is also what keeps `--screenshot` deterministic.
- **`assets/characters/0ad/<slug>/` is 31 WHOLE creature families — 940 clips, 84 MB LFS** (`biped`, `horse`, `wolf`, `ursidae_armature`, …). They exist to be browsed, so the panel's clip list has a substring filter (the one control there). Their clips carry 0 A.D.'s names, so none has `idle`/`attack`; the game still points at `quaternius`.
- **Its skeleton draws long spokes from the root.** `knee_*`, `elbow_*`, `handIK_*` and `footIK_*` are real joints, siblings of `hip` under the synthetic `__root__` — kept deliberately by the exporter, as Unreal keeps its Mannequin's. They also stretch the framing bounds, so the body sits smaller in view than the Quaternius rig does.
- **`ModelViewerView`'s generator list is: sphere, then `TreeCatalog()`, then one entry per prop** discovered under `assets/models/*/*.usdc` and **sorted by name** — `--generator <n>` indexes it, so an unsorted order would change which model a headless screenshot captures. The `.usdc` parses lazily inside `generate` (7 MB / 100k tris each); materials resolve eagerly via the `MaterialLibrary` cache.
- **A prop's prims are merged into ONE mesh** (treasure_chest is 5, rock_moss_set_01 is 6, all sharing one pack). That merge is a viewer presentation choice — `BuildImportedModels` still returns the list.
- **The prop's LOD chain is cached per generator (`prop_preview_`), not rebuilt per LOD click.** Parsing a `.usdc` and welding + decimating it is ~a second, and `RebuildScene` runs on every radio change.
- **The character viewer needs no `Sim`.** It drives `src/engine/animation/` directly, which is the separation that layer exists to keep — the runtime knows nothing about EnTT, characters or badlands.
- The shipped Quaternius rig is 53 joints and is **grounded at y=0** (joint origins span y=[0.000, 1.513]), so a character placed at a terrain height needs no vertical offset.

## The AI sandbox is a puppet-master layer, and it is one-way
- **A MODE says what world to build and who is in it**; the app then ticks and draws it. Modes today: `duel` (default), `sneak`, `teleport`, selected with `--mode` (`--seed N` for duel).
- **Nothing in `game/` or `badlands_game_lib` knows a mode exists.** Every system under observation behaves exactly as it does in the real game.
- **Testability never outranks layering.** If a mode needs a hook inside the sim, that is the wrong design — build the setup from Commands the game already has.
- Modes are view-free and are tested headless by `badlands_ai_sandbox_tests`.
