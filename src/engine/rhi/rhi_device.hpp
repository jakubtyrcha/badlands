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

struct DeviceDesc {
  BackendKind backend = BackendKind::Null;
  // Wraps the device in the validation decorator. Off in release/profiling
  // builds so validation cannot skew CPU measurements; on in tests and debug.
  bool enable_validation = false;
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

  // --- Validation ---
  // All 14 of the engine's existing Dawn validation sites assert "nothing went
  // wrong", none assert that an error is raised (probe C). So the contract is
  // a scoped query, not an error channel: begin a scope, do work, end it and
  // find out whether anything was observed.
  //
  // Returns nullopt when validation is disabled, so callers cannot mistake a
  // compiled-out check for a clean run.
  virtual void BeginValidationScope() = 0;
  virtual std::optional<std::string> EndValidationScope() = 0;
  virtual bool IsValidationEnabled() const = 0;
};

// Creates a device. Returns null (after logging) if the backend is
// unavailable on this platform.
std::unique_ptr<IRhiDevice> CreateDevice(const DeviceDesc& desc);

}  // namespace badlands::rhi
