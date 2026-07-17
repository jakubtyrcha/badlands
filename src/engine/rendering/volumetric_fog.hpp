#pragma once

// VolumetricFog — world-space, toroidally-addressed terrain fog (Task: fog
// rendering). Owns the media cascade 3D texture + sampler and drives two GPU
// steps each frame, recorded onto the caller's FrameContext:
//   1. fog_fill (compute)  — write media (scattering sigma_s + extinction
//      sigma_t) into the newly-scrolled voxel columns of the height-band
//      clipmap (procedural placeholder; a real sim would write the same
//      texture).
//   2. fog (fullscreen)    — ray-march the cascades to scene depth, lighting
//      each step (sun phase + SH ambient), and blend the premultiplied
//      (in-scatter, transmittance) result into the HDR target in place.
//
// Bespoke pass (not a MeshRenderingMaterial), wired directly to
// GpuPipelineGenerator — mirrors the GTAO compute pass. Game-agnostic; owned
// by SceneRenderer and invoked between deferred lighting and tonemap.
#include <cstdint>
#include <vector>

#include <dawn/webgpu_cpp.h>
#include <glm/glm.hpp>

#include "engine/rendering/fog_cascade.hpp"
#include "engine/rendering/fog_config.hpp"

namespace badlands {

class GpuPipelineGenerator;
class FrameContext;
class GpuTimer;
struct Camera;

class VolumetricFog {
 public:
  // device/gen: non-owning GPU handles (must outlive this). hdr_format: the
  // format of the HDR target the composite pass blends into (== the renderer's
  // accumulation format).
  void Initialize(wgpu::Device device, GpuPipelineGenerator* gen,
                  wgpu::TextureFormat hdr_format);

  void SetConfig(const FogConfig& config) { config_ = config; }
  const FogConfig& GetConfig() const { return config_; }
  FogConfig& MutableConfig() { return config_; }  // for the ImGui editor

  // Records the fill (compute) + composite (fullscreen blend into hdr_view)
  // onto `frame`. No-op when disabled / uninitialized / on pipeline-compile
  // failure. depth_view is the scene (G-buffer) reversed-Z depth;
  // shadow_view/shadow_sampler are the directional shadow map + its comparison
  // sampler (for light shafts, valid within the shadow box; lit outside).
  void Render(FrameContext& frame, GpuTimer& gpu_timer, const Camera& camera,
              wgpu::TextureView hdr_view, wgpu::TextureView depth_view,
              wgpu::TextureView shadow_view, wgpu::Sampler shadow_sampler,
              uint32_t width, uint32_t height);

 private:
  // (Re)creates the media 3D texture when the cascade dimensions change; flags
  // a full refill when geometry (extent/height/floor) changes.
  void EnsureTextures();
  // (Re)creates the fog raymarch target at `render_w`x`render_h` (full or
  // half screen) when the size changes.
  void EnsureFogTarget(uint32_t render_w, uint32_t render_h);

  wgpu::Device device_;
  GpuPipelineGenerator* pipeline_generator_ = nullptr;
  wgpu::TextureFormat hdr_format_ = wgpu::TextureFormat::RGBA16Float;

  FogConfig config_;

  fog::CascadeLayout built_layout_{};  // layout the current texture was built for
  bool textures_valid_ = false;
  wgpu::Texture media_texture_;
  wgpu::TextureView media_view_;
  wgpu::Sampler media_sampler_;

  std::vector<glm::ivec2> last_min_voxel_;  // per-cascade toroidal window origin
  bool force_fill_ = true;
  bool logged_compile_error_ = false;

  // Fog raymarch target (RGBA16Float: rgb = in-scatter, a = transmittance);
  // full or half screen. Upsampled + blended into HDR by the composite pass.
  wgpu::Texture fog_target_texture_;
  wgpu::TextureView fog_target_view_;
  uint32_t fog_target_w_ = 0;
  uint32_t fog_target_h_ = 0;

  uint32_t frame_index_ = 0;  // animates the spatial jitter
};

}  // namespace badlands
