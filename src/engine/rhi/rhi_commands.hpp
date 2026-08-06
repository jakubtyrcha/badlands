#pragma once

// Command recording.
//
// One encoder per frame, recording into one command buffer, matching sampo's
// ProcessingGraph execution model -- a render graph must be able to drive many
// passes through a single submit, so that shape is fixed here rather than
// discovered later (D7).
//
// Passes are borrowed pointers owned by the encoder and closed with `End()`
// rather than RAII. That is deliberate: "used after End" is a real bug class
// the validation decorator checks, and an explicit End gives it a point to
// check against.

#include <cstdint>
#include <span>
#include <string>

#include "engine/rhi/rhi_pipeline.hpp"
#include "engine/rhi/rhi_resources.hpp"
#include "engine/rhi/rhi_types.hpp"

namespace badlands::rhi {

struct ResourceTransition {
  IResource* resource = nullptr;
  ResourceState state = ResourceState::Undefined;
};

class IRenderPass {
 public:
  virtual ~IRenderPass() = default;

  virtual void SetPipeline(IRenderPipeline* pipeline) = 0;
  // `dynamic_offsets` supplies one value per entry marked
  // BindingEntry::dynamic_offset, in increasing slot order. This is WebGPU's
  // setBindGroup model (D2), and it is how per-frame data reaches a shader:
  // binding tables are immutable, so without it the only way to point at a
  // different slice each frame would be N tables per frame slot.
  virtual void SetBindingTable(uint32_t group, IBindingTable* table,
                               std::span<const uint32_t> dynamic_offsets = {}) = 0;
  virtual void SetIndexBuffer(IBuffer* buffer, IndexFormat format,
                              uint64_t offset = 0) = 0;

  virtual void SetViewport(float x, float y, float w, float h) = 0;
  virtual void SetScissor(uint32_t x, uint32_t y, uint32_t w, uint32_t h) = 0;

  virtual void Draw(uint32_t vertex_count, uint32_t instance_count = 1,
                    uint32_t first_vertex = 0, uint32_t first_instance = 0) = 0;
  virtual void DrawIndexed(uint32_t index_count, uint32_t instance_count = 1,
                           uint32_t first_index = 0, int32_t base_vertex = 0,
                           uint32_t first_instance = 0) = 0;
  // Reads DrawIndexedIndirectArgs from `args` at `offset`. The GPU writes that
  // struct, so nothing here reads back the count.
  virtual void DrawIndexedIndirect(IBuffer* args, uint64_t offset) = 0;

  virtual void End() = 0;
  virtual bool IsEnded() const = 0;
};

class IComputePass {
 public:
  virtual ~IComputePass() = default;

  virtual void SetPipeline(IComputePipeline* pipeline) = 0;
  // `dynamic_offsets` supplies one value per entry marked
  // BindingEntry::dynamic_offset, in increasing slot order. This is WebGPU's
  // setBindGroup model (D2), and it is how per-frame data reaches a shader:
  // binding tables are immutable, so without it the only way to point at a
  // different slice each frame would be N tables per frame slot.
  virtual void SetBindingTable(uint32_t group, IBindingTable* table,
                               std::span<const uint32_t> dynamic_offsets = {}) = 0;
  // Workgroup counts, not thread counts.
  virtual void Dispatch(uint32_t x, uint32_t y = 1, uint32_t z = 1) = 0;

  // Dispatch with the workgroup counts read from `args` on the GPU, so a pass
  // can be sized by something only the GPU knows.
  //
  // A ZERO count is legal here, unlike Dispatch, which refuses one. The counts
  // live in GPU memory and cannot be inspected at record time, and a zero-group
  // indirect dispatch is a well-defined no-op -- which is exactly what an empty
  // cull result produces, every frame, in a working program.
  virtual void DispatchIndirect(IBuffer* args, uint64_t offset = 0) = 0;

  virtual void End() = 0;
  virtual bool IsEnded() const = 0;
};

class ICommandEncoder {
 public:
  virtual ~ICommandEncoder() = default;

  // Declares the state a resource is about to be used in.
  //
  // The Metal backend IGNORES these -- Metal tracks hazards itself. They exist
  // so the validation decorator can check that the front end declared its
  // intent, which Metal structurally cannot reveal, and so the eventual DX12
  // backend has real barriers to emit. See the ResourceState comment in
  // rhi_types.hpp for why this is checked rather than merely recorded.
  virtual void Transition(IResource* resource, ResourceState state) = 0;
  virtual void TransitionMany(std::span<const ResourceTransition> batch) = 0;

  // Both return borrowed pointers owned by this encoder. Only one pass may be
  // open at a time.
  virtual IRenderPass* BeginRenderPass(const RenderPassDesc& desc) = 0;
  virtual IComputePass* BeginComputePass(const std::string& label = {}) = 0;

  virtual void CopyBufferToBuffer(IBuffer* src, uint64_t src_offset,
                                  IBuffer* dst, uint64_t dst_offset,
                                  uint64_t size) = 0;
  // Rows arrive tightly packed; the backend handles any alignment its API
  // requires. `mip`/`layer` select the subresource.
  virtual void CopyTextureToBuffer(ITexture* src, uint32_t mip, uint32_t layer,
                                   IBuffer* dst, uint64_t dst_offset) = 0;

  // Closes recording. Submit via IRhiDevice::Submit.
  virtual void Finish() = 0;
  virtual bool IsFinished() const = 0;
};

}  // namespace badlands::rhi
