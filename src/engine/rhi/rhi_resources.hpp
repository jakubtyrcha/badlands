#pragma once

// GPU resource interfaces. Created through IRhiDevice, owned by shared_ptr.
//
// Lifetime model, and why it is not just shared_ptr: a resource has an
// explicit `Destroy()` in addition to refcounted ownership, mirroring
// WebGPU's `buffer.Destroy()`. Refcounting alone makes use-after-free
// unrepresentable, which sounds good but removes the very error the
// validation decorator is meant to catch -- code that keeps a handle alive
// while the GPU memory behind it has been released. With an explicit Destroy,
// "used after free" becomes "used after Destroy", which is both a real bug
// class and a checkable one.

#include <cstdint>
#include <memory>
#include <span>
#include <string>

#include "engine/rhi/rhi_types.hpp"

namespace badlands::rhi {

// Common base for anything the encoder can transition or the validation
// decorator can track. Derived interfaces inherit VIRTUALLY so a backend can
// implement Destroy/IsDestroyed/GetLabel once in a shared mixin instead of
// repeating it per resource kind -- without that, a class deriving from both
// IBuffer and such a mixin gets two IResource subobjects and stays abstract.
class IResource {
 public:
  virtual ~IResource() = default;

  // Releases the backing GPU memory. Further use is a validation error, not
  // undefined behaviour, when the validation decorator is active. Idempotent.
  virtual void Destroy() = 0;
  virtual bool IsDestroyed() const = 0;

  virtual const std::string& GetLabel() const = 0;
};

class IBuffer : public virtual IResource {
 public:
  virtual uint64_t GetSize() const = 0;
  virtual BufferUsage GetUsage() const = 0;

  // Host-to-device upload. `offset + data.size()` must be within the buffer.
  // Ordered against subsequently submitted command buffers.
  virtual void Write(uint64_t offset, std::span<const uint8_t> data) = 0;

  // Blocking readback. Requires BufferUsage::MapRead. Returns false if the
  // buffer cannot be mapped or the range is out of bounds.
  //
  // Blocking is deliberate: readback exists for tests and for screenshots,
  // both of which want the value now. Dawn's async MapAsync + ProcessEvents
  // dance is one of the things that DELETES in this port -- Metal and DX12 are
  // both synchronous here (probe: ~4% of the wgpu surface is async plumbing).
  virtual bool Read(uint64_t offset, std::span<uint8_t> out) = 0;
};

class ITextureView : public virtual IResource {
 public:
  virtual class ITexture* GetTexture() const = 0;
  virtual Format GetFormat() const = 0;
};

class ITexture : public virtual IResource {
 public:
  virtual uint32_t GetWidth() const = 0;
  virtual uint32_t GetHeight() const = 0;
  virtual uint32_t GetArrayLayers() const = 0;
  virtual uint32_t GetMipLevels() const = 0;
  virtual Format GetFormat() const = 0;
  virtual TextureUsage GetUsage() const = 0;

  // Views are owned by the texture and live as long as it does, so callers can
  // hold the raw pointer. Repeated calls with the same descriptor may return
  // the same view.
  virtual ITextureView* CreateView(const TextureViewDesc& desc = {}) = 0;

  // Convenience for the common whole-resource case.
  virtual ITextureView* GetDefaultView() = 0;

  // Host-to-device upload of one mip/layer. `data` is tightly packed rows of
  // `FormatByteSize(format) * width` bytes.
  virtual void Write(uint32_t mip, uint32_t layer,
                     std::span<const uint8_t> data) = 0;
};

class ISampler : public virtual IResource {
 public:
  virtual const SamplerDesc& GetDesc() const = 0;
};

// One Slang `ParameterBlock<T>` worth of bindings, resolved against a
// pipeline's reflection. Immutable once created -- rebinding means creating a
// new table, which keeps the validation decorator's shadow state simple and
// matches how both Metal argument buffers and D3D12 descriptor tables want to
// be used.
class IBindingTable : public virtual IResource {
 public:
  virtual uint32_t GetGroup() const = 0;
};

using BufferPtr = std::shared_ptr<IBuffer>;
using TexturePtr = std::shared_ptr<ITexture>;
using SamplerPtr = std::shared_ptr<ISampler>;
using BindingTablePtr = std::shared_ptr<IBindingTable>;

}  // namespace badlands::rhi
