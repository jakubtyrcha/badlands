cmake_minimum_required(VERSION 3.24)
project(badlands LANGUAGES C CXX OBJC OBJCXX)   # OBJC/OBJCXX: Dawn Metal backend on Apple
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include(FetchContent)

# SDL3 (crib from ../sampo/CMakeLists.txt lines ~28-35)
FetchContent_Declare(SDL3 GIT_REPOSITORY https://github.com/libsdl-org/SDL.git GIT_TAG release-3.2.0)
FetchContent_MakeAvailable(SDL3)

# Dawn (WebGPU), built from source and pinned — see cmake/FetchDawn.cmake.
include(cmake/FetchDawn.cmake)

# Dear ImGui (docking branch). Task S2.A1 only compiles it into
# badlands_engine to de-risk the fetch/build; A2 wires up ImGui calls.
FetchContent_Declare(imgui GIT_REPOSITORY https://github.com/ocornut/imgui.git GIT_TAG docking)
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
# PROFILE release — ALWAYS, even in a Debug C++ build (same reasoning as
# noiser-bundle below). This crate is the image decoder, and terrain startup
# decodes 18 material textures (1024^2 each) through it. Built with cargo's dev
# profile (opt-level 0, what CMAKE_BUILD_TYPE=Debug otherwise selects) a single
# 1024^2 JPEG/PNG decode costs ~130-200 ms instead of ~20 ms, so the six-pack
# terrain load takes ~2.3 s of a Debug startup. Nothing here is C++-debuggable
# anyway — it's a prebuilt staticlib behind a C ABI — so there is no
# debuggability to trade away.
corrosion_import_crate(MANIFEST_PATH ${CMAKE_SOURCE_DIR}/src/crates/assets/Cargo.toml
                       PROFILE release)
corrosion_set_env_vars(assets "RUSTFLAGS=-Cpanic=unwind")

# nav: 2D visibility-graph path service (src/crates/nav), Rust staticlib,
# behind the GamePathfinder C ABI. Game-layer pathfinding — unused in Stage 1
# (link-only here; wired up by the C++ game layer in Stage 2). No dependency
# of this crate shares its own name, so no Corrosion package-ambiguity
# workaround is needed (as with assets, unlike wesl_ffi).
corrosion_import_crate(MANIFEST_PATH ${CMAKE_SOURCE_DIR}/src/crates/nav/Cargo.toml)
corrosion_set_env_vars(nav "RUSTFLAGS=-Cpanic=unwind")

# noiser-bundle: aggregates noiser-compiler + noiser-vm (third_party/noiser)
# into ONE Rust staticlib (crates/noiser-bundle) so the noiser C++ VM wrapper
# (third_party/noiser/noiser-vm/src, compiled into badlands_game_lib below)
# links libstd exactly once. It is a member of the root workspace (which also
# contains the legacy Rust app); Corrosion invokes `cargo build -p
# noiser-bundle`, which only resolves noiser-bundle + its two path deps, not
# the legacy app's wgpu/winit/naga deps. The package name has a dash, and
# Corrosion's imported CMake target always replaces dashes with underscores
# (CorrosionGenerator.cmake), so the target is `noiser_bundle`, not
# `noiser-bundle`.
#
# PROFILE release — ALWAYS, even in a Debug C++ build. The noiser VM is an
# interpreter sitting on mapgen's hot path: the field script evaluates ~16
# Perlin octaves per sample, so a 2000x2000 map is ~64M octave evaluations
# through it. Built with cargo's dev profile (opt-level 0, which is what
# CMAKE_BUILD_TYPE=Debug otherwise selects) that alone costs ~39s of a ~40s
# generation. Nothing here is C++-debuggable anyway — it's a prebuilt staticlib
# behind a C ABI — so there is no debuggability to trade away.
corrosion_import_crate(MANIFEST_PATH ${CMAKE_SOURCE_DIR}/crates/noiser-bundle/Cargo.toml
                       PROFILE release)
corrosion_set_env_vars(noiser_bundle "RUSTFLAGS=-Cpanic=unwind")

# badlands_engine: the ported-from-sampo rendering engine (Stage 1) plus the
# game-agnostic app framework (Stage 2 S2.A1: SdlViewerApp/AppView). No game
# types/logic live here — see src/engine/app/app_view.hpp.
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
    src/engine/rendering/geometry/aabb.cpp
    src/engine/rendering/geometry/cube_sphere.cpp
    src/engine/rendering/geometry/textured_mesh_builders.cpp
    src/engine/rendering/geometry/mesh_builder_utils.cpp
    src/engine/rendering/geometry/primitive_mesh_builders.cpp
    src/engine/rendering/geometry/extrusion_mesh_builder.cpp
    src/engine/rendering/geometry/building_parts_builder.cpp
    src/engine/rendering/components/mesh_components.cpp
    src/engine/scene/scene_graph.cpp
    src/engine/app/sdl_input_util.cpp
    src/engine/core/ray.cpp
    src/engine/rendering/frustum.cpp
    src/engine/rendering/debug_line_buffer.cpp
    src/engine/rendering/debug_line_expand.cpp
    src/engine/rendering/passes/render_debug_lines.cpp
    src/engine/rendering/passes/render_textured_mesh.cpp
    src/engine/rendering/passes/render_skybox.cpp
    src/engine/rendering/passes/render_gbuffer_debug.cpp
    src/engine/rendering/gbuffer.cpp
    src/engine/rendering/shadow_map.cpp
    src/engine/rendering/gpu_timer.cpp
    src/engine/rendering/volumetric_fog.cpp
    src/engine/rendering/fog_simulation.cpp
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
    src/engine/ui/imgui_impl_wgpu_custom.cpp
    src/engine/ui/editor_ui.cpp
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
)
target_link_libraries(badlands_engine PUBLIC SDL3::SDL3 webgpu wesl_ffi assets nav nlohmann_json::nlohmann_json)
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
    ${CMAKE_SOURCE_DIR}/src/crates/nav/include
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    ${taskflow_SOURCE_DIR}
)

# badlands_game_lib: the C++ game simulation (EnTT + glm + spdlog + the
# noiser-scripted brains), pure C++ with no engine/SDL/Dawn deps — geometry
# that needs engine types comes in a later task (S2.C). Includes the
# sampo::noiser C++ VM wrapper (pure delegation over the noiser Rust VM's C
# ABI); badlands uses the VM only, so noiser_jit_stubs.cpp stubs the JIT
# (Cranelift) C ABI.
#
# Named badlands_game_lib, not badlands_game: the "game" app executable below
# is already named badlands_game (from S2.A1/A3), and CMake target names must
# be globally unique.
add_library(badlands_game_lib STATIC
    game/src/game.cpp
    game/src/brain.cpp
    game/src/placement.cpp
    game/src/movement.cpp
    game/src/heroes.cpp
    game/src/noiser_jit_stubs.cpp
    third_party/noiser/noiser-vm/src/noiser.cpp
    third_party/noiser/noiser-vm/src/noiser_crash.cpp
    # Task S2.C: procedural-geometry game data (no engine/SDL/Dawn deps — see
    # the file-level comments in src/game/building_catalog.{h,cpp} and
    # src/game/geometry/ploppable_rings.{h,cpp}).
    src/game/building_catalog.cpp
    src/game/geometry/ploppable_rings.cpp
    # Task S2.D: MaterialId -> PBR pack mapping (src/game/material_pack.h).
    src/game/material_pack.cpp
    # Task S2.G: walled AI-sandbox arena (src/game/arena.h), reusing
    # placement.h's centered-grid convention (kGridHalf/in_bounds_tile).
    src/game/arena.cpp
)
target_compile_features(badlands_game_lib PUBLIC cxx_std_23)
# noiser_common.h intentionally ships [[deprecated]] shims.
target_compile_options(badlands_game_lib PRIVATE -Wno-deprecated-declarations)
target_include_directories(badlands_game_lib PUBLIC
    ${CMAKE_SOURCE_DIR}/game/include
    ${CMAKE_SOURCE_DIR}/third_party/noiser/noiser-vm/src
    ${CMAKE_SOURCE_DIR}/third_party/noiser
    ${CMAKE_SOURCE_DIR}/third_party/glm
    ${CMAKE_SOURCE_DIR}/third_party/spdlog/include
    ${CMAKE_SOURCE_DIR}/third_party/entt/single_include
    # For "core/roof_shape.hpp" + "game/material_id.hpp" etc (Task S2.C) —
    # header search path only, no engine/SDL/Dawn dependency added.
    ${CMAKE_SOURCE_DIR}/src
)
# Task S2.G: arena.cpp includes game/src/placement.h directly (kGridHalf/
# in_bounds_tile grid reuse) -- PRIVATE since it's an implementation detail
# of badlands_game_lib, not part of its public header surface.
target_include_directories(badlands_game_lib PRIVATE
    ${CMAKE_SOURCE_DIR}/game/src
)
target_link_libraries(badlands_game_lib PUBLIC noiser_bundle)

# Three thin executables, each with its own main, all linking badlands_engine
# and badlands_game_lib. Stage 2 grows each one's own real AppView --
# badlands_viewer got its real AppView (ModelViewerView) in Task S2.E,
# badlands_game got its (GameView) in Task S2.F, badlands_ai_sandbox got its
# (AiSandboxView) in Task S2.G.
# building_scene.cpp (S2.F): ModelViewerView now reuses AddBuildingToScene for
# its building prefabs (S2-reviewfix C6), so the viewer executable compiles it
# too (its own copy; also compiled into badlands_game below).
set(badlands_app_sources_viewer
    src/viewer/main_viewer.cpp
    src/viewer/model_viewer_view.cpp
    src/game/scene/building_scene.cpp)
# Task S2.F: GameView (sim buildings via deferred + GameCameraController) +
# its building_scene helper. Compiled directly into the executable (like
# model_viewer_view.cpp above), not into badlands_engine/badlands_game_lib --
# it needs both libraries' headers (SceneGraph/MaterialLibrary +
# GameBuildingKind/building_visual), which the final executable link already
# provides transitively.
set(badlands_app_sources_game
    src/game/main_game.cpp
    src/game/scene/building_scene.cpp
    src/game/views/game_view.cpp)
# Task S2.G: AiSandboxView (walled arena via game/arena.h + 2 static
# capsules, deferred + GameCameraController). Compiled directly into the
# executable, same rationale as badlands_app_sources_game above.
set(badlands_app_sources_ai_sandbox
    src/ai_sandbox/main_ai_sandbox.cpp
    src/ai_sandbox/ai_sandbox_view.cpp)

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

# badlands_mapgen_lib: the map generation pipeline (config/fields/voronoi/biomes/
# height/sections + PNG/JSON outputs) as a static lib. Links badlands_engine for
# CpuImage (PNG write via the `assets` crate) and badlands_game_lib for the
# sampo::noiser C++ VM wrapper (parallel noise-field evaluation).
# -Wno-deprecated-declarations matches badlands_game_lib (the noiser headers ship
# [[deprecated]] shims).
add_library(badlands_mapgen_lib STATIC
    src/mapgen/authored_map.cpp
    src/mapgen/config.cpp
    src/mapgen/fog_generator.cpp
    src/mapgen/fields.cpp
    src/mapgen/hillshade.cpp
    src/mapgen/outputs.cpp
    src/mapgen/pipeline.cpp
    src/mapgen/script_eval.cpp
    src/mapgen/voronoi.cpp
    src/mapgen/biome_assign.cpp
    src/mapgen/heightmap.cpp
    src/mapgen/sections.cpp)
target_link_libraries(badlands_mapgen_lib PUBLIC badlands_engine badlands_game_lib)
target_compile_options(badlands_mapgen_lib PRIVATE -Wno-deprecated-declarations)

# badlands_patchgen: renders ONE noiser terrain script over a world patch to a
# grayscale + hillshaded PNG pair. The authoring loop for terrain scripts — pure CPU, no
# window. Scripts are functions of world meters, so a patch is a literal crop of the
# world (see scripts/mapgen/biomes/README.md).
add_executable(badlands_patchgen src/mapgen/main_patchgen.cpp)
target_link_libraries(badlands_patchgen PRIVATE badlands_mapgen_lib)
target_compile_options(badlands_patchgen PRIVATE -Wno-deprecated-declarations)
if(APPLE)
    target_link_options(badlands_patchgen PRIVATE "-Wl,-ld_new")
endif()

# badlands_mapview: THE map tool. `--preview-image-only` runs the generator and
# dumps the debug rasters + section graph (pure CPU, no window — this replaces
# the old standalone badlands_mapgen binary); otherwise it renders the generated
# map as indexed kTerrainBlend chunks with the biome-blend material, with a
# mouse-hover block/section grid.
add_executable(badlands_mapview
    src/mapview/main_mapview.cpp
    src/mapview/map_view_view.cpp
    src/mapview/biome_manifest.cpp
    src/game/geometry/terrain_mesh.cpp)
target_link_libraries(badlands_mapview PRIVATE badlands_engine badlands_game_lib badlands_mapgen_lib)
target_compile_options(badlands_mapview PRIVATE -Wno-deprecated-declarations)
if(APPLE)
    target_link_options(badlands_mapview PRIVATE "-Wl,-ld_new")
endif()

# badlands_mapgen_tests: Catch2 unit tests for the deterministic mapgen analysis
# (block reduction, section flood-fill + graph, biome classification, height
# composition). Pure CPU — compiles just those TUs (no noiser/Dawn/CpuImage),
# so it links nothing but Catch2. Runs on tiny synthetic patches.
add_executable(badlands_mapgen_tests
    src/mapgen/mapgen_tests.cpp
    src/mapgen/sections.cpp
    src/mapgen/biome_assign.cpp
    src/mapgen/heightmap.cpp
    third_party/catch2/extras/catch_amalgamated.cpp
)
target_include_directories(badlands_mapgen_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/glm
    ${CMAKE_SOURCE_DIR}/third_party/catch2/extras
)
add_test(NAME badlands_mapgen_tests COMMAND badlands_mapgen_tests WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# badlands_patch_eval_tests: the patch evaluator's world mapping (world =
# origin + (i+0.5)*extent/res), pinned with analytic ramp scripts. Needs the
# noiser VM, so it links badlands_game_lib (which PUBLIC-links noiser_bundle and
# carries no engine/SDL/Dawn dependency) -- NOT badlands_mapgen_lib, which would
# drag in the renderer for nothing.
add_executable(badlands_patch_eval_tests
    src/mapgen/patch_eval_tests.cpp
    src/mapgen/script_eval.cpp
    third_party/catch2/extras/catch_amalgamated.cpp
)
target_link_libraries(badlands_patch_eval_tests PRIVATE badlands_game_lib)
target_include_directories(badlands_patch_eval_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/catch2/extras
)
target_compile_options(badlands_patch_eval_tests PRIVATE -Wno-deprecated-declarations)
if(APPLE)
    target_link_options(badlands_patch_eval_tests PRIVATE "-Wl,-ld_new")
endif()
add_test(NAME badlands_patch_eval_tests COMMAND badlands_patch_eval_tests WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# badlands_authored_map_tests: the authored-map loader -- map_meta.json validation
# (dims, block-multiple, height range, meters_per_sample, and the biome-order
# label->enum contract) plus the two image decodes. Compiles authored_map.cpp
# directly and links the `assets` crate for the PNG decode/encode C ABI + nlohmann
# for the manifest parse; deliberately no engine/SDL/Dawn.
add_executable(badlands_authored_map_tests
    src/mapgen/authored_map_tests.cpp
    src/mapgen/authored_map.cpp
    third_party/catch2/extras/catch_amalgamated.cpp
)
target_link_libraries(badlands_authored_map_tests PRIVATE assets nlohmann_json::nlohmann_json)
target_include_directories(badlands_authored_map_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/crates/assets/include
    ${CMAKE_SOURCE_DIR}/third_party/glm
    ${CMAKE_SOURCE_DIR}/third_party/catch2/extras
)
if(APPLE)
    target_link_options(badlands_authored_map_tests PRIVATE "-Wl,-ld_new")
endif()
add_test(NAME badlands_authored_map_tests COMMAND badlands_authored_map_tests WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# badlands_fog_generator_tests: the biome -> fog-emitter generator. Pure CPU
# (fog_sim.hpp is header-only glm; biomes/field2d are headers), so it compiles
# fog_generator.cpp directly with no engine link.
add_executable(badlands_fog_generator_tests
    src/mapgen/fog_generator_tests.cpp
    src/mapgen/fog_generator.cpp
    third_party/catch2/extras/catch_amalgamated.cpp
)
target_include_directories(badlands_fog_generator_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/glm
    ${CMAKE_SOURCE_DIR}/third_party/catch2/extras
)
add_test(NAME badlands_fog_generator_tests COMMAND badlands_fog_generator_tests WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# badlands_game_tests: the existing Catch2 game-sim suite (game/tests/),
# re-enabled in CMake (previously only run via `cargo test --test cpp_tests`,
# see tests/cpp_tests.rs on the legacy Rust reference branch).
enable_testing()
add_executable(badlands_game_tests
    game/tests/sim_tests.cpp
    game/tests/noiser_smoke_tests.cpp
    game/tests/duel_test.cpp
    game/tests/placement_tests.cpp
    game/tests/movement_tests.cpp
    game/tests/heroes_tests.cpp
    third_party/catch2/extras/catch_amalgamated.cpp
)
target_include_directories(badlands_game_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/catch2/extras
    ${CMAKE_SOURCE_DIR}/game/src
)
target_compile_options(badlands_game_tests PRIVATE -Wno-deprecated-declarations)
target_link_libraries(badlands_game_tests PRIVATE badlands_game_lib)
add_test(NAME badlands_game_tests COMMAND badlands_game_tests WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# badlands_geometry_tests: Catch2 suite for Task S2.C's procedural geometry
# (primitives/rocks/building parts, src/engine/rendering/geometry) + the
# building visual catalog / ploppable rings (src/game). Pure CPU — no GPU
# init needed — despite linking badlands_engine (StaticTexturedMeshComponent
# carries wgpu::TextureView/Sampler members that are never touched here).
add_executable(badlands_geometry_tests
    game/tests/geometry_tests.cpp
    third_party/catch2/extras/catch_amalgamated.cpp
)
target_include_directories(badlands_geometry_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/catch2/extras
)
target_link_libraries(badlands_geometry_tests PRIVATE badlands_engine badlands_game_lib)
if(APPLE)
    target_link_options(badlands_geometry_tests PRIVATE "-Wl,-ld_new")
endif()
add_test(NAME badlands_geometry_tests COMMAND badlands_geometry_tests WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

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

# badlands_terrain_mesh_tests: pure-CPU tests for src/game/geometry/terrain_mesh
# — the tessellated-block mesh builder plus the SampleHeight/RaycastTerrain
# queries behind mouse picking. No GPU: only glm + the header-only mapgen
# Field2D/Biome types and the header-only Ray struct.
add_executable(badlands_terrain_mesh_tests
    src/game/geometry/terrain_mesh.cpp
    game/tests/terrain_mesh_tests.cpp
    game/tests/terrain_raycast_tests.cpp
    third_party/catch2/extras/catch_amalgamated.cpp
)
target_include_directories(badlands_terrain_mesh_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/glm
    ${CMAKE_SOURCE_DIR}/third_party/catch2/extras
)
add_test(NAME badlands_terrain_mesh_tests COMMAND badlands_terrain_mesh_tests WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

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
