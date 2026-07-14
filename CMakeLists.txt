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

# Corrosion for Rust integration (crib from ../sampo/CMakeLists.txt lines ~199-208).
FetchContent_Declare(
    Corrosion
    GIT_REPOSITORY https://github.com/corrosion-rs/corrosion.git
    GIT_TAG        v0.5
)
FetchContent_MakeAvailable(Corrosion)

# wesl_ffi: .wesl -> WGSL shader compilation (src/crates/wesl), Rust staticlib.
# The crate's Cargo package is named "wesl_ffi", not "wesl" — it depends on
# the `wesl` 0.4 crate, and Corrosion invokes `cargo rustc --package <name>`,
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

add_executable(badlands
    src/main.cpp
    src/engine/rendering/gpu_context.cpp
)
target_link_libraries(badlands PRIVATE SDL3::SDL3 webgpu wesl_ffi assets nav)
target_include_directories(badlands PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/crates/wesl/include
    ${CMAKE_SOURCE_DIR}/src/crates/assets/include
    ${CMAKE_SOURCE_DIR}/src/crates/nav/include
)

if(APPLE)
    # Use the new linker on Apple platforms (crib from ../sampo/CMakeLists.txt);
    # avoids issues linking the large monolithic Dawn dylib.
    target_link_options(badlands PRIVATE "-Wl,-ld_new")
endif()
