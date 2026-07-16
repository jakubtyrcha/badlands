#pragma once

// GpuTimer — per-pass GPU timing via WebGPU timestamp queries.
//
// The renderer wraps each pass: attach BeginPass("name") to the pass
// descriptor's timestampWrites, then ResolveFrame() once after all passes, and
// EndFrame() once after submit. Every ~120 frames one frame's timestamps are
// read back ASYNCHRONOUSLY (a single ProcessEvents/Tick per frame advances the
// in-flight map — no blocking on the render thread) and a per-pass breakdown is
// printed. While a readback is in flight, ResolveFrame is skipped so the
// readback buffer is never written while mapped. Inert unless Initialize() ran
// with timestamp support (the profiling build calls
// SceneRenderer::EnableGpuProfiling); zero cost otherwise.
#include <atomic>
#include <cstdint>
#include <iosfwd>
#include <memory>
#include <string>
#include <vector>

#include <dawn/webgpu_cpp.h>

namespace badlands {

class GpuTimer {
 public:
  // Creates the timestamp QuerySet + resolve/readback buffers. No-op (stays
  // disabled) if `supported` is false. `instance` is pumped to deliver the
  // async readback callbacks.
  void Initialize(wgpu::Instance instance, wgpu::Device device, bool supported,
                  uint32_t max_passes = 12);
  bool enabled() const { return enabled_; }

  // Call once at the start of each frame's encoding.
  void BeginFrame();
  // Register the next pass; returns the timestampWrites to attach to that
  // pass's descriptor, or nullptr (disabled / over capacity). The returned
  // pointer is valid until the next BeginFrame().
  const wgpu::PassTimestampWrites* BeginPass(const char* name);
  // Encode this frame's resolve + copy-to-readback onto `encoder`, after all
  // passes and before submit. Skipped while an async readback is in flight (so
  // the readback buffer is never overwritten while mapped).
  void ResolveFrame(wgpu::CommandEncoder& encoder);
  // Call once per frame after submit. Non-blocking: advances an in-flight
  // readback (one ProcessEvents/Tick), prints a per-pass breakdown to `out`
  // when it completes, and periodically kicks off a new readback.
  void EndFrame(std::ostream& out);

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

  // Async readback state (a map in flight blocks new resolves until it lands).
  bool map_pending_ = false;
  std::shared_ptr<std::atomic<bool>> map_done_;
  std::shared_ptr<std::atomic<bool>> map_ok_;
  std::vector<std::string> report_names_;  // names for the in-flight readback
  int sample_counter_ = 0;
};

}  // namespace badlands
