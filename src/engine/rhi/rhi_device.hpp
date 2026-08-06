#pragma once

// The RHI device: resource and pipeline creation, command submission, and the
// validation scope.
//
// Dispatch is virtual and devirtualization is NOT relied on (D5). The decisive
// reason is not cost -- draw counts here are tens to low hundreds per frame,
// so dispatch is microseconds against a 16 ms budget -- but that a virtual
// interface lets the validation layer be a DECORATOR wrapping any backend.
// That is how the Dawn validation this port gives up gets replaced.

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>

#include "engine/rhi/rhi_commands.hpp"
#include "engine/rhi/rhi_pipeline.hpp"
#include "engine/rhi/rhi_resources.hpp"
#include "engine/rhi/rhi_types.hpp"

namespace badlands::rhi {

enum class BackendKind : uint8_t {
  Null,   // records calls, keeps CPU-side buffer contents; the test double
  Metal,
  // Dx12 slots in here. The Null backend is what keeps this seam honest in the
  // meantime -- it implements every method and compiles everywhere, so the
  // interface cannot drift while only one real backend exists.
};

const char* ToString(BackendKind k);

// What a completed validation scope observed.
//
// A distinct type rather than an optional<string>, because "clean" and "never
// checked" are different facts and a caller that conflates them will report a
// device with validation compiled out as verified.
struct ValidationReport {
  // Empty exactly when the scope was clean; the two cannot disagree.
  std::string violations;

  bool IsClean() const { return violations.empty(); }
};
// Deliberately no operator bool: this type is almost always held in an
// optional, and two bools in one expression is the ambiguity the type exists
// to remove.

struct DeviceDesc {
  BackendKind backend = BackendKind::Null;
  // Wraps the device in the validation decorator. Off in release/profiling
  // builds so validation cannot skew CPU measurements; on in tests and debug.
  bool enable_validation = false;

  // How many frames may be outstanding at once. Lives HERE rather than on the
  // swapchain because the frame model works headless -- transient allocation
  // and deferred deletion are keyed on frame retirement whether or not
  // anything is being presented. A swapchain reads it back from the device
  // rather than carrying its own copy, so the two cannot disagree.
  //
  // Metal's maximumDrawableCount accepts only 2 or 3, so a swapchain will
  // refuse anything outside that range at creation.
  uint32_t frames_in_flight = 3;

  std::string label;
};

class IRhiDevice {
 public:
  virtual ~IRhiDevice() = default;

  virtual BackendKind GetBackend() const = 0;

  // The device this one wraps, or null if it wraps nothing. Only the
  // validation decorator returns non-null.
  //
  // Exists so backend-specific test helpers can reach past a decorator: a
  // `dynamic_cast` to a concrete device fails on a wrapped one, which made
  // every log-guarded assertion in the conformance list silently skip when
  // validation was enabled. A check that quietly does not run is worse than
  // one that fails.
  virtual IRhiDevice* Inner() { return nullptr; }

  // --- Resources ---
  virtual BufferPtr CreateBuffer(const BufferDesc& desc) = 0;
  virtual TexturePtr CreateTexture(const TextureDesc& desc) = 0;
  virtual SamplerPtr CreateSampler(const SamplerDesc& desc) = 0;

  // --- Shaders and pipelines ---
  // `source` is target-native shader source (MSL for Metal). The RHI never
  // invokes a shader compiler; the Slang layer above produces this.
  virtual ShaderModulePtr CreateShaderModule(const std::string& source,
                                             const ShaderReflection& reflection,
                                             const std::string& label = {}) = 0;
  virtual RenderPipelinePtr CreateRenderPipeline(
      const RenderPipelineDesc& desc) = 0;
  virtual ComputePipelinePtr CreateComputePipeline(
      const ComputePipelineDesc& desc) = 0;

  // One Slang ParameterBlock's worth of bindings, resolved against the
  // pipeline's reflection.
  virtual BindingTablePtr CreateBindingTable(const BindingTableDesc& desc) = 0;

  // --- Commands ---
  virtual std::unique_ptr<ICommandEncoder> CreateCommandEncoder(
      const std::string& label = {}) = 0;
  virtual void Submit(ICommandEncoder& encoder) = 0;
  // Blocks until all submitted work has completed. Tests and readback want
  // this; Metal and DX12 are both synchronous here, unlike Dawn.
  virtual void WaitIdle() = 0;

  // Submissions that have not yet retired. The retirement signal a frame model
  // will be built on, and the only way to observe that submissions are being
  // reclaimed rather than accumulating.
  //
  // GPU-TIMELINE LIFETIME: a submitted command buffer must keep the resources
  // it references alive until it retires. Metal provides this natively
  // (`[queue commandBuffer]` retains references), so the Metal backend gets it
  // for free -- which means Metal can never reveal a backend that fails to.
  // A DX12 backend has to implement it, exactly as it has to emit the barriers
  // Metal auto-tracks.
  //
  // Pure, not defaulted to 0: a default would answer "nothing is in flight"
  // for a backend that has not implemented retirement at all, which is
  // indistinguishable from "everything retired" and makes any test of it pass
  // vacuously. Each backend states its answer deliberately.
  virtual size_t InFlightCount() = 0;

  // --- Frame model ---
  //
  // A frame is the unit of GPU-timeline lifetime: transient allocations are
  // recycled and deferred deletions are freed when the frame that last
  // referenced them retires.
  //
  // BeginFrame BLOCKS until fewer than `frames_in_flight` frames are
  // outstanding, and that is deliberately where the pacing lives. Blocking at
  // swapchain acquire instead would stall the CPU *late* -- after input has
  // been sampled and the whole update has run -- which is the difference
  // between one frame of input latency and three.
  //
  // Frame indices start at 1, so LastRetiredFrame() == 0 means "nothing has
  // retired yet" and is not confusable with frame 0.
  virtual uint64_t BeginFrame() = 0;

  // Closes the frame. Every frame that begins must end, including one that
  // rendered nothing -- a frame with no submitted work retires immediately,
  // because otherwise the Nth skipped frame exhausts the pacing budget and
  // deadlocks. Skipped frames are normal: a minimized window produces them.
  virtual void EndFrame() = 0;

  // The most recently begun frame, and the newest frame known to have fully
  // retired on the GPU. Outstanding frames are the difference.
  virtual uint64_t CurrentFrame() const = 0;
  virtual uint64_t LastRetiredFrame() const = 0;
  virtual uint32_t FramesInFlight() const = 0;

  // Resources that have been Destroy()ed but whose memory is still held,
  // waiting for the frame that may still be reading them to retire.
  //
  // Exists so deferred deletion is OBSERVABLE. Metal releases these correctly
  // whether or not we defer, because a command buffer retains what it
  // references -- so without a count to assert on, the mechanism DX12 depends
  // on could be entirely broken here and every test would still pass.
  virtual size_t PendingDeletions() const = 0;

  // --- Validation ---
  // All 14 of the engine's existing Dawn validation sites assert "nothing went
  // wrong", none assert that an error is raised (probe C). So the contract is
  // a scoped query, not an error channel: begin a scope, do work, end it and
  // find out whether anything was observed.
  //
  // The optional distinguishes "no check ran" from "a check ran"; the
  // ValidationReport inside distinguishes clean from dirty. Those are two
  // different questions and the old signature answered both with nullopt --
  // so a caller on a device with validation compiled out read the same value
  // a clean run produced, and could not tell it had verified nothing
  // (rule 5).
  virtual void BeginValidationScope() = 0;
  virtual std::optional<ValidationReport> EndValidationScope() = 0;
  virtual bool IsValidationEnabled() const = 0;
};

// Creates a device. Returns null (after logging) if the backend is
// unavailable on this platform.
std::unique_ptr<IRhiDevice> CreateDevice(const DeviceDesc& desc);

}  // namespace badlands::rhi
