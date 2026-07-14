#pragma once

// Ported from sampo's src/rendering/material/material_instance_factory.hpp,
// namespace sampo -> badlands.
//
// Deviation: `RenderStateProvider* state_provider` is dropped from
// `BuildMaterialInstanceFactory`. sampo used it (+ `RenderStateId`) to
// resolve per-MaterialPassType render-target formats and blend state
// indirectly; that render-state system isn't ported to badlands yet (see the
// deviation note in gpu_pipeline_generator.hpp). `FactoryDescriptor` now
// carries `color_formats`/`depth_format` directly instead — see the
// deviation note in material.hpp (`MeshRenderingMaterial::TargetConfig`) and
// standard_material_factory.cpp (`BuildMaterialInstanceFactory`).
#include <memory>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#include <dawn/webgpu_cpp.h>

#include "engine/rendering/material/material_requirements.hpp"
#include "engine/rendering/material/rendering_material_instance.hpp"
#include "engine/rendering/shader/gpu_pipeline_generator.hpp"

namespace badlands {

class GpuPipelineGenerator;
class ScriptTextureProvider;

// === Recipe Items ===

struct DefaultTextureView {
  std::string param_name;
  wgpu::TextureView view;
  wgpu::Sampler sampler;
  TextureType type{TextureType::k2D};
};

struct NoiserMaterialScript {
  std::string param_name;
  std::string source;
  int resolution{64};
  std::unordered_map<std::string, MaterialParameterValue> params;
};

using RecipeItem = std::variant<DefaultTextureView, NoiserMaterialScript>;

// === Instance Parameters (per-instance overrides) ===

struct InstanceParams {
  std::vector<DefaultTextureView>
      texture_overrides;  // precedence: overrides > recipes > defaults
  std::unordered_map<std::string, MaterialParameterValue> uniform_overrides;
};

// === Factory Interface ===

class MaterialInstanceFactory {
 public:
  virtual ~MaterialInstanceFactory() = default;

  // MaterialPassType selects the material variant (blend state, shader features).
  // RenderPassType selects the pipeline within that variant (gbuffer, shadow, forward).
  // Returns nullptr if the combination is not available.
  virtual std::unique_ptr<RenderingMaterialInstance> CreateInstance(
      GeometryType geometry_type, MaterialPassType material_pass,
      RenderPassType render_pass,
      const InstanceParams& params = {}) = 0;
};

// === Factory Descriptor (data-driven factory construction) ===

struct FactoryDescriptor {
  std::string shader_name;
  std::string shader_path;
  std::string vs_entry = "vs_main";
  std::string fs_entry = "fs_main";
  std::vector<RecipeItem> recipes;
  // If non-empty, only register these pass type variants (default: all 3)
  std::vector<MaterialPassType> supported_pass_types;
  // If non-empty, only register these geometry types (default: both)
  std::vector<GeometryType> supported_geometry_types;

  // Render-target formats used to compile pipelines. Stage 1 has no
  // RenderState/RenderTarget port (see the deviation note above) — these
  // stand in for sampo's RenderStateId-driven format resolution. Defaults to
  // a single BGRA8Unorm color target (the typical swapchain format; see
  // GpuContext::GetSurfaceFormat) with no depth buffer.
  RenderTargetFormats color_formats = {wgpu::TextureFormat::BGRA8Unorm};
  wgpu::TextureFormat depth_format = wgpu::TextureFormat::Undefined;
};

// Build factory from descriptor. script_provider required when
// NoiserMaterialScript recipes are present (errors if null + script recipes).
std::unique_ptr<MaterialInstanceFactory> BuildMaterialInstanceFactory(
    const FactoryDescriptor& desc, wgpu::Device device, wgpu::Queue queue,
    GpuPipelineGenerator* shader_context,
    ScriptTextureProvider* script_provider = nullptr);

}  // namespace badlands
