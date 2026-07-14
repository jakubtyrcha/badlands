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
#include "engine/rendering/brdf_lut.hpp"
#include "engine/rendering/gbuffer.hpp"
#include "engine/rendering/material/material_instance_cache.hpp"
#include "engine/rendering/prefiltered_cubemap.hpp"
#include "engine/rendering/shader/gpu_pipeline_generator.hpp"

namespace badlands {

struct SceneContext;

// G-buffer debug visualization modes (Task S2.B4). When the SceneRenderer's
// mode is not kNone, the final surface resolve runs
// shaders/passes/gbuffer_debug.wesl (visualizing the selected channel)
// INSTEAD OF the tonemap pass — see SceneRenderer::Render's Pass 4 and
// engine/rendering/passes/render_gbuffer_debug.hpp.
enum class GBufferDebugMode {
  None,
  Depth,      // linearized depth, grayscale
  Normals,    // decoded world normal, n*0.5+0.5
  Albedo,     // raw (unlit) albedo
  Roughness,  // material.r, grayscale
  Hdr,        // the lit HDR buffer, tonemapped — same as kNone, a convenience
              // passthrough so the selector can cover the whole HDR->surface
              // resolve without a separate "off" state.
};

// Deferred renderer: G-buffer geometry pass -> deferred lighting (sun + SH
// ambient) into an HDR target -> fullscreen tonemap resolve to the surface.
//
// Usage:
//   SceneRenderer renderer;
//   renderer.Initialize(gpu.GetDevice(), gpu.GetQueue(), &pipeline_gen,
//                       gpu.GetSurfaceFormat(), width, height,
//                       gpu.HasR8UnormStorage());
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
  // has_r8unorm_storage: GpuContext::HasR8UnormStorage() — whether the
  // device has the TextureFormatsTier1 feature (R8Unorm storage-texture
  // support). Recorded as has_r8unorm_storage_; Stage 3 M6's GTAO compute
  // pass gates its AO texture's StorageBinding usage (and whether GTAO runs
  // at all) on this.
  void Initialize(wgpu::Device device, wgpu::Queue queue,
                  GpuPipelineGenerator* pipeline_gen,
                  wgpu::TextureFormat surface_format, uint32_t width,
                  uint32_t height, bool has_r8unorm_storage);

  // Recreates the HDR + G-buffer targets at a new size (e.g. on
  // SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED). No-op if unchanged.
  void Resize(uint32_t width, uint32_t height);

  // Renders one frame: geometry into the G-buffer (kGBuffer variant), a
  // fullscreen deferred-lighting pass (sun + SH ambient) into the HDR
  // target, then a fullscreen tonemap resolve HDR -> surface_view.
  // surface_view must be a view of a texture with
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

  // G-buffer debug visualization (Task S2.B4). SetDebugMode takes effect on
  // the next Render() call.
  void SetDebugMode(GBufferDebugMode mode) { debug_mode_ = mode; }
  GBufferDebugMode GetDebugMode() const { return debug_mode_; }

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
  // Lazily (re)builds prefiltered_ from scene.skybox_cubemap when the source
  // view or generation changes. No-op when unchanged.
  void UpdateIbl(const SceneContext& scene);

  wgpu::Device device_;
  wgpu::Queue queue_;
  GpuPipelineGenerator* pipeline_generator_ = nullptr;

  wgpu::TextureFormat surface_format_ = wgpu::TextureFormat::BGRA8Unorm;
  wgpu::TextureFormat accumulation_format_ = kAccumulationFormat;

  uint32_t width_ = 0;
  uint32_t height_ = 0;

  // HDR accumulation target (deferred lighting resolves here).
  wgpu::Texture hdr_color_texture_;
  wgpu::TextureView hdr_color_view_;

  // Deferred G-buffer: 3 MRT color targets + reversed-Z depth. The depth
  // target doubles as this renderer's depth buffer (sampled by the deferred
  // lighting pass) — there is no separate standalone depth texture.
  GBuffer gbuffer_;

  // Resolved-material-instance cache, shared across the forward pass's
  // draw calls (and across frames — keyed by factory + geometry + pass +
  // texture/uniform config, see material_instance_cache.hpp).
  MaterialInstanceCache material_instance_cache_;

  // === IBL (S2.B2) ===
  // BRDF split-sum LUT — generated once in Initialize (view-independent).
  BrdfLut brdf_lut_;
  // Prefiltered specular environment — regenerated from scene.skybox_cubemap
  // when its generation changes (see UpdateIbl). May be invalid (no skybox).
  PrefilteredCubemap prefiltered_;
  // 1x1 black fallback cube + sampler so the deferred-lighting IBL bindings
  // (@9/@10) are ALWAYS valid even without a skybox (or with IBL disabled).
  wgpu::Texture fallback_cube_texture_;
  wgpu::TextureView fallback_cube_view_;
  wgpu::Sampler fallback_cube_sampler_;
  // Tracks the source cube (view + generation) prefiltered_ was built from.
  wgpu::TextureView ibl_source_view_;
  uint32_t ibl_source_generation_ = 0;
  bool has_prefiltered_ = false;

  // === G-buffer debug (S2.B4) ===
  GBufferDebugMode debug_mode_ = GBufferDebugMode::None;

  uint32_t min_uniform_offset_alignment_ = 256;

  // See Initialize()'s has_r8unorm_storage param.
  bool has_r8unorm_storage_ = false;
};

}  // namespace badlands
