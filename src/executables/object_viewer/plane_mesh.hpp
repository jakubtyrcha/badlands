#pragma once

// The debug plane: a tessellated quad on y = 0, and the first geometry the
// visibility buffer ever had.
//
// 128 TRIANGLES, NOT 2. A two-triangle quad makes the triangle-ID preview a
// two-colour image and the barycentric preview two gradients -- neither can
// tell a correct resolve from a plausible one. 8x8 quads give the ID view a
// pattern with structure and put internal edges in front of the gradient code,
// which is where a resolve that interpolates per-quad instead of per-triangle
// shows up.
//
// Structural parameters are compile-time and only the extent is a constant:
// the mesh is a fixture, not a knob.

#include "executables/object_viewer/mesh_types.hpp"

namespace badlands::object_viewer {

// 8x8 quads spanning [-half_extent, +half_extent] on y = 0, UVs tiling
// `uv_tiles` times across the whole span. ONE instance, overriding nothing --
// the plane takes its roughness and metallic from the material pack, which is
// what its debug-view oracles assert against.
//
// Winding is COUNTER-CLOCKWISE seen from +y (above), matching FrontFace::Ccw
// with the plane's normal pointing up.
// The defaults, NAMED so an oracle can predict the parameterization instead of
// repeating the numbers. A second copy of 5.0/4.0 in a test is a second thing
// to update, and the failure mode is an assertion that quietly measures a
// different plane than the one being drawn.
inline constexpr float kPlaneHalfExtent = 5.0f;
inline constexpr float kPlaneUvTiles = 4.0f;

SceneMesh BuildPlaneMesh(float half_extent = kPlaneHalfExtent,
                         float uv_tiles = kPlaneUvTiles);

}  // namespace badlands::object_viewer
