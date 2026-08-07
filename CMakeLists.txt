# Raised from 3.24 to match ozz-animation's own floor
# (third_party/ozz-animation/CMakeLists.txt); everything else here configures
# under 3.24. A subdirectory may legally declare a higher minimum than its
# parent, so this is NOT what makes add_subdirectory(ozz) work — the real effect
# is setting policies CMP0125..CMP0169 to NEW project-wide, which is why it is
# stated here rather than left implicit.
cmake_minimum_required(VERSION 3.30)
project(badlands LANGUAGES C CXX OBJC OBJCXX)   # OBJC/OBJCXX: Dawn Metal backend on Apple
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include(FetchContent)

# SDL3 (crib from ../sampo/CMakeLists.txt lines ~28-35)
FetchContent_Declare(SDL3 GIT_REPOSITORY https://github.com/libsdl-org/SDL.git GIT_TAG release-3.2.0)
FetchContent_MakeAvailable(SDL3)

# Dawn (WebGPU), built from source and pinned — see cmake/FetchDawn.cmake.
include(cmake/FetchDawn.cmake)

# Dear ImGui (docking branch), PINNED to a commit. Do NOT use a bare branch name
# (GIT_TAG docking) here: FetchContent then tracks a moving branch, so a cache
# refresh silently upgrades ImGui and can break the custom Dawn backend against
# API changes (e.g. 1.92 obsoleted ImDrawData::CmdListsCount and reworked the
# font-texture protocol). Bump this SHA deliberately and re-test the backend.
FetchContent_Declare(imgui GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG ca49eff3980443a97c470e09fe55b1740cfb9584)  # 1.92.9 WIP (docking), 2026-07-22
FetchContent_MakeAvailable(imgui)

# Corrosion for Rust integration (crib from ../sampo/CMakeLists.txt lines ~199-208).
FetchContent_Declare(
    Corrosion
    GIT_REPOSITORY https://github.com/corrosion-rs/corrosion.git
    GIT_TAG        v0.5
)
FetchContent_MakeAvailable(Corrosion)

# nlohmann/json: header-only JSON parser, used by MaterialLibrary to read
# each pack's material.json manifest (Stage 3 M3).
FetchContent_Declare(nlohmann_json GIT_REPOSITORY https://github.com/nlohmann/json.git GIT_TAG v3.11.3)
FetchContent_MakeAvailable(nlohmann_json)

# Taskflow (header-only task-based parallelism, from sampo). Backs the shared
# thread pool in src/core/parallel.* (e.g. the multithreaded sky cube bake).
set(TF_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(TF_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    taskflow
    GIT_REPOSITORY https://github.com/taskflow/taskflow.git
    GIT_TAG        v3.6.0
)
FetchContent_MakeAvailable(taskflow)

# meshoptimizer: QEM mesh simplification with attribute + locked-vertex metrics,
# the build-side of the terrain cluster-LOD DAG (src/game/geometry/
# terrain_clusters.cpp). Vendored as a pinned submodule (v0.24, third_party/
# meshoptimizer); exports the static-lib target `meshoptimizer`. Demo/gltfpack
# off — we only need the library.
set(MESHOPT_BUILD_DEMO OFF CACHE BOOL "" FORCE)
set(MESHOPT_BUILD_GLTFPACK OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/meshoptimizer)

# ozz-animation: the skeletal-animation runtime (src/engine/animation), vendored
# as a pinned submodule (tag 0.17.0, third_party/ozz-animation). We take the
# RUNTIME only — ozz_base + ozz_animation — plus ozz_animation_offline, which the
# animation tests use to BUILD their skeleton/clip fixtures in-test rather than
# committing a binary (see badlands_animation_tests). ozz_geometry stays out
# until characters have skinned meshes.
#
# Samples/howtos/tests are off because we don't build upstream's; tools and the
# Fbx/glTF importers are off because the character data we ship is already
# converted (.ozz, assets/characters/), so nothing here needs the Fbx SDK.
set(ozz_build_samples OFF CACHE BOOL "" FORCE)
set(ozz_build_howtos OFF CACHE BOOL "" FORCE)
set(ozz_build_tests OFF CACHE BOOL "" FORCE)
set(ozz_build_tools OFF CACHE BOOL "" FORCE)
set(ozz_build_fbx OFF CACHE BOOL "" FORCE)
set(ozz_build_gltf OFF CACHE BOOL "" FORCE)
set(ozz_build_data OFF CACHE BOOL "" FORCE)
# Per-config name postfix (ozz_base_d etc.) off: our target_link_libraries name
# the targets directly, and the postfix only complicates the link line.
set(ozz_build_postfix OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/ozz-animation)

# tinyusdz: the USD reader behind src/engine/assets (badlands_usd_lib), vendored
# as a pinned submodule (third_party/tinyusdz). It is the only dependency-free
# USDC parser going; OpenUSD would dwarf every other dep here.
#
# The pin is an UNTAGGED dev commit (v0.9.9-rc7-1250-ge3a7b8369) on purpose, not
# carelessness. NO released tag can read our Poly Haven props, which are Blender
# USD exports carrying a MaterialX ND_standard_surface_surfaceshader:
#   - v0.9.4 (last stable) types MtlxAutodeskStandardSurface::subsurface_radius
#     as `float` while these files author `color3f`. The mismatch fails Stage
#     reconstruction, rejecting the ENTIRE file, geometry included.
#   - v0.9.9-rc7 fixes that field but its Tydra material converter then hard-
#     errors on anything that is not ND_open_pbr_surface_surfaceshader.
# The dev commit handles standard_surface and adds `strict_material_check`.
#
# So do NOT "restore" this to rc7 or any current tag -- both break the import.
# Moving to a stable tag means waiting for a release that carries both fixes.
#
# We take the STATIC lib (`tinyusdz::tinyusdz_static`) with Tydra ON — Tydra is
# the layer that turns a Stage into render-ready meshes (triangulation, the
# facevarying -> vertex split, normals and tangents), so without it we would be
# walking raw prims ourselves.
#
# Tests/examples/tools are already off by default for an add_subdirectory build
# (upstream keys that off PROJECT_IS_TOP_LEVEL); the sets below turn off what
# stays ON by default and that we do not read: audio decoding, MaterialX, the
# USD->glTF example, the pxr compatibility shim, and both writers (we only ever
# read a .usdc).
set(TINYUSDZ_WITH_TYDRA ON CACHE BOOL "" FORCE)
# Upstream compiles ITSELF with -Weverything -Werror, which our clang rejects
# (-Wthread-safety-negative on its own logger's std::lock_guard). That is a
# warning policy for tinyusdz's own CI, not a signal about our use of it, so we
# turn it off rather than patch a vendored submodule.
set(TINYUSDZ_NO_WERROR ON CACHE BOOL "" FORCE)
set(TINYUSDZ_WITH_AUDIO OFF CACHE BOOL "" FORCE)
set(TINYUSDZ_WITH_USDMTLX OFF CACHE BOOL "" FORCE)
set(TINYUSDZ_WITH_USD_TO_GLTF OFF CACHE BOOL "" FORCE)
set(TINYUSDZ_WITH_PXR_COMPAT_API OFF CACHE BOOL "" FORCE)
# The writers stay ON even though we only ever read. Turning them off in
# v0.9.9-rc7 leaves tinyusdz's own USDZ serialization path referencing
# usdc::SaveAsUSDCToMemory, which then fails to link -- an upstream wiring bug,
# not something our use triggers. The cost of leaving them in is build time.
set(TINYUSDZ_WITH_MODULE_USDA_WRITER ON CACHE BOOL "" FORCE)
set(TINYUSDZ_WITH_MODULE_USDC_WRITER ON CACHE BOOL "" FORCE)
add_subdirectory(third_party/tinyusdz)
# tinyusdz v0.9.4 predates C++23 and calls nonstd::make_unexpected(), which the
# vendored expected-lite only provides in its OWN implementation. We compile at
# gnu++23, where the header auto-selects std::expected instead and that function
# does not exist. Pin the selection back to nonstd (1 = nsel_EXPECTED_NONSTD).
#
# PUBLIC, and it has to be: usd_loader.cpp includes the same Tydra headers, so
# if the two sides disagreed about which `expected` they mean, the types in
# tinyusdz's signatures would not be the types we call them with.
target_compile_definitions(tinyusdz_static PUBLIC nsel_CONFIG_SELECT_EXPECTED=1)

# wesl_ffi: .wesl -> WGSL shader compilation (src/crates/wesl), Rust staticlib.
# The crate's Cargo package is named "wesl_ffi", not "wesl" — it depends on
# the `wesl` 0.2 crate, and Corrosion invokes `cargo rustc --package <name>`,
# which errors ("specification `wesl` is ambiguous") if the package shares a
# name with one of its own dependencies.
corrosion_import_crate(MANIFEST_PATH ${CMAKE_SOURCE_DIR}/src/crates/wesl/Cargo.toml)
# Corrosion's default panic=abort would make catch_unwind in the wesl_ffi
# crate's extern "C" thunks abort the process instead of catching; override
# to unwind.
corrosion_set_env_vars(wesl_ffi "RUSTFLAGS=-Cpanic=unwind")

# assets: JPEG decode + glTF pack texture-URI parsing (src/crates/assets),
# Rust staticlib. Unlike wesl_ffi, no dependency of this crate shares its
# own name, so no Corrosion package-ambiguity workaround is needed here.
#
# PROFILE release — ALWAYS, even in a Debug C++ build. This crate is the
# image decoder, and terrain startup
# decodes 18 material textures (1024^2 each) through it. Built with cargo's dev
# profile (opt-level 0, what CMAKE_BUILD_TYPE=Debug otherwise selects) a single
# 1024^2 JPEG/PNG decode costs ~130-200 ms instead of ~20 ms, so the six-pack
# terrain load takes ~2.3 s of a Debug startup. Nothing here is C++-debuggable
# anyway — it's a prebuilt staticlib behind a C ABI — so there is no
# debuggability to trade away.
corrosion_import_crate(MANIFEST_PATH ${CMAKE_SOURCE_DIR}/src/crates/assets/Cargo.toml
                       PROFILE release)
corrosion_set_env_vars(assets "RUSTFLAGS=-Cpanic=unwind")

# NOTE: the Rust `nav` crate (src/crates/nav, a visibility-graph path service)
# has been RETIRED. Pathfinding is now a C++ weighted quadtree navmesh
# (game/src/navmesh + game/src/nav_world.cpp) that also handles weighted terrain,
# which a visibility graph cannot express. The crate source is left dormant in
# the tree (deletable) but is no longer imported or linked.

# brainhost: wasmtime embedding (src/crates/brainhost) behind a narrow C ABI
# for loading/ticking game-AI "brain" wasm modules — see
# include/brainhost.h. Linked into badlands_game_lib below; the C++ caller is
# game/src/wasm_brain.cpp. No dependency of this crate shares its own name,
# so no Corrosion package-ambiguity workaround is needed (as with assets/nav,
# unlike wesl_ffi).
corrosion_import_crate(MANIFEST_PATH ${CMAKE_SOURCE_DIR}/src/crates/brainhost/Cargo.toml)
corrosion_set_env_vars(brainhost "RUSTFLAGS=-Cpanic=unwind")

# ui: the GAME UI feature-lib (src/crates/ui) — flexbox layout (`panes`) +
# glyph rasterization/text layout (`fontdue`) behind a coarse C ABI (one
# ui_build call per frame). This is the in-world game UI surface, deliberately
# separate from the Dear ImGui DEBUG UI (see CLAUDE.md). The crate is
# game-agnostic: it knows panels/labels/buttons, never heroes or gold. No
# dependency of this crate shares its own name, so no Corrosion
# package-ambiguity workaround is needed (as with assets/nav, unlike wesl_ffi).
#
# PROFILE release — same reasoning as `assets` above, for a different reason:
# ui_build runs EVERY FRAME (layout solve + text run layout for the whole HUD),
# so a cargo dev-profile build of `panes`/taffy would tax every frame of a
# Debug C++ build. Nothing here is C++-debuggable anyway.
corrosion_import_crate(MANIFEST_PATH ${CMAKE_SOURCE_DIR}/src/crates/ui/Cargo.toml
                       PROFILE release)
corrosion_set_env_vars(ui "RUSTFLAGS=-Cpanic=unwind")

# badlands_rhi: the native render-hardware interface replacing Dawn/WebGPU.
# See docs/superpowers/specs/2026-08-03-rhi-slang-exploration.md.
#
# DELIBERATELY does NOT link `webgpu`. The whole point of this library is to be
# the seam that has no backend vocabulary in it, so a `wgpu::` reference here
# must be a link error rather than a code-review catch. It also keeps the RHI
# test suite off Dawn entirely, which is what makes it fast enough to run on
# every build.
add_library(badlands_rhi STATIC
    src/engine/rhi/rhi_common.cpp
    src/engine/rhi/rhi_frame_allocator.cpp
    src/engine/rhi/null/null_rhi.cpp
    src/engine/rhi/validation/validation_rhi.cpp
)
target_include_directories(badlands_rhi PUBLIC
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/spdlog/include
)
target_compile_features(badlands_rhi PUBLIC cxx_std_23)
# Validation is opt-in so it can never skew a profiling build. On for Debug and
# for the test suites; a release/profiling build compiles it out entirely and
# CreateDevice warns loudly if someone asks for it anyway.
target_compile_definitions(badlands_rhi PUBLIC
    $<$<NOT:$<CONFIG:Release>>:BADLANDS_RHI_VALIDATION>)
# Reversed-Z is an RHI invariant (depth clears to 0, opaque compares
# GreaterEqual), so anything built on the RHI that uses glm must produce
# zero-to-one depth. Carried here rather than left to each consumer: without
# it glm::perspective emits OpenGL [-1,1] depth and every fragment silently
# fails the depth test, which is exactly how rhi_lab first rendered pure sky.
target_compile_definitions(badlands_rhi PUBLIC GLM_FORCE_DEPTH_ZERO_TO_ONE)

# badlands_render_graph: pass declaration, ordering, and barrier derivation.
# Depends on the RHI and NOTHING else -- in particular not on badlands_slang.
# The graph is handed pipelines; it never compiles a shader, so the dependency
# runs one way and a headless test needs no shader toolchain.
add_library(badlands_render_graph STATIC src/engine/graph/render_graph.cpp)
target_link_libraries(badlands_render_graph PUBLIC badlands_rhi)

# badlands_rhi_app: the windowed render loop on the RHI, shared by every app
# that opens a window. Separate from badlands_rhi so the RHI itself never links
# SDL — the RHI runs headless, and a swapchain is only one of its consumers.
# APPLE-gated because acquiring a native surface is per-platform; the loop above
# it is not, so DX12 adds an arm rather than a second shell.
if(APPLE)
    add_library(badlands_rhi_app STATIC src/engine/app/rhi_app_shell.cpp)
    target_link_libraries(badlands_rhi_app PUBLIC badlands_rhi SDL3::SDL3)
    target_include_directories(badlands_rhi_app PUBLIC ${CMAKE_SOURCE_DIR}/src)
endif()


if(APPLE)
    # ARC only for the Metal backend — the rest of the project is MRR, and a
    # backend this size is far safer with automatic lifetimes.
    target_sources(badlands_rhi PRIVATE src/engine/rhi/metal/metal_rhi.mm)
    set_source_files_properties(src/engine/rhi/metal/metal_rhi.mm
        PROPERTIES COMPILE_FLAGS "-fobjc-arc")
    target_compile_definitions(badlands_rhi PUBLIC BADLANDS_RHI_METAL)
    target_link_libraries(badlands_rhi PUBLIC
        "-framework Metal" "-framework Foundation")
endif()

# badlands_slang: Slang -> (target source + engine reflection). Sits ABOVE the
# RHI; the RHI never invokes a compiler, so the dependency runs one way.
#
# The SDK is gitignored (see tools/slang_probe/README.md for how to fetch it),
# so this target and its tests are SKIPPED when it is absent rather than
# breaking the build for a fresh clone.
set(BADLANDS_SLANG_ROOT "${CMAKE_SOURCE_DIR}/third_party/toolchains/slang"
    CACHE PATH "Prebuilt Slang SDK root")
if(EXISTS "${BADLANDS_SLANG_ROOT}/include/slang.h")
    message(STATUS "badlands_slang: enabled (${BADLANDS_SLANG_ROOT})")
    add_library(badlands_slang STATIC src/engine/slang/slang_compiler.cpp)
    target_include_directories(badlands_slang PUBLIC
        ${CMAKE_SOURCE_DIR}/src
        ${BADLANDS_SLANG_ROOT}/include
        ${CMAKE_SOURCE_DIR}/third_party/spdlog/include)
    target_link_libraries(badlands_slang PUBLIC
        badlands_rhi "${BADLANDS_SLANG_ROOT}/lib/libslang.dylib")
    set_target_properties(badlands_slang PROPERTIES
        BUILD_RPATH "${BADLANDS_SLANG_ROOT}/lib"
        INSTALL_RPATH "${BADLANDS_SLANG_ROOT}/lib")
    set(BADLANDS_HAVE_SLANG ON)
else()
    message(STATUS "badlands_slang: skipped (no SDK at ${BADLANDS_SLANG_ROOT})")
    set(BADLANDS_HAVE_SLANG OFF)
endif()

# badlands_engine: the ported-from-sampo rendering engine (Stage 1) plus the
# game-agnostic app framework (Stage 2 S2.A1: SdlViewerApp/AppView). No game
# types/logic live here — see src/engine/app/app_view.hpp.
# badlands_usd_lib: the USD import layer (src/engine/assets). Deliberately its
# OWN target rather than a handful of files inside badlands_engine, because it
# links tinyusdz and NOT SDL3/Dawn/badlands_engine — which is what lets
# badlands_usd_tests exercise the parser with no GPU and no window system in
# the link line. Every other "pure CPU" suite here still drags in all of
# badlands_engine; this one does not.
#
# Keep it that way: usd_scene.hpp exposes plain arrays and strings precisely so
# nothing here needs the engine's mesh type (which pulls in <dawn/webgpu_cpp.h>).
# The conversion to TexturedMeshResult lives in badlands_engine, one layer up.
add_library(badlands_usd_lib STATIC
    src/engine/assets/usd_loader.cpp
    src/engine/assets/usd_material_binding.cpp
)
target_include_directories(badlands_usd_lib PUBLIC
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/spdlog/include)
# tinyusdz PRIVATE — both the link and the include path. Its headers are an
# implementation detail of usd_loader.cpp, and keeping them off this target's
# INTERFACE is what stops a tinyusdz type reaching a consumer through
# usd_scene.hpp. (Upstream does not export its own include dir, so we name it.)
target_include_directories(badlands_usd_lib PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/tinyusdz/src)
target_link_libraries(badlands_usd_lib PRIVATE tinyusdz::tinyusdz_static)

# badlands_rigpack_lib: the OFFLINE animation packer (tools/rigpack) -- turns the
# 0 A.D. import intermediate into the runtime rig asset AnimationSet loads.
#
# Its own target, and deliberately a THIN link line: ozz's offline builders, glm
# and nlohmann, with no SDL3, no Dawn and no badlands_engine. Same discipline as
# badlands_usd_lib above. It is an asset tool, so nothing it needs belongs in the
# runtime and nothing in the runtime belongs here.
#
# (tools/protogen is NOT wired into the build; that is specific to protogen being
# a throwaway exploration, and does not generalise to tools/.)
add_library(badlands_rigpack_lib STATIC
    tools/rigpack/intermediate.cpp
    tools/rigpack/recipe.cpp
    tools/rigpack/pack.cpp
)
target_include_directories(badlands_rigpack_lib PUBLIC
    ${CMAKE_SOURCE_DIR}/tools
    ${CMAKE_SOURCE_DIR}/third_party/glm)
target_link_libraries(badlands_rigpack_lib PUBLIC
    ozz_animation_offline ozz_animation ozz_base nlohmann_json::nlohmann_json)

add_executable(badlands_rigpack tools/rigpack/main.cpp)
target_link_libraries(badlands_rigpack PRIVATE badlands_rigpack_lib)

add_library(badlands_engine STATIC
    src/engine/rendering/gpu_context.cpp
    src/engine/rendering/vertex_layout.cpp
    src/engine/rendering/shader/shader_reflection.cpp
    src/engine/rendering/shader/gpu_pipeline_generator.cpp
    src/engine/rendering/context/render_pass_context.cpp
    src/engine/rendering/context/frame_context.cpp
    src/engine/rendering/material/material.cpp
    src/engine/rendering/material/material_instance.cpp
    src/engine/rendering/material/material_requirements.cpp
    src/engine/rendering/material/material_instance_cache.cpp
    src/engine/rendering/material/standard_material_factory.cpp
    src/engine/rendering/material/standard_rendering_material_instance.cpp
    src/engine/rendering/gpu_instance_renderer.cpp
    src/engine/rendering/instanced_mesh_field.cpp
    src/engine/rendering/geometry/aabb.cpp
    src/engine/rendering/geometry/cube_sphere.cpp
    src/engine/rendering/geometry/textured_mesh_builders.cpp
    src/engine/rendering/geometry/mesh_builder_utils.cpp
    src/engine/rendering/geometry/primitive_mesh_builders.cpp
    src/engine/rendering/geometry/extrusion_mesh_builder.cpp
    src/engine/rendering/geometry/lake_surface_builder.cpp
    src/engine/rendering/water_material.cpp
    src/engine/rendering/geometry/building_parts_builder.cpp
    src/engine/rendering/geometry/usd_mesh_adapter.cpp
    src/engine/rendering/components/mesh_components.cpp
    src/engine/scene/scene_graph.cpp
    src/engine/rendering/frustum.cpp
    src/engine/rendering/debug_line_buffer.cpp
    src/engine/rendering/debug_line_expand.cpp
    src/engine/rendering/passes/render_debug_lines.cpp
    src/engine/rendering/passes/render_textured_mesh.cpp
    src/engine/rendering/passes/render_forward.cpp
    src/engine/rendering/passes/render_skybox.cpp
    src/engine/rendering/passes/render_gbuffer_debug.cpp
    src/engine/rendering/passes/render_projected_decals.cpp
    src/engine/rendering/gbuffer.cpp
    src/engine/rendering/shadow_map.cpp
    src/engine/rendering/gpu_timer.cpp
    src/engine/rendering/volumetric_fog.cpp
    src/engine/rendering/fog_simulation.cpp
    src/engine/rendering/color_grading.cpp
    src/engine/rendering/scene_renderer.cpp
    src/engine/rendering/texture_loader.cpp
    src/engine/rendering/material_library.cpp
    # Render-test primitives ported from sampo (CpuImage/ColorRenderTarget/
    # TextureReadback); exercised by badlands_readback_tests. CpuImage is
    # game-agnostic (src/core) but its .cpp routes PNG I/O through the assets
    # crate, so it compiles here where `assets` is linked.
    src/core/util/cpu_image.cpp
    src/core/profiler.cpp
    src/core/parallel.cpp
    src/engine/rendering/color_render_target.cpp
    src/engine/rendering/texture_readback.cpp
    # Task S2.E: shared scene-building helper (TexturedMeshResult + a
    # MaterialLibrary DeferredMaterial -> a SceneGraph node), reused by
    # badlands_viewer/game/ai_sandbox's real AppViews.
    src/engine/rendering/scene_build.cpp
    src/engine/rendering/cubemap_builder.cpp
    src/engine/rendering/light_environment.cpp
    src/engine/rendering/hosek_wilkie.cpp
    src/engine/rendering/daylight.cpp
    src/engine/rendering/brdf_lut.cpp
    src/engine/rendering/prefiltered_cubemap.cpp
    src/engine/app/sdl_viewer_app.cpp
    src/engine/app/imgui_mouse_input.cpp
    src/engine/app/screenshot.cpp
    src/engine/app/screenshot_recorder.cpp
    src/engine/app/orbit_camera_controller.cpp
    src/engine/app/game_camera_controller.cpp
    src/engine/app/sdl_input_util.cpp
    src/engine/core/ray.cpp
    # Skeletal animation runtime (ozz): loading, sampling, blending and the
    # skeleton debug-line emitter. Game-agnostic — no EnTT and no game types,
    # so badlands_viewer drives it with no Sim present.
    src/engine/animation/skeleton.cpp
    src/engine/animation/animation_clip.cpp
    src/engine/animation/pose.cpp
    src/engine/animation/sampler.cpp
    src/engine/animation/animation_set.cpp
    src/engine/animation/skeleton_lines.cpp
    src/engine/animation/attachment_lines.cpp
    src/engine/ui/imgui_impl_wgpu_custom.cpp
    src/engine/ui/editor_ui.cpp
    src/engine/ui/ui_renderer.cpp
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
)
target_link_libraries(badlands_engine PUBLIC SDL3::SDL3 webgpu wesl_ffi assets ui nlohmann_json::nlohmann_json
    # USD import (src/engine/assets). PUBLIC because usd_mesh_adapter.hpp is an
    # engine header that exposes UsdSceneData/UsdMaterialBinding by value, so
    # every consumer needs those declarations. The dependency points this way
    # and only this way: badlands_usd_lib knows nothing about the engine.
    badlands_usd_lib
    # Skeletal animation runtime (src/engine/animation). PUBLIC because the
    # engine's animation headers expose ozz types (Skeleton::raw(), the SoA
    # locals a Pose holds), so every consumer needs ozz's include path.
    ozz_base ozz_animation)
if(APPLE)
    # CAMetalLayer color-space tagging (Display-P3 / EDR): the pinned Dawn
    # rejects wgpu::SurfaceColorManagement, so GpuContext sets the layer
    # properties directly — see src/engine/rendering/metal_layer_color.mm.
    target_sources(badlands_engine PRIVATE src/engine/rendering/metal_layer_color.mm)
    # QuartzCore: CAMetalLayer; CoreGraphics: the kCGColorSpace* name constants.
    target_link_libraries(badlands_engine PUBLIC
        "-framework QuartzCore" "-framework CoreGraphics")
endif()
# WebGPU/Dawn expects clip-space NDC z in [0,1] (D3D/Metal/Vulkan convention),
# but GLM defaults to OpenGL's [-1,1] NDC z unless told otherwise. Without
# this, glm::perspective() maps the near plane to NDC z=2.0 (outside the
# [0,1] clip volume -> clipped) instead of z=1.0, and Camera::GetProj()'s
# reversed-Z remap (src/engine/core/camera.hpp) lands its near/far boundary
# in the wrong place. Applies to all engine/app sources compiled into
# `badlands_engine` (PUBLIC so it also applies to the executables' own TUs).
target_compile_definitions(badlands_engine PUBLIC GLM_FORCE_DEPTH_ZERO_TO_ONE)
# Lightweight CPU scope profiler (PROFILE_SCOPE markers; ported from sampo).
# PUBLIC so the define reaches the executables' own TUs (GameView etc.) that
# place markers. Compiled out entirely when OFF.
option(BADLANDS_SCOPE_PROFILING "Enable the CPU scope profiler (PROFILE_SCOPE)" ON)
if(BADLANDS_SCOPE_PROFILING)
    target_compile_definitions(badlands_engine PUBLIC BADLANDS_PROFILING)
endif()
target_include_directories(badlands_engine PUBLIC
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/glm
    ${CMAKE_SOURCE_DIR}/third_party/spdlog/include
    ${CMAKE_SOURCE_DIR}/third_party/entt/single_include
    ${CMAKE_SOURCE_DIR}/src/crates/wesl/include
    ${CMAKE_SOURCE_DIR}/src/crates/assets/include
    ${CMAKE_SOURCE_DIR}/src/crates/ui/include
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    ${taskflow_SOURCE_DIR}
)

# badlands_game_lib: the C++ game simulation (EnTT + glm + spdlog), pure C++
# with no engine/SDL/Dawn deps — geometry that needs engine types comes in a
# later task (S2.C). The hero brain is wasm-only now (game/src/wasm_brain.h).
#
# Named badlands_game_lib, not badlands_game: the "game" app executable below
# is already named badlands_game (from S2.A1/A3), and CMake target names must
# be globally unique.
add_library(badlands_game_lib STATIC
    game/src/game.cpp
    game/src/sim.cpp
    # The animation projection: summarises what each character is DOING into a
    # CharacterAnim for the render layer. Runs last in step_world; nothing under
    # game/ reads it back.
    game/src/anim_projection.cpp
    game/src/combat.cpp
    game/src/command.cpp
    game/src/creature_catalog.cpp
    game/src/placement.cpp
    game/src/movement.cpp
    game/src/heroes.cpp
    game/src/skills.cpp
    # Timed statuses (stun and whatever follows): timers only -- what a status
    # DOES is enforced by the systems that read it (game/src/status.h).
    game/src/status.cpp
    # Threat: fixed calibration anchors that double as the brains' fight-or-
    # flee input (game/src/threat_table.h). Authored, never derived.
    game/src/threat_table.cpp
    # Attack commitment: wind-up (cancellable) + recovery (not), during which
    # the attacker neither moves nor thinks (game/src/strike.h).
    game/src/strike.cpp
    # Casting: validation, target resolution, the pre-rolled attack test, and
    # applying the ops an effect asked for (game/src/skill_cast.h). Separate
    # from skills.cpp, which stays world-free so it can link standalone into
    # the manifest test target below.
    game/src/skill_cast.cpp
    game/src/skill_focus.cpp
    game/src/progression.cpp
    game/src/entity_memory.cpp
    game/src/needs.cpp
    # Intention contract engine (inbox, wake rule, apply seam) the wasm hero
    # brain runs on -- see docs/design/intention-contract.html and
    # game/src/intention.h.
    game/src/intention.cpp
    # Hero perception (observe_hero/weights_for) -- all that remains of the
    # deleted C++ hero decision layer (town_brain.cpp); its sole consumer now
    # is the wasm hero brain (wasm_brain.cpp), the sole hero decision-maker.
    game/src/hero_perception.cpp
    # The wasm hero brain runtime (brainhost C ABI) -- see game/src/wasm_brain.h.
    game/src/wasm_brain.cpp
    game/src/activity_catalog.cpp
    # Names for CommandKindId + BuildingKind (the command log's rows). Same
    # badlands_sim.hpp-only dependency set as activity_catalog.cpp.
    game/src/command_catalog.cpp
    game/src/activity_histogram.cpp
    game/src/behaviours/blocks.cpp
    game/src/behaviours/selectors.cpp
    game/src/behaviours/perception.cpp
    game/src/critter_brain.cpp
    game/src/townfolk_brain.cpp
    game/src/monster_brain.cpp
    game/src/economy.cpp
    game/src/vision.cpp
    game/src/exploration.cpp
    # Task S2.C: procedural-geometry game data (no engine/SDL/Dawn deps — see
    # the file-level comments in src/game/building_catalog.{h,cpp} and
    # src/game/geometry/ploppable_rings.{h,cpp}).
    src/game/building_catalog.cpp
    src/game/geometry/ploppable_rings.cpp
    # Task 1 (tree-LOD feature): general meshoptimizer-based mesh
    # simplification helper (pure geometry math, no engine/tree types).
    src/game/geometry/mesh_lod.cpp
    # Task S2.D: MaterialId -> PBR pack mapping (src/game/material_pack.h).
    src/game/material_pack.cpp
    # Frozen map-data contract + the symbolic greybox generator behind it
    # (pure CPU: the mapgen Biome enum + glm, no engine/GPU deps).
    src/game/map/map_data.cpp
    src/game/map/symbolic_map_generator.cpp
    src/game/map/flat_map_generator.cpp
    # Nav core: adaptive quadtree navmesh + HPA* pathfinding (pure CPU over the
    # injected NavSource; glm only, no engine/GPU/EnTT deps).
    game/src/navmesh/quadtree.cpp
    game/src/navmesh/graph.cpp
    game/src/navmesh/navmesh.cpp
    # Sim<->nav bridge: SimNavSource adapter + biome cost policy + rebuild.
    game/src/nav_world.cpp
)
target_compile_features(badlands_game_lib PUBLIC cxx_std_23)
target_include_directories(badlands_game_lib PUBLIC
    ${CMAKE_SOURCE_DIR}/game/include
    ${CMAKE_SOURCE_DIR}/third_party/glm
    ${CMAKE_SOURCE_DIR}/third_party/spdlog/include
    ${CMAKE_SOURCE_DIR}/third_party/entt/single_include
    # For "core/roof_shape.hpp" + "game/material_id.hpp" etc (Task S2.C) —
    # header search path only, no engine/SDL/Dawn dependency added.
    ${CMAKE_SOURCE_DIR}/src
    # brainhost's C ABI (game/src/wasm_brain.h) -- mirrors how nav's include
    # dir is added to badlands_engine above.
    ${CMAKE_SOURCE_DIR}/src/crates/brainhost/include
)
# The sim's own headers: an implementation detail of badlands_game_lib, not
# part of its public header surface (which is game/include/badlands_sim.hpp).
target_include_directories(badlands_game_lib PRIVATE
    ${CMAKE_SOURCE_DIR}/game/src
)
# brainhost: wasmtime-backed brain wasm runtime (game/src/wasm_brain.cpp) --
# mirrors how nav is linked into badlands_engine above.
target_link_libraries(badlands_game_lib PUBLIC brainhost)
# meshoptimizer: backs src/game/geometry/mesh_lod.cpp's SimplifyMesh (Task 1,
# tree-LOD feature). PRIVATE -- an implementation detail of mesh_lod.cpp, not
# part of badlands_game_lib's public header surface.
target_link_libraries(badlands_game_lib PRIVATE meshoptimizer)

# Three thin executables, each with its own main, all linking badlands_engine
# and badlands_game_lib. Each app grows its own real AppView.
set(badlands_app_sources_viewer
    src/executables/viewer/main_viewer.cpp
    src/executables/viewer/model_viewer_view.cpp
    # The app's second AppView (`--character`): one skeleton on a floor, posed
    # by the engine animation runtime and drawn as debug lines. Chosen at
    # startup by main_viewer.cpp -- the two views do not switch in-session.
    src/executables/viewer/character_viewer_view.cpp
    src/game/geometry/tree_generator.cpp
    src/game/geometry/branch_junction.cpp
    src/game/geometry/leaf_texture.cpp
    # Volumetric-foliage Phase 1: CPU leaf-card voxelizer (splat cards into a
    # cell grid, emit one jittered tet per occupied cell). Pure geometry, no
    # renderer changes yet.
    src/game/geometry/leaf_voxelizer.cpp
    # Multi-mode instanced tree grid (Task 4): builds an InstancedMeshField
    # from a catalog TreeOptions -- see game/visual/tree_field.hpp.
    src/game/visual/instanced_lod_model.cpp
    src/game/visual/instanced_lod_field.cpp
    src/game/visual/tree_lod_model.cpp
    # Triangle LODs for imported props -- the viewer is its only consumer, so
    # it is not in the mapview list beside the tree producer.
    src/game/visual/prop_lod_model.cpp
    src/game/visual/impostor_atlas.cpp
    src/game/visual/impostor_baker.cpp)
# Task S2.F: GameView (sim buildings via deferred + GameCameraController) +
# its building_scene helper. Compiled directly into the executable (like
# model_viewer_view.cpp above), not into badlands_engine/badlands_game_lib --
# it needs both libraries' headers (SceneGraph/MaterialLibrary +
# GameBuildingKind/building_visual), which the final executable link already
# provides transitively.
set(badlands_app_sources_game
    src/executables/game/main_game.cpp
    src/executables/game/game_view.cpp
    # Visual entity-component layer (SceneComposer) + terrain geometry + the
    # symbolic-map building/biome plumbing, now that the game renders the
    # symbolic map's terrain/water/buildings through the composer.
    src/game/visual/scene_composer.cpp
    src/game/visual/vision_overlay_pass.cpp
    src/game/visual/cone_overlay_pass.cpp
    src/game/visual/nav_debug_overlay.cpp
    # Character skeletons drawn over the capsules, toggled in the debug UI:
    # the sim's CharacterAnim -> a clip -> the engine animation runtime.
    src/game/visual/character_animation.cpp
    src/game/visual/skeleton_debug_overlay.cpp
    src/game/visual/selection_decals.cpp
    src/game/visual/world_labels.cpp
    src/game/scene/building_composer.cpp
    src/game/geometry/terrain_mesh.cpp
    src/game/geometry/water_surface.cpp
    src/game/brain_asset.cpp        # hero brain wasm bytes loaded as data
    src/game/factors_manifest.cpp   # behaviour tuning loaded as data
    src/game/creature_manifest.cpp  # creature-stat overrides loaded as data
    src/game/skill_manifest.cpp     # skill template overrides loaded as data
    # Game UI (the in-world HUD + world picking), distinct from the ImGui debug
    # UI. Game-specific, so it lives here rather than in badlands_engine --
    # only the game-agnostic UiRenderer does.
    src/game/ui/hud.cpp
    src/game/ui/picking.cpp
    src/mapview/biome_manifest.cpp)
# AiSandboxView: the host for a SandboxMode (deferred blockout +
# GameCameraController). Compiled directly into the executable, same rationale
# as badlands_app_sources_game above.
set(badlands_app_sources_ai_sandbox
    src/executables/ai_sandbox/main_ai_sandbox.cpp
    src/executables/ai_sandbox/ai_sandbox_view.cpp
    # The puppet-master layer: what world to build, who is in it, when to
    # restage. View-free (see sandbox_mode.hpp) and tested standalone by
    # badlands_ai_sandbox_tests.
    src/executables/ai_sandbox/arena.cpp
    src/executables/ai_sandbox/duel_mode.cpp
    src/executables/ai_sandbox/sneak_mode.cpp
    src/executables/ai_sandbox/teleport_mode.cpp
    src/game/visual/nav_debug_overlay.cpp  # shared nav debug overlay (also in game)
    src/game/visual/character_animation.cpp      # shared skeleton overlay (also in game)
    src/game/visual/skeleton_debug_overlay.cpp
    src/game/brain_asset.cpp        # hero brain wasm bytes loaded as data
    src/game/factors_manifest.cpp   # behaviour tuning loaded as data
    src/game/creature_manifest.cpp  # creature-stat overrides loaded as data
    src/game/skill_manifest.cpp)    # skill template overrides loaded as data

foreach(app viewer game ai_sandbox)
    add_executable(badlands_${app} ${badlands_app_sources_${app}})
    target_link_libraries(badlands_${app} PRIVATE badlands_engine badlands_game_lib)
    if(APPLE)
        # Use the new linker on Apple platforms (crib from
        # ../sampo/CMakeLists.txt); avoids issues linking the large
        # monolithic Dawn dylib.
        target_link_options(badlands_${app} PRIVATE "-Wl,-ld_new")
    endif()
endforeach()

# badlands_foliage_lib: the game-agnostic foliage placement library
# (src/foliage/). Deliberately links NEITHER badlands_engine NOR
# badlands_mapgen_lib: it compiles the EDT TU directly (that is why
# distance_to_mask was split out of generator.cpp) so the whole placement half
# stays pure CPU -- no Dawn, no EnTT, no engine -- and is unit-testable without
# a GPU. glm/spdlog/FastNoiseLite are headers only.
add_library(badlands_foliage_lib STATIC
    src/foliage/foliage_types.cpp
    src/foliage/forest_type.cpp
    src/foliage/depth_field.cpp
    src/foliage/scatter.cpp
    src/mapgen/distance_field.cpp)
target_compile_features(badlands_foliage_lib PUBLIC cxx_std_23)
target_include_directories(badlands_foliage_lib PUBLIC
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/glm
    ${CMAKE_SOURCE_DIR}/third_party/spdlog/include)
target_include_directories(badlands_foliage_lib PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/FastNoiseLite)

# badlands_mapgen_lib: terrain data vocabulary + river algebra + on-disk raster
# I/O. NOT a pipeline stage -- all three procgen stages sit on it.
#
# LINKS NOTHING, deliberately. It once linked badlands_engine for CpuImage (PNG
# write via the `assets` crate), but the only users of that were outputs.cpp and
# hillshade.cpp, both since deleted; no TU or header here reaches an engine
# header any more. Keeping the link would drag Dawn into every consumer, which
# is exactly why four test targets used to hand-compile these TUs instead of
# linking this lib. They link it now.
#
# The same reasoning retires the FastNoiseLite include dir: nothing here samples
# noise (the only mention left is a comment in distance_field.hpp).
add_library(badlands_mapgen_lib STATIC
    # Exact EDT, its own TU so a consumer can link the distance transform ALONE
    # (src/foliage/ compiles this TU directly rather than linking this lib).
    src/mapgen/distance_field.cpp
    src/mapgen/hydrology.cpp
    src/mapgen/river_clip.cpp
    src/mapgen/river_prune.cpp
    src/mapgen/river_graph.cpp
    src/mapgen/river_arcs.cpp
    src/mapgen/river_carve.cpp
    # The coarse (protogen) artifact's on-disk form: world.txt's manifest and
    # rivers.bin's binary RiverGraph dump. Both are also compiled directly
    # into the standalone protogen TU (see tools/protogen/README.md) -- kept
    # buildable with a bare `c++ -std=c++23 -I src -I third_party/glm`, no
    # engine/spdlog/taskflow, so that stays possible.
    src/mapgen/coarse_io.cpp
    src/mapgen/river_io.cpp)
target_compile_features(badlands_mapgen_lib PUBLIC cxx_std_23)
target_include_directories(badlands_mapgen_lib PUBLIC
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/glm
    ${CMAKE_SOURCE_DIR}/third_party/spdlog/include)

# badlands_patch_lib: THE CONTRACT (mapgen/patch_data.hpp) -- PatchRequest,
# PatchData, PatchSource, and compute_elevation_range. Every stage 3 consumer
# may see this; it is the one frozen interface in the procgen split, so it gets
# its own target rather than riding along in badlands_mapgen_lib.
add_library(badlands_patch_lib STATIC
    src/mapgen/patch_data.cpp)
target_link_libraries(badlands_patch_lib PUBLIC badlands_mapgen_lib)

# badlands_patch_providers: the IMPLEMENTATIONS of the contract above --
# on-disk I/O (patch_io), the analytic fixture (synthetic_patch_source), and
# the disk-backed PatchSource (file_patch_source). Only an app's main links
# this: stage 3 proper (badlands_mapview_view below) links only
# badlands_patch_lib, so it can see the PatchSource interface but never a
# concrete provider.
add_library(badlands_patch_providers STATIC
    src/mapgen/patch_io.cpp
    src/mapgen/synthetic_patch_source.cpp
    src/mapgen/file_patch_source.cpp
    src/mapgen/coarse_world_patch_source.cpp
    src/mapgen/cubic_sample.cpp
    src/mapgen/relief_filter.cpp)
target_link_libraries(badlands_patch_providers PUBLIC badlands_patch_lib)

# badlands_mapview_view: everything the map tool needs to fetch a patch and
# render it, EXCEPT the main that picks which PatchSource to fetch from.
#
# THIS IS THE POINT: map_view_view.cpp compiles into a library that links
# badlands_patch_lib (the PatchSource INTERFACE) but not badlands_patch_providers
# (the concrete implementations), so it cannot reach around
# mapgen::PatchSource to a concrete provider -- only main_mapview.cpp, which
# links the providers directly, may choose one.
add_library(badlands_mapview_view STATIC
    src/executables/mapview/map_view_view.cpp
    src/mapview/biome_manifest.cpp
    src/mapview/biome_splat.cpp
    src/mapview/lake_surface.cpp
    src/mapview/river_surface.cpp
    src/game/geometry/terrain_mesh.cpp
    src/game/geometry/terrain_clusters.cpp
    # Shared MapData->render cluster-terrain module, used by both apps (game_view
    # wired in M7.2). Owns the DAG, its vertex-color material factory, the LOD
    # cut, and the Terrain debug UI.
    src/game/map/cluster_terrain.cpp
    # Foliage: the tree geometry pipeline (also compiled into badlands_viewer),
    # the multi-model instanced field, badlands' forest content table, the
    # per-cell cull + upload renderer, and the synthetic forest test map.
    src/game/geometry/tree_generator.cpp
    src/game/geometry/branch_junction.cpp
    src/game/geometry/leaf_texture.cpp
    src/game/geometry/leaf_voxelizer.cpp
    src/game/visual/instanced_lod_model.cpp
    src/game/visual/instanced_lod_field.cpp
    src/game/visual/tree_lod_model.cpp
    src/game/visual/impostor_atlas.cpp
    src/game/visual/forest_catalog.cpp
    src/game/visual/forest_renderer.cpp
    src/game/visual/impostor_baker.cpp
    src/game/map/forest_test_map_generator.cpp)
target_link_libraries(badlands_mapview_view PUBLIC badlands_engine badlands_game_lib badlands_patch_lib
                                                    badlands_foliage_lib meshoptimizer)
target_include_directories(badlands_mapview_view PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/FastNoiseLite)
if(APPLE)
    # PUBLIC, not PRIVATE: target_link_options on a STATIC library are only
    # honoured for consumers (PUBLIC/INTERFACE), never for the library's own
    # (nonexistent) link step -- this must reach badlands_mapview's final link.
    target_link_options(badlands_mapview_view PUBLIC "-Wl,-ld_new")
endif()

# badlands_mapview: THE map tool's executable. Its main (main_mapview.cpp) is
# the only TU that picks a mapgen::PatchSource implementation (--load /
# --synthetic / --test-map), so it is the only target that links
# badlands_patch_providers alongside the view library above.
add_executable(badlands_mapview
    src/executables/mapview/main_mapview.cpp)
target_link_libraries(badlands_mapview PRIVATE badlands_mapview_view badlands_patch_providers)

# badlands_terrain_cluster_tests: Catch2 invariants for the terrain cluster-LOD
# DAG build (error monotonicity, sphere nesting, crack-free seams, grid
# arithmetic). Pure CPU on synthetic MapData heightfields — compiles just the
# build TU + its deps (map_data for the frozen contract it samples, the
# header-only Aabb) and links meshoptimizer; no Dawn/GPU.
add_executable(badlands_terrain_cluster_tests
    src/game/geometry/terrain_clusters_tests.cpp
    src/game/geometry/terrain_clusters.cpp
    src/game/map/map_data.cpp
    src/engine/rendering/geometry/aabb.cpp
    src/core/parallel.cpp  # ParallelFor: the DAG build's per-group parallelism
    third_party/catch2/extras/catch_amalgamated.cpp
)
target_include_directories(badlands_terrain_cluster_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/glm
    ${CMAKE_SOURCE_DIR}/third_party/spdlog/include
    ${CMAKE_SOURCE_DIR}/third_party/catch2/extras
    ${taskflow_SOURCE_DIR}  # taskflow backs src/core/parallel.cpp
)
target_link_libraries(badlands_terrain_cluster_tests PRIVATE meshoptimizer)
add_test(NAME badlands_terrain_cluster_tests COMMAND badlands_terrain_cluster_tests WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# badlands_terrain_cluster_bench: the GOLDEN OUTPUT PIN + benchmarks for the same
# build, on a fixture whose cost profile matches the production map. Separate
# target from the invariant suite above because it needs the real mapgen river
# carve (river_carve/river_arcs — both pure CPU): an analytic stand-in would not
# reproduce the per-hot-tile cost that dominates the build.
# The BENCHMARK cases are tagged [.][bench] so ctest runs only the golden checks;
# time them with `scripts/test.sh badlands_terrain_cluster_bench "[bench]"`.
add_executable(badlands_terrain_cluster_bench
    src/game/geometry/terrain_clusters_bench.cpp
    src/game/geometry/terrain_clusters.cpp
    src/game/map/map_data.cpp
    src/engine/rendering/geometry/aabb.cpp
    src/core/parallel.cpp
    third_party/catch2/extras/catch_amalgamated.cpp
)
target_include_directories(badlands_terrain_cluster_bench PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/catch2/extras
    ${taskflow_SOURCE_DIR}
)
# river_carve/river_arcs/river_graph/hydrology come from the lib, which links
# nothing -- so this stays a pure-CPU, fast-to-build test binary.
target_link_libraries(badlands_terrain_cluster_bench PRIVATE
    badlands_mapgen_lib meshoptimizer)
add_test(NAME badlands_terrain_cluster_bench COMMAND badlands_terrain_cluster_bench WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# badlands_river_tests: routing/hydrology + the river chain (graph, arcs, carve)
# — structural invariants on synthetic terrains (routing, spill levels,
# hydraulics, network extraction, corridor carving). Pure CPU.
#
# Was badlands_erosion_tests. Renamed because it covers no erosion: the sim it
# was named for is gone, and the name is a CMake target plus a scripts/test.sh
# regex -- no #include anywhere depends on it.
add_executable(badlands_river_tests
    src/mapgen/hydrology_tests.cpp
    src/mapgen/river_graph_tests.cpp
    src/mapgen/river_clip_tests.cpp
    src/mapgen/river_prune_tests.cpp
    src/mapgen/river_arcs_tests.cpp
    src/mapgen/river_carve_tests.cpp
    third_party/catch2/extras/catch_amalgamated.cpp
)
target_include_directories(badlands_river_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/catch2/extras
)
target_link_libraries(badlands_river_tests PRIVATE badlands_mapgen_lib)
add_test(NAME badlands_river_tests COMMAND badlands_river_tests
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# badlands_artifact_io_tests: the coarse (protogen) artifact's on-disk form --
# world.txt's manifest and rivers.bin's binary river-graph dump. HARD RULE: no
# test here runs the coarse sim; every fixture is hand-built. coarse_io.cpp /
# river_io.cpp themselves come from badlands_mapgen_lib, not compiled again
# here.
add_executable(badlands_artifact_io_tests
    src/mapgen/coarse_io_tests.cpp
    src/mapgen/river_io_tests.cpp
    third_party/catch2/extras/catch_amalgamated.cpp
)
target_include_directories(badlands_artifact_io_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/catch2/extras
)
target_link_libraries(badlands_artifact_io_tests PRIVATE badlands_mapgen_lib)
add_test(NAME badlands_artifact_io_tests COMMAND badlands_artifact_io_tests
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# badlands_game_tests: the existing Catch2 game-sim suite (game/tests/),
# re-enabled in CMake (previously only run via `cargo test --test cpp_tests`,
# see tests/cpp_tests.rs on the legacy Rust reference branch).
enable_testing()
add_executable(badlands_game_tests
    game/tests/combat_tests.cpp
    game/tests/sim_tests.cpp
    game/tests/placement_tests.cpp
    game/tests/movement_tests.cpp
    game/tests/heroes_tests.cpp
    game/tests/entity_memory_tests.cpp
    game/tests/command_tests.cpp
    game/tests/clock_tests.cpp
    game/tests/needs_tests.cpp
    game/tests/determinism_tests.cpp
    game/tests/archetype_tests.cpp
    game/tests/activity_selector_tests.cpp
    game/tests/chat_tests.cpp
    game/tests/exploration_tests.cpp
    game/tests/activity_stats_tests.cpp
    game/tests/critter_tests.cpp
    game/tests/townfolk_tests.cpp
    game/tests/rat_tests.cpp
    game/tests/hunter_tests.cpp
    game/tests/vision_tests.cpp
    game/tests/facing_tests.cpp
    # The animation projection: what the render layer reads to pick a clip.
    # Drives step_world so the projector runs in its real position (last).
    game/tests/anim_projection_tests.cpp
    # Hero behavior, wasm-driven (Task 4: the C++ decision layer is gone --
    # the shipping brain is the sole hero decision-maker). Histogram-in-spirit
    # outcomes over a run, plus two smokes salvaged from the deleted
    # hero_brain_parity_tests.cpp.
    game/tests/hero_behavior_tests.cpp
    # Brain wire format (game/src/brain_abi.h): pure struct-layout checks, no
    # sim needed.
    game/tests/brain_abi_tests.cpp
    # The wasm hero brain (v2, the intention contract) wired into the sim --
    # see game/src/wasm_brain.h. No twin-brain parity anymore (the C++
    # reference/mock-hero-fallback comparison died with v1's wire; the
    # ported Nim brain -- scripts/brains/nim/hero.nim -- is the sole hero
    # decision layer for a wasm-loaded world).
    game/tests/wasm_brain_tests.cpp
    # Task 9: the set_factors_of validation boundary (sanitize_factors,
    # sim.cpp) -- adversarial-config red/green repros.
    game/tests/factors_sanitize_tests.cpp
    game/tests/events_tests.cpp
    game/tests/navmesh_tests.cpp
    game/tests/nav_world_tests.cpp
    # Game UI: world picking + HUD hit-testing. Pure CPU -- the HUD build goes
    # through the `ui` Rust crate (layout + font), never the GPU, so it runs
    # here rather than needing a device.
    game/tests/game_ui_tests.cpp
    src/game/ui/hud.cpp
    src/game/ui/picking.cpp
    # Selection highlights: the selection -> ProjectedDecal mapping. Pure
    # geometry over the sim snapshot rows + the engine's header-only decal
    # primitive, so it needs no GPU and no engine link.
    game/tests/selection_decals_tests.cpp
    src/game/visual/selection_decals.cpp
    # Floating world labels: screen projection + dampened depth scale + the
    # generic timed-label pool. Pure CPU math over glm (+ header-only Camera),
    # so it runs here without a device -- same rationale as the decals above.
    game/tests/world_labels_tests.cpp
    src/game/visual/world_labels.cpp
    # Skill vocabulary: catalog, grant table, Skills component (game/src/skills.h).
    game/tests/skills_tests.cpp
    # Timed statuses: application/refresh/expiry + the cooldown sweep
    # (game/src/status.h).
    game/tests/status_tests.cpp
    # Skill effects in isolation: the pure (cast context -> op batch) contract
    # (game/src/skill_abi.h). No world, no Sim -- that IS the test.
    game/tests/skill_effect_tests.cpp
    # Casting end to end: shield-bash through the UseSkill command handler
    # (game/src/skill_cast.h).
    game/tests/shield_bash_tests.cpp
    # Imperceptibility and the armour ward: the two statuses whose rules live
    # where the decisions already happen (perception, effective_combatant).
    game/tests/sneak_tests.cpp
    # A skill that takes TIME: SkillTrigger::Intention, executed
    # (game/src/skill_focus.h).
    game/tests/skill_focus_tests.cpp
    # Hero progression: leveling curve + XP accrual (game/src/progression.h).
    game/tests/progression_tests.cpp
    game/tests/growth_tests.cpp
    game/tests/threat_table_tests.cpp
    game/tests/strike_tests.cpp
    game/tests/disengage_tests.cpp
    # Intention contract: inbox, wake rule, apply seam (game/src/intention.h).
    game/tests/intention_tests.cpp
    third_party/catch2/extras/catch_amalgamated.cpp
)
target_include_directories(badlands_game_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/catch2/extras
    ${CMAKE_SOURCE_DIR}/game/src
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/crates/ui/include
    ${CMAKE_SOURCE_DIR}/third_party/glm
    ${CMAKE_SOURCE_DIR}/third_party/spdlog/include
)
target_link_libraries(badlands_game_tests PRIVATE badlands_game_lib ui)
add_test(NAME badlands_game_tests COMMAND badlands_game_tests WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# badlands_geometry_tests: Catch2 suite for Task S2.C's procedural geometry
# (primitives/rocks/building parts, src/engine/rendering/geometry) + the
# building visual catalog / ploppable rings (src/game). Pure CPU — no GPU
# init needed — despite linking badlands_engine (StaticTexturedMeshComponent
# carries wgpu::TextureView/Sampler members that are never touched here).
add_executable(badlands_geometry_tests
    game/tests/geometry_tests.cpp
    # USD -> engine mesh conversion. It lives here rather than in
    # badlands_usd_tests because TexturedMeshResult pulls in Dawn headers, and
    # keeping badlands_usd_tests free of those is the whole point of that
    # target. Inputs are hand-built, so no .usdc is opened here.
    src/engine/tests/usd_mesh_adapter_tests.cpp
    # Hidden [.report] characterization of how far the shipped props decimate.
    # Unlike the adapter tests above it DOES open the real .usdc files, which is
    # why this target links badlands_usd_lib -- the question it answers (does
    # edge collapse floor out on unwelded prop geometry?) cannot be asked of
    # hand-built input.
    game/tests/prop_lod_report_tests.cpp
    # The prop triangle-LOD producer. CPU only -- its fixture is a hand-built
    # flat-shaded grid, which reproduces the shipped props' pathology without
    # opening one. Its sources are compiled in directly because they belong to
    # the viewer app's list, not badlands_game_lib.
    game/tests/prop_lod_model_tests.cpp
    src/game/visual/prop_lod_model.cpp
    src/game/visual/instanced_lod_model.cpp
    third_party/catch2/extras/catch_amalgamated.cpp
)
target_include_directories(badlands_geometry_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/catch2/extras
)
target_link_libraries(badlands_geometry_tests PRIVATE badlands_engine badlands_game_lib badlands_usd_lib)
if(APPLE)
    target_link_options(badlands_geometry_tests PRIVATE "-Wl,-ld_new")
endif()
add_test(NAME badlands_geometry_tests COMMAND badlands_geometry_tests WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_executable(badlands_tree_tests
    game/tests/tree_generator_tests.cpp
    # Junction geometry (socket footprints, loop stitching, ring refinement):
    # pure math over section polylines, so it needs no device.
    game/tests/branch_junction_tests.cpp
    game/tests/leaf_voxelizer_tests.cpp
    # badlands' forest content table (which TreeCatalog preset each foliage
    # model index IS). Lives here rather than in badlands_foliage_tests because
    # it needs TreeCatalog(), which the foliage library deliberately cannot see.
    game/tests/forest_catalog_tests.cpp
    # The CPU foliage-cell cull predicate: header-only, no GPU, so the shadow
    # caster + crown overhang behaviour is testable without a device.
    game/tests/foliage_cell_cull_tests.cpp
    # Bounding box -> spacing circle, likewise header-only and GPU-free.
    game/tests/crown_bounds_tests.cpp
    # The screen-space LOD ladder: pure math over a model's size and triangle
    # count, so its structural guarantees (ascending cutoffs, falling budgets,
    # room for the impostor) are pinned with no device.
    game/tests/lod_screen_space_tests.cpp
    # The impostor's hemi-octahedral view parameterization: shared by the baker
    # and the material, so it is pinned where neither needs a device.
    game/tests/octahedral_tests.cpp
    # The impostor atlas' tile layout -- likewise shared by baker and material.
    # The .cpp comes along for BindImpostorAtlas' refuse-an-unbuilt-atlas guard;
    # nothing here creates a device.
    game/tests/impostor_atlas_tests.cpp
    src/game/visual/impostor_atlas.cpp
    src/game/visual/forest_catalog.cpp
    src/game/geometry/tree_generator.cpp
    src/game/geometry/branch_junction.cpp
    src/game/geometry/leaf_texture.cpp
    src/game/geometry/leaf_voxelizer.cpp
    third_party/catch2/extras/catch_amalgamated.cpp
)
target_include_directories(badlands_tree_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/catch2/extras
)
target_link_libraries(badlands_tree_tests PRIVATE badlands_engine badlands_game_lib
                                                 badlands_foliage_lib)
if(APPLE)
    target_link_options(badlands_tree_tests PRIVATE "-Wl,-ld_new")
endif()
add_test(NAME badlands_tree_tests COMMAND badlands_tree_tests WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# badlands_tree_field_gpu_tests: volumetric-foliage Phase 3's headless GPU
# suite -- builds a real Oak crown (GenerateLeafMesh + VoxelizeLeafCards, the
# same CPU pipeline model_viewer_view.cpp's Voxel L0/L1/L2 modes drive) as a
# deferred VoxelFoliage entity in a real SceneGraph and renders it through the
# full SceneRenderer pipeline (G-buffer -> shadow map -> deferred lighting ->
# tonemap), modeled on badlands_forward_pass_test's TestGpu harness shape and
# src/engine/tests/gpu_instance_tests.cpp's RenderSceneWithFields full-frame
# pattern. Needs a working GPU adapter (Metal on macOS) + the shaders/ dir
# (WORKING_DIRECTORY = repo root); compiles the tree/leaf-voxelizer sources
# directly (mirrors badlands_tree_tests above) rather than linking them from
# badlands_game_lib (neither is a member of that library -- see
# badlands_app_sources_viewer). Phase 5 extends this suite to exercise
# TreeField/BuildTreeField (voxel-crown instanced field + field shadows), so
# also compiles the instanced-LOD sources directly for the same reason
# (also only a member of badlands_app_sources_viewer, not badlands_game_lib).
add_executable(badlands_tree_field_gpu_tests
    game/tests/tree_field_gpu_tests.cpp
    # The impostor bake (volumetric-foliage LOD4): verified by reading the
    # atlas back, so it needs a real adapter like the rest of this target.
    game/tests/impostor_bake_gpu_tests.cpp
    # The impostor's RENDER path, on an artificial sphere whose impostor has an
    # analytic expected value -- every impostor bug that reached review lived
    # here and was invisible to the atlas-readback tests above.
    game/tests/impostor_render_gpu_tests.cpp
    src/game/visual/impostor_atlas.cpp
    src/game/visual/impostor_baker.cpp
    src/game/geometry/tree_generator.cpp
    src/game/geometry/branch_junction.cpp
    src/game/geometry/leaf_texture.cpp
    src/game/geometry/leaf_voxelizer.cpp
    src/game/visual/instanced_lod_model.cpp
    src/game/visual/instanced_lod_field.cpp
    src/game/visual/tree_lod_model.cpp
    third_party/catch2/extras/catch_amalgamated.cpp
)
target_include_directories(badlands_tree_field_gpu_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/catch2/extras
    ${CMAKE_SOURCE_DIR}/src/engine/tests
)
target_link_libraries(badlands_tree_field_gpu_tests PRIVATE badlands_engine badlands_game_lib)
if(APPLE)
    target_link_options(badlands_tree_field_gpu_tests PRIVATE "-Wl,-ld_new")
endif()
add_test(NAME badlands_tree_field_gpu_tests COMMAND badlands_tree_field_gpu_tests WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# badlands_animation_tests: Catch2 suite for the skeletal-animation runtime
# (src/engine/animation). Links ozz_animation_offline so the SKELETON AND CLIP
# FIXTURES ARE BUILT IN-TEST (RawSkeleton/RawAnimation -> builders -> a temp
# .ozz) and loaded back through the real Load() path — no fixture binary is
# committed, and nothing here asserts against assets/characters/, so retuning
# shipped data can never break it. Pure CPU: no device, no window.
add_executable(badlands_animation_tests
    src/engine/tests/animation_tests.cpp
    # badlands' half of the chain: which clip a projected gameplay state plays.
    # Pure functions over CharacterAnim, so they need no Sim and no device --
    # the source is compiled in directly (as badlands_tree_field_gpu_tests does)
    # rather than pulled from an app target.
    src/game/tests/character_animation_tests.cpp
    src/game/visual/character_animation.cpp
    # The overlay end to end, minus the GPU: it needs only a Sim and the assets,
    # so the WIRING is testable headlessly -- which a screenshot cannot do, since
    # SaveScreenshot calls Update(0.0f) and the sim never ticks.
    src/game/tests/skeleton_debug_overlay_tests.cpp
    src/game/visual/skeleton_debug_overlay.cpp
    # The offline packer, round-tripped through the real AnimationSet::Load --
    # which is why it lives in THIS suite rather than its own: the assertion
    # that matters is that what rigpack writes is what the runtime reads.
    tools/rigpack/tests/rigpack_tests.cpp
    third_party/catch2/extras/catch_amalgamated.cpp
)
target_include_directories(badlands_animation_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/catch2/extras
)
target_link_libraries(badlands_animation_tests PRIVATE badlands_engine badlands_game_lib ozz_animation_offline badlands_rigpack_lib)
if(APPLE)
    target_link_options(badlands_animation_tests PRIVATE "-Wl,-ld_new")
endif()
add_test(NAME badlands_animation_tests COMMAND badlands_animation_tests WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# badlands_usd_tests: Catch2 suite for the USD import layer
# (src/engine/assets/). The ONLY test target here that does not link
# badlands_engine — it takes badlands_usd_lib alone, so there is no Dawn and no
# SDL3 in the link line and a failure can only be the parser's. It reads the
# real assets/models/*.usdc, hence the source-dir working directory.
add_executable(badlands_usd_tests
    src/engine/tests/usd_loader_tests.cpp
    src/engine/tests/usd_material_binding_tests.cpp
    third_party/catch2/extras/catch_amalgamated.cpp
)
target_include_directories(badlands_usd_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/catch2/extras
)
target_link_libraries(badlands_usd_tests PRIVATE badlands_usd_lib)
if(APPLE)
    target_link_options(badlands_usd_tests PRIVATE "-Wl,-ld_new")
endif()
add_test(NAME badlands_usd_tests COMMAND badlands_usd_tests WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# badlands_material_requirements_tests: Catch2 suite for the material
# texture-slot registry (engine/rendering/material/material_requirements.cpp).
# Pure CPU — the registry's ctor only fills a map. Exists because a slot-name
# mismatch between the registry and a caller's InstanceParams override is
# SILENT (StandardMaterialFactory matches by exact string, then falls through
# to the slot default), so the registered names have to be pinned.
add_executable(badlands_material_requirements_tests
    src/engine/tests/material_requirements_tests.cpp
    third_party/catch2/extras/catch_amalgamated.cpp
)
target_include_directories(badlands_material_requirements_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/catch2/extras
)
target_link_libraries(badlands_material_requirements_tests PRIVATE badlands_engine)
if(APPLE)
    target_link_options(badlands_material_requirements_tests PRIVATE "-Wl,-ld_new")
endif()
add_test(NAME badlands_material_requirements_tests COMMAND badlands_material_requirements_tests WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# badlands_decal_math_tests: Catch2 suite for the projected-decal math
# (engine/rendering/decal_math.hpp) — the CPU mirror of
# shaders/passes/decals.wesl. Pure CPU (header-only math + glm), so it runs the
# SDFs / perimeter parameterisation / dash pattern without a GPU; it is the
# oracle the shader transcription is checked against.
add_executable(badlands_decal_math_tests
    src/engine/tests/decal_math_tests.cpp
    third_party/catch2/extras/catch_amalgamated.cpp
)
target_include_directories(badlands_decal_math_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/catch2/extras
)
target_link_libraries(badlands_decal_math_tests PRIVATE badlands_engine)
if(APPLE)
    target_link_options(badlands_decal_math_tests PRIVATE "-Wl,-ld_new")
endif()
add_test(NAME badlands_decal_math_tests COMMAND badlands_decal_math_tests WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# badlands_checker_texture_tests: Catch2 suite for the pure RGBA8 checkerboard
# fill (engine/rendering/checker_texture.hpp) — the GPU-free core of
# MaterialLibrary::CheckerAlbedo. Pure CPU, modeled on badlands_decal_math_tests.
add_executable(badlands_checker_texture_tests
    src/engine/tests/checker_texture_tests.cpp
    third_party/catch2/extras/catch_amalgamated.cpp
)
target_include_directories(badlands_checker_texture_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/catch2/extras
)
target_link_libraries(badlands_checker_texture_tests PRIVATE badlands_engine)
if(APPLE)
    target_link_options(badlands_checker_texture_tests PRIVATE "-Wl,-ld_new")
endif()
add_test(NAME badlands_checker_texture_tests COMMAND badlands_checker_texture_tests WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# badlands_daylight_tests: Catch2 suite for the day/night system — the pure-CPU
# Hosek-Wilkie RGB port (golden values vs the reference), the ComputeDaylight
# sun/moon/handover math, and the fixed-timestep accumulator. Pure CPU (no GPU),
# modeled on badlands_geometry_tests; links badlands_engine for daylight/HW.
add_executable(badlands_daylight_tests
    game/tests/daylight_tests.cpp
    third_party/catch2/extras/catch_amalgamated.cpp
)
target_include_directories(badlands_daylight_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/catch2/extras
)
target_link_libraries(badlands_daylight_tests PRIVATE badlands_engine badlands_game_lib)
if(APPLE)
    target_link_options(badlands_daylight_tests PRIVATE "-Wl,-ld_new")
endif()
add_test(NAME badlands_daylight_tests COMMAND badlands_daylight_tests WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# badlands_input_tests: Catch2 suite for the event-driven mouse tracker
# (imgui_mouse_input). Pure tests for ImGuiButtonIndex/ApplyButton, headless-ImGui
# behaviour (same-frame click, foreign-windowID release, drag-leaves-window,
# idle-leave), and a RAII-guarded stock-backend repro of the windowID-drop wedge
# the tracker sidesteps. Links badlands_engine (carries SDL3 + ImGui + the
# imgui_impl_sdl3 backend); creates a hidden window in-test.
add_executable(badlands_input_tests
    game/tests/imgui_input_tests.cpp
    third_party/catch2/extras/catch_amalgamated.cpp
)
target_include_directories(badlands_input_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/catch2/extras
)
target_link_libraries(badlands_input_tests PRIVATE badlands_engine badlands_game_lib)
if(APPLE)
    target_link_options(badlands_input_tests PRIVATE "-Wl,-ld_new")
endif()
add_test(NAME badlands_input_tests COMMAND badlands_input_tests WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# badlands_fog_tests: Catch2 suite for the volumetric-fog cascade addressing
# math (height-band clipmap: voxel sizes, camera-centered windows, cascade
# selection, and the toroidal dirty-region computation). Pure CPU — the math
# lives header-only in src/engine/rendering/fog_cascade.hpp (glm only, no
# Dawn), so this target needs no engine/GPU link, just the src + glm include
# paths.
add_executable(badlands_fog_tests
    game/tests/fog_cascade_tests.cpp
    game/tests/fog_sim_tests.cpp
    third_party/catch2/extras/catch_amalgamated.cpp
)
target_include_directories(badlands_fog_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/catch2/extras
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/glm
)
add_test(NAME badlands_fog_tests COMMAND badlands_fog_tests WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# badlands_water_tests: Catch2 suite for the water surface wave/normal math
# (engine/rendering/water_waves.hpp) + the octahedron encode/decode CPU mirror
# (engine/rendering/octahedron.hpp). Pure CPU — header-only glm math, no Dawn —
# modeled on badlands_fog_tests. shaders/common/water_waves.wesl mirrors the
# same constants; badlands_water_gpu_test is the GPU-vs-CPU cross-check.
add_executable(badlands_water_tests
    game/tests/water_tests.cpp
    third_party/catch2/extras/catch_amalgamated.cpp
)
target_include_directories(badlands_water_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/catch2/extras
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/glm
)
add_test(NAME badlands_water_tests COMMAND badlands_water_tests WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# badlands_shadow_test: Task T4's Catch2 suite for the directional-shadow
# system (T2's ShadowMap + T3's deferred sampling) -- a headless GPU render
# harness (shadow_test_harness.*, built on the sampo-ported GPU-test infra:
# gpu_test_helpers's headless device + ColorRenderTarget/TextureReadback
# R32Float offscreen readback, see task-8-report.md) + a pure-CPU
# signed-distance edge oracle (shadow_test_geometry.*) + Test 1 (edge-leak)
# and Test 2 (sub-texel snapping). Unlike badlands_geometry_tests, this DOES
# exercise the GPU (SceneRenderer::Render) -- see task-4-report.md for the
# harness approach.
add_executable(badlands_shadow_test
    game/tests/shadow_test_geometry.cpp
    game/tests/shadow_test_harness.cpp
    game/tests/shadow_tests.cpp
    third_party/catch2/extras/catch_amalgamated.cpp
)
target_include_directories(badlands_shadow_test PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/catch2/extras
    ${CMAKE_SOURCE_DIR}/src/engine/tests
)
target_link_libraries(badlands_shadow_test PRIVATE badlands_engine badlands_game_lib)
if(APPLE)
    target_link_options(badlands_shadow_test PRIVATE "-Wl,-ld_new")
endif()
add_test(NAME badlands_shadow_test COMMAND badlands_shadow_test WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# badlands_fog_gpu_test: graphical volumetric-fog suite. Headless GPU harness
# (self-contained, on gpu_test_helpers + ColorRenderTarget + TextureReadback)
# that fills the media 3D texture (the interface under test) with an inlined
# test-pattern shader (shaders/tests/fog_fill_test.wesl), runs the PRODUCTION
# raymarch (shaders/passes/fog.wesl), and checks accumulation, shafts,
# depth-aware upsample, phase, decoupled scattering, and band bounding against
# analytic ground truth (+ reference PNGs). Needs a working GPU adapter (Metal
# on macOS); runs locally via ctest.
add_executable(badlands_fog_gpu_test
    game/tests/fog_gpu_tests.cpp
    third_party/catch2/extras/catch_amalgamated.cpp
)
target_include_directories(badlands_fog_gpu_test PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/catch2/extras
    ${CMAKE_SOURCE_DIR}/src/engine/tests
)
target_link_libraries(badlands_fog_gpu_test PRIVATE badlands_engine)
if(APPLE)
    target_link_options(badlands_fog_gpu_test PRIVATE "-Wl,-ld_new")
endif()
add_test(NAME badlands_fog_gpu_test COMMAND badlands_fog_gpu_test WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# badlands_water_gpu_test: headless GPU cross-check for the water surface. Renders
# the water material's G-buffer variant (shaders/material/water.wesl) and reads
# the shader-computed octahedron normal back from the RG16Float G-buffer normals
# target, decoding + comparing against src/engine/rendering/water_waves.hpp (the
# same C++ core badlands_water_tests unit-tests). Needs a working GPU adapter.
add_executable(badlands_water_gpu_test
    game/tests/water_gpu_tests.cpp
    third_party/catch2/extras/catch_amalgamated.cpp
)
target_include_directories(badlands_water_gpu_test PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/catch2/extras
    ${CMAKE_SOURCE_DIR}/src/engine/tests
)
target_link_libraries(badlands_water_gpu_test PRIVATE badlands_engine)
if(APPLE)
    target_link_options(badlands_water_gpu_test PRIVATE "-Wl,-ld_new")
endif()
add_test(NAME badlands_water_gpu_test COMMAND badlands_water_gpu_test WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# badlands_forward_pass_test: headless GPU tests for the forward render path
# (ported/adapted from sampo's forward material binding test). Verifies the
# forward-opaque solid-colour variant and the forward-transparent premultiplied
# blend against a depth-read-only attachment (which also validates depth_write
# as a per-material property). Uses shaders/tests/forward_debug.wesl.
add_executable(badlands_forward_pass_test
    game/tests/forward_pass_tests.cpp
    third_party/catch2/extras/catch_amalgamated.cpp
)
target_include_directories(badlands_forward_pass_test PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/catch2/extras
    ${CMAKE_SOURCE_DIR}/src/engine/tests
)
target_link_libraries(badlands_forward_pass_test PRIVATE badlands_engine)
if(APPLE)
    target_link_options(badlands_forward_pass_test PRIVATE "-Wl,-ld_new")
endif()
add_test(NAME badlands_forward_pass_test COMMAND badlands_forward_pass_test WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# badlands_fog_sim_gpu_test: headless GPU suite for the map fog GENERATOR
# (FogSimulation emitters/broadphase + the compute/fog_fill composer +
# shaders/common/fog_emitters.wesl). Composes the media from single emitters and
# reads back sigma_t (via the tests/media_slice_extract R32Float copy), verifying
# each emitter's cascade values against a CPU mirror: disc exact, physical
# sigma_t, additivity, and noise determinism/animation. Needs a GPU adapter.
add_executable(badlands_fog_sim_gpu_test
    game/tests/fog_sim_gpu_tests.cpp
    third_party/catch2/extras/catch_amalgamated.cpp
)
target_include_directories(badlands_fog_sim_gpu_test PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/catch2/extras
    ${CMAKE_SOURCE_DIR}/src/engine/tests
)
target_link_libraries(badlands_fog_sim_gpu_test PRIVATE badlands_engine)
if(APPLE)
    target_link_options(badlands_fog_sim_gpu_test PRIVATE "-Wl,-ld_new")
endif()
add_test(NAME badlands_fog_sim_gpu_test COMMAND badlands_fog_sim_gpu_test WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# badlands_readback_tests: GPU texture-readback suite ported from sampo
# (tests/readback_tests.cpp -> ReadbackTests). Exercises the four render-test
# primitives ported from sampo — CpuImage, TextureReadback, ColorRenderTarget,
# and the headless gpu_test_helpers. Unlike the two suites above this actually
# creates a Dawn device and renders, so it needs a working GPU adapter (Metal
# on macOS); there is no CI, so it runs locally via ctest.
# badlands_rhi_tests: the RHI interface + Null-backend suite. No GPU, no Dawn,
# no window — it links badlands_rhi only, so it runs anywhere and stays fast.
# The shared conformance list in src/engine/tests/rhi_conformance.hpp is what
# badlands_rhi_metal_tests will replay against Metal.
add_executable(badlands_rhi_tests
    src/engine/tests/rhi_tests.cpp
    src/engine/tests/rhi_validation_tests.cpp
    third_party/catch2/extras/catch_amalgamated.cpp
)
target_include_directories(badlands_rhi_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/catch2/extras
    ${CMAKE_SOURCE_DIR}/src/engine/tests
)
target_link_libraries(badlands_rhi_tests PRIVATE badlands_rhi)
if(APPLE)
    target_link_options(badlands_rhi_tests PRIVATE "-Wl,-ld_new")
endif()
add_test(NAME badlands_rhi_tests COMMAND badlands_rhi_tests WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# badlands_render_graph_tests: the graph against Null, with no GPU. The
# transitions it derives are only observable through the validation decorator —
# Metal renders correctly whether or not any were declared — so this is where
# the graph's barrier derivation is actually checked.
add_executable(badlands_render_graph_tests
    src/engine/tests/render_graph_tests.cpp
    third_party/catch2/extras/catch_amalgamated.cpp
)
target_include_directories(badlands_render_graph_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/catch2/extras
    ${CMAKE_SOURCE_DIR}/src/engine/tests
)
target_link_libraries(badlands_render_graph_tests PRIVATE badlands_render_graph)
if(APPLE)
    target_link_options(badlands_render_graph_tests PRIVATE "-Wl,-ld_new")
endif()
add_test(NAME badlands_render_graph_tests COMMAND badlands_render_graph_tests
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# badlands_rhi_metal_tests: the SAME conformance list against the real backend,
# plus readback-based correctness Null cannot assert. Needs a Metal device.
if(APPLE)
    add_executable(badlands_rhi_metal_tests
        src/engine/tests/rhi_metal_tests.cpp
        third_party/catch2/extras/catch_amalgamated.cpp
    )
    target_include_directories(badlands_rhi_metal_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/third_party/catch2/extras
        ${CMAKE_SOURCE_DIR}/third_party/glm
        ${CMAKE_SOURCE_DIR}/src/engine/tests
    )
    target_link_libraries(badlands_rhi_metal_tests PRIVATE badlands_rhi)
    target_link_options(badlands_rhi_metal_tests PRIVATE "-Wl,-ld_new")
    add_test(NAME badlands_rhi_metal_tests COMMAND badlands_rhi_metal_tests
             WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    # Metal's own validation layers, always on for this suite. They catch
    # invalid API use that a passing test otherwise hides — the first time they
    # were switched on they aborted the suite on an invalid dispatch.
    set_tests_properties(badlands_rhi_metal_tests PROPERTIES
        ENVIRONMENT "MTL_DEBUG_LAYER=1;MTL_SHADER_VALIDATION=1")
endif()

# badlands_rhi_lab: the MVP that proves the RHI + Slang + visibility-buffer
# stack end to end. Headless — the MVP RHI has no swapchain — so it renders a
# frame and writes a PNG.
if(BADLANDS_HAVE_SLANG AND APPLE)
    # Compiles the cluster DAG build + its deps directly, the same way
    # badlands_terrain_cluster_tests does — the lab wants the real Nanite-style
    # DAG but none of the renderer it is replacing.
    add_executable(badlands_rhi_lab
        src/executables/rhi_lab/main.cpp
        src/executables/rhi_lab/lab_scene.cpp
        src/game/geometry/terrain_clusters.cpp
        src/game/map/map_data.cpp
        src/engine/rendering/geometry/aabb.cpp
        src/core/parallel.cpp
    )
    target_link_libraries(badlands_rhi_lab PRIVATE
        badlands_slang badlands_rhi_app assets meshoptimizer SDL3::SDL3)
    target_include_directories(badlands_rhi_lab PRIVATE
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/third_party/glm
        ${CMAKE_SOURCE_DIR}/third_party/spdlog/include
        ${CMAKE_SOURCE_DIR}/src/crates/assets/include
        ${taskflow_SOURCE_DIR})
    target_link_options(badlands_rhi_lab PRIVATE "-Wl,-ld_new")

    # Argument rejection, asserted through the exit code. The lab is an
    # executable with no test binary of its own, so this is the honest
    # mechanism -- and these arguments used to be ACCEPTED: an out-of-range
    # count truncated into a valid-looking value and rendered silently wrong
    # geometry with exit status 0.
    # The resize DECISION, tested without a window. The loop's own bug -- gating
    # on the render targets, which are not rebuilt at zero size -- is invisible
    # to a windowed test unless you happen to minimize and restore to exactly
    # the same size, so it gets a deterministic test of its own.
    add_executable(badlands_rhi_app_tests
        src/engine/tests/surface_size_tests.cpp
        third_party/catch2/extras/catch_amalgamated.cpp)
    target_include_directories(badlands_rhi_app_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/third_party/catch2/extras)
    add_test(NAME badlands_rhi_app_tests COMMAND badlands_rhi_app_tests
             WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

    # Windowed resize, end to end: open a window, ask for a different size,
    # and assert everything sized to the frame followed the reported PIXEL
    # size. LABELLED and excluded from the default run because it needs a
    # display -- over SSH or with the screen locked it would fail for reasons
    # that say nothing about the code, and a suite that flakes gets ignored.
    # Run it with: ctest -L display
    add_test(NAME rhi_lab_windowed_resize
             COMMAND badlands_rhi_lab --self-test 40 --width 800 --height 600
             WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    set_tests_properties(rhi_lab_windowed_resize PROPERTIES
                         LABELS display
                         TIMEOUT 120)

    foreach(bad_args
        "--nodes;4294967298"      # truncates to 2 without a range check
        "--width;4294967297"      # truncates to 1
        "--nodes;-5"              # wraps huge when narrowed
        "--tau;notanumber"        # used to reach std::terminate
        "--height;0")
        string(REPLACE ";" "_" bad_name "${bad_args}")
        string(REPLACE "-" "" bad_name "${bad_name}")
        # --out into the build tree: these must exit before rendering, but if
        # one ever stops rejecting it writes its PNG here rather than dropping
        # an untracked LFS-matching file into the repo root. That is exactly
        # what happened while proving these tests fail without their fix.
        add_test(NAME rhi_lab_rejects_${bad_name}
                 COMMAND badlands_rhi_lab ${bad_args}
                         --out ${CMAKE_BINARY_DIR}/rhi_lab_reject_check.png
                 WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
        set_tests_properties(rhi_lab_rejects_${bad_name}
                             PROPERTIES WILL_FAIL TRUE)
    endforeach()
endif()

# badlands_object_viewer: the next iteration of badlands_viewer, on the RHI and
# the render graph. Windowed and headless run the SAME graph — only the imported
# output texture differs — so the headless test exercises the real path.
#
# BOTH halves of the guard: it links badlands_slang and badlands_imgui_rhi, and
# neither target EXISTS when the Slang SDK is absent. CMake does not refuse the
# link at generate time — an unknown plain name becomes a raw -l flag — so the
# target gets created anyway and fails to COMPILE on a missing <imgui.h>, taking
# the whole default build down with it on a fresh clone.
if(BADLANDS_HAVE_SLANG AND APPLE)
    add_executable(badlands_object_viewer
        src/executables/object_viewer/main.cpp
        src/executables/object_viewer/line_pass.cpp
        src/engine/rendering/debug_line_buffer.cpp
        src/engine/rendering/debug_line_expand.cpp)
    target_link_libraries(badlands_object_viewer PRIVATE
        badlands_render_graph badlands_rhi_app badlands_slang
        badlands_imgui_rhi assets)
    target_sources(badlands_object_viewer PRIVATE
        ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp)
    target_include_directories(badlands_object_viewer PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/third_party/glm
        ${CMAKE_SOURCE_DIR}/third_party/spdlog/include
        ${CMAKE_SOURCE_DIR}/src/crates/assets/include)
    target_link_options(badlands_object_viewer PRIVATE "-Wl,-ld_new")

    # Headless render, asserted through the exit code and a pixel check. Writes
    # into the BUILD tree: a stray .png in the repo root would match an LFS
    # pattern in .gitattributes and get committed as a pointer.
    add_test(NAME object_viewer_headless
             COMMAND badlands_object_viewer --headless --width 64 --height 64
                     --out ${CMAKE_BINARY_DIR}/object_viewer_headless.png
             WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    # The debug-line pass, asserted on pixels: the segment covers the middle row
    # and half the width, the centre texel is the red it was given, and the
    # corners stay the clear colour.
    add_test(NAME object_viewer_grid
             COMMAND badlands_object_viewer --headless --scene grid
                     --width 256 --height 192
                     --out ${CMAKE_BINARY_DIR}/object_viewer_grid.png
             WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    add_test(NAME object_viewer_lines
             COMMAND badlands_object_viewer --headless --scene lines
                     --width 64 --height 64
                     --out ${CMAKE_BINARY_DIR}/object_viewer_lines.png
             WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

    foreach(bad_args "--width;0" "--height;4294967297" "--frames;-1" "--nope")
        string(REPLACE ";" "_" bad_name "${bad_args}")
        string(REPLACE "-" "" bad_name "${bad_name}")
        add_test(NAME object_viewer_rejects_${bad_name}
                 COMMAND badlands_object_viewer ${bad_args} --headless
                         --out ${CMAKE_BINARY_DIR}/object_viewer_reject.png
                 WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
        set_tests_properties(object_viewer_rejects_${bad_name}
                             PROPERTIES WILL_FAIL TRUE)
    endforeach()

    # Windowed, behind the display label like rhi_lab's: over SSH or with the
    # screen locked it would fail for reasons that say nothing about the code.
    add_test(NAME object_viewer_windowed
             COMMAND badlands_object_viewer --frames 20 --width 640 --height 480
             WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    set_tests_properties(object_viewer_windowed PROPERTIES
                         LABELS display TIMEOUT 120)
    # --scene really reaches the windowed path. It used to be parsed and then
    # ignored there, which is rule 4's accepted-and-ignored trap; a run that
    # renders is the cheapest thing that can tell the difference.
    # Escape must survive an event consumer that takes everything -- an ImGui
    # widget with keyboard focus is exactly that, and it used to swallow the
    # documented quit key.
    add_test(NAME object_viewer_escape_survives_capture
             COMMAND badlands_object_viewer --self-test-escape --frames 60
                     --width 320 --height 240
             WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    set_tests_properties(object_viewer_escape_survives_capture PROPERTIES
                         LABELS display TIMEOUT 120)
    add_test(NAME object_viewer_windowed_scene
             COMMAND badlands_object_viewer --scene clear --frames 10
                     --width 320 --height 240
             WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    set_tests_properties(object_viewer_windowed_scene PROPERTIES
                         LABELS display TIMEOUT 120)
endif()

# badlands_imgui_rhi: the Dear ImGui renderer backend against the RHI, replacing
# imgui_impl_wgpu_custom. Needs Slang (its shader) and the graph (it adds a
# pass); the imgui SOURCES come from the same FetchContent the engine uses.
if(BADLANDS_HAVE_SLANG AND APPLE)
    add_library(badlands_imgui_rhi STATIC
        src/engine/ui/imgui_impl_rhi.cpp
        ${imgui_SOURCE_DIR}/imgui.cpp
        ${imgui_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    )
    target_link_libraries(badlands_imgui_rhi PUBLIC
        badlands_render_graph badlands_slang)
    target_include_directories(badlands_imgui_rhi PUBLIC
        ${imgui_SOURCE_DIR}
        ${imgui_SOURCE_DIR}/backends)

    # Pixels, not eyeballs: a fixed-coordinate draw list plus a readback turns
    # "the UI looks right" into numbers a regression can fail.
    add_executable(badlands_imgui_rhi_tests
        src/engine/tests/imgui_rhi_tests.cpp
        third_party/catch2/extras/catch_amalgamated.cpp)
    target_include_directories(badlands_imgui_rhi_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/third_party/catch2/extras
        ${CMAKE_SOURCE_DIR}/src/engine/tests)
    target_link_libraries(badlands_imgui_rhi_tests PRIVATE badlands_imgui_rhi)
    target_link_options(badlands_imgui_rhi_tests PRIVATE "-Wl,-ld_new")
    add_test(NAME badlands_imgui_rhi_tests COMMAND badlands_imgui_rhi_tests
             WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    set_tests_properties(badlands_imgui_rhi_tests PROPERTIES
        ENVIRONMENT "MTL_DEBUG_LAYER=1;MTL_SHADER_VALIDATION=1")
endif()

# badlands_slang_tests: compile, cache, hot-reload and reflection, against
# test-local fixtures. Skipped along with badlands_slang when the SDK is absent.
if(BADLANDS_HAVE_SLANG)
    add_executable(badlands_slang_tests
        src/engine/tests/slang_compiler_tests.cpp
        third_party/catch2/extras/catch_amalgamated.cpp
    )
    target_include_directories(badlands_slang_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/third_party/catch2/extras
        ${CMAKE_SOURCE_DIR}/src/engine/tests
    )
    target_link_libraries(badlands_slang_tests PRIVATE badlands_slang)
    if(APPLE)
        target_link_options(badlands_slang_tests PRIVATE "-Wl,-ld_new")
    endif()
    add_test(NAME badlands_slang_tests COMMAND badlands_slang_tests
             WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endif()

# badlands_splat_tests: the Dreams-style splat chain, proven by reading pixels.
# Slang for three of the four passes and hand-written MSL for the atomic one --
# Slang 2026.14.1 emits the 32-bit atomic intrinsic for Metal, which MSL rejects
# on a 64-bit type. It needs BOTH a Slang SDK and a Metal device, so it cannot
# live in badlands_slang_tests (no device) or badlands_rhi_metal_tests (no
# compiler).
if(BADLANDS_HAVE_SLANG AND APPLE)
    add_executable(badlands_splat_tests
        src/engine/tests/splat_tests.cpp
        third_party/catch2/extras/catch_amalgamated.cpp
    )
    target_include_directories(badlands_splat_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/third_party/catch2/extras
        ${CMAKE_SOURCE_DIR}/third_party/glm
        ${CMAKE_SOURCE_DIR}/src/engine/tests
    )
    target_link_libraries(badlands_splat_tests PRIVATE badlands_slang)
    target_link_options(badlands_splat_tests PRIVATE "-Wl,-ld_new")
    add_test(NAME badlands_splat_tests COMMAND badlands_splat_tests
             WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    set_tests_properties(badlands_splat_tests PROPERTIES
        ENVIRONMENT "MTL_DEBUG_LAYER=1;MTL_SHADER_VALIDATION=1")
endif()

add_executable(badlands_readback_tests
    src/engine/tests/readback_tests.cpp
    third_party/catch2/extras/catch_amalgamated.cpp
)
target_include_directories(badlands_readback_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/catch2/extras
    ${CMAKE_SOURCE_DIR}/src/engine/tests
)
target_link_libraries(badlands_readback_tests PRIVATE badlands_engine)
if(APPLE)
    target_link_options(badlands_readback_tests PRIVATE "-Wl,-ld_new")
endif()
add_test(NAME badlands_readback_tests COMMAND badlands_readback_tests WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# badlands_terrain_blend_tests: GPU test for the terrain-blend material — renders
# a kTerrainBlend quad blending an R/G/B texture_2d_array and reads back the
# unlit G-buffer albedo via GBufferDebugMode::Albedo. Needs a working GPU
# adapter (Metal on macOS) + the shaders/ dir (WORKING_DIRECTORY = repo root).
add_executable(badlands_terrain_blend_tests
    src/engine/tests/terrain_blend_tests.cpp
    third_party/catch2/extras/catch_amalgamated.cpp
)
target_include_directories(badlands_terrain_blend_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/catch2/extras
    ${CMAKE_SOURCE_DIR}/src/engine/tests
)
target_link_libraries(badlands_terrain_blend_tests PRIVATE badlands_engine)
if(APPLE)
    target_link_options(badlands_terrain_blend_tests PRIVATE "-Wl,-ld_new")
endif()
add_test(NAME badlands_terrain_blend_tests COMMAND badlands_terrain_blend_tests WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# badlands_decal_tests: GPU test for the projected-decal pass — renders rings
# and rounded rects through SceneRenderer into an offscreen R32Float target and
# reads back the red channel at projected world points. Covers outline-only
# rendering, the yaw convention, the vertical projector band, steep-surface
# rejection, and a dash cross-check against the decal_math.hpp CPU mirror (which
# is what validates the shaders/passes/decals.wesl transcription). Needs a
# working GPU adapter + the shaders/ dir (WORKING_DIRECTORY = repo root).
add_executable(badlands_decal_tests
    src/engine/tests/decal_pass_tests.cpp
    third_party/catch2/extras/catch_amalgamated.cpp
)
target_include_directories(badlands_decal_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/catch2/extras
    ${CMAKE_SOURCE_DIR}/src/engine/tests
)
target_link_libraries(badlands_decal_tests PRIVATE badlands_engine)
if(APPLE)
    target_link_options(badlands_decal_tests PRIVATE "-Wl,-ld_new")
endif()
add_test(NAME badlands_decal_tests COMMAND badlands_decal_tests WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# badlands_resolve_composite_tests: GPU test for the resolve pass's UI-overlay
# composite (tonemapping.wesl + SceneContext::ui_overlay) — pins the
# gamma-space blend and the P3/EDR paths against a CPU mirror. Same harness
# shape as badlands_decal_tests (R32Float readback; needs a GPU + shaders/).
add_executable(badlands_resolve_composite_tests
    src/engine/tests/resolve_composite_tests.cpp
    third_party/catch2/extras/catch_amalgamated.cpp
)
target_include_directories(badlands_resolve_composite_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/catch2/extras
    ${CMAKE_SOURCE_DIR}/src/engine/tests
)
target_link_libraries(badlands_resolve_composite_tests PRIVATE badlands_engine)
if(APPLE)
    target_link_options(badlands_resolve_composite_tests PRIVATE "-Wl,-ld_new")
endif()
add_test(NAME badlands_resolve_composite_tests COMMAND badlands_resolve_composite_tests WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# badlands_gpu_instance_tests: GPU-driven instanced-rendering feature, Phase A
# (primitives spike) — de-risks atomics in a compute shader
# (shaders/tests/atomic_append_test.wesl, the first shader in this repo to use
# atomic<u32>), DrawIndexedIndirect (RenderPassContext::DrawIndexedIndirect +
# shaders/tests/indirect_draw_test.wesl), and the reusable BufferReadback test
# helper (src/engine/tests/buffer_readback.*) that both of the above need.
# Later phases (GPU cull/LOD/compaction) extend this same target. Needs a
# working GPU adapter (Metal on macOS) + the shaders/ dir (WORKING_DIRECTORY =
# repo root).
add_executable(badlands_gpu_instance_tests
    src/engine/tests/gpu_instance_tests.cpp
    src/engine/tests/buffer_readback.cpp
    third_party/catch2/extras/catch_amalgamated.cpp
)
target_include_directories(badlands_gpu_instance_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/catch2/extras
    ${CMAKE_SOURCE_DIR}/src/engine/tests
)
target_link_libraries(badlands_gpu_instance_tests PRIVATE badlands_engine)
if(APPLE)
    target_link_options(badlands_gpu_instance_tests PRIVATE "-Wl,-ld_new")
endif()
add_test(NAME badlands_gpu_instance_tests COMMAND badlands_gpu_instance_tests WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# badlands_terrain_mesh_tests: pure-CPU tests for src/game/geometry/terrain_mesh
# — the SampleHeight/RaycastTerrain queries behind mouse picking. No GPU: only
# glm + the header-only mapgen Field2D/Biome types and the header-only Ray struct.
add_executable(badlands_terrain_mesh_tests
    src/game/map/map_data.cpp
    src/game/geometry/terrain_mesh.cpp
    src/game/geometry/water_surface.cpp
    game/tests/terrain_mesh_tests.cpp
    game/tests/terrain_raycast_tests.cpp
    game/tests/biome_blend_tests.cpp
    third_party/catch2/extras/catch_amalgamated.cpp
)
target_include_directories(badlands_terrain_mesh_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/glm
    ${CMAKE_SOURCE_DIR}/third_party/catch2/extras
)
add_test(NAME badlands_terrain_mesh_tests COMMAND badlands_terrain_mesh_tests WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# badlands_symbolic_map_tests: pure-CPU tests for the symbolic greybox map
# generator (src/game/map/symbolic_map_generator) — biome stamping, flat plane,
# lake basin carve, and lake-outline extraction. No GPU: glm + header-only
# mapgen Field2D/Biome only (same shape as badlands_terrain_mesh_tests).
add_executable(badlands_symbolic_map_tests
    src/game/map/map_data.cpp
    src/game/map/symbolic_map_generator.cpp
    src/game/map/flat_map_generator.cpp
    # The synthetic forest map + the MapData -> foliage::TerrainQuery adapter:
    # the seam between the map contract and the placement library.
    src/game/map/forest_test_map_generator.cpp
    game/tests/forest_test_map_tests.cpp
    src/game/geometry/water_surface.cpp
    game/tests/symbolic_map_tests.cpp
    third_party/catch2/extras/catch_amalgamated.cpp
)
target_link_libraries(badlands_symbolic_map_tests PRIVATE badlands_foliage_lib)
target_include_directories(badlands_symbolic_map_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/glm
    ${CMAKE_SOURCE_DIR}/third_party/catch2/extras
    ${CMAKE_SOURCE_DIR}/third_party/FastNoiseLite
)
add_test(NAME badlands_symbolic_map_tests COMMAND badlands_symbolic_map_tests WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# badlands_frustum_tests: pure-CPU tests for view-frustum AABB culling.
add_executable(badlands_frustum_tests
    src/engine/tests/frustum_tests.cpp
    src/engine/rendering/frustum.cpp
    src/engine/rendering/geometry/aabb.cpp
    third_party/catch2/extras/catch_amalgamated.cpp
)
target_include_directories(badlands_frustum_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/glm
    ${CMAKE_SOURCE_DIR}/third_party/catch2/extras
)
target_compile_definitions(badlands_frustum_tests PRIVATE GLM_FORCE_DEPTH_ZERO_TO_ONE)
add_test(NAME badlands_frustum_tests COMMAND badlands_frustum_tests WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# badlands_debug_line_tests: pure-CPU tests for the world-space thick-line quad
# expansion (near-plane clipping so segments crossing behind the camera don't
# produce garbage vertices).
add_executable(badlands_debug_line_tests
    src/engine/tests/debug_line_expand_tests.cpp
    src/engine/rendering/debug_line_expand.cpp
    src/engine/rendering/debug_line_buffer.cpp
    third_party/catch2/extras/catch_amalgamated.cpp
)
target_include_directories(badlands_debug_line_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/glm
    ${CMAKE_SOURCE_DIR}/third_party/catch2/extras
)
add_test(NAME badlands_debug_line_tests COMMAND badlands_debug_line_tests WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# badlands_texture_array_tests: GPU test for PackTexturesIntoArray — packs
# mipped 2D textures into a texture_2d_array and reads back per-layer to check
# layer identity/ordering + that the source mip chains survive the copy, plus
# the null/size/format validation contracts. Needs a GPU adapter + shaders/.
add_executable(badlands_texture_array_tests
    src/engine/tests/texture_array_tests.cpp
    third_party/catch2/extras/catch_amalgamated.cpp
)
target_include_directories(badlands_texture_array_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/catch2/extras
    ${CMAKE_SOURCE_DIR}/src/engine/tests
)
target_link_libraries(badlands_texture_array_tests PRIVATE badlands_engine)
if(APPLE)
    target_link_options(badlands_texture_array_tests PRIVATE "-Wl,-ld_new")
endif()
add_test(NAME badlands_texture_array_tests COMMAND badlands_texture_array_tests WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# badlands_biome_manifest_tests: pure-CPU tests for the biome -> material-pack
# manifest resolution (order == Biome enum == terrain array layer index).
# Reads the shipped manifest, so WORKING_DIRECTORY = repo root.
add_executable(badlands_biome_manifest_tests
    src/mapview/tests/biome_manifest_tests.cpp
    src/mapview/biome_manifest.cpp
    third_party/catch2/extras/catch_amalgamated.cpp
)
target_include_directories(badlands_biome_manifest_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/glm
    ${CMAKE_SOURCE_DIR}/third_party/spdlog/include
    ${CMAKE_SOURCE_DIR}/third_party/catch2/extras
)
target_link_libraries(badlands_biome_manifest_tests PRIVATE nlohmann_json::nlohmann_json)
add_test(NAME badlands_biome_manifest_tests COMMAND badlands_biome_manifest_tests WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# badlands_biome_splat_tests: pure-CPU tests for the biome -> splat raster the
# cluster terrain samples by world XZ (slot index == Biome enum == terrain array
# layer index, weights sum to full, at most two layers per texel).
# badlands_foliage_tests: the foliage placement library — the underlying
# methods (depth curve, hash, clump remap, EDT depth field, slope probe, cell
# indexing) each against an analytic answer, then the whole-generator
# guarantees (determinism, spacing, depth ordering, terrain rejection). Pure
# CPU: links badlands_foliage_lib, no Dawn/GPU.
add_executable(badlands_foliage_tests
    src/foliage/tests/foliage_tests.cpp
    third_party/catch2/extras/catch_amalgamated.cpp
)
target_link_libraries(badlands_foliage_tests PRIVATE badlands_foliage_lib)
target_include_directories(badlands_foliage_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/glm
    ${CMAKE_SOURCE_DIR}/third_party/catch2/extras
    # The clump-window test measures the ACTUAL fBm distribution the defaults
    # were tuned against, so it needs the generator's own noise source.
    ${CMAKE_SOURCE_DIR}/third_party/FastNoiseLite
)
add_test(NAME badlands_foliage_tests COMMAND badlands_foliage_tests WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

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

# badlands_patch_io_tests: the on-disk patch form -- the level convention
# (depth = max(0, level - height)), lake derivation, and the size/range checks
# that stand between a wrong manifest and a silently reinterpreted patch.
add_executable(badlands_patch_io_tests
    src/mapgen/patch_io_tests.cpp
    third_party/catch2/extras/catch_amalgamated.cpp
)
target_include_directories(badlands_patch_io_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/catch2/extras
)
target_link_libraries(badlands_patch_io_tests PRIVATE badlands_patch_providers)
add_test(NAME badlands_patch_io_tests COMMAND badlands_patch_io_tests WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# badlands_patch_tests: CoarseWorldPatchSource -- resample (both directions),
# water rebuild, biome classification from the manifest cutoffs, and river
# clip/rebase/prune, plus the cross-provider structural invariants shared with
# SyntheticPatchSource. HARD RULE: no test here may run the coarse sim; every
# fixture is a synthetic coarse artifact the test writes itself.
add_executable(badlands_patch_tests
    src/mapgen/coarse_world_patch_source_tests.cpp
    third_party/catch2/extras/catch_amalgamated.cpp
)
target_include_directories(badlands_patch_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/catch2/extras
)
target_link_libraries(badlands_patch_tests PRIVATE badlands_patch_providers)
add_test(NAME badlands_patch_tests COMMAND badlands_patch_tests WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# badlands_relief_tests: the stage-2 relief chain -- Catmull-Rom point
# sampling with analytic gradients (cubic_sample.hpp) and the per-point
# erosion detail filter (relief_filter.hpp). Same hard rule as
# badlands_patch_tests: fixtures are hand-written, never simulated.
add_executable(badlands_relief_tests
    src/mapgen/relief_filter_tests.cpp
    third_party/catch2/extras/catch_amalgamated.cpp
)
target_include_directories(badlands_relief_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/catch2/extras
)
target_link_libraries(badlands_relief_tests PRIVATE badlands_patch_providers)
add_test(NAME badlands_relief_tests COMMAND badlands_relief_tests WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# badlands_lake_surface_tests: pure-CPU tests for the per-lake water surface and
# its buried skirt -- the overlap that keeps a later wave displacement from
# opening a gap at the shoreline. Builds PatchData fixtures now, so it links
# badlands_patch_providers for the contract.
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
target_link_libraries(badlands_lake_surface_tests PRIVATE badlands_patch_providers)
add_test(NAME badlands_lake_surface_tests COMMAND badlands_lake_surface_tests WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# badlands_river_surface_tests: pure-CPU tests for the river channel ribbon --
# the arc-chain fit turned into draped, hydraulically-sized geometry. Links
# badlands_patch_providers (a superset of badlands_mapgen_lib, which river_arcs
# still comes from) so it builds PatchData fixtures alongside the lake surface
# suite above.
add_executable(badlands_river_surface_tests
    src/mapview/tests/river_surface_tests.cpp
    src/mapview/river_surface.cpp
    third_party/catch2/extras/catch_amalgamated.cpp
)
target_include_directories(badlands_river_surface_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/catch2/extras
)
target_link_libraries(badlands_river_surface_tests PRIVATE badlands_patch_providers)
add_test(NAME badlands_river_surface_tests COMMAND badlands_river_surface_tests WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# badlands_factors_manifest_tests: behaviour tuning loaded as data
# (assets/creatures/factors.json -> badlands::SimFactors). App-layer, so the sim
# keeps its lean dependency set. Reads the shipped file, so cwd = repo root.
add_executable(badlands_factors_manifest_tests
    src/game/tests/factors_manifest_tests.cpp
    src/game/factors_manifest.cpp
    # The AI vocabulary tables (activity catalog + default weights + hero class
    # names). Depends on badlands_sim.hpp alone, so it links here without
    # dragging in the sim -- which is why this target stays lean.
    game/src/activity_catalog.cpp
    game/src/activity_histogram.cpp
    third_party/catch2/extras/catch_amalgamated.cpp
)
target_include_directories(badlands_factors_manifest_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/game/include
    ${CMAKE_SOURCE_DIR}/third_party/glm
    ${CMAKE_SOURCE_DIR}/third_party/spdlog/include
    ${CMAKE_SOURCE_DIR}/third_party/entt/single_include
    ${CMAKE_SOURCE_DIR}/third_party/catch2/extras
)
target_link_libraries(badlands_factors_manifest_tests PRIVATE nlohmann_json::nlohmann_json)
add_test(NAME badlands_factors_manifest_tests COMMAND badlands_factors_manifest_tests WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# badlands_skill_manifest_tests: skill template overrides loaded as data
# (assets/skills/skills.json -> badlands::SkillCatalog). App-layer, so the sim
# keeps its lean dependency set. Reads the shipped file, so cwd = repo root.
add_executable(badlands_skill_manifest_tests
    src/game/tests/skill_manifest_tests.cpp
    src/game/skill_manifest.cpp
    # The skill vocabulary table (compiled SkillCatalog defaults + name
    # lookup). Depends on badlands_sim.hpp plus the game-side component/view
    # headers it declares alongside, so this target also needs game/src on
    # its include path (see below) -- entt and command.h are header-only, so
    # no extra sources are dragged in.
    game/src/skills.cpp
    third_party/catch2/extras/catch_amalgamated.cpp
)
target_include_directories(badlands_skill_manifest_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/game/include
    ${CMAKE_SOURCE_DIR}/game/src
    ${CMAKE_SOURCE_DIR}/third_party/glm
    ${CMAKE_SOURCE_DIR}/third_party/spdlog/include
    ${CMAKE_SOURCE_DIR}/third_party/entt/single_include
    ${CMAKE_SOURCE_DIR}/third_party/catch2/extras
)
target_link_libraries(badlands_skill_manifest_tests PRIVATE nlohmann_json::nlohmann_json)
add_test(NAME badlands_skill_manifest_tests COMMAND badlands_skill_manifest_tests WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# badlands_ai_sandbox_tests: the sandbox's PUPPET-MASTER layer -- arena layouts
# and the duel round lifecycle. Links badlands_game_lib and nothing else: no
# engine, no Dawn, no SDL. That is deliberate rather than economical -- it is a
# compile-time proof that a SandboxMode is view-free, so the same code the
# window drives can be tested without one.
add_executable(badlands_ai_sandbox_tests
    src/executables/ai_sandbox/tests/ai_sandbox_tests.cpp
    src/executables/ai_sandbox/arena.cpp
    src/executables/ai_sandbox/duel_mode.cpp
    src/executables/ai_sandbox/sneak_mode.cpp
    src/executables/ai_sandbox/teleport_mode.cpp
    third_party/catch2/extras/catch_amalgamated.cpp
)
target_include_directories(badlands_ai_sandbox_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/game/include
    ${CMAKE_SOURCE_DIR}/game/src
    ${CMAKE_SOURCE_DIR}/third_party/glm
    ${CMAKE_SOURCE_DIR}/third_party/spdlog/include
    ${CMAKE_SOURCE_DIR}/third_party/entt/single_include
    ${CMAKE_SOURCE_DIR}/third_party/catch2/extras
)
target_link_libraries(badlands_ai_sandbox_tests PRIVATE badlands_game_lib)
add_test(NAME badlands_ai_sandbox_tests COMMAND badlands_ai_sandbox_tests WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# badlands_ray_tests: pure-CPU tests for screen-pixel -> world-ray picking
# (src/engine/core/ray) and the fixed-angle GameCameraController's zoom that
# rides on it. glm-only: camera.hpp is header-only and the controller pulls in
# nothing else (deliberately no sdl_input_util here — that would drag in SDL3).
add_executable(badlands_ray_tests
    src/engine/tests/ray_tests.cpp
    src/engine/tests/game_camera_controller_tests.cpp
    src/engine/core/ray.cpp
    src/engine/app/game_camera_controller.cpp
    src/engine/app/orbit_camera_controller.cpp
    third_party/catch2/extras/catch_amalgamated.cpp
)
target_include_directories(badlands_ray_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/glm
    ${CMAKE_SOURCE_DIR}/third_party/catch2/extras
)
target_compile_definitions(badlands_ray_tests PRIVATE GLM_FORCE_DEPTH_ZERO_TO_ONE)
add_test(NAME badlands_ray_tests COMMAND badlands_ray_tests WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# ============================================================================
# editors/shapeshifter — the SDF editor, moved in from its own repo.
#
# Its C++ core builds here so there is one source of truth for it; the SwiftUI
# app shell around it stays on XcodeGen (editors/shapeshifter/project.yml),
# which links the static library this produces. That split is deliberate: CMake
# is a poor fit for a SwiftUI .app bundle, and Xcode is a poor fit for the rest
# of this repo, so each keeps what it is good at.
#
# This target deliberately does NOT link badlands_rhi yet. Shapeshifter still
# talks to Metal through vendored metal-cpp; porting it onto the RHI is the next
# step, not this one, and an unused link dependency would only advertise an
# integration that does not exist.
# ============================================================================
set(SHAPESHIFTER_DIR ${CMAKE_SOURCE_DIR}/editors/shapeshifter)

add_library(shapeshifter_core STATIC
    ${SHAPESHIFTER_DIR}/core/src/camera.cpp
    ${SHAPESHIFTER_DIR}/core/src/camera_controller.cpp
    ${SHAPESHIFTER_DIR}/core/src/dcsdd.cpp
    ${SHAPESHIFTER_DIR}/core/src/editor.cpp
    ${SHAPESHIFTER_DIR}/core/src/gizmo.cpp
    ${SHAPESHIFTER_DIR}/core/src/lines.cpp
    ${SHAPESHIFTER_DIR}/core/src/mesh_job.cpp
    ${SHAPESHIFTER_DIR}/core/src/metal_impl.cpp
    ${SHAPESHIFTER_DIR}/core/src/navigation.cpp
    ${SHAPESHIFTER_DIR}/core/src/picking.cpp
    ${SHAPESHIFTER_DIR}/core/src/renderer.cpp
    ${SHAPESHIFTER_DIR}/core/src/scene.cpp
    ${SHAPESHIFTER_DIR}/core/src/sdf.cpp
)
# core/include is PUBLIC because the Swift app consumes ShapeshifterCore.h and
# its module.modulemap from there. core/src is PRIVATE and quote-only (-iquote):
# its headers are included as "camera.h", and putting that directory on the
# angle-bracket path is what used to let core/src/math.h shadow the C library's
# <math.h> in any TU that pulled in <cmath>. The header is now math_util.h so
# the collision cannot recur, but the narrower path is still the right default.
target_include_directories(shapeshifter_core
    PUBLIC
        ${SHAPESHIFTER_DIR}/core/include
        ${SHAPESHIFTER_DIR}/shaders          # shared_types.h, ground_grid.h, sdf_scene.h
        ${SHAPESHIFTER_DIR}/vendor/metal-cpp
)
target_compile_options(shapeshifter_core PRIVATE -iquote${SHAPESHIFTER_DIR}/core/src)

# DCSDD reconstruction is expensive numerical code (dense per-cell least-squares
# solves x hundreds of cells x outer iterations). Unoptimized it runs ~43x slower
# than -O2 — measured on a single-cube remesh at the editor's config, 243.6s vs
# 5.6s — which makes the app unusable for interactive editing. This project sets
# no CMAKE_BUILD_TYPE, so the default config carries no -O flag at all; opt this
# one target in rather than changing the whole repo's defaults.
target_compile_options(shapeshifter_core PRIVATE
    $<$<OR:$<CONFIG:>,$<CONFIG:Debug>>:-O2>)

target_link_libraries(shapeshifter_core PUBLIC
    "-framework Metal" "-framework Foundation"
    "-framework QuartzCore" "-framework CoreGraphics")

# shapeshifter_core_tests: the editor core's own suite, moved across as-is.
# It is still on doctest rather than this repo's Catch2 — porting 15 files is
# mechanical but it does not belong in a commit whose claim is "only paths and
# build files moved". ctest runs it either way, so scripts/test.sh covers it.
add_executable(shapeshifter_core_tests
    ${SHAPESHIFTER_DIR}/tests/core/camera_controller_tests.cpp
    ${SHAPESHIFTER_DIR}/tests/core/camera_tests.cpp
    ${SHAPESHIFTER_DIR}/tests/core/dcsdd_tests.cpp
    ${SHAPESHIFTER_DIR}/tests/core/drag_tests.cpp
    ${SHAPESHIFTER_DIR}/tests/core/gizmo_tests.cpp
    ${SHAPESHIFTER_DIR}/tests/core/ground_grid_tests.cpp
    ${SHAPESHIFTER_DIR}/tests/core/lines_tests.cpp
    ${SHAPESHIFTER_DIR}/tests/core/mesh_job_tests.cpp
    ${SHAPESHIFTER_DIR}/tests/core/navigation_tests.cpp
    ${SHAPESHIFTER_DIR}/tests/core/picking_tests.cpp
    ${SHAPESHIFTER_DIR}/tests/core/renderer_tests.cpp
    ${SHAPESHIFTER_DIR}/tests/core/scene_tests.cpp
    ${SHAPESHIFTER_DIR}/tests/core/sdf_scene_tests.cpp
    ${SHAPESHIFTER_DIR}/tests/core/sdf_tests.cpp
    ${SHAPESHIFTER_DIR}/tests/core/test_main.cpp
)
# Keep this list in sync with `ls editors/shapeshifter/tests/core/*.cpp`. It is
# spelled out rather than globbed to match the rest of this file, which means a
# new test file is silently not built until it is added here -- sdf_scene_tests
# was dropped exactly that way when the list was first transcribed.
target_include_directories(shapeshifter_core_tests PRIVATE
    ${SHAPESHIFTER_DIR}/tests/vendor)
target_compile_options(shapeshifter_core_tests PRIVATE
    -iquote${SHAPESHIFTER_DIR}/core/src)
target_link_libraries(shapeshifter_core_tests PRIVATE shapeshifter_core)
if(APPLE)
    target_link_options(shapeshifter_core_tests PRIVATE "-Wl,-ld_new")
endif()
add_test(NAME shapeshifter_core_tests COMMAND shapeshifter_core_tests
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

