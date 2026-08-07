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

// Tessellation, also structural: subdivisions along each octahedron edge, so
// the sphere is 8 * kSphereSubdivisions^2 triangles.
//
// A SUBDIVIDED OCTAHEDRON, NOT A UV SPHERE, and the difference is visible.
// A UV sphere's rings converge at the poles: the last ring collapses into a fan
// of degenerate slivers, its UVs pinch to a point, and the tangent frame is
// undefined where the ring has zero circumference. That shows up as a smeared,
// aliased artifact on the tips of every sphere -- worst on exactly the mirror
// and low-roughness cells where the reflection is sharpest.
//
// Subdividing an octahedron has no poles at all. Every triangle is within a
// factor of ~1.5 of every other in area, there is no vertex with unbounded
// valence, and the UVs come from the OCTAHEDRAL MAP, which is what the
// tessellation already is -- so the parameterization and the geometry agree by
// construction rather than by a projection that has to pinch somewhere.
inline constexpr uint32_t kSphereSubdivisions = 24;

// Radius and spacing are size, so they are runtime -- but they are also the
// bounds the camera frames, so they are returned rather than assumed.
struct SphereGridBounds {
  glm::vec3 center{0.0f};
  float radius = 1.0f;         // of the whole grid, for framing
  float sphere_radius = 1.0f;  // of one sphere, for the near clamp
};

// The octahedral map, and its inverse of sorts. A direction to uv in [0,1]^2,
// with the LOWER hemisphere folded outward and mirrored across the diagonals --
// the standard full-sphere octahedral parameterization.
//
// Distinct from HemiOctEncode in src/game/visual/octahedral.hpp, which covers
// only y >= 0 for impostor atlases. This is the full sphere, and object_viewer
// does not link that layer anyway.
// AMBIGUOUS ON THE FOUR LOWER SEAMS, deliberately: this is the form a shader
// has, recovering the fold branch from the direction. Mesh generation must use
// OctEncodeOnFace instead.
glm::vec2 OctEncode(glm::vec3 dir);

// The same map with the fold branch taken from the FACE rather than from the
// point. The four edges running from the -Y apex to the equator lie exactly in
// n.x == 0 or n.z == 0, where the map is two-valued -- and the tessellation
// puts a whole lattice row on each of them, so this is not a measure-zero
// concern. `face_sx`/`face_sz` are the signs of the octahedron face's X and Z
// corners.
glm::vec2 OctEncodeOnFace(glm::vec3 on_face, float face_sx, float face_sz);

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
