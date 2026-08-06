# Standalone probe harness for the RHI/Slang exploration.
#
# DELIBERATELY NOT part of the root build: the main CMakeLists must not gain a
# Slang dependency while we are only probing (Dawn builds are heavy enough).
# Configure and build this on its own:
#
#   cmake -S tools/slang_probe -B tools/slang_probe/build -DCMAKE_BUILD_TYPE=Release
#   cmake --build tools/slang_probe/build
#   ./tools/slang_probe/build/slang_probe --all
#
# Run from the repo root -- shader paths resolve relative to cwd.
cmake_minimum_required(VERSION 3.20)
project(slang_probe CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(SLANG_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/toolchains/slang"
    CACHE PATH "Prebuilt Slang SDK root")

if(NOT EXISTS "${SLANG_ROOT}/include/slang.h")
    message(FATAL_ERROR
        "Slang SDK not found at ${SLANG_ROOT}. See tools/slang_probe/README.md.")
endif()

add_executable(slang_probe probe.cpp)
target_include_directories(slang_probe PRIVATE "${SLANG_ROOT}/include")
target_link_libraries(slang_probe PRIVATE "${SLANG_ROOT}/lib/libslang.dylib")

# The Slang dylibs resolve each other via @rpath at runtime.
set_target_properties(slang_probe PROPERTIES
    BUILD_RPATH "${SLANG_ROOT}/lib"
    INSTALL_RPATH "${SLANG_ROOT}/lib")

# Optional WESL baseline for probe A. Enabled only when the main build has
# already produced libwesl_ffi, so the probe never forces a Rust build.
find_library(WESL_FFI_LIB
    NAMES wesl_ffi
    PATHS "${CMAKE_CURRENT_SOURCE_DIR}/../../build"
          "${CMAKE_CURRENT_SOURCE_DIR}/../../src/crates/wesl/target/release"
          "${CMAKE_CURRENT_SOURCE_DIR}/../../src/crates/wesl/target/debug"
    PATH_SUFFIXES "" "release" "debug"
    NO_DEFAULT_PATH)

if(WESL_FFI_LIB)
    message(STATUS "slang_probe: WESL baseline enabled (${WESL_FFI_LIB})")
    target_compile_definitions(slang_probe PRIVATE PROBE_HAVE_WESL=1)
    target_include_directories(slang_probe PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/../../src/crates/wesl/include")
    target_link_libraries(slang_probe PRIVATE "${WESL_FFI_LIB}")
else()
    message(STATUS "slang_probe: WESL baseline disabled (libwesl_ffi not built)")
endif()
