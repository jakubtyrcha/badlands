// Ported from sampo's src/rendering/shader/gpu_pipeline_generator.cpp,
// namespace sampo -> badlands, trimmed to the sync on-demand API — see the
// deviation note in gpu_pipeline_generator.hpp for why the async
// material-registration API (RegisterPipeline/PipelineHandle/taskflow/...)
// isn't ported here.
#include "engine/rendering/shader/gpu_pipeline_generator.hpp"

#include <algorithm>

#include <spdlog/spdlog.h>

#include "wesl_ffi.h"

namespace badlands {

namespace {

static size_t HashCombine(size_t seed, size_t value) {
  return seed ^ (value + 0x9e3779b9 + (seed << 6) + (seed >> 2));
}

}  // namespace

GpuPipelineGenerator::GpuPipelineGenerator(wgpu::Device device,
                                           const std::string& shader_base_dir)
    : device_(device) {
  SetShaderDirectory(shader_base_dir);
}

// ============================================================================
// Shader directory management
// ============================================================================

void GpuPipelineGenerator::SetShaderDirectory(const std::string& path) {
  shader_directory_ = path;
  if (!shader_directory_.empty() && shader_directory_.back() != '/') {
    shader_directory_ += '/';
  }
}

void GpuPipelineGenerator::AddShaderDirectory(const std::string& path) {
  std::string dir = path;
  if (!dir.empty() && dir.back() != '/') {
    dir += '/';
  }
  additional_shader_directories_.push_back(std::move(dir));
}

// ============================================================================
// Sync on-demand API (for ProcessingGraph nodes, post-processing)
// ============================================================================

std::shared_ptr<const CompiledPipeline> GpuPipelineGenerator::GetPipeline(
    const RenderPipelineDeclaration& decl,
    const RenderTargetFormats& target_formats) {
  size_t decl_hash = HashDeclaration(decl);
  PipelineCacheKey key{decl_hash, target_formats};

  auto it = render_cache_.find(key);
  if (it != render_cache_.end()) {
    return it->second;
  }

  // Compile WESL -> WGSL
  std::vector<const char*> feature_ptrs;
  for (const auto& f : decl.features) {
    feature_ptrs.push_back(f.c_str());
  }

  std::vector<const char*> dir_ptrs;
  for (const auto& d : additional_shader_directories_) {
    dir_ptrs.push_back(d.c_str());
  }

  WeslCompileResult wesl_result = wesl_compile_file_with_dirs(
      shader_directory_.c_str(), decl.shader_path.c_str(),
      feature_ptrs.empty() ? nullptr : feature_ptrs.data(),
      feature_ptrs.size(),
      dir_ptrs.empty() ? nullptr : dir_ptrs.data(), dir_ptrs.size());

  if (wesl_result.error) {
    spdlog::error("GpuPipelineGenerator: Failed to compile {}: {}",
                  decl.shader_path, wesl_result.error);
    wesl_free_result(wesl_result);
    return nullptr;
  }

  std::string wgsl(wesl_result.wgsl);
  wesl_free_result(wesl_result);

  // Create shader module
  WGPUShaderSourceWGSL wgsl_desc = {};
  wgsl_desc.chain.sType = WGPUSType_ShaderSourceWGSL;
  wgsl_desc.code =
      WGPUStringView{.data = wgsl.c_str(), .length = wgsl.length()};

  wgpu::ShaderModuleDescriptor module_desc;
  module_desc.nextInChain = reinterpret_cast<wgpu::ChainedStruct*>(&wgsl_desc);
  auto shader_module = device_.CreateShaderModule(&module_desc);

  if (!shader_module) {
    spdlog::error(
        "GpuPipelineGenerator: Failed to create shader module: {}",
        decl.shader_path);
    return nullptr;
  }

  // Reflect bindings and uniforms
  auto reflected_bindings = ReflectShader(wgsl);
  auto uniform_buffers = ReflectUniforms(wgsl);
  auto bind_group_layouts = CreateLayoutsFromReflection(
      device_, reflected_bindings,
      {.force_group1_dynamic_offsets = true,
       .texture_sample_type_overrides = decl.texture_sample_type_overrides});

  // Create pipeline layout
  wgpu::PipelineLayoutDescriptor layout_desc;
  layout_desc.bindGroupLayoutCount = bind_group_layouts.size();
  layout_desc.bindGroupLayouts = bind_group_layouts.empty() ? nullptr : bind_group_layouts.data();
  wgpu::PipelineLayout pipeline_layout =
      device_.CreatePipelineLayout(&layout_desc);

  // Build vertex buffer layout
  VertexLayoutInfo vertex_info = GetVertexLayoutInfo(decl.vertex_layout);
  wgpu::VertexBufferLayout vertex_buffer_layout;
  if (decl.vertex_layout != VertexLayout::kFullscreen) {
    vertex_buffer_layout.arrayStride = vertex_info.stride;
    vertex_buffer_layout.stepMode = vertex_info.step_mode;
    vertex_buffer_layout.attributeCount = vertex_info.attributes.size();
    vertex_buffer_layout.attributes = vertex_info.attributes.data();
  }

  // Build render pipeline descriptor
  wgpu::RenderPipelineDescriptor pipeline_desc;
  pipeline_desc.layout = pipeline_layout;

  pipeline_desc.vertex.module = shader_module;
  pipeline_desc.vertex.entryPoint = WGPUStringView{
      .data = decl.vs_entry.c_str(), .length = decl.vs_entry.length()};
  if (decl.vertex_layout != VertexLayout::kFullscreen) {
    pipeline_desc.vertex.bufferCount = 1;
    pipeline_desc.vertex.buffers = &vertex_buffer_layout;
  }

  // Fragment state
  wgpu::FragmentState fragment_state;
  fragment_state.module = shader_module;
  fragment_state.entryPoint = WGPUStringView{.data = decl.fs_entry.c_str(),
                                             .length = decl.fs_entry.length()};

  // Color targets (MRT-ready)
  std::vector<wgpu::ColorTargetState> color_targets;
  std::vector<wgpu::BlendState> blend_states;
  color_targets.reserve(target_formats.size());
  blend_states.reserve(target_formats.size());

  for (const auto& format : target_formats) {
    wgpu::ColorTargetState color_target;
    color_target.format = format;
    color_target.writeMask = wgpu::ColorWriteMask::All;

    if (decl.custom_blend) {
      blend_states.push_back(*decl.custom_blend);
      color_target.blend = &blend_states.back();
    } else if (decl.blend_enabled) {
      wgpu::BlendState blend;
      blend.color.srcFactor = decl.premultiplied_alpha
                                  ? wgpu::BlendFactor::One
                                  : wgpu::BlendFactor::SrcAlpha;
      blend.color.dstFactor = wgpu::BlendFactor::OneMinusSrcAlpha;
      blend.color.operation = wgpu::BlendOperation::Add;
      blend.alpha.srcFactor = wgpu::BlendFactor::One;
      blend.alpha.dstFactor = wgpu::BlendFactor::OneMinusSrcAlpha;
      blend.alpha.operation = wgpu::BlendOperation::Add;
      blend_states.push_back(blend);
      color_target.blend = &blend_states.back();
    }

    color_targets.push_back(color_target);
  }

  fragment_state.targetCount = color_targets.size();
  fragment_state.targets = color_targets.data();
  pipeline_desc.fragment = &fragment_state;

  // Depth stencil
  wgpu::DepthStencilState depth_stencil;
  if (decl.depth_format != wgpu::TextureFormat::Undefined) {
    depth_stencil.format = decl.depth_format;
    depth_stencil.depthWriteEnabled =
        decl.depth_write ? WGPUOptionalBool_True : WGPUOptionalBool_False;
    depth_stencil.depthCompare = decl.depth_compare;
    pipeline_desc.depthStencil = &depth_stencil;
  } else {
    pipeline_desc.depthStencil = nullptr;
  }

  // Primitive state
  pipeline_desc.primitive.topology = decl.topology;
  pipeline_desc.primitive.cullMode = decl.cull_mode;
  pipeline_desc.primitive.frontFace = wgpu::FrontFace::CCW;

  // Multisample
  pipeline_desc.multisample.count = 1;
  pipeline_desc.multisample.mask = ~0u;
  pipeline_desc.multisample.alphaToCoverageEnabled = false;

  wgpu::RenderPipeline pipeline = device_.CreateRenderPipeline(&pipeline_desc);
  if (!pipeline) {
    spdlog::error("GpuPipelineGenerator: Failed to create pipeline: {}",
                  decl.shader_path);
    return nullptr;
  }

  auto compiled = std::make_shared<CompiledPipeline>(CompiledPipeline{
      .pipeline = pipeline,
      .pipeline_layout = pipeline_layout,
      .bind_group_layouts = std::move(bind_group_layouts),
      .reflected_bindings = std::move(reflected_bindings),
      .uniform_buffers = std::move(uniform_buffers),
  });

  render_cache_[key] = compiled;
  return compiled;
}

std::shared_ptr<const CompiledComputePipeline>
GpuPipelineGenerator::GetComputePipeline(
    const std::string& shader_path, const std::vector<std::string>& features) {
  size_t key = HashComputeKey(shader_path, features);

  auto it = compute_cache_.find(key);
  if (it != compute_cache_.end()) {
    return it->second;
  }

  // Compile WESL -> WGSL
  std::vector<const char*> feature_ptrs;
  for (const auto& f : features) {
    feature_ptrs.push_back(f.c_str());
  }

  std::vector<const char*> dir_ptrs;
  for (const auto& d : additional_shader_directories_) {
    dir_ptrs.push_back(d.c_str());
  }

  WeslCompileResult wesl_result = wesl_compile_file_with_dirs(
      shader_directory_.c_str(), shader_path.c_str(),
      feature_ptrs.empty() ? nullptr : feature_ptrs.data(),
      feature_ptrs.size(),
      dir_ptrs.empty() ? nullptr : dir_ptrs.data(), dir_ptrs.size());

  if (wesl_result.error) {
    spdlog::error("GpuPipelineGenerator: Failed to compile compute {}: {}",
                  shader_path, wesl_result.error);
    wesl_free_result(wesl_result);
    return nullptr;
  }

  std::string wgsl(wesl_result.wgsl);
  wesl_free_result(wesl_result);

  // Create shader module
  WGPUShaderSourceWGSL wgsl_desc = {};
  wgsl_desc.chain.sType = WGPUSType_ShaderSourceWGSL;
  wgsl_desc.code =
      WGPUStringView{.data = wgsl.c_str(), .length = wgsl.length()};

  wgpu::ShaderModuleDescriptor module_desc;
  module_desc.nextInChain = reinterpret_cast<wgpu::ChainedStruct*>(&wgsl_desc);
  auto shader_module = device_.CreateShaderModule(&module_desc);

  if (!shader_module) {
    spdlog::error(
        "GpuPipelineGenerator: Failed to create compute shader module: {}",
        shader_path);
    return nullptr;
  }

  // Reflect workgroup size
  WgslReflectionResult reflect_result = wgsl_reflect(wgsl.c_str());
  uint32_t ws[3] = {8, 8, 1};
  if (!reflect_result.error) {
    ws[0] = reflect_result.workgroup_size[0];
    ws[1] = reflect_result.workgroup_size[1];
    ws[2] = reflect_result.workgroup_size[2];
  }
  wgsl_free_reflection(reflect_result);

  // Reflect bindings
  auto reflected_bindings = ReflectShader(wgsl);

  // Create compute pipeline
  wgpu::ComputePipelineDescriptor pipeline_desc;
  pipeline_desc.compute.module = shader_module;
  pipeline_desc.compute.entryPoint = WGPUStringView{"main", 4};

  auto pipeline = device_.CreateComputePipeline(&pipeline_desc);
  if (!pipeline) {
    spdlog::error(
        "GpuPipelineGenerator: Failed to create compute pipeline: {}",
        shader_path);
    return nullptr;
  }

  // Extract bind group layouts directly from the pipeline.
  // Auto-layout pipelines require bind groups to use the pipeline's own
  // layouts; reflection-created layouts are NOT compatible.
  uint32_t max_group = 0;
  for (const auto& ref : reflected_bindings) {
    max_group = std::max(max_group, ref.group);
  }
  std::vector<wgpu::BindGroupLayout> bind_group_layouts;
  for (uint32_t g = 0; g <= max_group; ++g) {
    bind_group_layouts.push_back(pipeline.GetBindGroupLayout(g));
  }

  auto compiled =
      std::make_shared<CompiledComputePipeline>(CompiledComputePipeline{
          .pipeline = pipeline,
          .layout = pipeline.GetBindGroupLayout(0),
          .bind_group_layouts = std::move(bind_group_layouts),
          .workgroup_size = {ws[0], ws[1], ws[2]},
          .reflected_bindings = std::move(reflected_bindings),
      });

  compute_cache_[key] = compiled;
  return compiled;
}

void GpuPipelineGenerator::InvalidateAll() {
  render_cache_.clear();
  compute_cache_.clear();
}

// ============================================================================
// Shared shader utilities
// ============================================================================

wgpu::ShaderModule GpuPipelineGenerator::CreateShaderModule(
    const std::string& source) {
  WGPUShaderSourceWGSL wgsl_desc = {};
  wgsl_desc.chain.sType = WGPUSType_ShaderSourceWGSL;
  wgsl_desc.code =
      WGPUStringView{.data = source.c_str(), .length = source.length()};

  wgpu::ShaderModuleDescriptor desc;
  desc.nextInChain = reinterpret_cast<const wgpu::ChainedStruct*>(&wgsl_desc);
  return device_.CreateShaderModule(&desc);
}

// ============================================================================
// Hash helpers for sync cache
// ============================================================================

size_t GpuPipelineGenerator::HashDeclaration(
    const RenderPipelineDeclaration& decl) const {
  size_t h = std::hash<std::string>{}(decl.shader_path);
  h = HashCombine(h, std::hash<std::string>{}(decl.vs_entry));
  h = HashCombine(h, std::hash<std::string>{}(decl.fs_entry));
  h = HashCombine(h, std::hash<int>{}(static_cast<int>(decl.vertex_layout)));
  h = HashCombine(h, std::hash<int>{}(static_cast<int>(decl.topology)));
  h = HashCombine(h, std::hash<int>{}(static_cast<int>(decl.cull_mode)));
  h = HashCombine(h, std::hash<bool>{}(decl.blend_enabled));
  h = HashCombine(h, std::hash<bool>{}(decl.premultiplied_alpha));
  if (decl.custom_blend) {
    h = HashCombine(h, std::hash<int>{}(static_cast<int>(decl.custom_blend->color.operation)));
    h = HashCombine(h, std::hash<int>{}(static_cast<int>(decl.custom_blend->color.srcFactor)));
    h = HashCombine(h, std::hash<int>{}(static_cast<int>(decl.custom_blend->color.dstFactor)));
  }
  h = HashCombine(h, std::hash<bool>{}(decl.depth_write));
  h = HashCombine(h, std::hash<int>{}(static_cast<int>(decl.depth_compare)));
  h = HashCombine(h, std::hash<int>{}(static_cast<int>(decl.depth_format)));
  h = HashCombine(h, decl.features.size());
  for (const auto& f : decl.features) {
    h = HashCombine(h, std::hash<std::string>{}(f));
  }
  // Texture sample overrides change the bind group layout. `std::map` iteration
  // is in sorted key order, so two declarations with the same overrides hash
  // identically.
  h = HashCombine(h, decl.texture_sample_type_overrides.size());
  for (const auto& [key, sample_type] : decl.texture_sample_type_overrides) {
    h = HashCombine(h, std::hash<uint32_t>{}(key.first));
    h = HashCombine(h, std::hash<uint32_t>{}(key.second));
    h = HashCombine(h, std::hash<int>{}(static_cast<int>(sample_type)));
  }
  return h;
}

size_t GpuPipelineGenerator::HashComputeKey(
    const std::string& shader_path,
    const std::vector<std::string>& features) const {
  size_t h = std::hash<std::string>{}(shader_path);
  h = HashCombine(h, features.size());
  for (const auto& f : features) {
    h = HashCombine(h, std::hash<std::string>{}(f));
  }
  return h;
}

// ============================================================================
// Free functions for bind group creation
// ============================================================================

wgpu::BindGroup CreateBindGroup(wgpu::Device device,
                                const CompiledPipeline& pipeline,
                                uint32_t group,
                                std::span<const wgpu::BindGroupEntry> entries) {
  wgpu::BindGroupDescriptor bg_desc;
  bg_desc.layout = pipeline.bind_group_layouts[group];
  bg_desc.entryCount = entries.size();
  bg_desc.entries = entries.data();
  return device.CreateBindGroup(&bg_desc);
}

wgpu::BindGroup CreateComputeBindGroup(
    wgpu::Device device, const CompiledComputePipeline& pipeline,
    std::span<const wgpu::BindGroupEntry> entries) {
  wgpu::BindGroupDescriptor bg_desc;
  bg_desc.layout = pipeline.layout;
  bg_desc.entryCount = entries.size();
  bg_desc.entries = entries.data();
  return device.CreateBindGroup(&bg_desc);
}

}  // namespace badlands
