#pragma once

// The roughness x metallic sphere chart: what IBL is judged on.
//
// A flat plane is nearly useless for this. Its reflection vector barely varies
// across the surface, so it samples one narrow cone of the environment and a
// broken prefilter chain looks entirely plausible. A sphere sweeps a full
// hemisphere; a GRID of them sweeps the material parameters as well, which is
// the only arrangement where a wrong mip mapping or a wrong F0 lerp is VISIBLE
// rather than merely present.
//
// ONE sphere, N instances. Every sphere is the same mesh at a different offset
// with different material constants, which is exactly what DrawInfo carries --
// so the grid costs one draw and no per-draw uniform plumbing.

#include "executables/object_viewer/mesh_types.hpp"

namespace badlands::object_viewer {

// Structural, so compile-time. The chart's shape is a fixture, not a knob:
// 7 roughness steps across, one dielectric row and one metal row down.
inline constexpr uint32_t kRoughnessSteps = 7;
inline constexpr uint32_t kMetallicSteps = 2;
inline constexpr uint32_t kSphereCount = kRoughnessSteps * kMetallicSteps;

// Tessellation, also structural. 32x16 is smooth enough that a highlight reads
// as a highlight rather than as facets, which matters because faceting looks
// exactly like a too-sharp prefilter.
inline constexpr uint32_t kSphereSegments = 32;
inline constexpr uint32_t kSphereRings = 16;

// Radius and spacing are size, so they are runtime -- but they are also the
// bounds the camera frames, so they are returned rather than assumed.
struct SphereGridBounds {
  glm::vec3 center{0.0f};
  float radius = 1.0f;         // of the whole grid, for framing
  float sphere_radius = 1.0f;  // of one sphere, for the near clamp
};

// Roughness sweeps 0..1 left to right; the first row is dielectric and the
// second is metal. Both are OVERRIDDEN per instance, so the pack's ARM map does
// not decide them -- the whole point is a controlled sweep.
SceneMesh BuildSphereGrid(float sphere_radius = 1.0f, float spacing = 2.6f);

// The bounds the same call produces, without building the mesh.
SphereGridBounds SphereGridExtent(float sphere_radius = 1.0f,
                                  float spacing = 2.6f);

// The roughness, metallic and world position a given sphere carries. Exposed so
// a headless oracle predicts what it should see rather than re-deriving the
// layout -- an oracle with its own copy of the layout tests itself.
float SphereRoughness(uint32_t column);
float SphereMetallic(uint32_t row);
glm::vec3 SphereGridCenter(uint32_t column, uint32_t row,
                           float spacing = 2.6f);

}  // namespace badlands::object_viewer
