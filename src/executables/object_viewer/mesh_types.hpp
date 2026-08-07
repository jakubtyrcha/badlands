#pragma once

// The vertex and per-instance types the visibility buffer and its resolve share.
//
// Split out of plane_mesh.hpp once the sphere grid arrived: two meshes now use
// them, and "PlaneVertex" named the first caller rather than the thing.
//
// Every struct here is MIRRORED IN shaders/slang/object_viewer/vis_common.slang
// and the two must agree byte for byte. MSL pads a float3 to 16 bytes, so the
// obvious packed layout is one size on the CPU and another on the GPU, and
// every element after the first is read from the wrong offset -- which shears
// geometry rather than failing.

#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

namespace badlands::object_viewer {

// One vertex, THREE float4s (48 bytes).
struct MeshVertex {
  glm::vec4 pos_nx;   // xyz position, w normal.x
  glm::vec4 nyz_uv;   // xy normal.yz, zw uv
  glm::vec4 tangent;  // xyz tangent, w bitangent sign
};
static_assert(sizeof(MeshVertex) == 48);

// Which fields of DrawInfo::material the resolve should believe.
//
// AN EXPLICIT MASK, NOT A SENTINEL. "negative roughness means use the map"
// would make one value mean two things (RHI rule 5, and the same reasoning
// applies above it), and it would quietly reinterpret a legitimately-zero
// roughness. The plane scene clears the mask, so nothing is accepted-and-
// ignored either (rule 4).
inline constexpr uint32_t kOverrideRoughness = 1u << 0;
inline constexpr uint32_t kOverrideMetallic = 1u << 1;

// One INSTANCE, indexed by the visibility buffer's draw slot.
//
// `first_index`/`first_vertex` are real: the resolve fetches a triangle's three
// indices through them. They are 0 for a single-mesh scene, but that is a value
// rather than an assumption.
struct DrawInfo {
  uint32_t first_index = 0;
  uint32_t first_vertex = 0;
  uint32_t material_slot = 0;
  uint32_t override_mask = 0;
  glm::vec4 offset{0.0f};    // xyz world offset for this instance
  glm::vec4 material{0.0f};  // x roughness, y metallic
};
static_assert(sizeof(DrawInfo) == 48);

// A mesh plus the instances that draw it.
//
// ONE mesh and N instances rather than N meshes: the sphere grid is the same
// sphere fourteen times, differing only in where it sits and what material
// constants it carries. That is exactly what an instanced draw expresses, and
// it keeps the draw slot -- which the visibility buffer already packs -- as the
// instance id, with no per-draw uniform plumbing at all.
struct SceneMesh {
  std::vector<MeshVertex> vertices;
  std::vector<uint32_t> indices;
  std::vector<DrawInfo> draws;

  uint32_t TriangleCount() const { return uint32_t(indices.size() / 3); }
  uint32_t InstanceCount() const { return uint32_t(draws.size()); }
};

// The visibility buffer packs (draw_slot << 24) | (primitive + 1), so the two
// fields have hard ceilings: more triangles overflows the primitive field into
// the draw field, and more instances overflows the draw field entirely.
//
// The SHADER masks, which contains the damage; these are what REPORT it. A mask
// alone would make the corruption silent, and the CPU is the only side that
// knows either count.
inline constexpr uint32_t kMaxPrimitivesPerDraw = (1u << 24) - 1u;
inline constexpr uint32_t kMaxDrawSlots = 1u << 8;

// Returns false, after logging, if `mesh` cannot be addressed by the packing.
bool ValidateSceneMesh(const SceneMesh& mesh);

}  // namespace badlands::object_viewer
