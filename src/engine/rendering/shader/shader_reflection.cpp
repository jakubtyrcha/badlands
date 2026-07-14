// Ported from sampo's src/rendering/shader/shader_reflection.cpp, namespace
// sampo -> badlands.
//
// Deviations:
// - `#include <fmt/format.h>` -> `#include <spdlog/fmt/fmt.h>`: badlands
//   vendors spdlog (third_party/spdlog) but not a standalone fmt library;
//   spdlog/fmt/fmt.h exposes the same `fmt::format` API via spdlog's bundled
//   header-only fmt copy (SPDLOG_FMT_EXTERNAL is not defined), so no new
//   third-party dependency is introduced.
// - `#include <algorithm>` added explicitly for `std::max` (transitively
//   available in sampo's build, not guaranteed here).
#include "engine/rendering/shader/shader_reflection.hpp"

#include <algorithm>
#include <map>
#include <optional>

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include "wesl_ffi.h"

namespace badlands {

namespace {

wgpu::TextureViewDimension MapTextureDimension(uint32_t dim) {
  switch (dim) {
    case WGSL_TEXTURE_DIM_1D: return wgpu::TextureViewDimension::e1D;
    case WGSL_TEXTURE_DIM_2D: return wgpu::TextureViewDimension::e2D;
    case WGSL_TEXTURE_DIM_2D_ARRAY: return wgpu::TextureViewDimension::e2DArray;
    case WGSL_TEXTURE_DIM_3D: return wgpu::TextureViewDimension::e3D;
    case WGSL_TEXTURE_DIM_CUBE: return wgpu::TextureViewDimension::Cube;
    case WGSL_TEXTURE_DIM_CUBE_ARRAY: return wgpu::TextureViewDimension::CubeArray;
    default: return wgpu::TextureViewDimension::e2D;
  }
}

wgpu::TextureFormat MapStorageTextureFormat(uint32_t fmt) {
  switch (fmt) {
    case WGSL_STORAGE_FORMAT_R32FLOAT: return wgpu::TextureFormat::R32Float;
    case WGSL_STORAGE_FORMAT_R32UINT: return wgpu::TextureFormat::R32Uint;
    case WGSL_STORAGE_FORMAT_R32SINT: return wgpu::TextureFormat::R32Sint;
    case WGSL_STORAGE_FORMAT_RG32FLOAT: return wgpu::TextureFormat::RG32Float;
    case WGSL_STORAGE_FORMAT_RG32UINT: return wgpu::TextureFormat::RG32Uint;
    case WGSL_STORAGE_FORMAT_RG32SINT: return wgpu::TextureFormat::RG32Sint;
    case WGSL_STORAGE_FORMAT_RGBA8UNORM: return wgpu::TextureFormat::RGBA8Unorm;
    case WGSL_STORAGE_FORMAT_RGBA8SNORM: return wgpu::TextureFormat::RGBA8Snorm;
    case WGSL_STORAGE_FORMAT_RGBA8UINT: return wgpu::TextureFormat::RGBA8Uint;
    case WGSL_STORAGE_FORMAT_RGBA8SINT: return wgpu::TextureFormat::RGBA8Sint;
    case WGSL_STORAGE_FORMAT_BGRA8UNORM: return wgpu::TextureFormat::BGRA8Unorm;
    case WGSL_STORAGE_FORMAT_RGBA16FLOAT: return wgpu::TextureFormat::RGBA16Float;
    case WGSL_STORAGE_FORMAT_RGBA16UINT: return wgpu::TextureFormat::RGBA16Uint;
    case WGSL_STORAGE_FORMAT_RGBA16SINT: return wgpu::TextureFormat::RGBA16Sint;
    case WGSL_STORAGE_FORMAT_RGBA32FLOAT: return wgpu::TextureFormat::RGBA32Float;
    case WGSL_STORAGE_FORMAT_RGBA32UINT: return wgpu::TextureFormat::RGBA32Uint;
    case WGSL_STORAGE_FORMAT_RGBA32SINT: return wgpu::TextureFormat::RGBA32Sint;
    case WGSL_STORAGE_FORMAT_R16FLOAT: return wgpu::TextureFormat::R16Float;
    case WGSL_STORAGE_FORMAT_RG16FLOAT: return wgpu::TextureFormat::RG16Float;
    case WGSL_STORAGE_FORMAT_R8UNORM: return wgpu::TextureFormat::R8Unorm;
    case WGSL_STORAGE_FORMAT_RG8UNORM: return wgpu::TextureFormat::RG8Unorm;
    default: return wgpu::TextureFormat::Undefined;
  }
}

wgpu::StorageTextureAccess MapStorageTextureAccess(uint32_t access) {
  switch (access) {
    case WGSL_STORAGE_ACCESS_WRITE_ONLY: return wgpu::StorageTextureAccess::WriteOnly;
    case WGSL_STORAGE_ACCESS_READ_ONLY: return wgpu::StorageTextureAccess::ReadOnly;
    case WGSL_STORAGE_ACCESS_READ_WRITE: return wgpu::StorageTextureAccess::ReadWrite;
    default: return wgpu::StorageTextureAccess::WriteOnly;
  }
}

}  // namespace

std::vector<ReflectedBinding> ReflectShader(const std::string& wgsl_source) {
  WgslReflectionResult result = wgsl_reflect(wgsl_source.c_str());

  if (result.error != nullptr) {
    spdlog::error("WGSL reflection error: {}", result.error);
    wgsl_free_reflection(result);
    return {};
  }

  std::vector<ReflectedBinding> bindings;
  bindings.reserve(result.binding_count);

  for (size_t i = 0; i < result.binding_count; i++) {
    const WgslBinding& b = result.bindings[i];
    ReflectedBinding binding;
    binding.group = b.group;
    binding.binding = b.binding;
    binding.name = b.name ? std::string(b.name) : std::string();

    // Map binding type
    switch (b.binding_type) {
      case WGSL_BINDING_UNIFORM:
        binding.buffer_type = wgpu::BufferBindingType::Uniform;
        break;
      case WGSL_BINDING_STORAGE:
        binding.buffer_type = wgpu::BufferBindingType::Storage;
        break;
      case WGSL_BINDING_STORAGE_READ_ONLY:
        binding.buffer_type = wgpu::BufferBindingType::ReadOnlyStorage;
        break;
      case WGSL_BINDING_TEXTURE:
        // Map texture sample type
        switch (b.texture_sample_type) {
          case WGSL_TEXTURE_SAMPLE_FLOAT:
            binding.texture_type = wgpu::TextureSampleType::Float;
            break;
          case WGSL_TEXTURE_SAMPLE_DEPTH:
            binding.texture_type = wgpu::TextureSampleType::Depth;
            break;
          case WGSL_TEXTURE_SAMPLE_SINT:
            binding.texture_type = wgpu::TextureSampleType::Sint;
            break;
          case WGSL_TEXTURE_SAMPLE_UINT:
            binding.texture_type = wgpu::TextureSampleType::Uint;
            break;
          case WGSL_TEXTURE_SAMPLE_UNFILTERABLE_FLOAT:
            binding.texture_type = wgpu::TextureSampleType::UnfilterableFloat;
            break;
          default:
            binding.texture_type = wgpu::TextureSampleType::Float;
            break;
        }
        binding.texture_dimension = MapTextureDimension(b.texture_dimension);
        break;
      case WGSL_BINDING_STORAGE_TEXTURE:
        binding.is_storage_texture = true;
        binding.storage_format = MapStorageTextureFormat(b.storage_texture_format);
        binding.storage_access = MapStorageTextureAccess(b.storage_texture_access);
        binding.texture_dimension = MapTextureDimension(b.texture_dimension);
        break;
      case WGSL_BINDING_SAMPLER:
        // Map sampler type
        switch (b.sampler_type) {
          case WGSL_SAMPLER_FILTERING:
            binding.sampler_type = wgpu::SamplerBindingType::Filtering;
            break;
          case WGSL_SAMPLER_NON_FILTERING:
            binding.sampler_type = wgpu::SamplerBindingType::NonFiltering;
            break;
          case WGSL_SAMPLER_COMPARISON:
            binding.sampler_type = wgpu::SamplerBindingType::Comparison;
            break;
          default:
            binding.sampler_type = wgpu::SamplerBindingType::Filtering;
            break;
        }
        break;
      default:
        binding.buffer_type = wgpu::BufferBindingType::Uniform;
        break;
    }

    // Map visibility flags
    binding.visibility = wgpu::ShaderStage::None;
    if (b.visibility & WGSL_STAGE_VERTEX) {
      binding.visibility = binding.visibility | wgpu::ShaderStage::Vertex;
    }
    if (b.visibility & WGSL_STAGE_FRAGMENT) {
      binding.visibility = binding.visibility | wgpu::ShaderStage::Fragment;
    }
    if (b.visibility & WGSL_STAGE_COMPUTE) {
      binding.visibility = binding.visibility | wgpu::ShaderStage::Compute;
    }

    bindings.push_back(binding);
  }

  wgsl_free_reflection(result);
  return bindings;
}

namespace {

void PopulateLayoutEntry(wgpu::BindGroupLayoutEntry& entry,
                         const ReflectedBinding& binding,
                         bool dynamic_offset = false,
                         wgpu::TextureSampleType texture_sample_override =
                             wgpu::TextureSampleType::Undefined) {
  entry.binding = binding.binding;
  entry.visibility = binding.visibility;

  if (binding.is_storage_texture) {
    entry.storageTexture.format = binding.storage_format;
    entry.storageTexture.access = binding.storage_access;
    entry.storageTexture.viewDimension = binding.texture_dimension;
  } else if (binding.buffer_type != wgpu::BufferBindingType::Undefined) {
    entry.buffer.type = binding.buffer_type;
    entry.buffer.hasDynamicOffset =
        dynamic_offset &&
        binding.buffer_type == wgpu::BufferBindingType::Uniform;
    entry.buffer.minBindingSize = 0;
  } else if (binding.texture_type != wgpu::TextureSampleType::Undefined) {
    entry.texture.sampleType =
        texture_sample_override != wgpu::TextureSampleType::Undefined
            ? texture_sample_override
            : binding.texture_type;
    entry.texture.viewDimension = binding.texture_dimension;
    entry.texture.multisampled = false;
  } else if (binding.sampler_type != wgpu::SamplerBindingType::Undefined) {
    entry.sampler.type = binding.sampler_type;
  }
}

}  // namespace

wgpu::BindGroupLayout CreateLayoutFromReflection(
    wgpu::Device device, const std::vector<ReflectedBinding>& bindings) {
  std::vector<wgpu::BindGroupLayoutEntry> entries;

  for (const auto& binding : bindings) {
    wgpu::BindGroupLayoutEntry entry{};
    PopulateLayoutEntry(entry, binding);
    entries.push_back(entry);
  }

  wgpu::BindGroupLayoutDescriptor desc{};
  desc.entryCount = entries.size();
  desc.entries = entries.data();

  return device.CreateBindGroupLayout(&desc);
}

std::vector<wgpu::BindGroupLayout> CreateLayoutsFromReflection(
    wgpu::Device device, const std::vector<ReflectedBinding>& bindings,
    LayoutGenerationOptions options) {
  // Find max group number
  uint32_t max_group = 0;
  for (const auto& binding : bindings) {
    max_group = std::max(max_group, binding.group);
  }

  // Group bindings by group number
  std::vector<std::vector<ReflectedBinding>> bindings_per_group(max_group + 1);
  for (const auto& binding : bindings) {
    bindings_per_group[binding.group].push_back(binding);
  }

  // Create layout for each group
  std::vector<wgpu::BindGroupLayout> layouts(max_group + 1);
  for (uint32_t group = 0; group <= max_group; ++group) {
    const auto& group_bindings = bindings_per_group[group];
    if (group_bindings.empty()) {
      layouts[group] = nullptr;
      continue;
    }

    std::vector<wgpu::BindGroupLayoutEntry> entries;
    for (const auto& binding : group_bindings) {
      wgpu::BindGroupLayoutEntry entry{};
      wgpu::TextureSampleType override_type = wgpu::TextureSampleType::Undefined;
      auto it = options.texture_sample_type_overrides.find(
          {binding.group, binding.binding});
      if (it != options.texture_sample_type_overrides.end()) {
        override_type = it->second;
      }
      PopulateLayoutEntry(
          entry, binding,
          /*dynamic_offset=*/(options.force_group1_dynamic_offsets && group == 1),
          /*texture_sample_override=*/override_type);
      entries.push_back(entry);
    }

    wgpu::BindGroupLayoutDescriptor desc{};
    desc.entryCount = entries.size();
    desc.entries = entries.data();

    layouts[group] = device.CreateBindGroupLayout(&desc);
  }

  return layouts;
}

std::vector<ReflectedUniformBuffer> ReflectUniforms(
    const std::string& wgsl_source) {
  WgslUniformReflectionResult result =
      wgsl_reflect_uniforms(wgsl_source.c_str());

  if (result.error != nullptr) {
    spdlog::error("WGSL uniform reflection error: {}", result.error);
    wgsl_free_uniform_reflection(result);
    return {};
  }

  std::vector<ReflectedUniformBuffer> buffers;
  buffers.reserve(result.buffer_count);

  for (size_t i = 0; i < result.buffer_count; i++) {
    const WgslUniformBuffer& b = result.buffers[i];
    ReflectedUniformBuffer buffer;
    buffer.group = b.group;
    buffer.binding = b.binding;
    buffer.name = b.name ? b.name : "";
    buffer.total_size = b.total_size;

    // Convert members
    buffer.members.reserve(b.member_count);
    for (size_t j = 0; j < b.member_count; j++) {
      const WgslUniformMember& m = b.members[j];
      UniformMember member;
      member.name = m.name ? m.name : "";
      member.offset = m.offset;
      member.size = m.size;

      // Map type ID to UniformType
      switch (m.type_id) {
        case WGSL_UNIFORM_TYPE_INT:
          member.type = UniformType::Int;
          break;
        case WGSL_UNIFORM_TYPE_UINT:
          member.type = UniformType::UInt;
          break;
        case WGSL_UNIFORM_TYPE_FLOAT:
          member.type = UniformType::Float;
          break;
        case WGSL_UNIFORM_TYPE_VEC2:
          member.type = UniformType::Vec2;
          break;
        case WGSL_UNIFORM_TYPE_VEC3:
          member.type = UniformType::Vec3;
          break;
        case WGSL_UNIFORM_TYPE_VEC4:
          member.type = UniformType::Vec4;
          break;
        case WGSL_UNIFORM_TYPE_MAT4:
          member.type = UniformType::Mat4;
          break;
        default:
          member.type = UniformType::Unknown;
          break;
      }

      buffer.members.push_back(member);
    }

    buffers.push_back(buffer);
  }

  wgsl_free_uniform_reflection(result);
  return buffers;
}

namespace {

// Map FFI vertex format constant to WebGPU VertexFormat
// Returns std::nullopt for unknown/undefined formats
std::optional<wgpu::VertexFormat> WgslFormatToVertexFormat(uint32_t format) {
  switch (format) {
    case WGSL_VERTEX_FORMAT_FLOAT32: return wgpu::VertexFormat::Float32;
    case WGSL_VERTEX_FORMAT_FLOAT32X2: return wgpu::VertexFormat::Float32x2;
    case WGSL_VERTEX_FORMAT_FLOAT32X3: return wgpu::VertexFormat::Float32x3;
    case WGSL_VERTEX_FORMAT_FLOAT32X4: return wgpu::VertexFormat::Float32x4;
    case WGSL_VERTEX_FORMAT_SINT32: return wgpu::VertexFormat::Sint32;
    case WGSL_VERTEX_FORMAT_SINT32X2: return wgpu::VertexFormat::Sint32x2;
    case WGSL_VERTEX_FORMAT_SINT32X3: return wgpu::VertexFormat::Sint32x3;
    case WGSL_VERTEX_FORMAT_SINT32X4: return wgpu::VertexFormat::Sint32x4;
    case WGSL_VERTEX_FORMAT_UINT32: return wgpu::VertexFormat::Uint32;
    case WGSL_VERTEX_FORMAT_UINT32X2: return wgpu::VertexFormat::Uint32x2;
    case WGSL_VERTEX_FORMAT_UINT32X3: return wgpu::VertexFormat::Uint32x3;
    case WGSL_VERTEX_FORMAT_UINT32X4: return wgpu::VertexFormat::Uint32x4;
    default: return std::nullopt;
  }
}

}  // namespace

std::vector<ReflectedVertexInput> ReflectVertexInputs(
    const std::string& wgsl_source) {
  WgslVertexInputReflectionResult result =
      wgsl_reflect_vertex_inputs(wgsl_source.c_str());

  if (result.error != nullptr) {
    spdlog::error("WGSL vertex input reflection error: {}", result.error);
    wgsl_free_vertex_input_reflection(result);
    return {};
  }

  std::vector<ReflectedVertexInput> inputs;
  inputs.reserve(result.input_count);

  for (size_t i = 0; i < result.input_count; i++) {
    const WgslVertexInput& vi = result.inputs[i];
    auto format = WgslFormatToVertexFormat(vi.format);
    if (!format) {
      spdlog::warn("Unknown vertex format {} for input '{}'", vi.format,
                   vi.name ? vi.name : "(unnamed)");
      continue;
    }
    ReflectedVertexInput input;
    input.location = vi.location;
    input.name = vi.name ? vi.name : "";
    input.format = *format;
    inputs.push_back(input);
  }

  wgsl_free_vertex_input_reflection(result);
  return inputs;
}

namespace {

const char* VertexFormatToString(wgpu::VertexFormat format) {
  switch (format) {
    case wgpu::VertexFormat::Float32: return "Float32";
    case wgpu::VertexFormat::Float32x2: return "Float32x2";
    case wgpu::VertexFormat::Float32x3: return "Float32x3";
    case wgpu::VertexFormat::Float32x4: return "Float32x4";
    case wgpu::VertexFormat::Sint32: return "Sint32";
    case wgpu::VertexFormat::Sint32x2: return "Sint32x2";
    case wgpu::VertexFormat::Sint32x3: return "Sint32x3";
    case wgpu::VertexFormat::Sint32x4: return "Sint32x4";
    case wgpu::VertexFormat::Uint32: return "Uint32";
    case wgpu::VertexFormat::Uint32x2: return "Uint32x2";
    case wgpu::VertexFormat::Uint32x3: return "Uint32x3";
    case wgpu::VertexFormat::Uint32x4: return "Uint32x4";
    default: return "Unknown";
  }
}

}  // namespace

std::string ValidateVertexInputs(
    const std::vector<ReflectedVertexInput>& shader_inputs,
    const std::vector<wgpu::VertexAttribute>& expected_attrs) {
  // Check attribute count
  if (shader_inputs.size() != expected_attrs.size()) {
    return fmt::format(
        "Vertex attribute count mismatch: shader expects {} inputs, "
        "layout provides {}",
        shader_inputs.size(), expected_attrs.size());
  }

  // Build map of shader inputs by location for comparison
  std::map<uint32_t, const ReflectedVertexInput*> shader_by_loc;
  for (const auto& input : shader_inputs) {
    shader_by_loc[input.location] = &input;
  }

  // Validate each expected attribute
  for (const auto& attr : expected_attrs) {
    auto it = shader_by_loc.find(attr.shaderLocation);
    if (it == shader_by_loc.end()) {
      return fmt::format("Shader missing vertex input at location {}",
                         attr.shaderLocation);
    }
    const auto* shader_input = it->second;
    if (shader_input->format != attr.format) {
      return fmt::format(
          "Vertex format mismatch at location {}: shader expects {}, "
          "layout provides {}",
          attr.shaderLocation, VertexFormatToString(shader_input->format),
          VertexFormatToString(attr.format));
    }
  }

  return "";  // Success
}

std::vector<ReflectedFragmentOutput> ReflectFragmentOutputs(
    const std::string& wgsl_source) {
  WgslFragmentOutputReflectionResult result =
      wgsl_reflect_fragment_outputs(wgsl_source.c_str());

  if (result.error != nullptr) {
    spdlog::error("WGSL fragment output reflection error: {}", result.error);
    wgsl_free_fragment_output_reflection(result);
    return {};
  }

  std::vector<ReflectedFragmentOutput> outputs;
  outputs.reserve(result.output_count);

  for (size_t i = 0; i < result.output_count; i++) {
    const WgslFragmentOutput& fo = result.outputs[i];
    ReflectedFragmentOutput output;
    output.location = fo.location;
    output.name = fo.name ? fo.name : "";
    output.component_count = fo.component_count;
    outputs.push_back(output);
  }

  wgsl_free_fragment_output_reflection(result);
  return outputs;
}

uint32_t GetFormatComponentCount(wgpu::TextureFormat format) {
  switch (format) {
    case wgpu::TextureFormat::R8Unorm:
    case wgpu::TextureFormat::R8Snorm:
    case wgpu::TextureFormat::R16Float:
    case wgpu::TextureFormat::R32Float:
      return 1;
    case wgpu::TextureFormat::RG8Unorm:
    case wgpu::TextureFormat::RG8Snorm:
    case wgpu::TextureFormat::RG16Float:
    case wgpu::TextureFormat::RG32Float:
      return 2;
    default:
      return 4;  // RGBA8Unorm, BGRA8Unorm, RGBA16Float, etc.
  }
}

std::string ValidateFragmentOutputs(
    const std::vector<ReflectedFragmentOutput>& outputs,
    const std::vector<wgpu::TextureFormat>& color_formats) {
  if (color_formats.empty()) {
    return "";  // Shadow pass or no color targets — always valid
  }

  if (outputs.size() < color_formats.size()) {
    return fmt::format(
        "Fragment output count mismatch: shader has {} outputs, "
        "render target expects {} color targets",
        outputs.size(), color_formats.size());
  }

  for (size_t i = 0; i < color_formats.size(); i++) {
    uint32_t format_components = GetFormatComponentCount(color_formats[i]);
    // Find output at location i
    const ReflectedFragmentOutput* match = nullptr;
    for (const auto& output : outputs) {
      if (output.location == static_cast<uint32_t>(i)) {
        match = &output;
        break;
      }
    }
    if (!match) {
      return fmt::format(
          "No fragment output at location {} for color target", i);
    }
    if (match->component_count < format_components) {
      return fmt::format(
          "Fragment output '{}' at location {} has {} components, "
          "but color format requires {}",
          match->name, i, match->component_count, format_components);
    }
  }

  return "";  // Success
}

}  // namespace badlands
