// Ported (reconciled, see material.hpp) from sampo's
// src/rendering/material/material.cpp, namespace sampo -> badlands.
#include "engine/rendering/material/material.hpp"

namespace badlands {

MeshRenderingMaterial::MeshRenderingMaterial(
    GpuPipelineGenerator* generator, std::string name, std::string shader_path,
    std::string vs_entry, std::string fs_entry,
    std::vector<std::string> base_features, bool blend_enabled,
    bool premultiplied_alpha, std::map<RenderPassType, TargetConfig> pass_targets,
    wgpu::CullMode cull_mode)
    : generator_(generator),
      name_(std::move(name)),
      shader_path_(std::move(shader_path)),
      vs_entry_(std::move(vs_entry)),
      fs_entry_(std::move(fs_entry)),
      base_features_(std::move(base_features)),
      blend_enabled_(blend_enabled),
      premultiplied_alpha_(premultiplied_alpha),
      pass_targets_(std::move(pass_targets)),
      cull_mode_(cull_mode) {}

bool MeshRenderingMaterial::IsValid() const {
  return generator_ != nullptr && !shader_path_.empty();
}

RenderPipelineDeclaration MeshRenderingMaterial::BuildDeclaration(
    GeometryType geometry_type, RenderPassType pass_type,
    const RenderConfig& config) const {
  RenderPipelineDeclaration decl;
  decl.shader_path = shader_path_;
  decl.vs_entry = vs_entry_;
  decl.fs_entry = fs_entry_;
  decl.vertex_layout = geometry_type == GeometryType::kSphericalMesh
                           ? VertexLayout::kCubeMapMesh
                           : VertexLayout::kTexturedMesh;
  decl.cull_mode = cull_mode_;
  decl.blend_enabled = blend_enabled_;
  decl.premultiplied_alpha = premultiplied_alpha_;
  if (config.wireframe) {
    decl.topology = wgpu::PrimitiveTopology::LineList;
  }

  // Feature flags: base features (from the MaterialPassType variant, e.g.
  // "transparent") + geometry/pass-derived features (mirrors sampo's
  // BuildFeatures helper in gpu_pipeline_generator.cpp).
  decl.features = base_features_;
  if (geometry_type == GeometryType::kSphericalMesh) {
    decl.features.push_back("sphere_mode");
  }
  if (pass_type == RenderPassType::kShadow) {
    decl.features.push_back("shadow_pass");
  }

  auto it = pass_targets_.find(pass_type);
  if (it != pass_targets_.end()) {
    decl.depth_write = it->second.depth_write;
    decl.depth_compare = it->second.depth_compare;
    decl.depth_format = it->second.depth_format;
  }

  return decl;
}

std::shared_ptr<const CompiledPipeline> MeshRenderingMaterial::GetCompiledPipeline(
    GeometryType geometry_type, RenderPassType pass_type,
    const RenderConfig& config) const {
  if (!generator_) {
    return nullptr;
  }
  RenderTargetFormats formats;
  if (auto it = pass_targets_.find(pass_type); it != pass_targets_.end()) {
    formats = it->second.color_formats;
  }
  RenderPipelineDeclaration decl = BuildDeclaration(geometry_type, pass_type, config);
  return generator_->GetPipeline(decl, formats);
}

wgpu::RenderPipeline MeshRenderingMaterial::GetPipeline(
    GeometryType geometry_type, RenderPassType pass_type,
    const RenderConfig& config) const {
  auto compiled = GetCompiledPipeline(geometry_type, pass_type, config);
  return compiled ? compiled->pipeline : nullptr;
}

wgpu::BindGroupLayout MeshRenderingMaterial::GetBindGroupLayout(
    GeometryType geometry_type, RenderPassType pass_type, uint32_t group) const {
  auto compiled = GetCompiledPipeline(geometry_type, pass_type);
  if (!compiled || group >= compiled->bind_group_layouts.size()) {
    return nullptr;
  }
  return compiled->bind_group_layouts[group];
}

std::set<uint32_t> MeshRenderingMaterial::GetExpectedBindings(
    GeometryType geometry_type, RenderPassType pass_type, uint32_t group) const {
  auto compiled = GetCompiledPipeline(geometry_type, pass_type);
  if (!compiled) {
    return {};
  }
  std::set<uint32_t> result;
  for (const auto& binding : compiled->reflected_bindings) {
    if (binding.group == group) {
      result.insert(binding.binding);
    }
  }
  return result;
}

const std::vector<ReflectedUniformBuffer>& MeshRenderingMaterial::GetUniformBuffers(
    GeometryType geometry_type, RenderPassType pass_type) const {
  static const std::vector<ReflectedUniformBuffer> empty;
  auto compiled = GetCompiledPipeline(geometry_type, pass_type);
  if (!compiled) {
    return empty;
  }
  return compiled->uniform_buffers;
}

const std::vector<ReflectedBinding>& MeshRenderingMaterial::GetReflectedBindings(
    GeometryType geometry_type, RenderPassType pass_type) const {
  static const std::vector<ReflectedBinding> empty;
  auto compiled = GetCompiledPipeline(geometry_type, pass_type);
  if (!compiled) {
    return empty;
  }
  return compiled->reflected_bindings;
}

}  // namespace badlands
