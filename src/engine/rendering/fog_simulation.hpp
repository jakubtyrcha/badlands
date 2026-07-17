#pragma once

// FogSimulation — the map fog generator's source manager (Task: map fog
// generator). Owns the emitter + broadphase GPU buffers and a time accumulator.
// The composer (compute/fog_fill.wesl) reads these each frame and writes the fog
// media; this class runs no compute pass of its own ("the sim" only drives the
// animation time). Game-agnostic: consumes world-static emitters as plain data.
#include <cstdint>
#include <span>
#include <vector>

#include <dawn/webgpu_cpp.h>
#include <glm/glm.hpp>

#include "engine/rendering/fog_sim.hpp"

namespace badlands {

struct FogSimParams {
  glm::vec2 map_min{-64.0f, -64.0f};
  glm::vec2 map_max{64.0f, 64.0f};
  float bp_cell_size = 32.0f;  // world-space broadphase cell (m)
};

class FogSimulation {
 public:
  void Initialize(wgpu::Device device, wgpu::Queue queue);
  bool IsValid() const { return valid_; }

  // (Re)build the emitter + broadphase GPU buffers from a world-static set.
  // Emitters are static, so this is called once (or on change), not per frame.
  void SetSources(std::span<const fog::Emitter> emitters,
                  const FogSimParams& params);

  // Accumulate sim-time (seconds; fed by the game's SimClock delta) for emitter
  // animation. No spatial state — the composed volume at a given Time() is exact.
  void AddTime(float sim_dt) { time_ += sim_dt; }
  float Time() const { return time_; }

  // Composer inputs.
  wgpu::Buffer EmitterBuffer() const { return emitter_buf_; }
  wgpu::Buffer BpCellsBuffer() const { return bp_cells_buf_; }
  wgpu::Buffer BpIndicesBuffer() const { return bp_indices_buf_; }
  glm::vec2 BpMin() const { return bp_min_; }
  float BpCellSize() const { return bp_cell_size_; }
  int BpNx() const { return bp_nx_; }
  int BpNz() const { return bp_nz_; }
  uint32_t EmitterCount() const { return emitter_count_; }

 private:
  wgpu::Device device_;
  wgpu::Queue queue_;
  bool valid_ = false;

  wgpu::Buffer emitter_buf_;     // GpuEmitter[]
  wgpu::Buffer bp_cells_buf_;    // vec2<u32> (offset,count)[]
  wgpu::Buffer bp_indices_buf_;  // u32[]
  glm::vec2 bp_min_{0.0f, 0.0f};
  float bp_cell_size_ = 32.0f;
  int bp_nx_ = 0;
  int bp_nz_ = 0;
  uint32_t emitter_count_ = 0;

  float time_ = 0.0f;
};

}  // namespace badlands
