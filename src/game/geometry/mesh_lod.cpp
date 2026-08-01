#include "game/geometry/mesh_lod.hpp"

#include <algorithm>

#include <meshoptimizer.h>

namespace badlands {

namespace {

// The weld + target-count preamble both simplifiers share. Welding coincident
// vertices via the canonical meshopt indexer keeps decimation from being
// blocked by duplicated positions/attributes at shared edges.
struct WeldedMesh {
  std::vector<float> vertices;
  std::vector<uint32_t> indices;
  size_t vertex_count = 0;
  size_t target_index_count = 0;  // triangle-aligned, >= 3
};

WeldedMesh WeldForSimplify(const std::vector<float>& vertices,
                            size_t floats_per_vertex,
                            const std::vector<uint32_t>& indices,
                            float target_ratio) {
  const size_t stride = floats_per_vertex * sizeof(float);
  const size_t vertex_count = vertices.size() / floats_per_vertex;

  std::vector<uint32_t> remap(vertex_count);
  const size_t welded_vertex_count = meshopt_generateVertexRemap(
      remap.data(), indices.data(), indices.size(), vertices.data(),
      vertex_count, stride);

  WeldedMesh out;
  out.vertices.resize(welded_vertex_count * floats_per_vertex);
  meshopt_remapVertexBuffer(out.vertices.data(), vertices.data(), vertex_count,
                             stride, remap.data());

  out.indices.resize(indices.size());
  meshopt_remapIndexBuffer(out.indices.data(), indices.data(), indices.size(),
                            remap.data());

  out.vertex_count = welded_vertex_count;
  size_t target = static_cast<size_t>(out.indices.size() * target_ratio);
  target -= target % 3;
  if (target < 3) target = std::min<size_t>(out.indices.size(), 3);
  out.target_index_count = target;
  return out;
}

// Drops the vertices the simplified index buffer no longer references (and
// reorders the survivors for fetch locality), rewriting `indices` in place.
// Without this a result carries the FULL welded vertex set no matter how few
// triangles survived -- at the aggressive end of an LOD chain that is almost
// all dead weight: a budget-decimated bark mesh can keep a couple of hundred
// of ~6000 welded vertices, so the GPU buffer would dwarf what its triangle
// count implies. Callers
// upload `vertices` wholesale, so the waste is real VRAM, not just slack.
SimplifiedMesh CompactVertices(std::vector<float> vertices,
                                size_t floats_per_vertex,
                                std::vector<uint32_t> indices) {
  const size_t stride = floats_per_vertex * sizeof(float);
  const size_t vertex_count = vertices.size() / floats_per_vertex;
  if (indices.empty() || vertex_count == 0) {
    return SimplifiedMesh{std::move(vertices), std::move(indices),
                           static_cast<uint32_t>(vertex_count)};
  }

  std::vector<float> packed(vertices.size());
  const size_t new_vertex_count =
      meshopt_optimizeVertexFetch(packed.data(), indices.data(), indices.size(),
                                   vertices.data(), vertex_count, stride);
  packed.resize(new_vertex_count * floats_per_vertex);
  return SimplifiedMesh{std::move(packed), std::move(indices),
                         static_cast<uint32_t>(new_vertex_count)};
}

}  // namespace

SimplifiedMesh SimplifyMesh(const std::vector<float>& vertices,
                             size_t floats_per_vertex,
                             const std::vector<uint32_t>& indices,
                             float target_ratio) {
  const size_t stride = floats_per_vertex * sizeof(float);
  const size_t vertex_count = vertices.size() / floats_per_vertex;

  if (target_ratio >= 1.0f || indices.size() < 3) {
    return SimplifiedMesh{vertices, indices,
                           static_cast<uint32_t>(vertex_count)};
  }

  WeldedMesh w =
      WeldForSimplify(vertices, floats_per_vertex, indices, target_ratio);

  std::vector<uint32_t> simplified(w.indices.size());
  float result_error = 0.0f;
  const size_t got = meshopt_simplify(
      simplified.data(), w.indices.data(), w.indices.size(), w.vertices.data(),
      w.vertex_count, stride, w.target_index_count,
      /*target_error=*/1.0f, /*options=*/0, &result_error);
  simplified.resize(got);

  return CompactVertices(std::move(w.vertices), floats_per_vertex,
                          std::move(simplified));
}

SimplifiedMesh SimplifyMeshSloppy(const std::vector<float>& vertices,
                                   size_t floats_per_vertex,
                                   const std::vector<uint32_t>& indices,
                                   float target_ratio) {
  const size_t stride = floats_per_vertex * sizeof(float);
  const size_t vertex_count = vertices.size() / floats_per_vertex;

  if (target_ratio >= 1.0f || indices.size() < 3) {
    return SimplifiedMesh{vertices, indices,
                           static_cast<uint32_t>(vertex_count)};
  }

  WeldedMesh w =
      WeldForSimplify(vertices, floats_per_vertex, indices, target_ratio);

  std::vector<uint32_t> simplified(w.indices.size());
  float result_error = 0.0f;
  const size_t got = meshopt_simplifySloppy(
      simplified.data(), w.indices.data(), w.indices.size(), w.vertices.data(),
      w.vertex_count, stride, w.target_index_count,
      /*target_error=*/1.0f, &result_error);
  simplified.resize(got);

  // Clustering can collapse a whole mesh away (every cluster degenerate) and
  // return nothing. An empty bark mesh is a visible hole, not a cheap LOD, so
  // fall back to error-bounded collapse -- it won't hit the target, but it
  // always returns geometry.
  if (simplified.empty()) {
    return SimplifyMesh(vertices, floats_per_vertex, indices, target_ratio);
  }

  return CompactVertices(std::move(w.vertices), floats_per_vertex,
                          std::move(simplified));
}

}  // namespace badlands
