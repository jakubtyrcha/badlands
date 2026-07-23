#pragma once

// Fog-of-war overlay: the game's implementation of the engine's generic
// ScenePostPass hook (engine/rendering/scene_post_pass.hpp). It owns the vision
// texture (RG8: R=discovered, G=visible) uploaded from the sim's VisionField,
// and a fullscreen pipeline (shaders/game/vision.wesl) that, run late in the
// HDR pipeline, blacks out terra-incognita and desaturates dormant terrain.
//
// The vision field lives in the SIM coordinate frame; the game supplies the
// sim->world offset (rendered_world = sim_pos + offset) so the pass maps a
// reconstructed world position back into the grid before sampling.

#include <dawn/webgpu_cpp.h>
#include <glm/glm.hpp>

#include "badlands_sim.hpp"  // VisionField
#include "engine/rendering/scene_post_pass.hpp"

namespace badlands {

class GpuPipelineGenerator;

class VisionOverlayPass : public ScenePostPass {
 public:
  bool Initialize(wgpu::Device device, wgpu::Queue queue,
                  GpuPipelineGenerator* pipeline_gen);

  // rendered_world = sim_pos + offset. Set once from the app's sim->render map.
  void SetSimToWorldOffset(glm::vec2 offset) { sim_to_world_ = offset; }

  bool& mutable_enabled() { return enabled_; }
  bool enabled() const { return enabled_; }

  // (Re)size + upload the latest field. Cheap when the size is unchanged. A
  // null/empty field leaves the pass a no-op (Execute renders nothing).
  void Upload(const VisionField& field);

  // ScenePostPass: modulate the scene HDR colour by the vision texture.
  void Execute(const PostSceneContext& ctx) override;

 private:
  wgpu::Device device_;
  wgpu::Queue queue_;
  GpuPipelineGenerator* pipeline_gen_ = nullptr;

  wgpu::Sampler sampler_;
  wgpu::Texture texture_;
  wgpu::TextureView view_;
  int nx_ = 0, nz_ = 0;

  // world->uv mapping derived from the field + sim_to_world_ each Upload().
  glm::vec2 origin_{0.0f};    // grid world min (sim min + offset)
  glm::vec2 inv_size_{0.0f};  // 1 / (n * texel) per axis
  glm::vec2 sim_to_world_{0.0f};

  bool enabled_ = true;
  bool has_field_ = false;
};

}  // namespace badlands
