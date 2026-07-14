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

add_executable(badlands
    src/main.cpp
    src/engine/rendering/gpu_context.cpp
)
target_link_libraries(badlands PRIVATE SDL3::SDL3 webgpu)
target_include_directories(badlands PRIVATE ${CMAKE_SOURCE_DIR}/src)

if(APPLE)
    # Use the new linker on Apple platforms (crib from ../sampo/CMakeLists.txt);
    # avoids issues linking the large monolithic Dawn dylib.
    target_link_options(badlands PRIVATE "-Wl,-ld_new")
endif()
