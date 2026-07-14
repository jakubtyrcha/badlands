#pragma once

// Ported from sampo's src/rendering/material/material.{hpp,cpp}, namespace
// sampo -> badlands.
//
// KEY reconciliation vs. sampo: C2 (gpu_pipeline_generator.hpp) kept only the
// GpuPipelineGenerator's *sync on-demand* `GetPipeline(RenderPipelineDeclaration,
// RenderTargetFormats)` API and dropped sampo's *async* material-registration
// API (`RegisterMaterialPipelines`/`PipelineHandle`/`MaterialPipelineConfig`/
// `GetRegisteredPipeline`/taskflow). sampo's `MeshRenderingMaterial` was a thin
// wrapper that looked up *pre-registered* pipelines by name (built via
// `BuildPipelineName` + `context_->GetRegisteredPipeline(name)`,
// `GetBindGroupLayout(name, group)`, `GetUniformBuffers(name)`, ...).
//
// This port instead makes `MeshRenderingMaterial` build a
// `RenderPipelineDeclaration` on demand from (geometry, pass, config) and call
// the generator's sync `GetPipeline` directly — compiling (and caching, inside
// `GpuPipelineGenerator`'s own declaration-hash cache) the requested variant
// the first time it's needed. No taskflow, no name-keyed registry: the
// declaration hash *is* the cache key. Because of this:
// - `MeshRenderingMaterial::IsValid()` now means "configured" (has a
//   generator + shader path), not "at least one variant already compiled" —
//   compilation is lazy, so nothing has necessarily been attempted yet.
//   Callers that need to know whether a *specific* (geometry, pass) variant
//   actually compiles must call `GetPipeline`/`GetCompiledPipeline` and check
//   the result, exactly as `StandardMaterialFactory::CreateInstance` does.
// - The legacy no-arg `GetPipeline()` (sampo's "use base name directly")
//   isn't ported: it relied purely on the name-keyed registry and nothing in
//   sampo's material/ directory itself ever called it (verified by grep) —
//   only the always-parameterized `GetPipeline(geometry, pass, config)` is
//   exercised by `StandardMaterialFactory`/`StandardRenderingMaterialInstance`.
// - Per-render-pass target formats (color formats / depth format), which
//   sampo resolved indirectly via `RenderStateHandle`/`RenderStateProvider`
//   (not ported — see the gpu_pipeline_generator.hpp deviation note), are
//   supplied directly as a `TargetConfig` per `RenderPassType` at
//   construction time (see `BuildMaterialInstanceFactory` in
//   standard_material_factory.cpp, which populates this from
//   `FactoryDescriptor::color_formats`/`depth_format` instead of a
//   `RenderStateId`). Stage 1 has no G-buffer/shadow renderer, so callers
//   populate this uniformly for now; the per-pass map is kept so a future
//   RenderState port can differentiate gbuffer/shadow/forward targets without
//   reshaping this class.
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>
#include <dawn/webgpu_cpp.h>

#include "core/geometry_type.hpp"
#include "engine/rendering/shader/gpu_pipeline_generator.hpp"

namespace badlands {

// Render pass type determines which shader variant to use
enum class RenderPassType {
  kGBuffer,  // Full material output (normals, albedo, roughness)
  kShadow,   // Depth-only pass
  kForward   // Forward rendering
};

// Material pass type determines which render pass the material goes to
enum class MaterialPassType {
  kDeferred,           // G-buffer pass (default) - processed by deferred lighting
  kForwardOpaque,      // Forward opaque pass - renders directly after deferred
  kForwardTransparent  // Forward transparent pass - renders last with blending
};

// Render configuration options
struct RenderConfig {
  bool wireframe = false;
  // Future: MSAA, etc.
};

// Shader program reference (path + entry point)
struct ShaderProgram {
  std::string path;   // e.g., "material/textured_mesh.wesl"
  std::string entry;  // e.g., "vs_main"
};

// Material: Scene-level definition specifying shader programs.
// No vertex layout or features - material adapts to mesh type at render time.
struct Material {
  ShaderProgram vertex_program;
  ShaderProgram fragment_program;
  MaterialPassType pass_type = MaterialPassType::kDeferred;
};

// MeshRenderingMaterial compiles (lazily, on demand) the shader variant for a
// given (geometry, pass, config) combination via GpuPipelineGenerator's sync
// GetPipeline, and exposes the resulting pipeline / bind group layouts /
// reflection data. This is the compiled/runtime representation of a material.
class MeshRenderingMaterial {
 public:
  // Render-target configuration for one RenderPassType. `color_formats` empty
  // means a depth-only pass (e.g. shadow).
  struct TargetConfig {
    RenderTargetFormats color_formats;
    wgpu::TextureFormat depth_format = wgpu::TextureFormat::Undefined;
    bool depth_write = true;
    wgpu::CompareFunction depth_compare = wgpu::CompareFunction::Less;
  };

  MeshRenderingMaterial() = default;
  MeshRenderingMaterial(GpuPipelineGenerator* generator, std::string name,
                        std::string shader_path, std::string vs_entry,
                        std::string fs_entry,
                        std::vector<std::string> base_features,
                        bool blend_enabled, bool premultiplied_alpha,
                        std::map<RenderPassType, TargetConfig> pass_targets,
                        wgpu::CullMode cull_mode = wgpu::CullMode::Back);

  // Check if material is configured (has a generator + shader path). Does NOT
  // imply any variant has successfully compiled — compilation is lazy; see
  // the file-level comment above.
  bool IsValid() const;

  // Get the compiled pipeline bundle (pipeline + layouts + reflection) for a
  // specific geometry/pass/config combination, compiling it on first use.
  std::shared_ptr<const CompiledPipeline> GetCompiledPipeline(
      GeometryType geometry_type, RenderPassType pass_type,
      const RenderConfig& config = {}) const;

  // Get pipeline for specific geometry/pass/config combination.
  wgpu::RenderPipeline GetPipeline(GeometryType geometry_type,
                                   RenderPassType pass_type,
                                   const RenderConfig& config = {}) const;

  // Get bind group layout for specific geometry/pass combination.
  wgpu::BindGroupLayout GetBindGroupLayout(GeometryType geometry_type,
                                           RenderPassType pass_type,
                                           uint32_t group = 0) const;

  // Get expected binding indices for a specific geometry/pass/group
  // combination, from shader reflection.
  std::set<uint32_t> GetExpectedBindings(GeometryType geometry_type,
                                         RenderPassType pass_type,
                                         uint32_t group = 0) const;

  // Get uniform buffer reflection data (for MaterialInstance).
  const std::vector<ReflectedUniformBuffer>& GetUniformBuffers(
      GeometryType geometry_type, RenderPassType pass_type) const;

  // Get reflected bindings for auto-deriving texture requirements.
  const std::vector<ReflectedBinding>& GetReflectedBindings(
      GeometryType geometry_type, RenderPassType pass_type) const;

  // Get material name (for debugging)
  const std::string& GetName() const { return name_; }

 private:
  RenderPipelineDeclaration BuildDeclaration(GeometryType geometry_type,
                                             RenderPassType pass_type,
                                             const RenderConfig& config) const;

  GpuPipelineGenerator* generator_ = nullptr;
  std::string name_;  // Base material name (e.g., "textured_mesh"), debug only
  std::string shader_path_;
  std::string vs_entry_ = "vs_main";
  std::string fs_entry_ = "fs_main";
  std::vector<std::string> base_features_;
  bool blend_enabled_ = false;
  bool premultiplied_alpha_ = false;
  std::map<RenderPassType, TargetConfig> pass_targets_;
  wgpu::CullMode cull_mode_ = wgpu::CullMode::Back;
};

}  // namespace badlands
