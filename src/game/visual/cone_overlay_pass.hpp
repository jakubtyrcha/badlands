#pragma once

// Gameplay debug overlay: draws each unit's vision cone as a flat, alpha-blended
// triangular polygon floating just above the terrain. The game's implementation
// of the engine's generic ScenePostPass hook (scene_post_pass.hpp); it owns a
// pipeline built from shaders/game/overlay.wesl (kPolygon layout: pos + rgba)
// and a per-frame vertex buffer of cone triangles the game fills from the sim
// snapshot. Depth-tested against the G-buffer (so buildings occlude it) but
// never depth-writing. Off by default; toggled from the GameView debug panel.

#include <cstdint>
#include <vector>

#include <dawn/webgpu_cpp.h>

#include "engine/rendering/scene_post_pass.hpp"

namespace badlands {

class GpuPipelineGenerator;

class ConeOverlayPass : public ScenePostPass {
 public:
  bool Initialize(wgpu::Device device, wgpu::Queue queue,
                  GpuPipelineGenerator* pipeline_gen);

  bool& mutable_enabled() { return enabled_; }
  bool enabled() const { return enabled_; }

  // Replace the triangle soup to draw. `verts` is the kPolygon layout: 7 floats
  // per vertex (pos.xyz, rgba), 3 vertices per triangle. Uploaded on the next
  // Execute. An empty soup makes the pass a no-op.
  void SetTriangles(std::vector<float> verts) { pending_ = std::move(verts); }

  void Execute(const PostSceneContext& ctx) override;

 private:
  wgpu::Device device_;
  wgpu::Queue queue_;
  GpuPipelineGenerator* pipeline_gen_ = nullptr;

  wgpu::Buffer vbuf_;
  uint64_t vbuf_capacity_ = 0;  // bytes
  std::vector<float> pending_;

  bool enabled_ = false;  // a debug layer: off until the user toggles it
};

}  // namespace badlands
