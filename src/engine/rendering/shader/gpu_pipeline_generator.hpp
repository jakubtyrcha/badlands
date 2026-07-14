#pragma once

// Ported from sampo's src/rendering/shader/gpu_pipeline_generator.{hpp,cpp},
// namespace sampo -> badlands.
//
// Deviation from sampo: this port keeps only the *sync on-demand* pipeline
// API (GetPipeline/GetComputePipeline, used by post-processing/fullscreen
// passes) and drops sampo's *async material-registration* API
// (RegisterPipeline/RegisterMaterialPipelines/PipelineHandle/
// MaterialPipelineConfig/PipelineCompatibilityParams/wireframe recompile).
// That surface pulls in sampo's material system (rendering/material/
// material.hpp), scene geometry config (world/config/geometry_config.hpp),
// render-state provider (rendering/state/render_state.hpp,
// rendering/state/render_target.hpp) and a taskflow-based async compile
// executor — none of which exist in badlands yet (no material/ECS system has
// been ported in Stage 1 up to this task), and taskflow isn't vendored here
// (third_party/ has spdlog/glm/entt/catch2/noiser only). The sync API is
// self-contained (only needs vertex_layout.hpp + shader_reflection.hpp) and
// is exactly what a lower-level "compile this shader into a pipeline, give
// me back reflected bind group layouts" API needs; the async registration
// system lands with the later material/ECS re-platform tasks.
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>
#include <dawn/webgpu_cpp.h>

#include "engine/rendering/shader/shader_reflection.hpp"
#include "engine/rendering/vertex_layout.hpp"

namespace badlands {

// Declaration for the sync on-demand pipeline API.
struct RenderPipelineDeclaration {
  std::string shader_path;
  std::string vs_entry = "vs_main";
  std::string fs_entry = "fs_main";
  VertexLayout vertex_layout = VertexLayout::kFullscreen;
  wgpu::PrimitiveTopology topology = wgpu::PrimitiveTopology::TriangleList;
  wgpu::CullMode cull_mode = wgpu::CullMode::None;
  bool blend_enabled = false;
  bool premultiplied_alpha = false;
  // Explicit blend state — overrides blend_enabled/premultiplied_alpha.
  std::optional<wgpu::BlendState> custom_blend;
  bool depth_write = false;
  wgpu::CompareFunction depth_compare = wgpu::CompareFunction::Always;
  wgpu::TextureFormat depth_format = wgpu::TextureFormat::Undefined;
  std::vector<std::string> features;

  // Override the reflected `TextureSampleType` for specific sampled-texture
  // bindings. Keyed by `(group, binding)`. Used when binding an R32Float
  // (or other unfilterable) texture view to a slot that the shader reflection
  // classifies as `Float` (filterable) because wesl_ffi cannot see the view
  // format. See `LayoutGenerationOptions::texture_sample_type_overrides`.
  std::map<std::pair<uint32_t, uint32_t>, wgpu::TextureSampleType>
      texture_sample_type_overrides;
};

struct CompiledPipeline {
  wgpu::RenderPipeline pipeline;
  wgpu::PipelineLayout pipeline_layout;
  std::vector<wgpu::BindGroupLayout> bind_group_layouts;
  std::vector<ReflectedBinding> reflected_bindings;
  std::vector<ReflectedUniformBuffer> uniform_buffers;
};

struct CompiledComputePipeline {
  wgpu::ComputePipeline pipeline;
  wgpu::BindGroupLayout layout;                          // group 0 (backward compat)
  std::vector<wgpu::BindGroupLayout> bind_group_layouts; // ALL groups
  uint32_t workgroup_size[3] = {8, 8, 1};
  std::vector<ReflectedBinding> reflected_bindings;
};

using RenderTargetFormats = std::vector<wgpu::TextureFormat>;

// GPU pipeline generator: compiles a WESL shader (via wesl_ffi) to WGSL,
// reflects its bindings/uniforms/vertex-inputs/fragment-outputs (via naga,
// also through wesl_ffi), and builds the corresponding Dawn pipeline +
// bind group layouts. Results are cached by declaration hash.
class GpuPipelineGenerator {
 public:
  GpuPipelineGenerator(wgpu::Device device, const std::string& shader_base_dir);

  GpuPipelineGenerator(const GpuPipelineGenerator&) = delete;
  GpuPipelineGenerator& operator=(const GpuPipelineGenerator&) = delete;

  std::shared_ptr<const CompiledPipeline> GetPipeline(
      const RenderPipelineDeclaration& decl,
      const RenderTargetFormats& target_formats);

  std::shared_ptr<const CompiledComputePipeline> GetComputePipeline(
      const std::string& shader_path,
      const std::vector<std::string>& features = {});

  // Shader directory management
  void SetShaderDirectory(const std::string& path);
  const std::string& GetShaderDirectory() const { return shader_directory_; }
  void AddShaderDirectory(const std::string& path);

  // Drops all cached pipelines (e.g. on shader hot-reload).
  void InvalidateAll();

 private:
  wgpu::ShaderModule CreateShaderModule(const std::string& source);

  // Hash helpers for sync cache
  using PipelineCacheKey = std::pair<size_t, RenderTargetFormats>;
  size_t HashDeclaration(const RenderPipelineDeclaration& decl) const;
  size_t HashComputeKey(const std::string& shader_path,
                        const std::vector<std::string>& features) const;

  wgpu::Device device_;
  std::string shader_directory_;
  std::vector<std::string> additional_shader_directories_;

  std::map<PipelineCacheKey, std::shared_ptr<CompiledPipeline>> render_cache_;
  std::map<size_t, std::shared_ptr<CompiledComputePipeline>> compute_cache_;
};

// Free functions for bind group creation
wgpu::BindGroup CreateBindGroup(
    wgpu::Device device, const CompiledPipeline& pipeline, uint32_t group,
    std::span<const wgpu::BindGroupEntry> entries);

wgpu::BindGroup CreateComputeBindGroup(
    wgpu::Device device, const CompiledComputePipeline& pipeline,
    std::span<const wgpu::BindGroupEntry> entries);

}  // namespace badlands
