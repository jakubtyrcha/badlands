// Ported (reconciled, see material_instance.hpp) from sampo's
// src/rendering/material/material_instance.cpp, namespace sampo -> badlands.
#include "engine/rendering/material/material_instance.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>

namespace badlands {

MaterialInstance::MaterialInstance(const MeshRenderingMaterial* base_material,
                                   GeometryType geometry_type,
                                   RenderPassType pass_type)
    : base_material_(base_material),
      geometry_type_(geometry_type),
      pass_type_(pass_type) {}

bool MaterialInstance::IsValid() const {
  return base_material_ != nullptr && base_material_->IsValid();
}

void MaterialInstance::SetTexture(uint32_t group, uint32_t texture_binding,
                                  uint32_t sampler_binding,
                                  wgpu::TextureView view,
                                  wgpu::Sampler sampler) {
  TextureBinding new_binding{group, texture_binding, sampler_binding, view,
                             sampler};

  // Find existing binding at this texture slot
  auto it = std::find_if(textures_.begin(), textures_.end(),
                         [group, texture_binding](const TextureBinding& t) {
                           return t.group == group &&
                                  t.texture_binding == texture_binding;
                         });

  if (it != textures_.end()) {
    *it = new_binding;
  } else {
    textures_.push_back(new_binding);
  }
  dirty_ = true;
}

void MaterialInstance::ClearTexture(uint32_t group, uint32_t texture_binding) {
  auto it = std::find_if(textures_.begin(), textures_.end(),
                         [group, texture_binding](const TextureBinding& t) {
                           return t.group == group &&
                                  t.texture_binding == texture_binding;
                         });

  if (it != textures_.end()) {
    textures_.erase(it);
    dirty_ = true;
  }
}

std::vector<TextureBinding> MaterialInstance::GetTexturesForGroup(
    uint32_t group) const {
  std::vector<TextureBinding> result;
  for (const auto& tex : textures_) {
    if (tex.group == group) {
      result.push_back(tex);
    }
  }
  return result;
}

wgpu::BindGroup MaterialInstance::GetOrCreateBindGroup(wgpu::Device device,
                                                       uint32_t group) {
  auto it = cached_bind_groups_.find(group);
  if (!dirty_ && it != cached_bind_groups_.end() && it->second) {
    return it->second;
  }

  CreateBindGroup(device, group);
  return cached_bind_groups_[group];
}

void MaterialInstance::Invalidate() {
  dirty_ = true;
  cached_bind_groups_.clear();
}

void MaterialInstance::CreateBindGroup(wgpu::Device device, uint32_t group) {
  if (!IsValid()) {
    return;
  }

  auto layout = base_material_->GetBindGroupLayout(geometry_type_, pass_type_, group);
  if (!layout) {
    return;
  }

  // Build bind group entries from our texture bindings
  std::vector<wgpu::BindGroupEntry> entries;
  for (const auto& tex : textures_) {
    if (tex.group != group) {
      continue;
    }

    // Add texture view entry
    if (tex.view) {
      wgpu::BindGroupEntry entry{};
      entry.binding = tex.texture_binding;
      entry.textureView = tex.view;
      entries.push_back(entry);
    }

    // Add sampler entry
    if (tex.sampler) {
      wgpu::BindGroupEntry entry{};
      entry.binding = tex.sampler_binding;
      entry.sampler = tex.sampler;
      entries.push_back(entry);
    }
  }

  wgpu::BindGroupDescriptor desc{};
  desc.layout = layout;
  desc.entryCount = entries.size();
  desc.entries = entries.data();

  cached_bind_groups_[group] = device.CreateBindGroup(&desc);
  dirty_ = false;
}

// ============================================================
// Material Constants Implementation
// ============================================================

void MaterialInstance::SetInt(const std::string& name, int32_t value) {
  auto it = constant_map_.find(name);
  if (it != constant_map_.end()) {
    // Update existing
    if (it->second.type != UniformType::Int) {
      spdlog::warn(
          "MaterialInstance::SetInt: type mismatch for '{}', expected Int",
          name);
      return;
    }
    int_values_[it->second.index] = value;
  } else {
    // Add new
    size_t index = int_values_.size();
    int_values_.push_back(value);
    constant_map_[name] = {UniformType::Int, index};
  }
  constants_dirty_ = true;
}

void MaterialInstance::SetUInt(const std::string& name, uint32_t value) {
  auto it = constant_map_.find(name);
  if (it != constant_map_.end()) {
    if (it->second.type != UniformType::UInt) {
      spdlog::warn(
          "MaterialInstance::SetUInt: type mismatch for '{}', expected UInt",
          name);
      return;
    }
    uint_values_[it->second.index] = value;
  } else {
    size_t index = uint_values_.size();
    uint_values_.push_back(value);
    constant_map_[name] = {UniformType::UInt, index};
  }
  constants_dirty_ = true;
}

void MaterialInstance::SetFloat(const std::string& name, float value) {
  auto it = constant_map_.find(name);
  if (it != constant_map_.end()) {
    if (it->second.type != UniformType::Float) {
      spdlog::warn(
          "MaterialInstance::SetFloat: type mismatch for '{}', expected Float",
          name);
      return;
    }
    float_values_[it->second.index] = value;
  } else {
    size_t index = float_values_.size();
    float_values_.push_back(value);
    constant_map_[name] = {UniformType::Float, index};
  }
  constants_dirty_ = true;
}

void MaterialInstance::SetVec2(const std::string& name,
                               const glm::vec2& value) {
  auto it = constant_map_.find(name);
  if (it != constant_map_.end()) {
    if (it->second.type != UniformType::Vec2) {
      spdlog::warn(
          "MaterialInstance::SetVec2: type mismatch for '{}', expected Vec2",
          name);
      return;
    }
    vec2_values_[it->second.index] = value;
  } else {
    size_t index = vec2_values_.size();
    vec2_values_.push_back(value);
    constant_map_[name] = {UniformType::Vec2, index};
  }
  constants_dirty_ = true;
}

void MaterialInstance::SetVec3(const std::string& name,
                               const glm::vec3& value) {
  auto it = constant_map_.find(name);
  if (it != constant_map_.end()) {
    if (it->second.type != UniformType::Vec3) {
      spdlog::warn(
          "MaterialInstance::SetVec3: type mismatch for '{}', expected Vec3",
          name);
      return;
    }
    vec3_values_[it->second.index] = value;
  } else {
    size_t index = vec3_values_.size();
    vec3_values_.push_back(value);
    constant_map_[name] = {UniformType::Vec3, index};
  }
  constants_dirty_ = true;
}

void MaterialInstance::SetVec4(const std::string& name,
                               const glm::vec4& value) {
  auto it = constant_map_.find(name);
  if (it != constant_map_.end()) {
    if (it->second.type != UniformType::Vec4) {
      spdlog::warn(
          "MaterialInstance::SetVec4: type mismatch for '{}', expected Vec4",
          name);
      return;
    }
    vec4_values_[it->second.index] = value;
  } else {
    size_t index = vec4_values_.size();
    vec4_values_.push_back(value);
    constant_map_[name] = {UniformType::Vec4, index};
  }
  constants_dirty_ = true;
}

void MaterialInstance::SetMat4(const std::string& name,
                               const glm::mat4& value) {
  auto it = constant_map_.find(name);
  if (it != constant_map_.end()) {
    if (it->second.type != UniformType::Mat4) {
      spdlog::warn(
          "MaterialInstance::SetMat4: type mismatch for '{}', expected Mat4",
          name);
      return;
    }
    mat4_values_[it->second.index] = value;
  } else {
    size_t index = mat4_values_.size();
    mat4_values_.push_back(value);
    constant_map_[name] = {UniformType::Mat4, index};
  }
  constants_dirty_ = true;
}

uint32_t MaterialInstance::GetUniformBufferSize() const {
  if (!IsValid()) {
    return 0;
  }
  const auto& uniform_buffers = base_material_->GetUniformBuffers(geometry_type_, pass_type_);
  if (uniform_buffers.empty()) {
    return 0;
  }
  // Return size of first uniform buffer (typically group 1 binding 0)
  return uniform_buffers[0].total_size;
}

wgpu::Buffer MaterialInstance::GetOrCreateUniformBuffer(wgpu::Device device) {
  if (constant_map_.empty()) {
    return nullptr;
  }

  if (!constants_dirty_ && uniform_buffer_) {
    return uniform_buffer_;
  }

  BuildUniformBuffer(device);
  return uniform_buffer_;
}

void MaterialInstance::BuildUniformBuffer(wgpu::Device device) {
  if (!IsValid() || constant_map_.empty()) {
    return;
  }

  // Get reflection data from the material (cached in GpuPipelineGenerator)
  const auto& uniform_buffers = base_material_->GetUniformBuffers(geometry_type_, pass_type_);
  if (uniform_buffers.empty()) {
    spdlog::warn(
        "MaterialInstance::BuildUniformBuffer: no uniform buffers found in "
        "shader");
    return;
  }

  // Use first uniform buffer (typically group 1 binding 0 for per-object
  // uniforms)
  const ReflectedUniformBuffer& buffer_info = uniform_buffers[0];
  uint32_t buffer_size = buffer_info.total_size;

  if (buffer_size == 0) {
    return;
  }

  // Allocate CPU-side buffer
  std::vector<uint8_t> data(buffer_size, 0);

  // Write each constant at its reflected offset
  for (const auto& member : buffer_info.members) {
    auto it = constant_map_.find(member.name);
    if (it == constant_map_.end()) {
      continue;  // Constant not set, leave as zero
    }

    const ConstantLocation& loc = it->second;
    void* dst = data.data() + member.offset;

    switch (loc.type) {
      case UniformType::Int:
        std::memcpy(dst, &int_values_[loc.index], sizeof(int32_t));
        break;
      case UniformType::UInt:
        std::memcpy(dst, &uint_values_[loc.index], sizeof(uint32_t));
        break;
      case UniformType::Float:
        std::memcpy(dst, &float_values_[loc.index], sizeof(float));
        break;
      case UniformType::Vec2:
        std::memcpy(dst, &vec2_values_[loc.index], sizeof(glm::vec2));
        break;
      case UniformType::Vec3:
        std::memcpy(dst, &vec3_values_[loc.index], sizeof(glm::vec3));
        break;
      case UniformType::Vec4:
        std::memcpy(dst, &vec4_values_[loc.index], sizeof(glm::vec4));
        break;
      case UniformType::Mat4:
        std::memcpy(dst, &mat4_values_[loc.index], sizeof(glm::mat4));
        break;
      default:
        break;
    }
  }

  // Always create a new buffer when dirty. This is necessary because shared
  // material instances (via MaterialInstanceCache) may be used by multiple
  // entities per frame. Each draw call needs its own buffer since
  // queue.writeBuffer to a shared buffer would overwrite previous data before
  // the GPU processes the render pass. The previous buffer stays alive through
  // bind group references (WebGPU ref-counting) — see the deviation note in
  // material_instance.hpp re: dropping GpuResourceManager-based deferred
  // deletion for Stage 1.
  wgpu::BufferDescriptor desc{};
  desc.size = buffer_size;
  desc.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
  desc.mappedAtCreation = true;
  desc.label =
      WGPUStringView{.data = "MaterialInstance_UniformBuffer", .length = 30};
  uniform_buffer_ = wgpu::Buffer(device.CreateBuffer(&desc));

  std::memcpy(uniform_buffer_.GetMappedRange(0, buffer_size), data.data(),
              buffer_size);
  uniform_buffer_.Unmap();
  constants_dirty_ = false;
}

}  // namespace badlands
