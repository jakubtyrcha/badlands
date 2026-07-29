#pragma once

// Ported from sampo's
// src/rendering/material/standard_rendering_material_instance.hpp, namespace
// sampo -> badlands (verbatim otherwise).
#include <memory>
#include <string>
#include <unordered_map>

#include "engine/rendering/material/material.hpp"
#include "engine/rendering/material/material_instance.hpp"
#include "engine/rendering/material/rendering_material_instance.hpp"

namespace badlands {

// Concrete RenderingMaterialInstance wrapping MeshRenderingMaterial +
// MaterialInstance.
class StandardRenderingMaterialInstance : public RenderingMaterialInstance {
 public:
  StandardRenderingMaterialInstance(const MeshRenderingMaterial* material,
                                    std::unique_ptr<MaterialInstance> instance,
                                    GeometryType geometry_type,
                                    RenderPassType pass_type);

  bool Bind(RenderPassContext& pass, FrameContext& frame) override;
  bool BindPerObject(RenderPassContext& pass, FrameContext& frame) override;
  bool BindInstanceData(RenderPassContext& pass, FrameContext& frame,
                        wgpu::Buffer compacted,
                        wgpu::Buffer bucket_base) override;

  MaterialParameterId GetParameterId(
      const std::string& name) const override;
  void SetParameter(MaterialParameterId id,
                    const MaterialParameterValue& value) override;

  void SetWireframe(bool enabled) override;

  GeometryType GetGeometryType() const override { return geometry_type_; }
  RenderPassType GetRenderPassType() const override { return pass_type_; }
  wgpu::RenderPipeline GetPipeline() const override;
  bool DeclaresBindGroup(uint32_t group) const override;
  bool IsValid() const override;

 private:
  void BuildParameterMap() const;

  // Lazily create + cache a zero-filled group-0 params UBO of `size` bytes, for
  // an instanced material bound with NO SetParameter: MaterialInstance builds no
  // constants buffer then, but the shader still declares the group-0 UBO and
  // Dawn requires every layout binding present at draw. Returns null if size==0.
  wgpu::Buffer GetOrCreateZeroedParamsBuffer(wgpu::Device device, uint32_t size);

  const MeshRenderingMaterial* material_;
  std::unique_ptr<MaterialInstance> instance_;
  GeometryType geometry_type_;
  RenderPassType pass_type_;
  bool wireframe_ = false;

  // Lazy name→handle map from shader reflection
  mutable std::unordered_map<std::string, MaterialParameterId> param_map_;
  // Reverse map: handle → name (for O(1) lookup in SetParameter)
  mutable std::unordered_map<uint32_t, std::string> handle_to_name_;
  mutable bool param_map_built_ = false;

  // BindInstanceData's group-1 bind group cache: the (compacted, bucket_base)
  // buffer pair it was built from, so a repeat call over the SAME stable
  // buffers (the normal case -- GpuInstanceRenderer owns both for its whole
  // lifetime) can reuse it instead of rebuilding every call.
  wgpu::BindGroup cached_instance_bind_group_;
  wgpu::Buffer cached_instance_compacted_;
  wgpu::Buffer cached_instance_bucket_base_;

  // Zero-filled stand-in for the group-0 params UBO when an instanced material
  // is bound without any SetParameter (see GetOrCreateZeroedParamsBuffer).
  wgpu::Buffer zeroed_params_buffer_;
};

}  // namespace badlands
