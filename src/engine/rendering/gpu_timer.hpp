#pragma once

// GpuTimer — per-pass GPU timing via WebGPU timestamp queries.
//
// The renderer wraps each pass: attach BeginPass("name") to the pass
// descriptor's timestampWrites, then ResolveFrame() once after all passes.
// Every ~120 frames one frame's timestamps are blocking-read and a per-pass
// breakdown is printed (that frame skips its resolve to avoid a write/map
// hazard). Inert unless Initialize() ran with timestamp support (the profiling
// build calls SceneRenderer::EnableGpuProfiling); zero cost otherwise.
#include <cstdint>
#include <iosfwd>
#include <string>
#include <vector>

#include <dawn/webgpu_cpp.h>

namespace badlands {

class GpuTimer {
 public:
  // Creates the timestamp QuerySet + resolve/readback buffers. No-op (stays
  // disabled) if `supported` is false. `instance` is pumped while awaiting the
  // periodic readback map.
  void Initialize(wgpu::Instance instance, wgpu::Device device, bool supported,
                  uint32_t max_passes = 12);
  bool enabled() const { return enabled_; }

  // Call once at the start of each frame's encoding.
  void BeginFrame();
  // Register the next pass; returns the timestampWrites to attach to that
  // pass's descriptor, or nullptr (disabled / over capacity / harvest frame).
  // The returned pointer is valid until the next BeginFrame().
  const wgpu::PassTimestampWrites* BeginPass(const char* name);
  // Encode this frame's resolve + copy-to-readback onto `encoder`, after all
  // passes and before submit. No-op on a harvest frame.
  void ResolveFrame(wgpu::CommandEncoder& encoder);
  // Call once per frame after submit. On a harvest frame, blocking-reads the
  // last resolved frame's timestamps and writes a per-pass breakdown to `out`.
  void MaybeReport(std::ostream& out);

 private:
  bool enabled_ = false;
  wgpu::Instance instance_;
  wgpu::Device device_;
  wgpu::QuerySet query_set_;
  wgpu::Buffer resolve_buffer_;   // QueryResolve | CopySrc
  wgpu::Buffer readback_buffer_;  // MapRead | CopyDst
  uint32_t max_passes_ = 0;

  std::vector<wgpu::PassTimestampWrites> writes_;  // this frame (reserved, no realloc)
  std::vector<std::string> names_;                 // this frame's pass names
  std::vector<std::string> last_names_;            // names of the last resolved frame
  bool harvest_frame_ = false;
  int frame_counter_ = 0;
};

}  // namespace badlands
