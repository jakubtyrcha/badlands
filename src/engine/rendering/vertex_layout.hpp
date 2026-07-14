#pragma once

// Ported from sampo's src/rendering/vertex_layout.hpp, namespace sampo ->
// badlands (verbatim otherwise: self-contained, only depends on Dawn).

#include <cstdint>
#include <vector>
#include <dawn/webgpu_cpp.h>

namespace badlands {

enum class VertexLayout {
  kStaticMesh,    // pos(vec3) + color(vec3) + normal(vec3) = 36 bytes
  kTerrainMesh,   // pos(vec3) + color(vec3) + normal(vec3) + cell_index(u32)
                  // = 40 bytes
  kTexturedMesh,  // pos(vec3) + uv(vec2) + normal(vec3) + tangent(vec3) = 44
                  // bytes
  kCubeMapMesh,   // pos(vec3) + normal(vec3) + tangent(vec3) = 36 bytes (no
                  // UV)
  kLine,          // pos(vec3) + color(vec3) = 24 bytes
  kThickLine,     // pos(vec3) + color(vec3) + dist_and_half_thickness(vec2)
                  // = 32 bytes
  kPolygon,       // pos(vec3) + color(vec4) = 28 bytes
  kText,          // pos(vec2) + glyph_index(u32) = 12 bytes
  kTextEffect,    // pos(vec2) + glyph_index(u32) + bbox_min(vec2) +
                  // bbox_max(vec2) = 28 bytes
  kFullscreen,    // No vertex buffer (generated in shader)
  kSplatQuad,          // pos_ndc(vec2) + local_uv(vec2) + src_uv(vec2) = 24 bytes
  kDelaunayTriangle,   // pos_ndc(vec2) + edge_dists(vec3) + src_uv(vec2) = 28 bytes
  kSplatCapsule,       // pos_ndc(vec2) + local_pos(vec2) + src_uv(vec2) + half_len_radius(vec2) = 32 bytes
  kTextBg,             // pos(vec2) + color(vec4) = 24 bytes
  kPos2d,              // pos(vec2) = 8 bytes (screen-space positions only)
  kPos2dSlot,          // pos(vec2) + slot_index(u32) = 12 bytes
                       // (per-vertex slot indirection; used by the cross-font
                       //  glyph-morph demo to bind each tight quad to a pair
                       //  of SDF variants).
};

struct VertexLayoutInfo {
  std::vector<wgpu::VertexAttribute> attributes;
  uint64_t stride = 0;
  wgpu::VertexStepMode step_mode = wgpu::VertexStepMode::Vertex;
};

VertexLayoutInfo GetVertexLayoutInfo(VertexLayout layout);

}  // namespace badlands
