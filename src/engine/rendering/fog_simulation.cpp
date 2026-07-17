#include "engine/rendering/fog_simulation.hpp"

#include <algorithm>

namespace badlands {
namespace {

// GPU-side mirror of shaders/common/fog_emitters.wesl's GpuEmitter. vec2 members
// sit at 8-aligned offsets; all others are 4-byte scalars → 80-byte stride,
// matching WGSL's array<GpuEmitter> layout.
struct GpuEmitter {
  glm::vec2 center;
  glm::vec2 half_extent;
  float rotation;
  float base_y;
  float height;
  uint32_t shape;
  uint32_t type;
  float magnitude;
  float radial_falloff;
  float vertical_falloff;
  float noise_freq;
  float noise_contrast;
  float scroll_x, scroll_y, scroll_z;
  float seed;
  float pad0, pad1;
};
static_assert(sizeof(GpuEmitter) == 80, "GpuEmitter must match WGSL");

wgpu::Buffer MakeStorage(wgpu::Device device, wgpu::Queue queue, const void* data,
                         size_t size) {
  wgpu::BufferDescriptor bd;
  bd.size = std::max<size_t>(16u, (size + 15u) & ~size_t{15u});
  bd.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst;
  wgpu::Buffer buf = device.CreateBuffer(&bd);
  if (data && size > 0) queue.WriteBuffer(buf, 0, data, size);
  return buf;
}

}  // namespace

void FogSimulation::Initialize(wgpu::Device device, wgpu::Queue queue) {
  device_ = device;
  queue_ = queue;
}

void FogSimulation::SetSources(std::span<const fog::Emitter> emitters,
                               const FogSimParams& params) {
  fog::Broadphase bp = fog::BuildBroadphase(emitters, params.map_min,
                                            params.map_max, params.bp_cell_size);
  bp_min_ = params.map_min;
  bp_cell_size_ = params.bp_cell_size;
  bp_nx_ = bp.nx;
  bp_nz_ = bp.nz;
  emitter_count_ = static_cast<uint32_t>(emitters.size());

  std::vector<GpuEmitter> packed(std::max<size_t>(1, emitters.size()));
  for (size_t i = 0; i < emitters.size(); ++i) {
    const fog::Emitter& e = emitters[i];
    GpuEmitter& g = packed[i];
    g.center = e.center;
    g.half_extent = e.half_extent;
    g.rotation = e.rotation;
    g.base_y = e.base_y;
    g.height = e.height;
    g.shape = static_cast<uint32_t>(e.shape);
    g.type = static_cast<uint32_t>(e.type);
    g.magnitude = e.magnitude;
    g.radial_falloff = e.radial_falloff;
    g.vertical_falloff = e.vertical_falloff;
    g.noise_freq = e.noise_freq;
    g.noise_contrast = e.noise_contrast;
    g.scroll_x = e.scroll.x;
    g.scroll_y = e.scroll.y;
    g.scroll_z = e.scroll.z;
    g.seed = e.seed;
    g.pad0 = g.pad1 = 0.0f;
  }
  emitter_buf_ = MakeStorage(device_, queue_, packed.data(),
                             packed.size() * sizeof(GpuEmitter));
  // glm::ivec2 (offset,count) is bit-identical to WGSL vec2<u32> for the
  // non-negative values stored.
  bp_cells_buf_ = MakeStorage(device_, queue_, bp.cells.data(),
                              bp.cells.size() * sizeof(glm::ivec2));
  bp_indices_buf_ = MakeStorage(device_, queue_, bp.indices.data(),
                                bp.indices.size() * sizeof(uint32_t));
  valid_ = true;
}

}  // namespace badlands
