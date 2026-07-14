#pragma once

// Small shared helpers for the procedural geometry generators (Task S2.C):
// primitive_mesh_builders.cpp, extrusion_mesh_builder.cpp,
// building_parts_builder.cpp. Not part of sampo's port — new for badlands's
// game geometry (the reference lived in Rust, src/scene/mesh.rs's
// MeshData::push_vertex).

#include <glm/glm.hpp>
#include <vector>

#include "engine/rendering/components/mesh_components.hpp"

namespace badlands {

// Appends one kTexturedMesh vertex (pos3+uv2+normal3+tangent3 = 11 floats,
// see textured_mesh_builders.hpp's kTexturedMeshFloatsPerVertex) to `out`.
void PushVertex(std::vector<float>& out, const glm::vec3& pos, const glm::vec2& uv,
                const glm::vec3& normal, const glm::vec3& tangent);

// Appends every vertex of `src` into `dst`, transforming positions by
// `transform` and normals/tangents by its linear part (tangents: the 3x3
// directly, since a tangent is a surface-following vector; normals: the
// inverse-transpose, so they stay perpendicular under non-uniform scale). UVs
// pass through unchanged. Used to place locally-generated primitives (walls,
// roofs, corner towers) within a building's local frame, and to merge
// multiple primitives (e.g. a tower's cylinder + cone cap) into one
// BuildingPart mesh.
void AppendTransformedMesh(StaticTexturedMeshComponent& dst,
                           const StaticTexturedMeshComponent& src,
                           const glm::mat4& transform);

}  // namespace badlands
