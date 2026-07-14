#pragma once

// Ported from sampo's src/rendering/shader/shader_reflection.hpp, namespace
// sampo -> badlands (verbatim otherwise).

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>
#include <dawn/webgpu_cpp.h>

namespace badlands {

// Type of a uniform buffer member
enum class UniformType : uint32_t {
  Int = 0,
  UInt = 1,
  Float = 2,
  Vec2 = 3,
  Vec3 = 4,
  Vec4 = 5,
  Mat4 = 6,
  Unknown = 255,
};

// A single member of a uniform buffer struct
struct UniformMember {
  std::string name;
  uint32_t offset;
  uint32_t size;
  UniformType type;
};

// A uniform buffer with its struct members
struct ReflectedUniformBuffer {
  uint32_t group;
  uint32_t binding;
  std::string name;
  std::vector<UniformMember> members;
  uint32_t total_size;
};

// Extracted from shader reflection
struct ReflectedBinding {
  uint32_t group;
  uint32_t binding;
  std::string name;
  wgpu::BufferBindingType buffer_type = wgpu::BufferBindingType::Undefined;
  wgpu::TextureSampleType texture_type = wgpu::TextureSampleType::Undefined;
  wgpu::TextureViewDimension texture_dimension =
      wgpu::TextureViewDimension::Undefined;
  wgpu::SamplerBindingType sampler_type = wgpu::SamplerBindingType::Undefined;
  wgpu::ShaderStage visibility = wgpu::ShaderStage::None;
  // Storage texture fields (only valid when is_storage_texture is true)
  bool is_storage_texture = false;
  wgpu::TextureFormat storage_format = wgpu::TextureFormat::Undefined;
  wgpu::StorageTextureAccess storage_access =
      wgpu::StorageTextureAccess::Undefined;
};

// Parse WGSL source and extract bind group layout information
std::vector<ReflectedBinding> ReflectShader(const std::string& wgsl_source);

// Create bind group layout from reflected bindings (legacy - assumes all group
// 0)
wgpu::BindGroupLayout CreateLayoutFromReflection(
    wgpu::Device device, const std::vector<ReflectedBinding>& bindings);

// Options for bind group layout generation
struct LayoutGenerationOptions {
  // Render pipelines use dynamic offsets on group 1 for per-object uniforms.
  // Compute pipelines don't use dynamic offsets.
  bool force_group1_dynamic_offsets = false;

  // Override the reflected `TextureSampleType` for specific bindings.
  //
  // Why this exists: wesl_ffi's shader reflection classifies any sampled f32
  // texture as `TextureSampleType::Float` (filterable) without consulting the
  // view's actual format. An R32Float texture view is `UnfilterableFloat` —
  // binding it to a `Float` layout is a WebGPU validation error without the
  // `float32-filterable` feature enabled. Callers that sample R32Float (or
  // any other unfilterable format) via `textureLoad` only must override the
  // reflected sample type here.
  //
  // Key: `(group, binding)`. Value: the replacement `TextureSampleType`.
  // Non-matching bindings pass through unchanged.
  std::map<std::pair<uint32_t, uint32_t>, wgpu::TextureSampleType>
      texture_sample_type_overrides;
};

// Create bind group layouts for all groups found in reflected bindings
// Returns a vector indexed by group number (sparse groups will have null
// layouts)
std::vector<wgpu::BindGroupLayout> CreateLayoutsFromReflection(
    wgpu::Device device, const std::vector<ReflectedBinding>& bindings,
    LayoutGenerationOptions options = {});

// Parse WGSL source and extract uniform buffer member information
std::vector<ReflectedUniformBuffer> ReflectUniforms(
    const std::string& wgsl_source);

// ============================================================================
// Vertex Input Reflection
// ============================================================================

// A vertex input extracted from shader (e.g., @location(0) pos: vec3<f32>)
struct ReflectedVertexInput {
  uint32_t location;
  std::string name;
  wgpu::VertexFormat format;
};

// Parse WGSL source and extract vertex input locations and formats.
// Looks for @location(N) annotations in structs used as vertex shader inputs.
std::vector<ReflectedVertexInput> ReflectVertexInputs(
    const std::string& wgsl_source);

// Validate that shader vertex inputs match the expected layout.
// Returns empty string on success, or error message describing the mismatch.
std::string ValidateVertexInputs(
    const std::vector<ReflectedVertexInput>& shader_inputs,
    const std::vector<wgpu::VertexAttribute>& expected_attrs);

// ============================================================================
// Fragment Output Reflection
// ============================================================================

// A fragment output extracted from shader (e.g., @location(0) -> vec4<f32>)
struct ReflectedFragmentOutput {
  uint32_t location;
  std::string name;
  uint32_t component_count;  // 1-4
};

// Parse WGSL source and extract fragment output locations and component counts.
std::vector<ReflectedFragmentOutput> ReflectFragmentOutputs(
    const std::string& wgsl_source);

// Get the number of components for a texture format (e.g., RGBA16Float -> 4).
uint32_t GetFormatComponentCount(wgpu::TextureFormat format);

// Validate that shader fragment outputs match render target color formats.
// Returns empty string on success, or error message describing the mismatch.
std::string ValidateFragmentOutputs(
    const std::vector<ReflectedFragmentOutput>& outputs,
    const std::vector<wgpu::TextureFormat>& color_formats);

}  // namespace badlands
