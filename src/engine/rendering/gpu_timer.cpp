// GpuTimer — see gpu_timer.hpp. Timestamp-query lifecycle adapted from sampo's
// GPU pass-timing (QuerySet -> PassTimestampWrites -> ResolveQuerySet ->
// readback), with a non-blocking async readback advanced one step per frame.
#include "engine/rendering/gpu_timer.hpp"

#include <ostream>

#include <spdlog/spdlog.h>

namespace badlands {

namespace {
constexpr int kReportIntervalFrames = 120;  // ~2 s at 60 fps
}  // namespace

void GpuTimer::Initialize(wgpu::Instance instance, wgpu::Device device,
                          bool supported, uint32_t max_passes) {
  enabled_ = false;
  if (!supported || max_passes == 0) return;
  instance_ = instance;
  device_ = device;
  max_passes_ = max_passes;

  wgpu::QuerySetDescriptor qs{};
  qs.type = wgpu::QueryType::Timestamp;
  qs.count = 2 * max_passes;  // begin + end per pass
  query_set_ = device.CreateQuerySet(&qs);

  const uint64_t bytes = static_cast<uint64_t>(2 * max_passes) * sizeof(uint64_t);
  wgpu::BufferDescriptor rb{};
  rb.size = bytes;
  rb.usage = wgpu::BufferUsage::QueryResolve | wgpu::BufferUsage::CopySrc;
  resolve_buffer_ = device.CreateBuffer(&rb);

  wgpu::BufferDescriptor mb{};
  mb.size = bytes;
  mb.usage = wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst;
  readback_buffer_ = device.CreateBuffer(&mb);

  writes_.reserve(max_passes);
  names_.reserve(max_passes);

  enabled_ = query_set_ && resolve_buffer_ && readback_buffer_;
  if (enabled_) spdlog::info("GpuTimer: per-pass GPU timing enabled");
}

void GpuTimer::BeginFrame() {
  if (!enabled_) return;
  writes_.clear();
  names_.clear();
}

const wgpu::PassTimestampWrites* GpuTimer::BeginPass(const char* name) {
  if (!enabled_ || names_.size() >= max_passes_) return nullptr;
  const uint32_t idx = static_cast<uint32_t>(names_.size());
  names_.emplace_back(name);
  wgpu::PassTimestampWrites w{};
  w.querySet = query_set_;
  w.beginningOfPassWriteIndex = 2 * idx;
  w.endOfPassWriteIndex = 2 * idx + 1;
  writes_.push_back(w);  // reserved to max_passes_, so no reallocation
  return &writes_.back();
}

void GpuTimer::ResolveFrame(wgpu::CommandEncoder& encoder) {
  // Skip while a readback is in flight: readback_buffer_ is mapped/pending and
  // must not be a CopyBufferToBuffer destination until EndFrame unmaps it.
  if (!enabled_ || map_pending_ || names_.empty()) return;
  const uint32_t count = static_cast<uint32_t>(names_.size());
  encoder.ResolveQuerySet(query_set_, 0, 2 * count, resolve_buffer_, 0);
  encoder.CopyBufferToBuffer(resolve_buffer_, 0, readback_buffer_, 0,
                             static_cast<uint64_t>(2 * count) * sizeof(uint64_t));
  last_names_ = names_;  // keep names aligned with the data now in readback_buffer_
}

void GpuTimer::EndFrame(std::ostream& out) {
  if (!enabled_) return;

  if (map_pending_) {
    // Advance the in-flight map WITHOUT blocking — deliver any completed
    // callback (AllowProcessEvents) and check once.
    instance_.ProcessEvents();
    device_.Tick();
    if (!map_done_->load()) return;  // still mapping; try again next frame

    const uint32_t count = static_cast<uint32_t>(report_names_.size());
    const uint64_t bytes = static_cast<uint64_t>(2 * count) * sizeof(uint64_t);
    if (map_ok_->load()) {
      const uint64_t* ts = static_cast<const uint64_t*>(
          readback_buffer_.GetConstMappedRange(0, bytes));
      if (ts) {
        out << "=== GPU passes (ms) ===\n";
        double total = 0.0;
        for (uint32_t i = 0; i < count; ++i) {
          const uint64_t begin = ts[2 * i];
          const uint64_t end = ts[2 * i + 1];
          const double ms =
              end >= begin ? static_cast<double>(end - begin) / 1e6 : 0.0;
          total += ms;
          out << "  " << report_names_[i] << ": " << ms << " ms\n";
        }
        out << "  (sum: " << total << " ms)\n";
      }
    } else {
      spdlog::warn("GpuTimer: timestamp readback map failed");
    }
    readback_buffer_.Unmap();
    map_pending_ = false;
    return;
  }

  // No readback in flight: periodically kick off a new one from the last
  // resolved frame's data (already in readback_buffer_).
  if (++sample_counter_ < kReportIntervalFrames) return;
  sample_counter_ = 0;
  if (last_names_.empty()) return;

  report_names_ = last_names_;
  const uint32_t count = static_cast<uint32_t>(report_names_.size());
  const uint64_t bytes = static_cast<uint64_t>(2 * count) * sizeof(uint64_t);
  map_done_ = std::make_shared<std::atomic<bool>>(false);
  map_ok_ = std::make_shared<std::atomic<bool>>(false);
  auto done = map_done_;
  auto ok = map_ok_;
  readback_buffer_.MapAsync(
      wgpu::MapMode::Read, 0, bytes, wgpu::CallbackMode::AllowProcessEvents,
      [done, ok](wgpu::MapAsyncStatus status, wgpu::StringView) {
        ok->store(status == wgpu::MapAsyncStatus::Success);
        done->store(true);
      });
  map_pending_ = true;
}

}  // namespace badlands
