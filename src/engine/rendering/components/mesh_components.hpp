#pragma once

// Ported from sampo's src/rendering/components/mesh_components.{hpp,cpp},
// namespace sampo -> badlands.
//
// Deviations:
// - Dropped `StaticMeshGpuComponent` and `StaticCubeMapTexturedMeshGpu` (the
//   wgpu::Buffer-owning GPU-side counterparts for the mesh kinds D3's forward
//   pass doesn't draw). Each one's `on_destroy` hook deferred buffer deletion
//   via `registry.ctx().find<sampo::GpuResourceManager*>()`, and
//   `GpuResourceManager` isn't ported to badlands yet — D1 already dropped
//   the same dependency from `MaterialInstance` for the same reason (see
//   task-D1-report.md deviation 4). Revisit once GpuResourceManager lands.
// - `StaticTexturedMeshGpuComponent` (below) was dropped by D2 for the same
//   reason and is re-added here, minimally, by D3: the forward-opaque mesh
//   pass (`rendering/passes/render_textured_mesh.cpp`) needs somewhere to
//   cache the uploaded vertex buffer across frames so it doesn't re-upload
//   every draw. Trimmed to just `{vertex_buffer, vertex_count}` — sampo's
//   `buffer_size`/`line_index_buffer`/`line_index_count` (wireframe support)
//   and `on_destroy` GpuResourceManager hook are omitted: D3's pass doesn't
//   support wireframe, and Dawn's `wgpu::Buffer` is itself a ref-counted
//   handle — the old buffer, if any, simply releases when overwritten or the
//   entity/component is destroyed (registry teardown), same reasoning as
//   `MaterialInstance`'s dropped `GpuResourceManager*` (D1 deviation 4).
// - Dropped the `#ifndef __EMSCRIPTEN__` guard (badlands Stage 1 has no
//   Emscripten/WASM build target; CMakeLists.txt targets native Dawn only).
// - Sampo defines these ECS component structs at *global* scope (only the
//   `sampo::MaterialInstance` forward declaration and `sampo::GpuResourceManager`
//   lookups were namespaced). This port places them in `namespace badlands`
//   instead, matching every other ported component/material type in this
//   codebase (D1's material components, `Transform`, etc.) — badlands has no
//   equivalent of sampo's "ECS components live at global scope" convention.
// - `GeometryType` now comes from `core/geometry_type.hpp`
//   (`badlands::GeometryType`), not sampo's global
//   `rendering/components/geometry_type.hpp` — see the same convention shift
//   already made by D1's material_requirements.hpp.

#include <cstdint>
#include <glm/glm.hpp>
#include <vector>
#include <dawn/webgpu_cpp.h>

#include "core/geometry_type.hpp"
#include "engine/rendering/geometry/aabb.hpp"

namespace badlands {

// Static mesh component containing geometry data (CPU-side)
struct StaticMeshComponent {
  std::vector<float>
      vertices;  // pos(3) + color(3) + normal(3) + cell_index(1) per vertex
  uint32_t vertex_count = 0;
  bool dirty = true;          // needs GPU upload
  bool keep_cpu_data = true;  // retain vertices after GPU upload for picking
};

// Extract all vertex positions from a StaticMeshComponent
std::vector<glm::vec3> ExtractVertexPositions(const StaticMeshComponent& mesh);

// Static textured mesh component with UV coordinates (CPU-side)
struct StaticTexturedMeshComponent {
  std::vector<float>
      vertices;  // pos(3) + uv(2) + normal(3) + tangent(3) per vertex
  uint32_t vertex_count = 0;
  bool dirty = true;

  wgpu::TextureView texture_view;
  wgpu::Sampler sampler;

  // Transform (model matrix applied during render)
  glm::mat4 transform{1.0f};

  // Geometry type affects how materials sample textures (2D vs cubemap)
  GeometryType geometry_type = GeometryType::kTexturedMesh;
};

// GPU-side vertex buffer for a StaticTexturedMeshComponent, uploaded lazily
// by the forward-opaque mesh pass on first draw (or after the CPU-side mesh
// is marked dirty again). See the file-level deviation note above for why
// this is a minimal re-addition (no GpuResourceManager, no wireframe index
// buffer) rather than sampo's full `StaticTexturedMeshGpuComponent`.
struct StaticTexturedMeshGpuComponent {
  wgpu::Buffer vertex_buffer;
  uint32_t vertex_count = 0;
};

class MaterialInstance;

// Static cubemap-textured mesh component (CPU-side)
// For spherical geometry with cubemap sampling (no UV)
struct StaticCubeMapTexturedMesh {
  std::vector<float>
      vertices;  // pos(3) + normal(3) + tangent(3) per vertex = 9 floats
  uint32_t vertex_count = 0;
  bool dirty = true;
};

// Cached local-space AABB computed once from pretransformed vertex positions.
struct StaticMeshAabbComponent {
  Aabb local;
};

// Component that attaches a MaterialInstance to an entity for custom material
// parameters When present, render passes should use this material instead of
// the default
struct MaterialInstanceComponent {
  MaterialInstance* instance = nullptr;  // Non-owning pointer
};

}  // namespace badlands
