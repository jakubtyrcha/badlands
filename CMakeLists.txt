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
corrosion_import_crate(MANIFEST_PATH ${CMAKE_SOURCE_DIR}/src/crates/assets/Cargo.toml)
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
corrosion_import_crate(MANIFEST_PATH ${CMAKE_SOURCE_DIR}/crates/noiser-bundle/Cargo.toml)
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
    src/engine/rendering/passes/render_textured_mesh.cpp
    src/engine/rendering/passes/render_skybox.cpp
    src/engine/rendering/passes/render_gbuffer_debug.cpp
    src/engine/rendering/gbuffer.cpp
    src/engine/rendering/scene_renderer.cpp
    src/engine/rendering/texture_loader.cpp
    src/engine/rendering/material_library.cpp
    # Task S2.E: shared scene-building helper (TexturedMeshResult + a
    # MaterialLibrary DeferredMaterial -> a SceneGraph node), reused by
    # badlands_viewer/game/ai_sandbox's real AppViews.
    src/engine/rendering/scene_build.cpp
    src/engine/rendering/cubemap_builder.cpp
    src/engine/rendering/light_environment.cpp
    src/engine/rendering/brdf_lut.cpp
    src/engine/rendering/prefiltered_cubemap.cpp
    src/engine/app/sdl_viewer_app.cpp
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
