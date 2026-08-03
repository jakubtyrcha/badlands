#pragma once

// The Null backend: implements every RHI method, records the command stream,
// and keeps CPU-side buffer contents.
//
// It does three jobs at once, which is why it exists rather than a
// compiled-out DX12 skeleton:
//
//   1. Keeps the seam honest. It implements the full interface and compiles
//      everywhere, so the RHI cannot drift while Metal is the only real
//      backend.
//   2. Is the test double. Every behavioural assertion runs against it with no
//      GPU, so the fast suite covers the interface contract.
//   3. Is where DX12 slots in later.
//
// Keeping real buffer bytes is what makes GPU-driven cases testable without a
// GPU: a test seeds an indirect-args buffer, `DrawIndexedIndirect` resolves it
// from those bytes, and the recorded command carries the resolved args for
// assertion. The same test then runs on Metal, where a compute dispatch writes
// that count for real.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "engine/rhi/rhi_device.hpp"

namespace badlands::rhi::null {

// One recorded command. Deliberately a flat struct rather than a variant --
// tests read two or three fields and a flat struct keeps the assertions
// readable.
struct RecordedCommand {
  enum class Kind : uint8_t {
    Transition,
    BeginRenderPass, EndRenderPass,
    BeginComputePass, EndComputePass,
    SetRenderPipeline, SetComputePipeline,
    SetBindingTable, SetIndexBuffer,
    SetViewport, SetScissor,
    Draw, DrawIndexed, DrawIndexedIndirect,
    Dispatch,
    CopyBufferToBuffer, CopyTextureToBuffer,
    Finish,
  };

  Kind kind = Kind::Finish;
  std::string label;

  // Identity of the object involved, for "was this the table I bound?" checks.
  const void* object = nullptr;
  uint32_t group = 0;

  // Draw/dispatch payloads. For DrawIndexedIndirect these are RESOLVED from
  // the indirect buffer's actual bytes, so a test can assert on what the GPU
  // would have drawn.
  DrawIndexedIndirectArgs draw_args{};
  uint32_t dispatch[3] = {0, 0, 0};

  // Transition payload.
  ResourceState state = ResourceState::Undefined;

  // Render pass attachment summary, so tests can assert load/store handling
  // without reconstructing the descriptor.
  uint32_t color_attachment_count = 0;
  bool has_depth = false;
  LoadOp first_color_load = LoadOp::Clear;
  StoreOp first_color_store = StoreOp::Store;
};

// Everything a test wants to inspect after recording.
class CommandLog {
 public:
  void Record(RecordedCommand cmd) { commands_.push_back(std::move(cmd)); }
  void Clear() { commands_.clear(); }

  const std::vector<RecordedCommand>& All() const { return commands_; }
  size_t Count(RecordedCommand::Kind kind) const;
  // Nth command of `kind`, or nullptr.
  const RecordedCommand* Find(RecordedCommand::Kind kind, size_t n = 0) const;

 private:
  std::vector<RecordedCommand> commands_;
};

// Creates a Null device. `CreateDevice(DeviceDesc{.backend = Null})` is the
// normal entry point; both return the same thing.
std::unique_ptr<IRhiDevice> CreateNullDevice(const std::string& label = {});

// The command log for a Null device, or nullptr for any other backend. Tests
// use this instead of downcasting; it also means a test written against the
// log degrades to a clear nullptr rather than a bad cast when pointed at Metal.
CommandLog* GetCommandLog(IRhiDevice& device);

}  // namespace badlands::rhi::null
