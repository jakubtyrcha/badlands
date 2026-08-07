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

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
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

  // Reads the buffer's CURRENT contents. Requires BufferUsage::MapRead.
  // Returns false if the buffer cannot be mapped or the range is out of bounds.
  //
  // IT DOES NOT WAIT FOR THE GPU, and that is the trap in its name. On unified
  // memory this is a memcpy out of live memory: if a submitted command buffer
  // is still writing the range, this reads whatever is there now. Every caller
  // today pairs it with IRhiDevice::WaitIdle, which stalls the WHOLE device to
  // synchronise one copy.
  //
  // For a texture, prefer IRhiDevice::ReadTexture: it waits for exactly the
  // copy that produced it, and it can notify instead of blocking.
  virtual bool Read(uint64_t offset, std::span<uint8_t> out) = 0;
};

// Invoked once, when a readback's data has landed on the CPU.
//
// THREAD: unspecified. It runs on whichever thread observes completion -- a
// backend completion thread in the normal case, or the CALLER'S thread if the
// copy had already finished when OnComplete was registered. Anything it touches
// must tolerate both.
using ReadbackCallback = std::function<void()>;

// A texture copy in flight, and the completion that says it has landed.
//
// AWAITABLE, NOT POLLED. Both target APIs hand back a real completion object --
// Metal a command buffer you can waitUntilCompleted or hang a listener off,
// DX12 a fence you can SetEventOnCompletion and then WaitForSingleObject -- so
// the interface exposes one rather than a readiness flag the caller has to spin
// on. Nothing here refuses for not being finished yet; you either wait or you
// are told.
class ITextureReadback : public virtual IResource {
 public:
  // Blocks the CALLING thread until the data has landed. Returns false, after
  // logging, if `timeout` expires first.
  //
  // FINITE BY DEFAULT on purpose. An unbounded wait turns a GPU hang into a
  // process that never returns and never says why, which is strictly worse
  // than an error -- and on Windows it is exactly what a TDR looks like from
  // inside the app.
  virtual bool Wait(std::chrono::milliseconds timeout =
                        std::chrono::milliseconds(5000)) = 0;

  // Registers a completion. Runs immediately, on the calling thread, if the
  // data has already landed; otherwise when it does. Called at most once, and
  // a second registration replaces the first.
  virtual void OnComplete(ReadbackCallback callback) = 0;

  // Whether the data has landed. A QUERY, not a gate: nothing in this
  // interface refuses because this is false.
  virtual bool IsReady() const = 0;

  // The texels, tightly packed. Valid from completion until this object is
  // destroyed -- the readback owns the staging memory and, on a backend that
  // needs one (DX12's readback heap), the mapping. Reading it before
  // completion is what Wait and OnComplete exist to prevent.
  virtual std::span<const uint8_t> Data() const = 0;

  virtual uint32_t GetWidth() const = 0;
  virtual uint32_t GetHeight() const = 0;
  virtual Format GetFormat() const = 0;
};

// The completion half of a readback, WRITTEN ONCE and embedded by every
// backend.
//
// Shared for the reason ResolveViewDesc and ValidateTextureDesc are: "signal,
// wake the waiter, run the callback exactly once" is fiddly enough that two
// copies would drift, and the drift would be a race -- the least reproducible
// kind of divergence there is. The BACKEND supplies only the trigger: Metal an
// addCompletedHandler, DX12 a fence event, Null the submission itself.
class ReadbackCompletion {
 public:
  // Called from whichever thread observes completion. Idempotent: a second
  // signal is ignored rather than running the callback twice.
  void Signal();

  // Blocks until signalled. False on timeout, having logged `label`.
  bool Wait(std::chrono::milliseconds timeout, std::string_view label);

  // Runs `callback` now if already signalled, otherwise on signal. Replaces any
  // previous registration.
  void Register(ReadbackCallback callback);

  bool IsReady() const;

 private:
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  bool ready_ = false;
  ReadbackCallback callback_;
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
  // Indices into `entries` for the entries that take a dynamic offset, in
  // increasing slot order. SetBindingTable's span is read in this order, so it
  // must be computed the same way everywhere -- see DynamicEntryOrder.
  std::vector<uint32_t> dynamic_entries;
};

// Which entries take a dynamic offset, in the order SetBindingTable's span
// supplies them (increasing slot).
//
// One implementation, shared by the resolver and the validation layer, because
// the two disagreeing about the ORDER would silently apply each offset to the
// wrong binding -- a wrong image with no error anywhere.
std::vector<uint32_t> DynamicEntryOrder(const std::vector<BindingEntry>& entries);

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
    const BindingTableDesc& desc, uint64_t min_buffer_offset_alignment);

// Turns a requested view range into a concrete one: fills in the "0 = all
// remaining" counts and bounds-checks the result against the texture.
//
// Shared by every backend so the two cannot disagree about what `base_layer=3`
// on a 2-layer texture means (rule 6), and so the bounds check exists exactly
// once (rule 8). Returns nullopt, after logging, when the range does not fit --
// a view onto layers the texture does not have is a caller bug, and silently
// clamping it to the whole resource is how "accepted and ignored" starts.
// Checks that a pipeline's blend states line up with its colour attachments,
// logging and returning false when they do not.
//
// Shared, and called by every backend BEFORE it builds anything, for the same
// reason ResolveBindingTable is shared: two copies of "check, log, refuse" is
// how Null and Metal came to disagree about a documented contract with nothing
// to catch it (rule 6).
bool ValidateBlendStates(const RenderPipelineDesc& desc);

// Whether `struct_size` bytes of indirect arguments fit in `args` at `offset`,
// logging and returning false when they do not. `what` names the call.
//
// A BACKEND precondition, not validation, so it must NOT compile out: the GPU
// reads these bytes itself, and an offset past the end is a read of whatever
// follows the buffer -- garbage draw counts, a corrupt frame, and a command
// buffer fault under Metal's debug layer.
//
// Shared because Null STRUCTURALLY has to check (it reads the bytes on the CPU
// to resolve the counts) while Metal does not, so a check written only where it
// was forced left the two backends disagreeing: Null dropped the call and its
// suite stayed green while Metal encoded it and faulted.
bool IndirectArgsInBounds(const IBuffer* args, uint64_t offset,
                          uint64_t struct_size, const char* what);

std::optional<TextureViewDesc> ResolveViewDesc(const TextureViewDesc& requested,
                                               const TextureDesc& texture,
                                               std::string_view texture_label);

// Whether `desc` describes a texture that can exist at all, logging and
// returning false when it cannot. A CREATION-TIME precondition (rule 13), so it
// must not compile out: a cube with five faces has no encodable form, and the
// validation decorator is not there in release to notice.
//
// Shared for the reason ResolveViewDesc is: Null refused nothing here and Metal
// refused nothing either, so the two agreed only by both being wrong. Written
// once, they cannot drift apart as backends are added.
bool ValidateTextureDesc(const TextureDesc& desc);

// Whether `src` can be read back from at (`mip`, `layer`), logging and
// returning false when it cannot. On success `out_bytes` receives the tightly
// packed size of that subresource.
//
// A CREATION-TIME refusal (rule 13) and shared for the rule-6 reason: a
// readback that cannot be encoded must not exist, and two backends deciding
// that separately is two chances to disagree about what "cannot".
bool ValidateReadbackSource(const ITexture* src, uint32_t mip, uint32_t layer,
                            size_t& out_bytes);

// Whether a Write() of `data` into (`mip`, `layer`) of `desc` is in bounds,
// logging and returning false when it is not. `label` names the texture.
//
// ALSO a backend precondition rather than validation: Metal's replaceRegion
// writes into a slice the caller named, so an out-of-range layer is a write
// through a bad index rather than a diagnosable mistake. Metal checked the data
// size and neither index; Null checked nothing at all -- rule 6, hidden because
// no test asked until a cube gave textures more than one layer.
bool ValidateTextureWrite(const TextureDesc& desc, std::string_view label,
                          uint32_t mip, uint32_t layer, size_t byte_count);

using BufferPtr = std::shared_ptr<IBuffer>;
using TextureReadbackPtr = std::shared_ptr<ITextureReadback>;
using TexturePtr = std::shared_ptr<ITexture>;
using SamplerPtr = std::shared_ptr<ISampler>;
using BindingTablePtr = std::shared_ptr<IBindingTable>;

}  // namespace badlands::rhi
