#pragma once

// Trimmed port of sampo's src/rendering/scene_renderer.{hpp,cpp}, namespace
// sampo -> badlands.
//
// Sampo's `SceneRenderer` is a large multi-pass deferred renderer: shadow
// map, G-buffer, skybox/IBL, contact shadows, GTAO, deferred lighting
// (composed via a taskflow-esque `ProcessingGraph`), forward
// opaque/transparent, water, debug spheres/G-buffer visualization, text/
// smoke overlays, bloom, spark overlay, ray-trace overlay, ImGui, then
// tonemapping. Per the D3 task brief, this port keeps ONLY:
//   - an HDR (Rgba16Float) accumulation target + reversed-Z depth
//     (Depth32Float) buffer, sized to the surface;
//   - a single forward-opaque pass (`RenderTexturedMeshes`, this task's
//     `rendering/passes/render_textured_mesh.cpp`) that clears and renders
//     into the HDR target;
//   - a fullscreen tonemap resolve (HDR -> the presentation surface),
//     hand-rolled here instead of via sampo's `ProcessingGraph`/
//     `TonemappingNode` (neither ported — see below).
//
// Everything else — G-buffer/deferred lighting, shadow map, GTAO, contact
// shadows, skybox, water, bloom, spark overlay, text/smoke, ImGui, the
// `ProcessingGraph` post-processing composer itself, horizon mapping, the
// mesh-cluster page pool, BRDF LUT / prefiltered cubemap (IBL), and render-
// target readback helpers (CpuImage/ColorRenderTarget/TextureReadback) — is
// omitted outright, not stubbed. A future task porting one of those
// subsystems re-adds the relevant members/passes without reshaping this
// class (mirrors the trim precedent set by D2's `SceneContext`).
#include <cstdint>
#include <entt/entt.hpp>
#include <dawn/webgpu_cpp.h>

#include "engine/core/camera.hpp"
#include "engine/rendering/material/material_instance_cache.hpp"
#include "engine/rendering/shader/gpu_pipeline_generator.hpp"

namespace badlands {

struct SceneContext;

// Forward-opaque + tonemap renderer: the whole Stage-1 render path.
//
// Usage:
//   SceneRenderer renderer;
//   renderer.Initialize(gpu.GetDevice(), gpu.GetQueue(), &pipeline_gen,
//                       gpu.GetSurfaceFormat(), width, height);
//   ...
//   renderer.Render(camera, registry, scene_context, surface_view);
class SceneRenderer {
 public:
  SceneRenderer() = default;

  // Non-copyable (owns GPU textures + a MaterialInstanceCache holding live
  // pipeline/bind-group state).
  SceneRenderer(const SceneRenderer&) = delete;
  SceneRenderer& operator=(const SceneRenderer&) = delete;

  // Movable.
  SceneRenderer(SceneRenderer&&) = default;
  SceneRenderer& operator=(SceneRenderer&&) = default;

  // device/queue: GPU handles (non-owning, must outlive this).
  // pipeline_gen: shared shader/pipeline cache (non-owning, must outlive
  // this) — used to compile the tonemap fullscreen pipeline on demand;
  // forward-mesh pipelines are compiled indirectly via each entity's
  // MaterialInstanceFactory (which owns its own MeshRenderingMaterial(s)
  // wrapping this same generator).
  // surface_format: the presentation surface's texture format — the
  // tonemap pass's resolve target format.
  // width/height: initial HDR/depth target size (surface pixel size).
  void Initialize(wgpu::Device device, wgpu::Queue queue,
                  GpuPipelineGenerator* pipeline_gen,
                  wgpu::TextureFormat surface_format, uint32_t width,
                  uint32_t height);

  // Recreates the HDR/depth targets at a new size (e.g. on
  // SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED). No-op if unchanged.
  void Resize(uint32_t width, uint32_t height);

  // Renders one frame: clears + runs the forward-opaque textured-mesh pass
  // into the HDR target, then resolves HDR -> surface_view with a
  // fullscreen tonemap pass. surface_view must be a view of a texture with
  // this renderer's configured surface_format and (width, height) (from
  // Initialize/Resize) — e.g. GpuContext::AcquireSurfaceTexture()'s result.
  void Render(const Camera& camera, entt::registry& registry,
              const SceneContext& scene, wgpu::TextureView surface_view);

  wgpu::TextureFormat GetAccumulationFormat() const {
    return accumulation_format_;
  }
  MaterialInstanceCache& GetMaterialInstanceCache() {
    return material_instance_cache_;
  }

  // HDR accumulation target format and reversed-Z depth-buffer format —
  // fixed constants in this trimmed renderer (not configurable via
  // Initialize). Exposed so callers can build MaterialInstanceFactory
  // pipelines (FactoryDescriptor::color_formats/depth_format) with matching
  // render-target formats before a SceneRenderer instance exists — see
  // build_test_scene()'s factory setup in main.cpp.
  static constexpr wgpu::TextureFormat kAccumulationFormat =
      wgpu::TextureFormat::RGBA16Float;
  static constexpr wgpu::TextureFormat kDepthFormat =
      wgpu::TextureFormat::Depth32Float;

 private:
  void CreateTargets(uint32_t width, uint32_t height);

  wgpu::Device device_;
  wgpu::Queue queue_;
  GpuPipelineGenerator* pipeline_generator_ = nullptr;

  wgpu::TextureFormat surface_format_ = wgpu::TextureFormat::BGRA8Unorm;
  wgpu::TextureFormat accumulation_format_ = kAccumulationFormat;

  uint32_t width_ = 0;
  uint32_t height_ = 0;

  // HDR accumulation target (forward pass renders here).
  wgpu::Texture hdr_color_texture_;
  wgpu::TextureView hdr_color_view_;

  // Reversed-Z depth buffer.
  wgpu::Texture depth_texture_;
  wgpu::TextureView depth_view_;

  // Resolved-material-instance cache, shared across the forward pass's
  // draw calls (and across frames — keyed by factory + geometry + pass +
  // texture/uniform config, see material_instance_cache.hpp).
  MaterialInstanceCache material_instance_cache_;

  uint32_t min_uniform_offset_alignment_ = 256;
};

}  // namespace badlands
