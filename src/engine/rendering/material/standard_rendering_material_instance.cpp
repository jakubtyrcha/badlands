// Ported from sampo's
// src/rendering/material/standard_rendering_material_instance.cpp, namespace
// sampo -> badlands.
//
// Deviation: `BuildParameterMap()` calls `material_->GetUniformBuffers(
// geometry_type_, pass_type_)` (explicit combo) instead of sampo's
// parameterless `GetUniformBuffers()` — see the deviation note in
// material_instance.hpp.
#include "engine/rendering/material/standard_rendering_material_instance.hpp"

#include <set>

#include <spdlog/spdlog.h>

#include "engine/rendering/context/frame_context.hpp"
#include "engine/rendering/context/render_pass_context.hpp"
#include "engine/rendering/shader/shader_reflection.hpp"

namespace badlands {

StandardRenderingMaterialInstance::StandardRenderingMaterialInstance(
    const MeshRenderingMaterial* material,
    std::unique_ptr<MaterialInstance> instance, GeometryType geometry_type,
    RenderPassType pass_type)
    : material_(material),
      instance_(std::move(instance)),
      geometry_type_(geometry_type),
      pass_type_(pass_type) {}

bool StandardRenderingMaterialInstance::Bind(RenderPassContext& pass,
                                             FrameContext& frame) {
  if (!IsValid()) {
    return false;
  }

  // 1. Get pipeline (observes hot-reload)
  auto pipeline = GetPipeline();
  if (!pipeline) {
    spdlog::warn("StandardRenderingMaterialInstance::Bind: null pipeline");
    return false;
  }
  pass.SetPipeline(pipeline);

  // 2. Build group 0 entries: frame UBO + textures + samplers
  auto frame_ubo = frame.GetFrameUniformBuffer();
  if (!frame_ubo) {
    spdlog::warn(
        "StandardRenderingMaterialInstance::Bind: null frame uniform buffer");
    return false;
  }

  std::vector<wgpu::BindGroupEntry> entries;

  // binding 0: frame uniform buffer
  wgpu::BindGroupEntry ubo_entry{};
  ubo_entry.binding = 0;
  ubo_entry.buffer = frame_ubo;
  ubo_entry.offset = 0;
  ubo_entry.size = WGPU_WHOLE_SIZE;
  entries.push_back(ubo_entry);

  // texture + sampler bindings from MaterialInstance
  // Deduplicate sampler entries — multiple textures may share one sampler binding
  std::set<uint32_t> emitted_sampler_bindings;
  auto textures = instance_->GetTexturesForGroup(0);
  for (const auto& tex : textures) {
    if (tex.view) {
      wgpu::BindGroupEntry tex_entry{};
      tex_entry.binding = tex.texture_binding;
      tex_entry.textureView = tex.view;
      entries.push_back(tex_entry);
    }
    if (tex.sampler &&
        !emitted_sampler_bindings.contains(tex.sampler_binding)) {
      wgpu::BindGroupEntry sampler_entry{};
      sampler_entry.binding = tex.sampler_binding;
      sampler_entry.sampler = tex.sampler;
      entries.push_back(sampler_entry);
      emitted_sampler_bindings.insert(tex.sampler_binding);
    }
  }

  // 3. Create + set bind group 0 using material's reflection-filtered creation
  auto layout =
      material_->GetBindGroupLayout(geometry_type_, pass_type_, 0);
  if (!layout) {
    spdlog::warn(
        "StandardRenderingMaterialInstance::Bind: null bind group layout");
    return false;
  }

  // Filter entries based on expected bindings
  auto expected =
      material_->GetExpectedBindings(geometry_type_, pass_type_, 0);
  std::vector<wgpu::BindGroupEntry> filtered;
  filtered.reserve(entries.size());
  for (const auto& e : entries) {
    if (expected.contains(e.binding)) {
      filtered.push_back(e);
    }
  }

  wgpu::BindGroupDescriptor bg_desc{};
  bg_desc.layout = layout;
  bg_desc.entryCount = filtered.size();
  bg_desc.entries = filtered.data();

  auto bind_group = frame.GetDevice().CreateBindGroup(&bg_desc);
  pass.SetBindGroup(0, bind_group);

  return true;
}

bool StandardRenderingMaterialInstance::BindPerObject(RenderPassContext& pass,
                                                      FrameContext& frame) {
  if (!IsValid()) {
    return false;
  }

  auto device = frame.GetDevice();

  // 1. Upload per-object uniform buffer
  auto ubo = instance_->GetOrCreateUniformBuffer(device);
  if (!ubo) {
    // No per-object uniforms set — not an error, just skip group 1
    return true;
  }

  // 2. Build bind group 1
  auto layout =
      material_->GetBindGroupLayout(geometry_type_, pass_type_, 1);
  if (!layout) {
    // No group 1 layout — shader doesn't use per-object uniforms
    return true;
  }

  wgpu::BindGroupEntry entry{};
  entry.binding = 0;
  entry.buffer = ubo;
  entry.offset = 0;
  entry.size = WGPU_WHOLE_SIZE;

  wgpu::BindGroupDescriptor bg_desc{};
  bg_desc.layout = layout;
  bg_desc.entryCount = 1;
  bg_desc.entries = &entry;

  auto bind_group = device.CreateBindGroup(&bg_desc);
  uint32_t dynamic_offset = 0;
  pass.SetBindGroup(1, bind_group, 1, &dynamic_offset);

  return true;
}

MaterialParameterId StandardRenderingMaterialInstance::GetParameterId(
    const std::string& name) const {
  BuildParameterMap();
  auto it = param_map_.find(name);
  if (it != param_map_.end()) {
    return it->second;
  }
  return {};  // invalid handle (0)
}

void StandardRenderingMaterialInstance::SetParameter(
    MaterialParameterId id, const MaterialParameterValue& value) {
  if (!id.IsValid() || !instance_) {
    return;
  }

  BuildParameterMap();

  // O(1) reverse lookup: handle → name
  auto name_it = handle_to_name_.find(id.handle);
  if (name_it == handle_to_name_.end()) {
    return;
  }
  const auto& name = name_it->second;

  // Validate type match and dispatch
  std::visit(
      [&](auto&& v) {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, int32_t>) {
          if (id.type != MaterialParameterId::Type::kInt) {
            spdlog::warn("SetParameter type mismatch: expected Int");
            return;
          }
          instance_->SetInt(name, v);
        } else if constexpr (std::is_same_v<T, uint32_t>) {
          if (id.type != MaterialParameterId::Type::kUInt) {
            spdlog::warn("SetParameter type mismatch: expected UInt");
            return;
          }
          instance_->SetUInt(name, v);
        } else if constexpr (std::is_same_v<T, float>) {
          if (id.type != MaterialParameterId::Type::kFloat) {
            spdlog::warn("SetParameter type mismatch: expected Float");
            return;
          }
          instance_->SetFloat(name, v);
        } else if constexpr (std::is_same_v<T, glm::vec2>) {
          if (id.type != MaterialParameterId::Type::kVec2) {
            spdlog::warn("SetParameter type mismatch: expected Vec2");
            return;
          }
          instance_->SetVec2(name, v);
        } else if constexpr (std::is_same_v<T, glm::vec3>) {
          if (id.type != MaterialParameterId::Type::kVec3) {
            spdlog::warn("SetParameter type mismatch: expected Vec3");
            return;
          }
          instance_->SetVec3(name, v);
        } else if constexpr (std::is_same_v<T, glm::vec4>) {
          if (id.type != MaterialParameterId::Type::kVec4) {
            spdlog::warn("SetParameter type mismatch: expected Vec4");
            return;
          }
          instance_->SetVec4(name, v);
        } else if constexpr (std::is_same_v<T, glm::mat4>) {
          if (id.type != MaterialParameterId::Type::kMat4) {
            spdlog::warn("SetParameter type mismatch: expected Mat4");
            return;
          }
          instance_->SetMat4(name, v);
        }
      },
      value);
}

void StandardRenderingMaterialInstance::SetWireframe(bool enabled) {
  wireframe_ = enabled;
}

wgpu::RenderPipeline StandardRenderingMaterialInstance::GetPipeline() const {
  if (!material_) {
    return nullptr;
  }
  return material_->GetPipeline(geometry_type_, pass_type_,
                                RenderConfig{.wireframe = wireframe_});
}

bool StandardRenderingMaterialInstance::IsValid() const {
  return material_ != nullptr && material_->IsValid() && instance_ != nullptr;
}

void StandardRenderingMaterialInstance::BuildParameterMap() const {
  if (param_map_built_) {
    return;
  }
  param_map_built_ = true;

  if (!material_) {
    return;
  }

  const auto& uniform_buffers = material_->GetUniformBuffers(geometry_type_, pass_type_);
  uint32_t next_handle = 1;
  for (const auto& buffer : uniform_buffers) {
    for (const auto& member : buffer.members) {
      MaterialParameterId pid;
      pid.handle = next_handle++;
      switch (member.type) {
        case UniformType::Int:
          pid.type = MaterialParameterId::Type::kInt;
          break;
        case UniformType::UInt:
          pid.type = MaterialParameterId::Type::kUInt;
          break;
        case UniformType::Float:
          pid.type = MaterialParameterId::Type::kFloat;
          break;
        case UniformType::Vec2:
          pid.type = MaterialParameterId::Type::kVec2;
          break;
        case UniformType::Vec3:
          pid.type = MaterialParameterId::Type::kVec3;
          break;
        case UniformType::Vec4:
          pid.type = MaterialParameterId::Type::kVec4;
          break;
        case UniformType::Mat4:
          pid.type = MaterialParameterId::Type::kMat4;
          break;
        default:
          pid.type = MaterialParameterId::Type::kFloat;
          break;
      }
      param_map_[member.name] = pid;
      handle_to_name_[pid.handle] = member.name;
    }
  }
}

// Non-virtual helper on base class
void RenderingMaterialInstance::SetParameterByName(
    const std::string& name, const MaterialParameterValue& value) {
  auto id = GetParameterId(name);
  if (id.IsValid()) {
    SetParameter(id, value);
  }
}

}  // namespace badlands
