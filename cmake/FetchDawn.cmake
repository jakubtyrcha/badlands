# Dawn (WebGPU) built from source, pinned to a resolved chromium/dawn commit.
#
# Ported from ../sampo/CMakeLists.txt's Dawn section (the WebGPU-distribution
# wrapper + resolved-SHA pin). Badlands Stage 1 targets macOS/Metal only, so
# the Emscripten/EMDAWNWEBGPU branch present in sampo is dropped here.
#
# Expects the includer to have already done `include(FetchContent)`.

set(WEBGPU_BACKEND "DAWN")
set(WEBGPU_BUILD_FROM_SOURCE ON CACHE BOOL "Build Dawn from source" FORCE)

# Pin Dawn to the resolved commit of chromium/7866 (not the moving branch
# tip). The WebGPU-distribution wrapper (FetchDawnSource.cmake) fetches
# `chromium/${DAWN_VERSION}` as a moving branch via a --depth=1
# DOWNLOAD_COMMAND, which silently drifts as the milestone branch advances.
# We instead:
#   1. Set DAWN_VERSION to 7866 so any version-derived wrapper logic is
#      correct.
#   2. Pre-declare the `dawn` FetchContent target with a DOWNLOAD_COMMAND
#      that fetches the exact resolved SHA. FetchContent is
#      first-declaration-wins, so this declaration beats the wrapper's own
#      `dawn` declaration inside FetchDawnSource.cmake (a no-op once `dawn`
#      is already populated).
# googlesource has allowAnySHA1InWant enabled, so a --depth=1 fetch of an
# arbitrary commit succeeds.
set(DAWN_VERSION "7866" CACHE STRING "Dawn milestone (chromium/<N>)" FORCE)
set(DAWN_PINNED_SHA "cae082e16781493ee0f709975b8ed2a6e72a20b3"
    CACHE STRING "Resolved commit of chromium/7866" FORCE)
set(DAWN_SOURCE_MIRROR "https://dawn.googlesource.com/dawn"
    CACHE STRING "Dawn source mirror" FORCE)
# No PATCH_COMMAND: the wrapper's bundled dawn.patch trims test-only
# submodules (dxc/dxheaders/protobuf/googletest/benchmark) from
# fetch_dawn_dependencies.py, but that file has been restructured at this pin
# (submodules renamed/moved) so the patch no longer applies. It was only a
# fetch-bandwidth optimization, never a correctness fix; we drop it and fetch
# the full dependency set instead.
FetchContent_Declare(
    dawn
    DOWNLOAD_COMMAND
        cd ${FETCHCONTENT_BASE_DIR}/dawn-src &&
        git init &&
        git fetch --depth=1 ${DAWN_SOURCE_MIRROR} ${DAWN_PINNED_SHA} &&
        git reset --hard FETCH_HEAD
)

# Dawn link mode (chromium/7866). The WebGPU-distribution wrapper hard-wires
# the `webgpu` INTERFACE target to the monolithic `webgpu_dawn` library, which
# only exists when DAWN_BUILD_MONOLITHIC_LIBRARY is SHARED/STATIC. Keep the
# monolithic build so we get the same artifact sampo does: a single shared
# `libwebgpu_dawn.dylib` linked at @rpath.
#
# The 7866 pin made DAWN_BUILD_MONOLITHIC_LIBRARY a tri-state STRING
# (SHARED/STATIC/OFF, default STATIC) and added a FATAL_ERROR when a
# SHARED/STATIC monolithic build is combined with BUILD_SHARED_LIBS=ON: the
# bundled monolithic library must statically absorb Dawn's internals rather
# than depend on per-component shared libs. Satisfy this by scoping
# BUILD_SHARED_LIBS=OFF to the Dawn subtree ONLY (restored immediately after)
# so Dawn's internals link static into the monolith without touching the
# project-wide default.
set(DAWN_BUILD_MONOLITHIC_LIBRARY "SHARED"
    CACHE STRING "Build Dawn as one shared library (libwebgpu_dawn)" FORCE)

FetchContent_Declare(
    webgpu
    GIT_REPOSITORY https://github.com/eliemichel/WebGPU-distribution.git
    GIT_TAG        17dcd42
)
set(_badlands_prev_build_shared_libs ${BUILD_SHARED_LIBS})
set(BUILD_SHARED_LIBS OFF)  # scoped: static Dawn internals -> monolithic shared lib
FetchContent_MakeAvailable(webgpu)
set(BUILD_SHARED_LIBS ${_badlands_prev_build_shared_libs})
unset(_badlands_prev_build_shared_libs)

# Exclude Dawn utility/test/glfw targets from the default build (not needed).
foreach(_dawn_exclude_target dawn_wgpu_utils dawn_test_utils dawn_system_utils dawn_glfw)
    if(TARGET ${_dawn_exclude_target})
        set_target_properties(${_dawn_exclude_target} PROPERTIES EXCLUDE_FROM_ALL TRUE)
    endif()
endforeach()
