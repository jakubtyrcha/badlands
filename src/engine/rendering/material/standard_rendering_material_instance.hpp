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

  MaterialParameterId GetParameterId(
      const std::string& name) const override;
  void SetParameter(MaterialParameterId id,
                    const MaterialParameterValue& value) override;

  void SetWireframe(bool enabled) override;

  GeometryType GetGeometryType() const override { return geometry_type_; }
  RenderPassType GetRenderPassType() const override { return pass_type_; }
  wgpu::RenderPipeline GetPipeline() const override;
  bool IsValid() const override;

 private:
  void BuildParameterMap() const;

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
};

}  // namespace badlands
