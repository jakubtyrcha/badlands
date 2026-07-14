#pragma once

// Parameterized building-block primitives for game geometry (Task S2.C):
// cube/cylinder/cone/gable-roof/capsule, all emitting the kTexturedMesh
// vertex format (pos3+uv2+normal3+tangent3, see textured_mesh_builders.hpp)
// with real per-vertex tangents. Game-agnostic — dimensions and shape only,
// no material/game types.
//
// Ported (shape + winding conventions) from the reference Rust generators
// (src/scene/mesh.rs @ 8ee93cc: build_unit_cube/build_cylinder/build_cone/
// build_gable_roof/build_unit_capsule), which scaled a fixed "unit" shape via
// an external model-matrix at render time and had no tangents. Here each
// generator takes its real dimensions directly and computes real tangents
// (dPos/du along the parameterization), since badlands's Stage 2 has no
// per-draw model-matrix scaling of a shared unit mesh — see
// building_parts_builder.cpp for how BuildBuildingParts reproduces the
// reference's per-building scale factors (roof height, tower radius, ...) by
// calling these generators with pre-scaled dimensions instead.

#include <glm/glm.hpp>

#include "engine/rendering/geometry/textured_mesh_builders.hpp"

namespace badlands {

// Box centered at the origin, spanning [-half_extents, half_extents] per
// axis. Per-face normals/tangents: 24 unique face-corner (position, normal,
// tangent, uv) combinations — 4 per face x 6 faces — emitted as a 36-vertex
// non-indexed triangle list (2 triangles/face), matching the existing
// kTexturedMesh convention of storing an expanded triangle list with no
// separate index buffer (e.g. GenerateQuadTexturedMesh's un-tessellated
// quad: 4 unique corners, 6 emitted vertices).
TexturedMeshResult GenerateCube(glm::vec3 half_extents);

// Cylinder: side + top + bottom caps (the reference build_cylinder had only a
// top cap; a bottom cap is added here so the mesh is closed). Base at y=0,
// top at y=height. Side UVs are cylindrical (u=angle/tau, v=y/height) with
// tangent along the circumference (dPos/du); cap UVs are planar
// (x,z)/(2*radius)+0.5, for which dPos/du is exactly +X — the caps' tangent.
TexturedMeshResult GenerateCylinder(float radius, float height, int segments);

// Cone: side + base cap. Base circle at y=0 (radius `radius`), apex at
// y=height. Side normals/tangents are per-triangle (faceted) like the
// reference; the base cap matches GenerateCylinder's bottom cap.
TexturedMeshResult GenerateCone(float radius, float height, int segments);

// Gable roof: rectangular base centered at the origin (spanning size.x by
// size.z) at y=0, ridge along local X at y=size.y. Direct port of the
// reference build_gable_roof, pre-scaled by `size` (the reference scaled a
// unit shape externally; here the real dimensions are baked in directly, as
// with the other generators in this file).
TexturedMeshResult GenerateGableRoof(glm::vec3 size);

// Capsule (character shape): a cylinder of `cylinder_height` capped by two
// hemispheres of `radius`. Base at y=0, top at y = 2*radius + cylinder_height
// (so scaling isn't needed to reach a given total height, unlike the
// reference's fixed unit capsule + external scale).
TexturedMeshResult GenerateCapsule(float radius, float cylinder_height, int segments);

}  // namespace badlands
