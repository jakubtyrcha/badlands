#pragma once

// Ported from sampo's src/rendering/material/material_instance.{hpp,cpp},
// namespace sampo -> badlands.
//
// Deviations:
// - `GpuResourceManager*` (sampo's deferred-buffer-deletion resource
//   manager) is dropped: it isn't ported to badlands yet (out of scope for
//   the material-system port; would require adding
//   rendering/gpu/gpu_resource_manager.* outside src/engine/rendering/
//   material/**). sampo's own destructor comment already documents the
//   fallback for `resource_manager_ == nullptr`: "RAII wrapper releases
//   buffer immediately (may cause GPU issues if buffer still in use -
//   callers should provide manager)". Dawn's wgpu::Buffer is a ref-counted
//   handle and bind groups retain the buffers they reference, so this is
//   safe for Stage 1 (no long-lived cross-frame reuse contention yet); a
//   deferred-deletion path can be reinstated once GpuResourceManager lands.
// - Constructor now takes `GeometryType`/`RenderPassType` explicitly. sampo's
//   `MeshRenderingMaterial::GetUniformBuffers()` was parameterless — it
//   queried the name-keyed pipeline registry with an internal
//   geometry/pass-variant fallback chain (see the deviation note in
//   material.hpp: that registry doesn't exist in this port). Since pipelines
//   now compile per explicit (geometry, pass) combination,
//   `MaterialInstance` needs to know which combination it was created for
//   in order to call `MeshRenderingMaterial::GetUniformBuffers(geometry,
//   pass)`. `StandardMaterialFactory::CreateInstance` already has both
//   values at construction time.
#include <glm/glm.hpp>
#include <string>
#include <unordered_map>
#include <vector>

#include <dawn/webgpu_cpp.h>

#include "engine/rendering/material/material.hpp"
#include "engine/rendering/shader/shader_reflection.hpp"

namespace badlands {

// A texture+sampler binding pair at explicit slots
struct TextureBinding {
  uint32_t group;
  uint32_t texture_binding;
  uint32_t sampler_binding;
  wgpu::TextureView view;
  wgpu::Sampler sampler;
};

// MaterialInstance represents a specific parameterization of a MeshRenderingMaterial.
// Holds per-instance texture+sampler bindings, material constants, and caches
// bind groups.
class MaterialInstance {
 public:
  MaterialInstance() = default;
  MaterialInstance(const MeshRenderingMaterial* base_material,
                   GeometryType geometry_type, RenderPassType pass_type);
  ~MaterialInstance() = default;

  // Non-copyable due to RAII buffer ownership
  MaterialInstance(const MaterialInstance&) = delete;
  MaterialInstance& operator=(const MaterialInstance&) = delete;

  // Movable
  MaterialInstance(MaterialInstance&&) noexcept = default;
  MaterialInstance& operator=(MaterialInstance&&) noexcept = default;

  // Base material access
  const MeshRenderingMaterial* GetBaseMaterial() const { return base_material_; }
  bool IsValid() const;

  // Texture management with explicit binding indices
  void SetTexture(uint32_t group, uint32_t texture_binding,
                  uint32_t sampler_binding, wgpu::TextureView view,
                  wgpu::Sampler sampler);
  void ClearTexture(uint32_t group, uint32_t texture_binding);

  // Get all bindings for a specific group
  std::vector<TextureBinding> GetTexturesForGroup(uint32_t group) const;

  // ============================================================
  // Material Constants API
  // ============================================================
  // Set constants by name. Type must match what shader reflection expects.

  void SetInt(const std::string& name, int32_t value);
  void SetUInt(const std::string& name, uint32_t value);
  void SetFloat(const std::string& name, float value);
  void SetVec2(const std::string& name, const glm::vec2& value);
  void SetVec3(const std::string& name, const glm::vec3& value);
  void SetVec4(const std::string& name, const glm::vec4& value);
  void SetMat4(const std::string& name, const glm::mat4& value);

  // Get the uniform buffer containing all material constants
  // Returns null buffer if no constants are set
  wgpu::Buffer GetOrCreateUniformBuffer(wgpu::Device device);

  // Get total size of uniform buffer (from reflection)
  uint32_t GetUniformBufferSize() const;

  // Check if any constants need to be written to GPU
  bool HasPendingConstantUpdates() const { return constants_dirty_; }

  // Get read-only access to constant map (for checking if constants are set)
  const auto& GetConstantMap() const { return constant_map_; }

  // ============================================================
  // Bind Group Management
  // ============================================================

  // Cached bind group (recreated when dirty)
  wgpu::BindGroup GetOrCreateBindGroup(wgpu::Device device, uint32_t group);

  // Force bind group recreation on next access
  void Invalidate();

  // Check if bind groups need recreation
  bool IsDirty() const { return dirty_; }

 private:
  void CreateBindGroup(wgpu::Device device, uint32_t group);
  void BuildUniformBuffer(wgpu::Device device);

  // Location of a constant in SOA storage
  struct ConstantLocation {
    UniformType type;
    size_t index;  // Index in the type-specific array
  };

  const MeshRenderingMaterial* base_material_ = nullptr;
  GeometryType geometry_type_ = GeometryType::kTexturedMesh;
  RenderPassType pass_type_ = RenderPassType::kForward;
  std::vector<TextureBinding> textures_;

  // Cached bind groups per group index
  std::unordered_map<uint32_t, wgpu::BindGroup> cached_bind_groups_;
  bool dirty_ = true;

  // ============================================================
  // SOA storage for material constants
  // ============================================================
  std::vector<int32_t> int_values_;
  std::vector<uint32_t> uint_values_;
  std::vector<float> float_values_;
  std::vector<glm::vec2> vec2_values_;
  std::vector<glm::vec3> vec3_values_;
  std::vector<glm::vec4> vec4_values_;
  std::vector<glm::mat4> mat4_values_;

  // Map from constant name -> location in SOA storage
  std::unordered_map<std::string, ConstantLocation> constant_map_;

  // Cached uniform buffer (rebuilt when constants_dirty_)
  wgpu::Buffer uniform_buffer_;
  bool constants_dirty_ = false;
};

}  // namespace badlands
