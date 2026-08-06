// Metal backend. Compiled with -fobjc-arc (see CMakeLists.txt).
//
// Notes that are design, not incident:
//
//   * Buffers use MTLResourceStorageModeShared. On Apple silicon memory is
//     unified, so readback is a memcpy from `contents` with no staging buffer
//     and no async mapping. That is one of the things this port DELETES
//     relative to Dawn, where the same operation needs MapAsync plus an event
//     pump.
//   * Binding tables apply as individual setBuffer/setTexture/setSamplerState
//     calls at the reflected index, NOT as argument buffers. Slang emits flat
//     [[buffer(N)]] bindings for plain globals, which is what the MVP shaders
//     use; ParameterBlock's argument buffers arrive with bindless, together
//     with the residency management they require.
//   * Resource-state transitions are ACCEPTED AND IGNORED here. Metal tracks
//     hazards itself. They exist for the validation decorator to check and for
//     the eventual DX12 backend to emit -- see rhi_types.hpp.

#include "engine/rhi/metal/metal_rhi.hpp"

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <objc/objc.h>

// objc_autoreleasePoolPush/Pop, for AutoreleasePoolScope.
extern "C" void* objc_autoreleasePoolPush(void);
extern "C" void objc_autoreleasePoolPop(void*);

#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <vector>

#include <spdlog/spdlog.h>

namespace badlands::rhi::metal {
namespace {

// ---------------------------------------------------------------------------
// Enum mapping
// ---------------------------------------------------------------------------

MTLPixelFormat ToMtl(Format f) {
  switch (f) {
    case Format::R8Unorm: return MTLPixelFormatR8Unorm;
    case Format::RGBA8Unorm: return MTLPixelFormatRGBA8Unorm;
    case Format::RGBA8UnormSrgb: return MTLPixelFormatRGBA8Unorm_sRGB;
    case Format::BGRA8Unorm: return MTLPixelFormatBGRA8Unorm;
    case Format::BGRA8UnormSrgb: return MTLPixelFormatBGRA8Unorm_sRGB;
    case Format::RG16Float: return MTLPixelFormatRG16Float;
    case Format::RGBA16Float: return MTLPixelFormatRGBA16Float;
    case Format::R32Float: return MTLPixelFormatR32Float;
    case Format::R32Uint: return MTLPixelFormatR32Uint;
    case Format::RG32Uint: return MTLPixelFormatRG32Uint;
    case Format::RGBA32Float: return MTLPixelFormatRGBA32Float;
    case Format::Depth32Float: return MTLPixelFormatDepth32Float;
    case Format::Undefined: return MTLPixelFormatInvalid;
  }
  return MTLPixelFormatInvalid;
}

MTLCompareFunction ToMtl(CompareFunction c) {
  switch (c) {
    case CompareFunction::Never: return MTLCompareFunctionNever;
    case CompareFunction::Less: return MTLCompareFunctionLess;
    case CompareFunction::LessEqual: return MTLCompareFunctionLessEqual;
    case CompareFunction::Greater: return MTLCompareFunctionGreater;
    case CompareFunction::GreaterEqual: return MTLCompareFunctionGreaterEqual;
    case CompareFunction::Equal: return MTLCompareFunctionEqual;
    case CompareFunction::NotEqual: return MTLCompareFunctionNotEqual;
    case CompareFunction::Always: return MTLCompareFunctionAlways;
  }
  return MTLCompareFunctionAlways;
}

MTLSamplerMinMagFilter ToMtlFilter(FilterMode m) {
  return m == FilterMode::Nearest ? MTLSamplerMinMagFilterNearest
                                  : MTLSamplerMinMagFilterLinear;
}
MTLSamplerMipFilter ToMtlMipFilter(FilterMode m) {
  return m == FilterMode::Nearest ? MTLSamplerMipFilterNearest
                                  : MTLSamplerMipFilterLinear;
}
MTLSamplerAddressMode ToMtl(AddressMode m) {
  switch (m) {
    case AddressMode::ClampToEdge: return MTLSamplerAddressModeClampToEdge;
    case AddressMode::Repeat: return MTLSamplerAddressModeRepeat;
    case AddressMode::MirrorRepeat: return MTLSamplerAddressModeMirrorRepeat;
  }
  return MTLSamplerAddressModeClampToEdge;
}

MTLLoadAction ToMtl(LoadOp op) {
  switch (op) {
    case LoadOp::Load: return MTLLoadActionLoad;
    case LoadOp::Clear: return MTLLoadActionClear;
    case LoadOp::DontCare: return MTLLoadActionDontCare;
  }
  return MTLLoadActionClear;
}
MTLStoreAction ToMtl(StoreOp op) {
  return op == StoreOp::Store ? MTLStoreActionStore : MTLStoreActionDontCare;
}

MTLCullMode ToMtl(CullMode m) {
  switch (m) {
    case CullMode::None: return MTLCullModeNone;
    case CullMode::Front: return MTLCullModeFront;
    case CullMode::Back: return MTLCullModeBack;
  }
  return MTLCullModeNone;
}

MTLPrimitiveType ToMtl(PrimitiveTopology t) {
  switch (t) {
    case PrimitiveTopology::TriangleList: return MTLPrimitiveTypeTriangle;
    case PrimitiveTopology::TriangleStrip: return MTLPrimitiveTypeTriangleStrip;
    case PrimitiveTopology::LineList: return MTLPrimitiveTypeLine;
  }
  return MTLPrimitiveTypeTriangle;
}

NSString* Ns(const std::string& s) {
  return [NSString stringWithUTF8String:s.c_str()];
}

// ---------------------------------------------------------------------------
// Resources
// ---------------------------------------------------------------------------

// The frame timeline: everything a Metal completion handler touches, plus the
// deferred-release queue keyed on it.
//
// One object, owned by shared_ptr, because completion handlers run on
// Metal-owned threads and MUST NOT capture the device. PruneRetired drops a
// command buffer as soon as its status reads Completed, and Metal sets that
// status BEFORE invoking the handler blocks -- so a buffer can leave
// in_flight_ while its handler is still pending, and the destructor's
// WaitIdle would then never wait for it. Capturing this instead means a late
// handler touches a live object no matter when the device died.
struct FrameTimeline {
  struct FrameState {
    uint32_t pending = 0;  // submitted buffers not yet completed
    bool ended = false;
  };

  // --- frame bookkeeping ---
  std::mutex frame_mutex;
  std::map<uint64_t, FrameState> frames;
  std::atomic<uint64_t> current_frame{0};
  std::atomic<uint64_t> last_retired{0};
  dispatch_semaphore_t sem = nil;

  // --- deferred release ---
  mutable std::mutex release_mutex;
  std::vector<std::pair<uint64_t, id>> pending_release;  // (frame, held object)

  // Monotonic: a frame that retires never un-retires, and out-of-order
  // completion (which one queue does not produce, but a second would) must not
  // move the watermark backwards.
  //
  // Deliberately does NOT signal the semaphore. Advancing the watermark and
  // returning a pacing slot are different things: WaitIdle does the first
  // without the second, because a slot it never took is not its to give back.
  void AdvanceRetired(uint64_t frame) {
    uint64_t seen = last_retired.load(std::memory_order_relaxed);
    while (frame > seen && !last_retired.compare_exchange_weak(
                               seen, frame, std::memory_order_release,
                               std::memory_order_relaxed)) {
    }
  }

  void RetireAndSignal(uint64_t frame) {
    AdvanceRetired(frame);
    dispatch_semaphore_signal(sem);
  }

  // Runs on a Metal-owned thread.
  void OnBufferComplete(uint64_t frame) {
    bool retire_now = false;
    {
      std::lock_guard<std::mutex> lock(frame_mutex);
      auto it = frames.find(frame);
      if (it == frames.end()) return;  // already retired
      if (--it->second.pending == 0 && it->second.ended) {
        frames.erase(it);
        retire_now = true;
      }
    }
    if (retire_now) RetireAndSignal(frame);
  }

  // Holds `obj` until the frame currently in flight retires. Conservative on
  // purpose: we do not track which frames actually referenced the resource,
  // and waiting for the newest one also covers every older one, since frames
  // retire in order.
  void Defer(id obj) {
    if (!obj) return;
    const uint64_t frame = current_frame.load(std::memory_order_acquire);
    // Destroyed outside any frame, or with nothing outstanding: no GPU can be
    // reading it, so ARC may release it right now.
    if (frame <= last_retired.load(std::memory_order_acquire)) return;
    std::lock_guard<std::mutex> lock(release_mutex);
    pending_release.emplace_back(frame, obj);
  }

  // Drops everything whose frame has retired. ARC releases as the entries go.
  void Collect() {
    const uint64_t retired = last_retired.load(std::memory_order_acquire);
    std::lock_guard<std::mutex> lock(release_mutex);
    std::erase_if(pending_release,
                  [retired](const auto& e) { return e.first <= retired; });
  }

  size_t PendingCount() const {
    std::lock_guard<std::mutex> lock(release_mutex);
    return pending_release.size();
  }
};

using FrameTimelinePtr = std::shared_ptr<FrameTimeline>;
// The old name, kept for the resource constructors that only use Defer().
using RetireQueuePtr = FrameTimelinePtr;

class MetalResource : public virtual IResource {
 public:
  MetalResource(std::string label, RetireQueuePtr retire)
      : label_(std::move(label)), retire_(std::move(retire)) {}
  bool IsDestroyed() const override { return destroyed_; }
  const std::string& GetLabel() const override { return label_; }

 protected:
  void MarkDestroyed() { destroyed_ = true; }
  // Hands `obj` to the frame timeline instead of letting ARC release it now.
  void Defer(id obj) {
    if (retire_) retire_->Defer(obj);
  }

 private:
  std::string label_;
  RetireQueuePtr retire_;
  bool destroyed_ = false;
};

class MetalBuffer final : public IBuffer, public MetalResource {
 public:
  MetalBuffer(id<MTLBuffer> buf, const BufferDesc& d, RetireQueuePtr retire)
      : MetalResource(d.label, std::move(retire)), buffer_(buf), size_(d.size),
        usage_(d.usage) {}

  void Destroy() override {
    Defer(buffer_);  // released when the frame in flight retires
    buffer_ = nil;
    MarkDestroyed();
  }

  uint64_t GetSize() const override { return size_; }
  BufferUsage GetUsage() const override { return usage_; }

  // Subtraction, not `offset + data.size() > size_`. That addition WRAPS: an
  // offset near UINT64_MAX sums to a small number, passes the guard, and
  // memcpys through a wild pointer. Rule 8 exists for exactly this, and these
  // two are the functions that actually write memory.
  void Write(uint64_t offset, std::span<const uint8_t> data) override {
    if (!buffer_) {
      spdlog::error("rhi/metal: Write to destroyed buffer '{}'", GetLabel());
      return;
    }
    if (data.size() > size_ || offset > size_ - data.size()) {
      spdlog::error(
          "rhi/metal: Write of {} bytes at offset {} runs past buffer '{}' of "
          "{} bytes",
          data.size(), offset, GetLabel(), size_);
      return;
    }
    std::memcpy(static_cast<uint8_t*>(buffer_.contents) + offset, data.data(),
                data.size());
  }

  // Unified memory: no staging copy, no async map.
  bool Read(uint64_t offset, std::span<uint8_t> out) override {
    if (!buffer_) {
      spdlog::error("rhi/metal: Read from destroyed buffer '{}'", GetLabel());
      return false;
    }
    if (out.size() > size_ || offset > size_ - out.size()) {
      spdlog::error(
          "rhi/metal: Read of {} bytes at offset {} runs past buffer '{}' of "
          "{} bytes",
          out.size(), offset, GetLabel(), size_);
      return false;
    }
    std::memcpy(out.data(),
                static_cast<const uint8_t*>(buffer_.contents) + offset,
                out.size());
    return true;
  }

  id<MTLBuffer> Handle() const { return buffer_; }

 private:
  id<MTLBuffer> buffer_;
  uint64_t size_;
  BufferUsage usage_;
};

class MetalTexture;

class MetalTextureView final : public ITextureView, public MetalResource {
 public:
  MetalTextureView(MetalTexture* owner, id<MTLTexture> tex, Format fmt,
                   std::string label, const TextureViewDesc& resolved,
                   RetireQueuePtr retire)
      : MetalResource(std::move(label), std::move(retire)), owner_(owner),
        texture_(tex), format_(fmt), desc_(resolved) {}

  void Destroy() override {
    Defer(texture_);
    texture_ = nil;
    MarkDestroyed();
  }
  // A view is destroyed once its texture is, even if nobody destroyed the view
  // directly -- callers hold views as raw borrowed pointers, so this is how
  // they find out rather than by dereferencing freed memory.
  bool IsDestroyed() const override;
  ITexture* GetTexture() const override;
  Format GetFormat() const override { return format_; }
  const TextureViewDesc& GetDesc() const override { return desc_; }
  id<MTLTexture> Handle() const { return texture_; }

 private:
  MetalTexture* owner_;
  id<MTLTexture> texture_;
  Format format_;
  TextureViewDesc desc_;
};

class MetalTexture final : public ITexture, public MetalResource {
 public:
  MetalTexture(id<MTLTexture> tex, const TextureDesc& d, RetireQueuePtr retire)
      : MetalResource(d.label, retire), texture_(tex), desc_(d),
        retire_(std::move(retire)) {}

  void Destroy() override {
    // Release the GPU memory but KEEP the view objects: callers hold them as
    // raw borrowed pointers, and freeing them here was a heap-use-after-free
    // that ASan caught and that Null (which never freed them) could not.
    for (auto& [key, view] : views_) view->Destroy();
    Defer(texture_);
    texture_ = nil;
    MarkDestroyed();
  }

  uint32_t GetWidth() const override { return desc_.width; }
  uint32_t GetHeight() const override { return desc_.height; }
  uint32_t GetArrayLayers() const override { return desc_.array_layers; }
  uint32_t GetMipLevels() const override { return desc_.mip_levels; }
  Format GetFormat() const override { return desc_.format; }
  TextureUsage GetUsage() const override { return desc_.usage; }

  ITextureView* CreateView(const TextureViewDesc& vd) override {
    // BEFORE the cache lookup. Destroy() keeps the view objects alive (callers
    // hold them as raw borrowed pointers), so a cache hit would otherwise hand
    // back a view wrapping a nil MTLTexture while an uncached range correctly
    // refused -- the same call answering two ways depending on cache history.
    if (IsDestroyed()) {
      spdlog::error("rhi/metal: CreateView on destroyed texture '{}'",
                    GetLabel());
      return nullptr;
    }

    const auto r = ResolveViewDesc(vd, desc_, GetLabel());
    if (!r) return nullptr;  // ResolveViewDesc logged why

    // Keyed on the whole resolved range: keying on (base_mip, base_layer)
    // alone made two views that differ only in COUNT collide, so the second
    // request silently got the first one's range.
    const ViewKey key{r->base_mip, r->mip_count, r->base_layer, r->layer_count};
    auto it = views_.find(key);
    if (it != views_.end()) return it->second.get();

    // A whole-resource view is the texture itself. Anything narrower needs a
    // real Metal view object -- without this the slicing was accepted and
    // ignored, and every view sampled the whole resource.
    id<MTLTexture> handle = texture_;
    const bool whole = r->base_mip == 0 && r->base_layer == 0 &&
                       r->mip_count == std::max(1u, desc_.mip_levels) &&
                       r->layer_count == std::max(1u, desc_.array_layers);
    if (!whole) {
      handle = [texture_
          newTextureViewWithPixelFormat:texture_.pixelFormat
                            textureType:texture_.textureType
                                 levels:NSMakeRange(r->base_mip, r->mip_count)
                                 slices:NSMakeRange(r->base_layer,
                                                    r->layer_count)];
      if (!handle) {
        spdlog::error(
            "rhi/metal: newTextureView on '{}' failed for mips [{}, {}) "
            "layers [{}, {})",
            GetLabel(), r->base_mip, r->base_mip + r->mip_count, r->base_layer,
            r->base_layer + r->layer_count);
        return nullptr;
      }
    }

    auto view = std::make_unique<MetalTextureView>(
        this, handle, desc_.format, desc_.label + ".view", *r, retire_);
    auto* raw = view.get();
    views_.emplace(key, std::move(view));
    return raw;
  }

  ITextureView* GetDefaultView() override { return CreateView({}); }

  void Write(uint32_t mip, uint32_t layer,
             std::span<const uint8_t> data) override {
    if (!texture_) return;
    const uint32_t w = std::max(1u, desc_.width >> mip);
    const uint32_t h = std::max(1u, desc_.height >> mip);
    const uint32_t bpr = w * FormatByteSize(desc_.format);
    if (data.size() < size_t(bpr) * h) {
      spdlog::error("rhi/metal: Write to '{}' is short ({} < {})",
                    GetLabel(), data.size(), size_t(bpr) * h);
      return;
    }
    [texture_ replaceRegion:MTLRegionMake2D(0, 0, w, h)
                mipmapLevel:mip
                      slice:layer
                  withBytes:data.data()
                bytesPerRow:bpr
              bytesPerImage:size_t(bpr) * h];
  }

  id<MTLTexture> Handle() const { return texture_; }

 private:
  // base_mip, mip_count, base_layer, layer_count -- all four, see CreateView.
  using ViewKey = std::tuple<uint32_t, uint32_t, uint32_t, uint32_t>;

  id<MTLTexture> texture_;
  TextureDesc desc_;
  RetireQueuePtr retire_;
  std::map<ViewKey, std::unique_ptr<MetalTextureView>> views_;
};

ITexture* MetalTextureView::GetTexture() const { return owner_; }
bool MetalTextureView::IsDestroyed() const {
  return MetalResource::IsDestroyed() || (owner_ && owner_->IsDestroyed());
}

class MetalSampler final : public ISampler, public MetalResource {
 public:
  MetalSampler(id<MTLSamplerState> s, const SamplerDesc& d,
               RetireQueuePtr retire)
      : MetalResource(d.label, std::move(retire)), sampler_(s), desc_(d) {}
  void Destroy() override {
    Defer(sampler_);
    sampler_ = nil;
    MarkDestroyed();
  }
  const SamplerDesc& GetDesc() const override { return desc_; }
  id<MTLSamplerState> Handle() const { return sampler_; }

 private:
  id<MTLSamplerState> sampler_;
  SamplerDesc desc_;
};

// A binding table is just the resolved entry list. Applying it is a handful of
// set* calls at the reflected index; see the file header for why not argument
// buffers.
// Built only from an already-resolved table, so an unresolvable slot cannot
// reach the record path: it fails at CreateBindingTable, which returns null.
class MetalBindingTable final : public IBindingTable, public MetalResource {
 public:
  MetalBindingTable(ResolvedBindingTable r, uint32_t group, std::string label,
                    RetireQueuePtr retire)
      : MetalResource(std::move(label), std::move(retire)), group_(group),
        resolved_(std::move(r)) {}
  void Destroy() override { MarkDestroyed(); }
  uint32_t GetGroup() const override { return group_; }
  const std::vector<BindingEntry>& Entries() const { return resolved_.entries; }
  const std::vector<uint32_t>& Indices() const { return resolved_.indices; }
  const std::vector<uint32_t>& DynamicEntries() const {
    return resolved_.dynamic_entries;
  }

 private:
  uint32_t group_;
  // Entries, their target indices, and shared ownership of everything they
  // reference -- all decided once, at creation.
  ResolvedBindingTable resolved_;
};

// ---------------------------------------------------------------------------
// Swapchain
// ---------------------------------------------------------------------------

// Two modes. With a CAMetalLayer it presents for real; without one it hands
// out its own textures, which keeps the acquire/present state machine
// exercised on Metal in a headless test run.
class MetalSwapchain final : public ISwapchain {
 public:
  MetalSwapchain(id<MTLDevice> device, id<MTLCommandQueue> queue,
                 const SwapchainDesc& desc, uint32_t depth,
                 RetireQueuePtr retire)
      : device_(device), queue_(queue), desc_(desc), depth_(depth),
        retire_(std::move(retire)) {
    if (desc_.native_window) {
      layer_ = (__bridge CAMetalLayer*)desc_.native_window;
      layer_.device = device_;
      layer_.pixelFormat = ToMtl(desc_.format);
      layer_.framebufferOnly = NO;  // the lab copies the backbuffer for tests
      layer_.maximumDrawableCount = depth_;
      layer_.displaySyncEnabled = desc_.vsync;
    }
    Recreate();
  }

  AcquiredFrame Acquire() override {
    if (acquired_) {
      // Two drawables held at once starves the pool and stalls the next
      // frame; on the headless path it simply means the caller lost track.
      spdlog::error(
          "rhi/metal: swapchain '{}' acquired twice without a Present",
          desc_.label);
      return {AcquireStatus::Skip, nullptr};
    }
    // A minimized window reports zero. Not an error, and never a 0x0 texture.
    if (desc_.width == 0 || desc_.height == 0) {
      return {AcquireStatus::Skip, nullptr};
    }

    CollectRetired();

    if (!layer_) {  // headless
      acquired_ = true;
      auto* view = images_[next_ % images_.size()]->GetDefaultView();
      next_ = (next_ + 1) % images_.size();
      return {AcquireStatus::Ok, view};
    }

    id<CAMetalDrawable> drawable = [layer_ nextDrawable];
    if (!drawable) {
      // allowsNextDrawableTimeout returns nil after roughly a second when the
      // pool is exhausted or the window is occluded. Transient: Skip, not
      // Lost -- recreating the surface here would be a pointless hitch.
      return {AcquireStatus::Skip, nullptr};
    }
    // CAMetalLayer does NOT guarantee the drawable matches the layer's new
    // bounds immediately after a resize, so a frame can arrive at the old
    // size. Rendering into it would stretch the image against everything else
    // sized for this frame.
    if (drawable.texture.width != desc_.width ||
        drawable.texture.height != desc_.height) {
      return {AcquireStatus::Skip, nullptr};
    }

    drawable_ = drawable;
    acquired_ = true;
    // A wrapper per acquire: the drawable's texture changes every frame, so
    // there is nothing stable to cache. One shared_ptr against a present is
    // not a cost worth designing around.
    current_ = std::make_shared<MetalTexture>(
        drawable.texture,
        TextureDesc{.width = desc_.width,
                    .height = desc_.height,
                    .format = desc_.format,
                    .usage = TextureUsage::RenderTarget | TextureUsage::CopySrc,
                    .label = desc_.label + ".drawable"},
        retire_);
    return {AcquireStatus::Ok, current_->GetDefaultView()};
  }

  void Present() override {
    if (!acquired_) {
      spdlog::error("rhi/metal: swapchain '{}' presented without an acquire",
                    desc_.label);
      return;
    }
    acquired_ = false;
    ++presented_;
    if (!drawable_) return;  // headless

    // presentDrawable: on a command buffer rather than [drawable present],
    // which shows the surface as soon as the CPU asks -- possibly before the
    // GPU has finished rendering into it. An empty buffer is enough: Metal
    // orders it after everything already committed on this queue.
    id<MTLCommandBuffer> cmd = [queue_ commandBuffer];
    cmd.label = @"present";
    [cmd presentDrawable:drawable_];
    [cmd commit];
    drawable_ = nil;
    current_.reset();
  }

  void Resize(uint32_t width, uint32_t height) override {
    if (width == desc_.width && height == desc_.height) return;
    desc_.width = width;
    desc_.height = height;
    if (layer_) layer_.drawableSize = CGSizeMake(width, height);
    Recreate();
  }

  uint32_t GetWidth() const override { return desc_.width; }
  uint32_t GetHeight() const override { return desc_.height; }
  Format GetFormat() const override { return desc_.format; }

 private:
  // See the Null swapchain: the texture OBJECTS must outlive Destroy(),
  // because callers hold views into them as raw borrowed pointers.
  void CollectRetired() {
    const uint64_t retired = retire_->last_retired.load(std::memory_order_acquire);
    std::erase_if(retired_,
                  [retired](const auto& e) { return e.first <= retired; });
  }

  void Recreate() {
    for (auto& img : images_) img->Destroy();
    if (!images_.empty()) {
      retired_.emplace_back(retire_->current_frame.load(std::memory_order_acquire),
                            std::move(images_));
    }
    images_.clear();
    acquired_ = false;
    drawable_ = nil;
    current_.reset();
    next_ = 0;
    if (layer_ || desc_.width == 0 || desc_.height == 0) return;

    for (uint32_t i = 0; i < depth_; ++i) {
      MTLTextureDescriptor* td = [[MTLTextureDescriptor alloc] init];
      td.pixelFormat = ToMtl(desc_.format);
      td.width = desc_.width;
      td.height = desc_.height;
      td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
      td.storageMode = MTLStorageModePrivate;
      id<MTLTexture> tex = [device_ newTextureWithDescriptor:td];
      if (!tex) {
        spdlog::error("rhi/metal: swapchain '{}' could not create image {}",
                      desc_.label, i);
        return;
      }
      images_.push_back(std::make_shared<MetalTexture>(
          tex,
          TextureDesc{.width = desc_.width,
                      .height = desc_.height,
                      .format = desc_.format,
                      .usage = TextureUsage::RenderTarget | TextureUsage::CopySrc,
                      .label = desc_.label + ".image" + std::to_string(i)},
          retire_));
    }
  }

  id<MTLDevice> device_;
  id<MTLCommandQueue> queue_;
  SwapchainDesc desc_;
  uint32_t depth_;
  RetireQueuePtr retire_;
  CAMetalLayer* layer_ = nil;
  id<CAMetalDrawable> drawable_ = nil;
  std::shared_ptr<MetalTexture> current_;
  std::vector<std::shared_ptr<MetalTexture>> images_;
  std::vector<std::pair<uint64_t, std::vector<std::shared_ptr<MetalTexture>>>>
      retired_;
  size_t next_ = 0;
  bool acquired_ = false;
  uint64_t presented_ = 0;
};

// ---------------------------------------------------------------------------
// Shaders and pipelines
// ---------------------------------------------------------------------------

class MetalShaderModule final : public IShaderModule {
 public:
  MetalShaderModule(id<MTLLibrary> lib, ShaderReflection r, std::string label)
      : library_(lib), reflection_(std::move(r)), label_(std::move(label)) {}
  const ShaderReflection& GetReflection() const override { return reflection_; }
  const std::string& GetLabel() const override { return label_; }
  id<MTLLibrary> Library() const { return library_; }

 private:
  id<MTLLibrary> library_;
  ShaderReflection reflection_;
  std::string label_;
};

ShaderReflection MergeReflection(const IShaderModule* a, const IShaderModule* b) {
  ShaderReflection out;
  auto append = [&out](const IShaderModule* m) {
    if (!m) return;
    const ShaderReflection& r = m->GetReflection();
    for (const auto& bind : r.bindings) {
      const bool dup = std::any_of(
          out.bindings.begin(), out.bindings.end(), [&](const auto& e) {
            return e.group == bind.group && e.slot == bind.slot;
          });
      if (!dup) out.bindings.push_back(bind);
    }
    for (const auto& ub : r.uniform_blocks) {
      const bool dup =
          std::any_of(out.uniform_blocks.begin(), out.uniform_blocks.end(),
                      [&](const auto& e) {
                        return e.group == ub.group && e.slot == ub.slot;
                      });
      if (!dup) out.uniform_blocks.push_back(ub);
    }
    out.entry_points.insert(out.entry_points.end(), r.entry_points.begin(),
                            r.entry_points.end());
  };
  append(a);
  append(b);
  return out;
}

class MetalRenderPipeline final : public IRenderPipeline {
 public:
  MetalRenderPipeline(id<MTLRenderPipelineState> pso,
                      id<MTLDepthStencilState> dss, const RenderPipelineDesc& d)
      : pso_(pso), depth_(dss), desc_(d),
        reflection_(MergeReflection(d.vertex_shader, d.fragment_shader)) {}

  const ShaderReflection& GetReflection() const override { return reflection_; }
  const RenderPipelineDesc& GetDesc() const override { return desc_; }
  id<MTLRenderPipelineState> Pso() const { return pso_; }
  id<MTLDepthStencilState> DepthState() const { return depth_; }
  MTLCullMode Cull() const { return ToMtl(desc_.cull_mode); }
  MTLWinding Winding() const {
    return desc_.front_face == FrontFace::Ccw ? MTLWindingCounterClockwise
                                              : MTLWindingClockwise;
  }
  MTLPrimitiveType Topology() const { return ToMtl(desc_.topology); }

 private:
  id<MTLRenderPipelineState> pso_;
  id<MTLDepthStencilState> depth_;
  RenderPipelineDesc desc_;
  ShaderReflection reflection_;
};

class MetalComputePipeline final : public IComputePipeline {
 public:
  MetalComputePipeline(id<MTLComputePipelineState> pso,
                       const ComputePipelineDesc& d)
      : pso_(pso), reflection_(MergeReflection(d.shader, nullptr)),
        entry_(d.entry) {}

  const ShaderReflection& GetReflection() const override { return reflection_; }
  void GetWorkgroupSize(uint32_t out[3]) const override {
    out[0] = out[1] = out[2] = 1;
    for (const auto& ep : reflection_.entry_points) {
      if (ep.name != entry_) continue;
      out[0] = ep.workgroup_size[0];
      out[1] = ep.workgroup_size[1];
      out[2] = ep.workgroup_size[2];
      return;
    }
  }
  id<MTLComputePipelineState> Pso() const { return pso_; }

 private:
  id<MTLComputePipelineState> pso_;
  ShaderReflection reflection_;
  std::string entry_;
};

// ---------------------------------------------------------------------------
// Binding application
// ---------------------------------------------------------------------------

// `index` is the reflected Metal binding index. Slang emits flat
// [[buffer(N)]] / [[texture(N)]] / [[sampler(N)]] for plain globals, and its
// reflection reports the same N -- which is what BindingLocation carries.
// The byte offset an entry binds at: its fixed base, plus the caller's
// dynamic value if it declared one. A count mismatch is reported by the
// validation layer; here a short span simply contributes nothing, so a
// release build binds the base offset rather than reading past the span.
uint64_t OffsetFor(const MetalBindingTable& table, size_t entry,
                   std::span<const uint32_t> dynamic_offsets) {
  const uint64_t base = table.Entries()[entry].buffer_offset;
  if (!table.Entries()[entry].dynamic_offset) return base;
  const auto& order = table.DynamicEntries();
  for (size_t k = 0; k < order.size(); ++k) {
    if (order[k] != entry) continue;
    return k < dynamic_offsets.size() ? base + dynamic_offsets[k] : base;
  }
  return base;
}

// Render and compute encoders are unrelated protocols with different
// selectors, so this is two functions rather than one with a runtime branch --
// a template would have to compile both selector sets against both encoders.

// Every binding goes to BOTH stages: Slang's ProgramLayout does not report
// which stages use a binding (probe B), so narrowing is not possible yet.
// Correct, slightly wasteful, and revisited only on evidence.
void ApplyTableGraphics(id<MTLRenderCommandEncoder> enc,
                        const MetalBindingTable& table,
                        std::span<const uint32_t> dynamic_offsets) {
  const auto& entries = table.Entries();
  const auto& indices = table.Indices();
  // Refused, not silently bound at the base offsets. Falling back would make
  // every dynamic binding point at frame 0's data forever -- the camera never
  // moves and the terrain never updates -- with no diagnostic on any code
  // path, because the decorator that would have caught it compiles out of a
  // release build (rule 12).
  if (dynamic_offsets.size() != table.DynamicEntries().size()) {
    spdlog::error(
        "rhi/metal: binding table '{}' declares {} dynamic offset(s) but {} "
        "were supplied -- binding nothing rather than guessing",
        table.GetLabel(), table.DynamicEntries().size(),
        dynamic_offsets.size());
    return;
  }
  for (size_t i = 0; i < entries.size(); ++i) {
    const BindingEntry& e = entries[i];
    const uint32_t index = indices[i];
    const uint64_t offset = OffsetFor(table, i, dynamic_offsets);
    switch (e.kind) {
      case BindingKind::UniformBuffer:
      case BindingKind::StorageBuffer:
      case BindingKind::ReadOnlyStorageBuffer: {
        auto* b = static_cast<MetalBuffer*>(e.buffer);
        if (!b) break;
        [enc setVertexBuffer:b->Handle() offset:offset atIndex:index];
        [enc setFragmentBuffer:b->Handle() offset:offset atIndex:index];
        break;
      }
      case BindingKind::SampledTexture: {
        auto* v = static_cast<MetalTextureView*>(e.texture_view);
        if (!v) break;
        [enc setVertexTexture:v->Handle() atIndex:index];
        [enc setFragmentTexture:v->Handle() atIndex:index];
        break;
      }
      case BindingKind::Sampler: {
        auto* s = static_cast<MetalSampler*>(e.sampler);
        if (!s) break;
        [enc setVertexSamplerState:s->Handle() atIndex:index];
        [enc setFragmentSamplerState:s->Handle() atIndex:index];
        break;
      }
    }
  }
}

void ApplyTableCompute(id<MTLComputeCommandEncoder> enc,
                       const MetalBindingTable& table,
                       std::span<const uint32_t> dynamic_offsets) {
  const auto& entries = table.Entries();
  const auto& indices = table.Indices();
  // Refused, not silently bound at the base offsets. Falling back would make
  // every dynamic binding point at frame 0's data forever -- the camera never
  // moves and the terrain never updates -- with no diagnostic on any code
  // path, because the decorator that would have caught it compiles out of a
  // release build (rule 12).
  if (dynamic_offsets.size() != table.DynamicEntries().size()) {
    spdlog::error(
        "rhi/metal: binding table '{}' declares {} dynamic offset(s) but {} "
        "were supplied -- binding nothing rather than guessing",
        table.GetLabel(), table.DynamicEntries().size(),
        dynamic_offsets.size());
    return;
  }
  for (size_t i = 0; i < entries.size(); ++i) {
    const BindingEntry& e = entries[i];
    const uint32_t index = indices[i];
    const uint64_t offset = OffsetFor(table, i, dynamic_offsets);
    switch (e.kind) {
      case BindingKind::UniformBuffer:
      case BindingKind::StorageBuffer:
      case BindingKind::ReadOnlyStorageBuffer: {
        auto* b = static_cast<MetalBuffer*>(e.buffer);
        if (!b) break;
        [enc setBuffer:b->Handle() offset:offset atIndex:index];
        break;
      }
      case BindingKind::SampledTexture: {
        auto* v = static_cast<MetalTextureView*>(e.texture_view);
        if (!v) break;
        [enc setTexture:v->Handle() atIndex:index];
        break;
      }
      case BindingKind::Sampler: {
        auto* s = static_cast<MetalSampler*>(e.sampler);
        if (!s) break;
        [enc setSamplerState:s->Handle() atIndex:index];
        break;
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Passes
// ---------------------------------------------------------------------------

class MetalRenderPass final : public IRenderPass {
 public:
  MetalRenderPass(id<MTLRenderCommandEncoder> enc) : enc_(enc) {}

  void SetPipeline(IRenderPipeline* p) override {
    auto* mp = static_cast<MetalRenderPipeline*>(p);
    if (!mp) return;
    pipeline_ = mp;
    [enc_ setRenderPipelineState:mp->Pso()];
    // Unconditional: see CreateRenderPipeline. Skipping this when a pipeline
    // has no depth state is how the previous draw's state leaked into it.
    [enc_ setDepthStencilState:mp->DepthState()];
    [enc_ setCullMode:mp->Cull()];
    [enc_ setFrontFacingWinding:mp->Winding()];
  }

  // No pipeline needed, and none consulted: the table's indices were resolved
  // when it was created, so binding before SetPipeline is genuinely harmless
  // here rather than a silent loss of every binding.
  void SetBindingTable(uint32_t group, IBindingTable* t,
                       std::span<const uint32_t> dynamic_offsets) override {
    auto* mt = static_cast<MetalBindingTable*>(t);
    if (!mt) {
      spdlog::error("rhi/metal: SetBindingTable at group {} with no table",
                    group);
      return;
    }
    ApplyTableGraphics(enc_, *mt, dynamic_offsets);
  }

  void SetIndexBuffer(IBuffer* b, IndexFormat f, uint64_t offset) override {
    index_buffer_ = static_cast<MetalBuffer*>(b);
    index_offset_ = offset;
    index_type_ = f == IndexFormat::Uint16 ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32;
    index_stride_ = f == IndexFormat::Uint16 ? 2 : 4;
  }

  void SetViewport(float x, float y, float w, float h) override {
    [enc_ setViewport:(MTLViewport){x, y, w, h, 0.0, 1.0}];
  }
  void SetScissor(uint32_t x, uint32_t y, uint32_t w, uint32_t h) override {
    [enc_ setScissorRect:(MTLScissorRect){x, y, w, h}];
  }

  void Draw(uint32_t vertex_count, uint32_t instance_count,
            uint32_t first_vertex, uint32_t first_instance) override {
    if (!pipeline_) {
      spdlog::error("rhi/metal: Draw with no pipeline bound");
      return;
    }
    [enc_ drawPrimitives:pipeline_->Topology()
             vertexStart:first_vertex
             vertexCount:vertex_count
           instanceCount:instance_count
            baseInstance:first_instance];
  }

  void DrawIndexed(uint32_t index_count, uint32_t instance_count,
                   uint32_t first_index, int32_t base_vertex,
                   uint32_t first_instance) override {
    if (!pipeline_) {
      spdlog::error("rhi/metal: DrawIndexed with no pipeline bound");
      return;
    }
    if (!index_buffer_) {
      spdlog::error("rhi/metal: DrawIndexed with no index buffer bound");
      return;
    }
    [enc_ drawIndexedPrimitives:pipeline_->Topology()
                     indexCount:index_count
                      indexType:index_type_
                    indexBuffer:index_buffer_->Handle()
              indexBufferOffset:index_offset_ + uint64_t(first_index) * index_stride_
                  instanceCount:instance_count
                     baseVertex:base_vertex
                   baseInstance:first_instance];
  }

  void DrawIndexedIndirect(IBuffer* args, uint64_t offset) override {
    if (!pipeline_) {
      spdlog::error("rhi/metal: DrawIndexedIndirect with no pipeline bound");
      return;
    }
    if (!index_buffer_) {
      spdlog::error("rhi/metal: DrawIndexedIndirect with no index buffer bound");
      return;
    }
    // dynamic_cast, not static_cast: a buffer from another backend is a real
    // caller mistake, and a static_cast reinterprets it into a plausible-
    // looking pointer that passes a null check and then hands Metal garbage.
    auto* mb = dynamic_cast<MetalBuffer*>(args);
    if (!mb || !mb->Handle()) {
      spdlog::error("rhi/metal: DrawIndexedIndirect with no argument buffer");
      return;
    }
    [enc_ drawIndexedPrimitives:pipeline_->Topology()
                      indexType:index_type_
                    indexBuffer:index_buffer_->Handle()
              indexBufferOffset:index_offset_
                 indirectBuffer:mb->Handle()
           indirectBufferOffset:offset];
  }

  void End() override {
    if (ended_) return;
    ended_ = true;
    [enc_ endEncoding];
  }
  bool IsEnded() const override { return ended_; }

 private:
  id<MTLRenderCommandEncoder> enc_;
  MetalRenderPipeline* pipeline_ = nullptr;
  MetalBuffer* index_buffer_ = nullptr;
  uint64_t index_offset_ = 0;
  uint32_t index_stride_ = 4;
  MTLIndexType index_type_ = MTLIndexTypeUInt32;
  bool ended_ = false;
};

class MetalComputePass final : public IComputePass {
 public:
  explicit MetalComputePass(id<MTLComputeCommandEncoder> enc) : enc_(enc) {}

  void SetPipeline(IComputePipeline* p) override {
    auto* mp = static_cast<MetalComputePipeline*>(p);
    if (!mp) return;
    pipeline_ = mp;
    [enc_ setComputePipelineState:mp->Pso()];
    mp->GetWorkgroupSize(threads_);
  }
  // See MetalRenderPass::SetBindingTable -- indices are resolved at creation.
  void SetBindingTable(uint32_t group, IBindingTable* t,
                       std::span<const uint32_t> dynamic_offsets) override {
    auto* mt = static_cast<MetalBindingTable*>(t);
    if (!mt) {
      spdlog::error("rhi/metal: SetBindingTable at group {} with no table",
                    group);
      return;
    }
    ApplyTableCompute(enc_, *mt, dynamic_offsets);
  }
  void Dispatch(uint32_t x, uint32_t y, uint32_t z) override {
    if (!pipeline_) {
      spdlog::error("rhi/metal: Dispatch with no pipeline bound");
      return;
    }
    [enc_ dispatchThreadgroups:MTLSizeMake(x, y, z)
         threadsPerThreadgroup:MTLSizeMake(threads_[0], threads_[1], threads_[2])];
  }

  void DispatchIndirect(IBuffer* args, uint64_t offset) override {
    if (!pipeline_) {
      spdlog::error("rhi/metal: DispatchIndirect with no pipeline bound");
      return;
    }
    // dynamic_cast, not static_cast -- see DrawIndexedIndirect.
    auto* mb = dynamic_cast<MetalBuffer*>(args);
    if (!mb || !mb->Handle()) {
      spdlog::error("rhi/metal: DispatchIndirect with no argument buffer");
      return;
    }
    [enc_ dispatchThreadgroupsWithIndirectBuffer:mb->Handle()
                           indirectBufferOffset:offset
                          threadsPerThreadgroup:MTLSizeMake(threads_[0],
                                                            threads_[1],
                                                            threads_[2])];
  }
  void End() override {
    if (ended_) return;
    ended_ = true;
    [enc_ endEncoding];
  }
  bool IsEnded() const override { return ended_; }

 private:
  id<MTLComputeCommandEncoder> enc_;
  MetalComputePipeline* pipeline_ = nullptr;
  uint32_t threads_[3] = {1, 1, 1};
  bool ended_ = false;
};

// ---------------------------------------------------------------------------
// Encoder
// ---------------------------------------------------------------------------

class MetalCommandEncoder final : public ICommandEncoder {
 public:
  MetalCommandEncoder(id<MTLCommandBuffer> cmd, std::string label)
      : cmd_(cmd), label_(std::move(label)) {
    if (!label_.empty()) cmd_.label = Ns(label_);
  }

  // Accepted and ignored: Metal auto-tracks hazards. See the file header.
  void Transition(IResource*, ResourceState) override {}
  void TransitionMany(std::span<const ResourceTransition>) override {}

  IRenderPass* BeginRenderPass(const RenderPassDesc& desc) override {
    MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
    for (size_t i = 0; i < desc.color_attachments.size(); ++i) {
      const auto& a = desc.color_attachments[i];
      auto* v = static_cast<MetalTextureView*>(a.view);
      if (!v) continue;
      rp.colorAttachments[i].texture = v->Handle();
      rp.colorAttachments[i].loadAction = ToMtl(a.load_op);
      rp.colorAttachments[i].storeAction = ToMtl(a.store_op);
      rp.colorAttachments[i].clearColor =
          MTLClearColorMake(a.clear_color[0], a.clear_color[1],
                            a.clear_color[2], a.clear_color[3]);
    }
    if (auto* dv = static_cast<MetalTextureView*>(desc.depth_attachment.view)) {
      rp.depthAttachment.texture = dv->Handle();
      rp.depthAttachment.loadAction = ToMtl(desc.depth_attachment.load_op);
      rp.depthAttachment.storeAction = ToMtl(desc.depth_attachment.store_op);
      // Reversed-Z: far is 0.0.
      rp.depthAttachment.clearDepth = desc.depth_attachment.clear_depth;
    }

    id<MTLRenderCommandEncoder> enc =
        [cmd_ renderCommandEncoderWithDescriptor:rp];
    if (!enc) {
      spdlog::error("rhi/metal: renderCommandEncoder failed for '{}'", desc.label);
      return nullptr;
    }
    if (!desc.label.empty()) enc.label = Ns(desc.label);
    render_passes_.push_back(std::make_unique<MetalRenderPass>(enc));
    return render_passes_.back().get();
  }

  IComputePass* BeginComputePass(const std::string& label) override {
    id<MTLComputeCommandEncoder> enc = [cmd_ computeCommandEncoder];
    if (!enc) {
      spdlog::error("rhi/metal: computeCommandEncoder failed for '{}'", label);
      return nullptr;
    }
    if (!label.empty()) enc.label = Ns(label);
    compute_passes_.push_back(std::make_unique<MetalComputePass>(enc));
    return compute_passes_.back().get();
  }

  void CopyBufferToBuffer(IBuffer* src, uint64_t so, IBuffer* dst, uint64_t dof,
                          uint64_t size) override {
    auto* s = static_cast<MetalBuffer*>(src);
    auto* d = static_cast<MetalBuffer*>(dst);
    if (!s || !d || size == 0) return;
    id<MTLBlitCommandEncoder> blit = [cmd_ blitCommandEncoder];
    [blit copyFromBuffer:s->Handle()
            sourceOffset:so
                toBuffer:d->Handle()
       destinationOffset:dof
                    size:size];
    [blit endEncoding];
  }

  void CopyTextureToBuffer(ITexture* src, uint32_t mip, uint32_t layer,
                           IBuffer* dst, uint64_t off) override {
    auto* s = static_cast<MetalTexture*>(src);
    auto* d = static_cast<MetalBuffer*>(dst);
    if (!s || !d) return;
    const uint32_t w = std::max(1u, s->GetWidth() >> mip);
    const uint32_t h = std::max(1u, s->GetHeight() >> mip);
    const uint32_t bpr = w * FormatByteSize(s->GetFormat());

    id<MTLBlitCommandEncoder> blit = [cmd_ blitCommandEncoder];
    [blit copyFromTexture:s->Handle()
                sourceSlice:layer
                sourceLevel:mip
               sourceOrigin:MTLOriginMake(0, 0, 0)
                 sourceSize:MTLSizeMake(w, h, 1)
                   toBuffer:d->Handle()
          destinationOffset:off
     destinationBytesPerRow:bpr
   destinationBytesPerImage:size_t(bpr) * h];
    [blit endEncoding];
  }

  void Finish() override { finished_ = true; }
  bool IsFinished() const override { return finished_; }

  id<MTLCommandBuffer> CommandBuffer() const { return cmd_; }

 private:
  id<MTLCommandBuffer> cmd_;
  std::string label_;
  std::vector<std::unique_ptr<MetalRenderPass>> render_passes_;
  std::vector<std::unique_ptr<MetalComputePass>> compute_passes_;
  bool finished_ = false;
};

// ---------------------------------------------------------------------------
// Device
// ---------------------------------------------------------------------------

MTLTextureUsage ToMtlUsage(TextureUsage u) {
  MTLTextureUsage out = MTLTextureUsageUnknown;
  if (Has(u, TextureUsage::Sampled)) out |= MTLTextureUsageShaderRead;
  if (Has(u, TextureUsage::Storage)) out |= MTLTextureUsageShaderWrite;
  if (Has(u, TextureUsage::RenderTarget) || Has(u, TextureUsage::DepthStencil)) {
    out |= MTLTextureUsageRenderTarget;
  }
  return out;
}

class MetalDevice final : public IRhiDevice {
 public:
  MetalDevice(id<MTLDevice> dev, id<MTLCommandQueue> queue, std::string label,
              uint32_t frames_in_flight)
      : device_(dev), queue_(queue), label_(std::move(label)),
        frames_in_flight_(frames_in_flight) {
    // The TIMELINE owns the semaphore, because the timeline is what signals
    // it from completion handlers. A second handle on the device left the
    // timeline's nil and dispatch_semaphore_signal(nil) crashed on the first
    // frame that retired.
    retire_->sem = dispatch_semaphore_create(frames_in_flight);
  }

  ~MetalDevice() override {
    WaitIdle();
    // A frame left open -- an error path that returned between BeginFrame and
    // EndFrame -- means one semaphore count was taken and never returned.
    // libdispatch TRAPS on destroying a semaphore below its initial value
    // ("Semaphore object deallocated while in use"), so the caller's mistake
    // would surface as a crash inside libdispatch with none of its own frames
    // in the backtrace. Say what actually happened, and rebalance.
    if (in_frame_) {
      spdlog::error(
          "rhi/metal: device destroyed with frame {} still open -- every "
          "BeginFrame must be matched by an EndFrame",
          current_frame_);
      in_frame_ = false;
      dispatch_semaphore_signal(retire_->sem);
    }
  }

  BackendKind GetBackend() const override { return BackendKind::Metal; }

  BufferPtr CreateBuffer(const BufferDesc& d) override {
    // Shared storage keeps readback a memcpy on unified memory.
    id<MTLBuffer> buf = [device_ newBufferWithLength:std::max<uint64_t>(d.size, 1)
                                             options:MTLResourceStorageModeShared];
    if (!buf) {
      spdlog::error("rhi/metal: newBuffer failed for '{}' ({} bytes)", d.label,
                    d.size);
      return nullptr;
    }
    if (!d.label.empty()) buf.label = Ns(d.label);
    return std::make_shared<MetalBuffer>(buf, d, retire_);
  }

  TexturePtr CreateTexture(const TextureDesc& d) override {
    MTLTextureDescriptor* td = [[MTLTextureDescriptor alloc] init];
    td.textureType = d.array_layers > 1 ? MTLTextureType2DArray : MTLTextureType2D;
    td.pixelFormat = ToMtl(d.format);
    td.width = d.width;
    td.height = d.height;
    td.arrayLength = d.array_layers;
    td.mipmapLevelCount = d.mip_levels;
    td.usage = ToMtlUsage(d.usage);
    // Depth and render targets must be private; everything else stays shared
    // so Write() can go through replaceRegion.
    td.storageMode = Has(d.usage, TextureUsage::DepthStencil)
                         ? MTLStorageModePrivate
                         : MTLStorageModeShared;

    id<MTLTexture> tex = [device_ newTextureWithDescriptor:td];
    if (!tex) {
      spdlog::error("rhi/metal: newTexture failed for '{}'", d.label);
      return nullptr;
    }
    if (!d.label.empty()) tex.label = Ns(d.label);
    return std::make_shared<MetalTexture>(tex, d, retire_);
  }

  SamplerPtr CreateSampler(const SamplerDesc& d) override {
    MTLSamplerDescriptor* sd = [[MTLSamplerDescriptor alloc] init];
    sd.minFilter = ToMtlFilter(d.min_filter);
    sd.magFilter = ToMtlFilter(d.mag_filter);
    sd.mipFilter = ToMtlMipFilter(d.mip_filter);
    sd.sAddressMode = ToMtl(d.address_u);
    sd.tAddressMode = ToMtl(d.address_v);
    sd.maxAnisotropy = std::max<uint16_t>(1, d.max_anisotropy);
    id<MTLSamplerState> s = [device_ newSamplerStateWithDescriptor:sd];
    if (!s) {
      spdlog::error("rhi/metal: newSamplerState failed for '{}'", d.label);
      return nullptr;
    }
    return std::make_shared<MetalSampler>(s, d, retire_);
  }

  ShaderModulePtr CreateShaderModule(const std::string& source,
                                     const ShaderReflection& refl,
                                     const std::string& label) override {
    NSError* err = nil;
    MTLCompileOptions* opts = [[MTLCompileOptions alloc] init];
    id<MTLLibrary> lib = [device_ newLibraryWithSource:Ns(source)
                                               options:opts
                                                 error:&err];
    if (!lib) {
      spdlog::error("rhi/metal: MSL compile failed for '{}': {}", label,
                    err ? err.localizedDescription.UTF8String : "unknown");
      return nullptr;
    }
    return std::make_shared<MetalShaderModule>(lib, refl, label);
  }

  RenderPipelinePtr CreateRenderPipeline(const RenderPipelineDesc& d) override {
    auto* vsm = static_cast<MetalShaderModule*>(d.vertex_shader);
    if (!vsm) {
      spdlog::error("rhi/metal: '{}' has no vertex shader", d.label);
      return nullptr;
    }
    auto* fsm = static_cast<MetalShaderModule*>(d.fragment_shader);

    MTLRenderPipelineDescriptor* pd = [[MTLRenderPipelineDescriptor alloc] init];
    pd.vertexFunction = [vsm->Library() newFunctionWithName:Ns(d.vertex_entry)];
    if (!pd.vertexFunction) {
      spdlog::error("rhi/metal: '{}' has no vertex entry '{}'", d.label,
                    d.vertex_entry);
      return nullptr;
    }
    if (fsm) {
      pd.fragmentFunction = [fsm->Library() newFunctionWithName:Ns(d.fragment_entry)];
      if (!pd.fragmentFunction) {
        spdlog::error("rhi/metal: '{}' has no fragment entry '{}'", d.label,
                      d.fragment_entry);
        return nullptr;
      }
    }
    for (size_t i = 0; i < d.color_formats.size(); ++i) {
      pd.colorAttachments[i].pixelFormat = ToMtl(d.color_formats[i]);
    }
    if (d.depth.format != Format::Undefined) {
      pd.depthAttachmentPixelFormat = ToMtl(d.depth.format);
    }
    // No vertex descriptor: the MVP pulls vertex data from storage buffers.

    NSError* err = nil;
    id<MTLRenderPipelineState> pso =
        [device_ newRenderPipelineStateWithDescriptor:pd error:&err];
    if (!pso) {
      spdlog::error("rhi/metal: render pipeline '{}' failed: {}", d.label,
                    err ? err.localizedDescription.UTF8String : "unknown");
      return nullptr;
    }

    // ALWAYS a state, even when depth is off entirely. A nil state meant
    // SetPipeline skipped setDepthStencilState:, so a depth-less pipeline
    // inherited whatever the previous draw left bound -- pipeline state has to
    // be fully determined by the pipeline (rule 7). "Off" is Always + no
    // write, stated explicitly.
    MTLDepthStencilDescriptor* dd = [[MTLDepthStencilDescriptor alloc] init];
    dd.depthCompareFunction = d.depth.test_enabled ? ToMtl(d.depth.compare)
                                                   : MTLCompareFunctionAlways;
    dd.depthWriteEnabled = d.depth.write_enabled;
    id<MTLDepthStencilState> dss =
        [device_ newDepthStencilStateWithDescriptor:dd];
    if (!dss) {
      spdlog::error("rhi/metal: depth-stencil state for '{}' failed", d.label);
      return nullptr;
    }
    return std::make_shared<MetalRenderPipeline>(pso, dss, d);
  }

  ComputePipelinePtr CreateComputePipeline(
      const ComputePipelineDesc& d) override {
    auto* sm = static_cast<MetalShaderModule*>(d.shader);
    if (!sm) {
      spdlog::error("rhi/metal: compute pipeline '{}' has no shader", d.label);
      return nullptr;
    }
    id<MTLFunction> fn = [sm->Library() newFunctionWithName:Ns(d.entry)];
    if (!fn) {
      // Slang renames an entry point called `main` to `main_0` on Metal, which
      // is a real porting trap -- say so rather than just failing.
      spdlog::error("rhi/metal: '{}' has no entry '{}' (note: Slang renames "
                    "`main` to `main_0` for Metal)", d.label, d.entry);
      return nullptr;
    }
    NSError* err = nil;
    id<MTLComputePipelineState> pso =
        [device_ newComputePipelineStateWithFunction:fn error:&err];
    if (!pso) {
      spdlog::error("rhi/metal: compute pipeline '{}' failed: {}", d.label,
                    err ? err.localizedDescription.UTF8String : "unknown");
      return nullptr;
    }
    return std::make_shared<MetalComputePipeline>(pso, d);
  }

  BindingTablePtr CreateBindingTable(const BindingTableDesc& d) override {
    auto resolved = ResolveBindingTable(d, MinBufferOffsetAlignment());
    if (!resolved) return nullptr;  // ResolveBindingTable logged why
    return std::make_shared<MetalBindingTable>(std::move(*resolved), d.group,
                                               d.label, retire_);
  }

  SwapchainPtr CreateSwapchain(const SwapchainDesc& d) override {
    // maximumDrawableCount accepts only 2 or 3. Refused rather than clamped:
    // a caller who asked for 4 and silently got 3 has a frame model that
    // disagrees with its own presentation depth.
    if (d.native_window &&
        (frames_in_flight_ < 2 || frames_in_flight_ > 3)) {
      spdlog::error(
          "rhi/metal: a presenting swapchain needs frames_in_flight of 2 or "
          "3, got {} -- CAMetalLayer.maximumDrawableCount accepts no other "
          "value",
          frames_in_flight_);
      return nullptr;
    }
    return std::make_unique<MetalSwapchain>(device_, queue_, d,
                                            frames_in_flight_, retire_);
  }

  std::unique_ptr<ICommandEncoder> CreateCommandEncoder(
      const std::string& label) override {
    id<MTLCommandBuffer> cmd = [queue_ commandBuffer];
    if (!cmd) {
      spdlog::error("rhi/metal: commandBuffer failed for '{}'", label);
      return nullptr;
    }
    return std::make_unique<MetalCommandEncoder>(cmd, label);
  }

  void Submit(ICommandEncoder& encoder) override {
    auto* me = static_cast<MetalCommandEncoder*>(&encoder);
    if (!me) return;

    // Drop anything that has already retired, so a long-running app does not
    // accumulate every command buffer it has ever submitted. Without this the
    // list only ever shrank in WaitIdle.
    PruneRetired();

    id<MTLCommandBuffer> cmd = me->CommandBuffer();

    // The handler MUST be attached before commit -- Metal asserts
    // "Completed handler provided after commit call" otherwise, which is why
    // the frame cannot simply hang one handler off its last buffer in
    // EndFrame. Instead every buffer of the frame carries one, and the frame
    // retires when its last outstanding buffer completes.
    if (in_frame_) {
      const uint64_t frame = current_frame_;
      {
        std::lock_guard<std::mutex> lock(retire_->frame_mutex);
        ++retire_->frames[frame].pending;
      }
      // Captures the shared timeline BY VALUE, not `this`. PruneRetired can
      // drop a buffer from in_flight_ before its handler runs, so the
      // destructor cannot guarantee it waited for every handler -- but a
      // handler holding a shared_ptr keeps what it touches alive regardless.
      FrameTimelinePtr timeline = retire_;
      [cmd addCompletedHandler:^(id<MTLCommandBuffer>) {
        timeline->OnBufferComplete(frame);
      }];
    }

    [cmd commit];
    in_flight_.push_back(cmd);
  }

  void WaitIdle() override {
    for (id<MTLCommandBuffer> cmd : in_flight_) {
      [cmd waitUntilCompleted];
      ReportIfFailed(cmd);
    }
    in_flight_.clear();
    // Every ENDED frame has now retired -- but a frame that is still open has
    // not, and the caller can go on binding resources into it. Declaring it
    // retired let Defer take its early-out and release a handle ARC was the
    // last owner of, which Null (whose WaitIdle only retires ended frames)
    // correctly refused to do. Same call, two answers, on a documented
    // observable.
    retire_->AdvanceRetired(in_frame_ ? current_frame_ - 1 : current_frame_);
    retire_->Collect();
  }

  size_t PendingDeletions() const override {
    return retire_->PendingCount();
  }

  // 32, not 256. Metal has no query for this -- the requirement is documented
  // per GPU family -- and the project's hardware floor is Apple silicon (M2+),
  // where constant-address-space buffer offsets need 32-byte alignment. Intel
  // Macs needed 256, and DX12 needs 256 for CBVs, so a DX12 backend will
  // report that instead.
  uint64_t MinBufferOffsetAlignment() const override { return 32; }

  bool Supports(DeviceFeature f) const override {
    switch (f) {
      case DeviceFeature::Atomic64MinMax:
        // Apple8 is where 64-bit atomic min/max on buffers arrives, and it is
        // the project's recorded hardware floor. Asked of the device rather
        // than assumed from the floor, because an older Mac would otherwise
        // render garbage with no diagnostic.
        return [device_ supportsFamily:MTLGPUFamilyApple8];
    }
    return false;
  }

  size_t InFlightCount() override {
    PruneRetired();
    return in_flight_.size();
  }

  // --- Frame model ---

  uint64_t BeginFrame() override {
    // Blocks here, at the TOP of the frame, rather than at nextDrawable.
    dispatch_semaphore_wait(retire_->sem, DISPATCH_TIME_FOREVER);
    ++current_frame_;
    in_frame_ = true;
    retire_->current_frame.store(current_frame_, std::memory_order_release);
    // Anything whose frame retired while we were away can go now.
    retire_->Collect();
    return current_frame_;
  }

  void EndFrame() override {
    const uint64_t frame = current_frame_;
    in_frame_ = false;

    bool retire_now = false;
    {
      std::lock_guard<std::mutex> lock(retire_->frame_mutex);
      auto& st = retire_->frames[frame];
      st.ended = true;
      // A frame that submitted nothing -- a skipped frame, which a minimized
      // or occluded window produces every tick -- has no completion handler
      // to retire it. Retire it here, or the semaphore count is never
      // returned and the Nth skipped frame blocks forever in BeginFrame.
      if (st.pending == 0) {
        retire_->frames.erase(frame);
        retire_now = true;
      }
    }
    if (retire_now) retire_->RetireAndSignal(frame);
  }

  uint64_t CurrentFrame() const override { return current_frame_; }
  uint64_t LastRetiredFrame() const override {
    return retire_->last_retired.load(std::memory_order_acquire);
  }
  uint32_t FramesInFlight() const override { return frames_in_flight_; }

  // The decorator owns validation; a bare Metal device observes nothing, so
  // nullopt means "nothing checked" rather than "clean".
  void BeginValidationScope() override {}
  std::optional<ValidationReport> EndValidationScope() override {
    return std::nullopt;
  }
  bool IsValidationEnabled() const override { return false; }

 private:
  // A command buffer that faulted -- shader trap, timeout, page fault -- is
  // still "retired", so it must be dropped from in_flight_. But dropping it
  // quietly turns a GPU fault into a readback full of zeroes, which surfaces
  // as an inexplicable value mismatch three layers up. Say it here, once.
  static void ReportIfFailed(id<MTLCommandBuffer> cmd) {
    if (cmd.status != MTLCommandBufferStatusError) return;
    NSError* err = cmd.error;
    spdlog::error("rhi/metal: command buffer '{}' FAILED on the GPU: {} ({})",
                  cmd.label ? cmd.label.UTF8String : "<unlabelled>",
                  err ? err.localizedDescription.UTF8String : "no error object",
                  err ? long(err.code) : 0L);
  }

  void PruneRetired() {
    in_flight_.erase(
        std::remove_if(in_flight_.begin(), in_flight_.end(),
                       [](id<MTLCommandBuffer> cmd) {
                         const MTLCommandBufferStatus s = cmd.status;
                         if (s == MTLCommandBufferStatusError) {
                           ReportIfFailed(cmd);
                           return true;
                         }
                         return s == MTLCommandBufferStatusCompleted;
                       }),
        in_flight_.end());
  }

  id<MTLDevice> device_;
  id<MTLCommandQueue> queue_;
  std::string label_;
  // Submitted but not yet retired. Metal keeps the resources a command buffer
  // references alive until it completes -- see the GPU-timeline note in
  // src/engine/rhi/CLAUDE.md for why a DX12 backend must do that itself.
  std::vector<id<MTLCommandBuffer>> in_flight_;

  // Frame model. The pacing semaphore and the per-frame bookkeeping live on
  // the shared FrameTimeline, not here, because completion handlers touch them
  // from Metal-owned threads and must not capture the device.
  struct FrameState {
    uint32_t pending = 0;  // submitted buffers not yet completed
    bool ended = false;
  };

  RetireQueuePtr retire_ = std::make_shared<FrameTimeline>();
  uint32_t frames_in_flight_ = 3;
  uint64_t current_frame_ = 0;
  bool in_frame_ = false;
};

}  // namespace

AutoreleasePoolScope::AutoreleasePoolScope()
    : pool_(objc_autoreleasePoolPush()) {}
AutoreleasePoolScope::~AutoreleasePoolScope() {
  objc_autoreleasePoolPop(pool_);
}

bool WeakHandleClearedAfterRetire(IRhiDevice& device) {
  __weak id<MTLBuffer> weak_handle = nil;
  bool held_while_in_flight = false;

  @autoreleasepool {
    device.BeginFrame();
    auto buf = device.CreateBuffer({.size = 4096,
                                    .usage = BufferUsage::Storage,
                                    .label = "weak_probe"});
    if (!buf) {
      spdlog::error("rhi/metal: weak probe could not create a buffer");
      return false;
    }
    weak_handle = static_cast<MetalBuffer*>(buf.get())->Handle();
    if (!weak_handle) {
      spdlog::error("rhi/metal: weak probe got a nil handle");
      return false;
    }

    buf->Destroy();
    buf.reset();  // drop the C++ object too; only the retire queue holds it now
    held_while_in_flight = weak_handle != nil;
    device.EndFrame();
  }

  device.WaitIdle();  // retires the frame and collects the queue
  @autoreleasepool {
  }
  const bool released = weak_handle == nil;

  if (!held_while_in_flight) {
    spdlog::error(
        "rhi/metal: a Destroy()ed handle was released while its frame was "
        "still in flight -- deferral is not happening");
  }
  if (!released) {
    spdlog::error(
        "rhi/metal: a Destroy()ed handle was never released after its frame "
        "retired -- the retire queue is stranding it");
  }
  return held_while_in_flight && released;
}

std::unique_ptr<IRhiDevice> CreateMetalDevice(const std::string& label,
                                              uint32_t frames_in_flight) {
  if (frames_in_flight == 0) {
    spdlog::error("rhi/metal: frames_in_flight must be at least 1");
    return nullptr;
  }
  id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
  if (!dev) {
    spdlog::error("rhi/metal: no Metal device available");
    return nullptr;
  }
  id<MTLCommandQueue> queue = [dev newCommandQueue];
  if (!queue) {
    spdlog::error("rhi/metal: newCommandQueue failed");
    return nullptr;
  }
  spdlog::info("rhi/metal: {} ({})", dev.name.UTF8String,
               label.empty() ? "unlabelled" : label.c_str());
  return std::make_unique<MetalDevice>(dev, queue, label, frames_in_flight);
}

}  // namespace badlands::rhi::metal
