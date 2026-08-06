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
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/rhi/rhi_types.hpp"

namespace badlands::rhi {

// Common base for anything the encoder can transition or the validation
// decorator can track. Derived interfaces inherit VIRTUALLY so a backend can
// implement Destroy/IsDestroyed/GetLabel once in a shared mixin instead of
// repeating it per resource kind -- without that, a class deriving from both
// IBuffer and such a mixin gets two IResource subobjects and stays abstract.
class IResource : public std::enable_shared_from_this<IResource> {
 public:
  IResource();
  virtual ~IResource() = default;

  // A process-unique, monotonically increasing identity.
  //
  // Tracking keyed on `IResource*` aliases: free a resource, allocate another,
  // and the allocator may hand back the same address -- so the new resource
  // inherits the old one's tracked state. Deferred deletion widens that window
  // considerably. An id is never reused, so it cannot.
  uint64_t Id() const { return id_; }

  // A share of ownership, so a binding table (or anything else that outlives
  // the caller's handle) can retain what it references. Returns null if the
  // resource is not owned by a shared_ptr.
  //
  // `weak_from_this().lock()` rather than `shared_from_this()` in a try/catch:
  // "not shared-owned" is an expected answer here, not an error, so it must
  // not cost a throw.
  std::shared_ptr<IResource> Share() { return weak_from_this().lock(); }

  // Gives up the caller's claim on the backing GPU memory. Idempotent.
  //
  // DEFERRED, not immediate. `IsDestroyed()` becomes true at once and further
  // use is a validation error, but the memory itself is released only when the
  // frame in flight at this moment retires. That is the only coherent meaning
  // once frames overlap: the GPU still reading a resource the caller has
  // finished with is NORMAL, not misuse, and freeing underneath it is a
  // use-after-free -- one Metal happens to survive, because a command buffer
  // retains what it references, and DX12 does not.
  virtual void Destroy() = 0;
  virtual bool IsDestroyed() const = 0;

  virtual const std::string& GetLabel() const = 0;

 private:
  uint64_t id_;
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

  // The RESOLVED range this view covers: `mip_count` and `layer_count` are
  // never 0 here, unlike in the descriptor that requested it. Exists so a
  // caller can tell a sliced view from a whole-resource one, which is also
  // what makes slicing testable rather than merely claimed.
  virtual const TextureViewDesc& GetDesc() const = 0;
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

// A binding table, resolved and validated once, ready for a backend to store.
//
// CREATION-TIME validity, not validation: a table that cannot be encoded must
// not exist, in release builds as much as in debug ones. That is why this
// returns an optional and why every backend refuses on nullopt, rather than
// leaving the decorator to notice later -- the decorator compiles out.
struct ResolvedBindingTable {
  std::vector<BindingEntry> entries;
  // Target-native binding index per entry, parallel to `entries`. Resolved
  // HERE rather than at record time: names and slots become indices once, at
  // resolve time (D9), so recording is a straight indexed walk with no
  // reflection scan and no way to encounter an unresolvable slot.
  std::vector<uint32_t> indices;
  // Shared ownership of everything the entries reference, so the table can
  // outlive the caller's handles.
  std::vector<std::shared_ptr<IResource>> retained;
};

// Resolves `desc` against its pipeline's reflection, retains what it
// references, and refuses -- returning nullopt after logging -- if any entry
// cannot be resolved or cannot be retained.
//
// ONE implementation for every backend, deliberately. Three copies of
// "resolve, retain, refuse" is precisely how Null and Metal came to disagree
// about a documented contract with nothing to catch it (rule 6). This is the
// shared binding resolver D8 called for, and the render graph's auto-binding
// will call it too.
//
// `location.index` is already the per-target field (Metal unifies the buffer
// index space, D3D12 splits srv/uav), so resolving here stays correct for a
// DX12 backend without a second implementation.
std::optional<ResolvedBindingTable> ResolveBindingTable(
    const BindingTableDesc& desc);

// Turns a requested view range into a concrete one: fills in the "0 = all
// remaining" counts and bounds-checks the result against the texture.
//
// Shared by every backend so the two cannot disagree about what `base_layer=3`
// on a 2-layer texture means (rule 6), and so the bounds check exists exactly
// once (rule 8). Returns nullopt, after logging, when the range does not fit --
// a view onto layers the texture does not have is a caller bug, and silently
// clamping it to the whole resource is how "accepted and ignored" starts.
std::optional<TextureViewDesc> ResolveViewDesc(const TextureViewDesc& requested,
                                               const TextureDesc& texture,
                                               std::string_view texture_label);

using BufferPtr = std::shared_ptr<IBuffer>;
using TexturePtr = std::shared_ptr<ITexture>;
using SamplerPtr = std::shared_ptr<ISampler>;
using BindingTablePtr = std::shared_ptr<IBindingTable>;

}  // namespace badlands::rhi
