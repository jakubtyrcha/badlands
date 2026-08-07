#pragma once

// The debug plane: a tessellated quad on y = 0, and the first geometry the
// visibility buffer has ever had.
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

#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

namespace badlands::object_viewer {

// One vertex, THREE float4s (48 bytes), matching PlaneVertex in
// shaders/slang/object_viewer/visbuffer.slang.
//
// Packed this way because MSL pads a float3 to 16 bytes: the obvious
// {float3 pos; float3 normal; float4 tangent; float2 uv} is 40 bytes here and
// 64 on the GPU, and every vertex after the first would be read from the wrong
// offset -- sheared geometry rather than an error.
struct PlaneVertex {
  glm::vec4 pos_nx;    // xyz position, w normal.x
  glm::vec4 nyz_uv;    // xy normal.yz, zw uv
  glm::vec4 tangent;   // xyz tangent, w bitangent sign
};
static_assert(sizeof(PlaneVertex) == 48);

// Mirrors DrawInfo in the shader. ONE LIVE ENTRY for now, and not speculative:
// the resolve genuinely reads first_index to locate a triangle's three indices.
// With a single draw the value is 0, but the fetch is real code on the real path.
struct DrawInfo {
  uint32_t first_index = 0;
  uint32_t first_vertex = 0;
  uint32_t material_slot = 0;
  uint32_t pad = 0;
};
static_assert(sizeof(DrawInfo) == 16);

struct PlaneMesh {
  std::vector<PlaneVertex> vertices;
  std::vector<uint32_t> indices;

  uint32_t TriangleCount() const { return uint32_t(indices.size() / 3); }
};

// 8x8 quads spanning [-half_extent, +half_extent] on y = 0, UVs tiling
// `uv_tiles` times across the whole span.
//
// Winding is COUNTER-CLOCKWISE seen from +y (above), matching FrontFace::Ccw
// with the plane's normal pointing up.
PlaneMesh BuildPlaneMesh(float half_extent = 5.0f, float uv_tiles = 4.0f);

// The visibility buffer packs (draw_slot << 24) | (primitive + 1), so a mesh of
// more than this many triangles would overflow the primitive field into the
// draw field and make the resolve fetch an out-of-bounds DrawInfo.
//
// The SHADER masks, which contains the damage; this is what REPORTS it. A mask
// alone would make the corruption silent, which the RHI's rule 1 forbids -- and
// the CPU is the only side that knows the triangle count at all.
inline constexpr uint32_t kMaxPrimitivesPerDraw = (1u << 24) - 1u;

// Returns false, after logging, if `mesh` cannot be addressed by the packing.
bool ValidatePrimitiveCount(const PlaneMesh& mesh);

}  // namespace badlands::object_viewer
