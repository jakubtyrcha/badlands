#include "engine/rhi/null/null_rhi.hpp"

#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <span>
#include <tuple>

#include <spdlog/spdlog.h>

namespace badlands::rhi::null {
namespace {

// ---------------------------------------------------------------------------
// Resources
// ---------------------------------------------------------------------------

// Mirrors the Metal backend's RetireQueue. Null has no GPU handles, but it
// must model the same OBSERVABLE behaviour or PendingDeletions() would mean
// two different things per backend (rule 6) -- and the DX12 backend's
// specification is written against exactly these assertions.
struct RetireQueue {
  mutable std::mutex mutex;
  // (frame that must retire first, the memory being held). Buffers move their
  // real bytes in here, so the memory genuinely outlives Destroy().
  std::vector<std::pair<uint64_t, std::vector<uint8_t>>> pending;
  uint64_t current_frame = 0;
  uint64_t last_retired = 0;

  void Defer(std::vector<uint8_t> held) {
    std::lock_guard<std::mutex> lock(mutex);
    // Destroyed outside any frame: nothing can be reading it.
    if (current_frame <= last_retired) return;
    pending.emplace_back(current_frame, std::move(held));
  }

  void Collect() {
    std::lock_guard<std::mutex> lock(mutex);
    std::erase_if(pending,
                  [this](const auto& e) { return e.first <= last_retired; });
  }

  size_t Count() const {
    std::lock_guard<std::mutex> lock(mutex);
    return pending.size();
  }
};

using RetireQueuePtr = std::shared_ptr<RetireQueue>;

class NullResource : public virtual IResource {
 public:
  NullResource(std::string label, RetireQueuePtr retire)
      : label_(std::move(label)), retire_(std::move(retire)) {}

  void Destroy() override {
    if (destroyed_) return;  // idempotent, and must not defer twice
    destroyed_ = true;
    if (retire_) retire_->Defer(TakeMemory());
  }
  bool IsDestroyed() const override { return destroyed_; }
  const std::string& GetLabel() const override { return label_; }

 protected:
  // The bytes this resource is holding, moved out. Only buffers have any; the
  // rest defer an empty payload so the COUNT still matches Metal's.
  virtual std::vector<uint8_t> TakeMemory() { return {}; }

 private:
  std::string label_;
  RetireQueuePtr retire_;
  bool destroyed_ = false;
};

class NullBuffer final : public IBuffer, public NullResource {
 public:
  NullBuffer(const BufferDesc& d, RetireQueuePtr retire)
      : NullResource(d.label, std::move(retire)), usage_(d.usage),
        data_(d.size, 0) {}

  uint64_t GetSize() const override { return data_.size(); }
  BufferUsage GetUsage() const override { return usage_; }

  // Subtraction, matching Metal: `offset + size` wraps and lets a wild offset
  // through the guard. See the note in metal_rhi.mm.
  void Write(uint64_t offset, std::span<const uint8_t> data) override {
    if (data.size() > data_.size() || offset > data_.size() - data.size()) {
      spdlog::error(
          "rhi/null: Write of {} bytes at offset {} runs past buffer '{}' of "
          "{} bytes",
          data.size(), offset, GetLabel(), data_.size());
      return;
    }
    std::memcpy(data_.data() + offset, data.data(), data.size());
  }

  bool Read(uint64_t offset, std::span<uint8_t> out) override {
    if (out.size() > data_.size() || offset > data_.size() - out.size()) {
      spdlog::error(
          "rhi/null: Read of {} bytes at offset {} runs past buffer '{}' of "
          "{} bytes",
          out.size(), offset, GetLabel(), data_.size());
      return false;
    }
    std::memcpy(out.data(), data_.data() + offset, out.size());
    return true;
  }

  // Real bytes, so an indirect draw can resolve its args without a GPU.
  const std::vector<uint8_t>& Bytes() const { return data_; }

 protected:
  std::vector<uint8_t> TakeMemory() override { return std::move(data_); }

 private:
  BufferUsage usage_;
  std::vector<uint8_t> data_;
};

class NullTexture;

class NullTextureView final : public ITextureView, public NullResource {
 public:
  NullTextureView(NullTexture* tex, Format fmt, std::string label,
                  const TextureViewDesc& resolved, RetireQueuePtr retire)
      : NullResource(std::move(label), std::move(retire)), texture_(tex),
        format_(fmt), desc_(resolved) {}
  ITexture* GetTexture() const override;
  bool IsDestroyed() const override;
  Format GetFormat() const override { return format_; }
  const TextureViewDesc& GetDesc() const override { return desc_; }

 private:
  NullTexture* texture_;
  Format format_;
  TextureViewDesc desc_;
};

class NullTexture final : public ITexture, public NullResource {
 public:
  NullTexture(const TextureDesc& d, RetireQueuePtr retire)
      : NullResource(d.label, retire), desc_(d), retire_(std::move(retire)) {}

  uint32_t GetWidth() const override { return desc_.width; }
  uint32_t GetHeight() const override { return desc_.height; }
  uint32_t GetArrayLayers() const override { return desc_.array_layers; }
  uint32_t GetMipLevels() const override { return desc_.mip_levels; }
  Format GetFormat() const override { return desc_.format; }
  TextureUsage GetUsage() const override { return desc_.usage; }

  // Same resolution, same refusals, same cache key as Metal -- the two must
  // not disagree about what a view descriptor means (rule 6).
  ITextureView* CreateView(const TextureViewDesc& vd) override {
    // Before the cache lookup, and before resolution -- same order as Metal.
    // Null has no GPU handle to go nil, so without this check the two backends
    // answered differently for a destroyed texture and only Metal refused.
    if (IsDestroyed()) {
      spdlog::error("rhi/null: CreateView on destroyed texture '{}'",
                    GetLabel());
      return nullptr;
    }
    const auto r = ResolveViewDesc(vd, desc_, GetLabel());
    if (!r) return nullptr;  // ResolveViewDesc logged why
    const ViewKey key{r->base_mip, r->mip_count, r->base_layer, r->layer_count};
    auto it = views_.find(key);
    if (it != views_.end()) return it->second.get();
    auto view = std::make_unique<NullTextureView>(
        this, desc_.format, desc_.label + ".view", *r, retire_);
    auto* raw = view.get();
    views_.emplace(key, std::move(view));
    return raw;
  }

  ITextureView* GetDefaultView() override { return CreateView({}); }

  void Write(uint32_t mip, uint32_t layer,
             std::span<const uint8_t> data) override {
    written_bytes_ += data.size();
    (void)mip;
    (void)layer;
  }

  uint64_t WrittenBytes() const { return written_bytes_; }

 private:
  using ViewKey = std::tuple<uint32_t, uint32_t, uint32_t, uint32_t>;

  TextureDesc desc_;
  RetireQueuePtr retire_;
  std::map<ViewKey, std::unique_ptr<NullTextureView>> views_;
  uint64_t written_bytes_ = 0;
};

ITexture* NullTextureView::GetTexture() const { return texture_; }
// Mirrors Metal: a view is destroyed once its texture is. Backends must not
// disagree about a documented contract (rule 6).
bool NullTextureView::IsDestroyed() const {
  return NullResource::IsDestroyed() || (texture_ && texture_->IsDestroyed());
}

class NullSampler final : public ISampler, public NullResource {
 public:
  NullSampler(const SamplerDesc& d, RetireQueuePtr retire)
      : NullResource(d.label, std::move(retire)), desc_(d) {}
  const SamplerDesc& GetDesc() const override { return desc_; }

 private:
  SamplerDesc desc_;
};

// Built only from an already-resolved table, exactly as Metal is: the shared
// resolver is what stops the two backends disagreeing about which tables are
// constructible (rule 6).
class NullBindingTable final : public IBindingTable, public NullResource {
 public:
  NullBindingTable(ResolvedBindingTable r, uint32_t group, std::string label,
                   RetireQueuePtr retire)
      : NullResource(std::move(label), std::move(retire)), group_(group),
        resolved_(std::move(r)) {}
  uint32_t GetGroup() const override { return group_; }
  const std::vector<BindingEntry>& Entries() const { return resolved_.entries; }
  const std::vector<uint32_t>& Indices() const { return resolved_.indices; }

 private:
  uint32_t group_;
  ResolvedBindingTable resolved_;
};

// ---------------------------------------------------------------------------
// Swapchain
// ---------------------------------------------------------------------------

// Models a real drawable pool rather than handing back one texture forever:
// consecutive frames must get DIFFERENT backbuffers, or a test that thinks it
// proved double-buffering proved nothing.
class NullSwapchain final : public ISwapchain {
 public:
  NullSwapchain(IRhiDevice& device, const SwapchainDesc& desc, uint32_t depth)
      : device_(device), desc_(desc), depth_(std::max(1u, depth)) {
    Recreate();
  }

  AcquiredFrame Acquire() override {
    if (fault_ == SwapchainFault::Lost) return {AcquireStatus::Lost, nullptr};
    if (fault_ == SwapchainFault::Skip) return {AcquireStatus::Skip, nullptr};
    // A zero-sized surface is what a minimized window reports. Never a 0x0
    // texture, and never an error -- just nothing to draw into this frame.
    if (desc_.width == 0 || desc_.height == 0) {
      return {AcquireStatus::Skip, nullptr};
    }
    if (acquired_) {
      spdlog::error(
          "rhi/null: swapchain '{}' acquired twice without a Present",
          desc_.label);
      return {AcquireStatus::Skip, nullptr};
    }
    CollectRetired();
    acquired_ = true;
    auto* view = images_[next_ % images_.size()]->GetDefaultView();
    next_ = (next_ + 1) % images_.size();
    return {AcquireStatus::Ok, view};
  }

  void Present() override {
    if (!acquired_) {
      spdlog::error("rhi/null: swapchain '{}' presented without an acquire",
                    desc_.label);
      return;
    }
    acquired_ = false;
    ++presented_;
  }

  void Resize(uint32_t width, uint32_t height) override {
    if (width == desc_.width && height == desc_.height) return;
    desc_.width = width;
    desc_.height = height;
    Recreate();
  }

  uint32_t GetWidth() const override { return desc_.width; }
  uint32_t GetHeight() const override { return desc_.height; }
  Format GetFormat() const override { return desc_.format; }

  void SetFault(SwapchainFault f) { fault_ = f; }
  uint64_t PresentCount() const { return presented_; }

 private:
  // Old backbuffers whose frame has retired can finally go.
  void CollectRetired() {
    const uint64_t retired = device_.LastRetiredFrame();
    std::erase_if(retired_,
                  [retired](const auto& e) { return e.first <= retired; });
  }

  void Recreate() {
    // Destroy() releases the memory through the frame timeline -- but the
    // texture OBJECTS have to outlive that, because callers hold views into
    // them as raw borrowed pointers and a view must outlive Destroy() on its
    // texture. Dropping them here dangled every view handed out before a
    // resize, which a test found by segfaulting.
    for (auto& img : images_) img->Destroy();
    if (!images_.empty()) {
      retired_.emplace_back(device_.CurrentFrame(), std::move(images_));
    }
    images_.clear();
    acquired_ = false;
    next_ = 0;
    if (desc_.width == 0 || desc_.height == 0) return;
    for (uint32_t i = 0; i < depth_; ++i) {
      images_.push_back(device_.CreateTexture(
          {.width = desc_.width,
           .height = desc_.height,
           .format = desc_.format,
           .usage = TextureUsage::RenderTarget | TextureUsage::CopySrc,
           .label = desc_.label + ".image" + std::to_string(i)}));
    }
  }

  IRhiDevice& device_;
  SwapchainDesc desc_;
  uint32_t depth_;
  std::vector<TexturePtr> images_;
  // (frame at which they were replaced, the images). Views into these stay
  // dereferenceable -- they report destroyed -- until the frame retires.
  std::vector<std::pair<uint64_t, std::vector<TexturePtr>>> retired_;
  size_t next_ = 0;
  bool acquired_ = false;
  uint64_t presented_ = 0;
  SwapchainFault fault_ = SwapchainFault::None;
};

// ---------------------------------------------------------------------------
// Shaders and pipelines
// ---------------------------------------------------------------------------

class NullShaderModule final : public IShaderModule {
 public:
  NullShaderModule(ShaderReflection r, std::string label)
      : reflection_(std::move(r)), label_(std::move(label)) {}
  const ShaderReflection& GetReflection() const override { return reflection_; }
  const std::string& GetLabel() const override { return label_; }

 private:
  ShaderReflection reflection_;
  std::string label_;
};

// Merges the reflection of the stages a pipeline actually uses, so a caller
// can resolve a binding by name without knowing which stage declared it.
ShaderReflection MergeReflection(const IShaderModule* a,
                                 const IShaderModule* b) {
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
      const bool dup = std::any_of(
          out.uniform_blocks.begin(), out.uniform_blocks.end(),
          [&](const auto& e) { return e.group == ub.group && e.slot == ub.slot; });
      if (!dup) out.uniform_blocks.push_back(ub);
    }
    out.entry_points.insert(out.entry_points.end(), r.entry_points.begin(),
                            r.entry_points.end());
  };
  append(a);
  append(b);
  return out;
}

class NullRenderPipeline final : public IRenderPipeline {
 public:
  explicit NullRenderPipeline(const RenderPipelineDesc& d)
      : desc_(d),
        reflection_(MergeReflection(d.vertex_shader, d.fragment_shader)) {}
  const ShaderReflection& GetReflection() const override { return reflection_; }
  const RenderPipelineDesc& GetDesc() const override { return desc_; }

 private:
  RenderPipelineDesc desc_;
  ShaderReflection reflection_;
};

class NullComputePipeline final : public IComputePipeline {
 public:
  explicit NullComputePipeline(const ComputePipelineDesc& d)
      : reflection_(MergeReflection(d.shader, nullptr)), entry_(d.entry) {}

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

 private:
  ShaderReflection reflection_;
  std::string entry_;
};

// ---------------------------------------------------------------------------
// Passes and encoder
// ---------------------------------------------------------------------------

class NullRenderPass final : public IRenderPass {
 public:
  NullRenderPass(CommandLog* log, std::string label)
      : log_(log), label_(std::move(label)) {}

  // Tracked, not merely logged. Metal refuses a draw with no pipeline bound,
  // so a Null that recorded one anyway would be the SILENT backend on the same
  // input -- the divergence rule 6 exists to stop, and the one that makes Null
  // a bad oracle for the shared conformance list.
  void SetPipeline(IRenderPipeline* p) override {
    pipeline_ = p;
    log_->Record({.kind = RecordedCommand::Kind::SetRenderPipeline,
                  .label = label_, .object = p});
  }
  void SetBindingTable(uint32_t group, IBindingTable* t,
                       std::span<const uint32_t> dynamic_offsets) override {
    RecordedCommand c{.kind = RecordedCommand::Kind::SetBindingTable,
                      .label = label_, .object = t, .group = group};
    // Recorded so a test can assert WHICH offsets reached the backend, with no
    // GPU. Otherwise dynamic offsets are only observable in pixels.
    c.dynamic_offsets.assign(dynamic_offsets.begin(), dynamic_offsets.end());
    log_->Record(std::move(c));
  }
  void SetIndexBuffer(IBuffer* b, IndexFormat, uint64_t) override {
    index_buffer_ = b;
    log_->Record({.kind = RecordedCommand::Kind::SetIndexBuffer,
                  .label = label_, .object = b});
  }
  void SetViewport(float, float, float, float) override {
    log_->Record({.kind = RecordedCommand::Kind::SetViewport, .label = label_});
  }
  void SetScissor(uint32_t, uint32_t, uint32_t, uint32_t) override {
    log_->Record({.kind = RecordedCommand::Kind::SetScissor, .label = label_});
  }

  void Draw(uint32_t vertex_count, uint32_t instance_count,
            uint32_t first_vertex, uint32_t first_instance) override {
    if (!pipeline_) {
      spdlog::error("rhi/null: Draw with no pipeline bound");
      return;
    }
    RecordedCommand c{.kind = RecordedCommand::Kind::Draw, .label = label_};
    c.draw_args.index_count = vertex_count;
    c.draw_args.instance_count = instance_count;
    c.draw_args.first_index = first_vertex;
    c.draw_args.first_instance = first_instance;
    log_->Record(std::move(c));
  }

  void DrawIndexed(uint32_t index_count, uint32_t instance_count,
                   uint32_t first_index, int32_t base_vertex,
                   uint32_t first_instance) override {
    if (!pipeline_) {
      spdlog::error("rhi/null: DrawIndexed with no pipeline bound");
      return;
    }
    if (!index_buffer_) {
      spdlog::error("rhi/null: DrawIndexed with no index buffer bound");
      return;
    }
    RecordedCommand c{.kind = RecordedCommand::Kind::DrawIndexed,
                      .label = label_, .object = index_buffer_};
    c.draw_args = {index_count, instance_count, first_index, base_vertex,
                   first_instance};
    log_->Record(std::move(c));
  }

  // Resolves the args from the indirect buffer's real bytes. This is what lets
  // a GPU-driven draw be asserted on without a GPU.
  void DrawIndexedIndirect(IBuffer* args, uint64_t offset) override {
    if (!pipeline_) {
      spdlog::error("rhi/null: DrawIndexedIndirect with no pipeline bound");
      return;
    }
    if (!index_buffer_) {
      spdlog::error("rhi/null: DrawIndexedIndirect with no index buffer bound");
      return;
    }
    auto* nb = dynamic_cast<NullBuffer*>(args);
    if (!nb) {
      spdlog::error(
          "rhi/null: DrawIndexedIndirect with no argument buffer (or one from "
          "another backend), so there is nothing to read a count from");
      return;
    }
    DrawIndexedIndirectArgs resolved{};
    if (!IndirectArgsInBounds(nb, offset, sizeof(resolved),
                              "rhi/null: DrawIndexedIndirect")) {
      return;
    }
    std::span<uint8_t> dst(reinterpret_cast<uint8_t*>(&resolved),
                           sizeof(resolved));
    if (!nb->Read(offset, dst)) return;  // Read logged why
    RecordedCommand c{.kind = RecordedCommand::Kind::DrawIndexedIndirect,
                      .label = label_, .object = args};
    c.draw_args = resolved;
    log_->Record(std::move(c));
  }

  void End() override {
    if (ended_) return;
    ended_ = true;
    log_->Record({.kind = RecordedCommand::Kind::EndRenderPass, .label = label_});
  }
  bool IsEnded() const override { return ended_; }

 private:
  CommandLog* log_;
  std::string label_;
  IRenderPipeline* pipeline_ = nullptr;
  IBuffer* index_buffer_ = nullptr;
  bool ended_ = false;
};

class NullComputePass final : public IComputePass {
 public:
  NullComputePass(CommandLog* log, std::string label)
      : log_(log), label_(std::move(label)) {}

  // Tracked for the same reason as the render pass's: Metal refuses a dispatch
  // with no pipeline bound, and a Null that recorded one would be the silent
  // half of a divergence.
  void SetPipeline(IComputePipeline* p) override {
    pipeline_ = p;
    log_->Record({.kind = RecordedCommand::Kind::SetComputePipeline,
                  .label = label_, .object = p});
  }
  void SetBindingTable(uint32_t group, IBindingTable* t,
                       std::span<const uint32_t> dynamic_offsets) override {
    RecordedCommand c{.kind = RecordedCommand::Kind::SetBindingTable,
                      .label = label_, .object = t, .group = group};
    // Recorded so a test can assert WHICH offsets reached the backend, with no
    // GPU. Otherwise dynamic offsets are only observable in pixels.
    c.dynamic_offsets.assign(dynamic_offsets.begin(), dynamic_offsets.end());
    log_->Record(std::move(c));
  }
  void Dispatch(uint32_t x, uint32_t y, uint32_t z) override {
    if (!pipeline_) {
      spdlog::error("rhi/null: Dispatch with no pipeline bound");
      return;
    }
    RecordedCommand c{.kind = RecordedCommand::Kind::Dispatch, .label = label_};
    c.dispatch[0] = x; c.dispatch[1] = y; c.dispatch[2] = z;
    log_->Record(std::move(c));
  }

  // Resolves the counts from the buffer's REAL bytes, exactly as
  // DrawIndexedIndirect does. That is what makes a GPU-driven dispatch count
  // assertable with no GPU: a test seeds the args and reads back what would
  // have been dispatched.
  void DispatchIndirect(IBuffer* args, uint64_t offset) override {
    if (!pipeline_) {
      spdlog::error("rhi/null: DispatchIndirect with no pipeline bound");
      return;
    }
    auto* nb = dynamic_cast<NullBuffer*>(args);
    if (!nb) {
      spdlog::error(
          "rhi/null: DispatchIndirect with no argument buffer (or one from "
          "another backend), so there is nothing to read a count from");
      return;
    }
    DispatchIndirectArgs resolved{};
    if (!IndirectArgsInBounds(nb, offset, sizeof(resolved),
                              "rhi/null: DispatchIndirect")) {
      return;
    }
    std::span<uint8_t> dst(reinterpret_cast<uint8_t*>(&resolved),
                           sizeof(resolved));
    if (!nb->Read(offset, dst)) return;  // Read logged why
    RecordedCommand c{.kind = RecordedCommand::Kind::DispatchIndirect,
                      .label = label_, .object = args};
    c.dispatch[0] = resolved.x;
    c.dispatch[1] = resolved.y;
    c.dispatch[2] = resolved.z;
    log_->Record(std::move(c));
  }
  void End() override {
    if (ended_) return;
    ended_ = true;
    log_->Record({.kind = RecordedCommand::Kind::EndComputePass,
                  .label = label_});
  }
  bool IsEnded() const override { return ended_; }

 private:
  CommandLog* log_;
  std::string label_;
  IComputePipeline* pipeline_ = nullptr;
  bool ended_ = false;
};

class NullCommandEncoder final : public ICommandEncoder {
 public:
  NullCommandEncoder(CommandLog* log, std::string label)
      : log_(log), label_(std::move(label)) {}

  void Transition(IResource* r, ResourceState s) override {
    log_->Record({.kind = RecordedCommand::Kind::Transition, .label = label_,
                  .object = r, .state = s});
  }
  void TransitionMany(std::span<const ResourceTransition> batch) override {
    for (const auto& t : batch) Transition(t.resource, t.state);
  }

  IRenderPass* BeginRenderPass(const RenderPassDesc& desc) override {
    RecordedCommand c{.kind = RecordedCommand::Kind::BeginRenderPass,
                      .label = desc.label};
    c.color_attachment_count = uint32_t(desc.color_attachments.size());
    c.has_depth = desc.depth_attachment.view != nullptr;
    if (!desc.color_attachments.empty()) {
      c.first_color_load = desc.color_attachments[0].load_op;
      c.first_color_store = desc.color_attachments[0].store_op;
    }
    if (c.has_depth) {
      c.depth_load = desc.depth_attachment.load_op;
      c.depth_store = desc.depth_attachment.store_op;
      c.depth_read_only = desc.depth_attachment.read_only;
    }
    log_->Record(std::move(c));
    render_passes_.push_back(
        std::make_unique<NullRenderPass>(log_, desc.label));
    return render_passes_.back().get();
  }

  IComputePass* BeginComputePass(const std::string& label) override {
    log_->Record({.kind = RecordedCommand::Kind::BeginComputePass,
                  .label = label});
    compute_passes_.push_back(std::make_unique<NullComputePass>(log_, label));
    return compute_passes_.back().get();
  }

  void CopyBufferToBuffer(IBuffer* src, uint64_t src_offset, IBuffer* dst,
                          uint64_t dst_offset, uint64_t size) override {
    // Move the real bytes, so a copy round-trip is assertable.
    auto* s = dynamic_cast<NullBuffer*>(src);
    auto* d = dynamic_cast<NullBuffer*>(dst);
    if (s && d && size > 0) {
      std::vector<uint8_t> tmp(size);
      if (s->Read(src_offset, tmp)) d->Write(dst_offset, tmp);
    }
    log_->Record({.kind = RecordedCommand::Kind::CopyBufferToBuffer,
                  .label = label_, .object = dst});
  }

  void CopyTextureToBuffer(ITexture* src, uint32_t, uint32_t, IBuffer* dst,
                           uint64_t) override {
    log_->Record({.kind = RecordedCommand::Kind::CopyTextureToBuffer,
                  .label = label_, .object = dst});
    (void)src;
  }

  void Finish() override {
    if (finished_) return;
    finished_ = true;
    log_->Record({.kind = RecordedCommand::Kind::Finish, .label = label_});
  }
  bool IsFinished() const override { return finished_; }

 private:
  CommandLog* log_;
  std::string label_;
  std::vector<std::unique_ptr<NullRenderPass>> render_passes_;
  std::vector<std::unique_ptr<NullComputePass>> compute_passes_;
  bool finished_ = false;
};

}  // namespace

// ---------------------------------------------------------------------------
// CommandLog
// ---------------------------------------------------------------------------

size_t CommandLog::Count(RecordedCommand::Kind kind) const {
  return size_t(std::count_if(commands_.begin(), commands_.end(),
                              [kind](const auto& c) { return c.kind == kind; }));
}

const RecordedCommand* CommandLog::Find(RecordedCommand::Kind kind,
                                        size_t n) const {
  size_t seen = 0;
  for (const auto& c : commands_) {
    if (c.kind != kind) continue;
    if (seen++ == n) return &c;
  }
  return nullptr;
}

// ---------------------------------------------------------------------------
// Device
// ---------------------------------------------------------------------------

class NullDevice final : public IRhiDevice {
 public:
  NullDevice(std::string label, uint32_t frames_in_flight)
      : label_(std::move(label)), frames_in_flight_(frames_in_flight) {}

  BackendKind GetBackend() const override { return BackendKind::Null; }

  BufferPtr CreateBuffer(const BufferDesc& d) override {
    return std::make_shared<NullBuffer>(d, retire_);
  }
  TexturePtr CreateTexture(const TextureDesc& d) override {
    return std::make_shared<NullTexture>(d, retire_);
  }
  SamplerPtr CreateSampler(const SamplerDesc& d) override {
    return std::make_shared<NullSampler>(d, retire_);
  }

  ShaderModulePtr CreateShaderModule(const std::string& source,
                                     const ShaderReflection& reflection,
                                     const std::string& label) override {
    (void)source;  // nothing to compile
    return std::make_shared<NullShaderModule>(reflection, label);
  }
  RenderPipelinePtr CreateRenderPipeline(
      const RenderPipelineDesc& d) override {
    // The SAME shared check Metal makes. Null runs no shaders and could not
    // care less what the blend states say -- but "which pipelines can exist"
    // is a contract, and a backend that accepts what another refuses is the
    // divergence rule 6 exists to stop.
    if (!ValidateBlendStates(d)) return nullptr;  // logged there
    return std::make_shared<NullRenderPipeline>(d);
  }
  ComputePipelinePtr CreateComputePipeline(
      const ComputePipelineDesc& d) override {
    return std::make_shared<NullComputePipeline>(d);
  }
  BindingTablePtr CreateBindingTable(const BindingTableDesc& d) override {
    auto resolved = ResolveBindingTable(d, MinBufferOffsetAlignment());
    if (!resolved) return nullptr;  // ResolveBindingTable logged why
    return std::make_shared<NullBindingTable>(std::move(*resolved), d.group,
                                              d.label, retire_);
  }

  SwapchainPtr CreateSwapchain(const SwapchainDesc& d) override {
    return std::make_unique<NullSwapchain>(*this, d, frames_in_flight_);
  }

  std::unique_ptr<ICommandEncoder> CreateCommandEncoder(
      const std::string& label) override {
    return std::make_unique<NullCommandEncoder>(&log_, label);
  }
  // Nothing executes, so nothing is ever in flight.
  void Submit(ICommandEncoder& encoder) override {
    log_.Record(
        {.kind = RecordedCommand::Kind::Submit, .object = &encoder});
  }
  void WaitIdle() override {
    RetireAll();
    retire_->Collect();
  }

  size_t PendingDeletions() const override { return retire_->Count(); }

  // The STRICTEST value any target requires, so a layout that satisfies Null
  // satisfies every backend. Null is where portability problems should surface
  // first, not last.
  uint64_t MinBufferOffsetAlignment() const override { return 256; }

  // Null executes no shaders, so it supports no shader-level feature. Claiming
  // otherwise would let a test "pass" against a backend that ran nothing.
  bool Supports(DeviceFeature) const override { return false; }
  // Null executes on Submit, so nothing is ever in flight. This is a real
  // answer, not a stub -- which is why the base declares it pure.
  size_t InFlightCount() override { return 0; }

  // --- Frame model ---
  //
  // Blocks exactly as Metal does, on the same contract. In Manual mode the
  // test is the GPU: it calls RetireOldestFrame to let BeginFrame through. A
  // test that begins more frames than it retires will hang, which is a test
  // bug and what ctest timeouts are for -- the alternative, refusing instead
  // of blocking, would make Null and Metal disagree about the contract
  // (rule 6).
  uint64_t BeginFrame() override {
    uint64_t frame = 0;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      cv_.wait(lock, [this] {
        return current_frame_ - last_retired_ < frames_in_flight_;
      });
      frame = ++current_frame_;
      PublishFrameLocked();
    }
    retire_->Collect();
    return frame;
  }

  void EndFrame() override {
    std::lock_guard<std::mutex> lock(mutex_);
    ended_.push_back(current_frame_);
    if (mode_ == RetirementMode::Immediate) RetireOldestLocked();
  }

  uint64_t CurrentFrame() const override {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_frame_;
  }
  uint64_t LastRetiredFrame() const override {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_retired_;
  }
  uint32_t FramesInFlight() const override { return frames_in_flight_; }

  void SetMode(RetirementMode mode) {
    std::lock_guard<std::mutex> lock(mutex_);
    mode_ = mode;
    if (mode_ == RetirementMode::Immediate) {
      while (RetireOldestLocked()) {
      }
    }
  }

  bool RetireOldest() {
    std::lock_guard<std::mutex> lock(mutex_);
    return RetireOldestLocked();
  }

  // The Null backend never observes anything itself; the validation decorator
  // is what fills these in when it wraps a device. nullopt here means exactly
  // "nothing checked" -- NOT a clean run, which is the distinction the report
  // type exists to preserve.
  void BeginValidationScope() override {}
  std::optional<ValidationReport> EndValidationScope() override {
    return std::nullopt;
  }
  bool IsValidationEnabled() const override { return false; }

  CommandLog& Log() { return log_; }

 private:
  // Retires the oldest ENDED frame. Frames retire in order, so the watermark
  // only ever moves forward by one.
  bool RetireOldestLocked() {
    if (ended_.empty()) return false;
    last_retired_ = ended_.front();
    ended_.pop_front();
    PublishFrameLocked();
    cv_.notify_all();
    return true;
  }

  // The retire queue has its own lock, so it carries a copy of the watermarks
  // rather than reaching back into this one.
  void PublishFrameLocked() {
    std::lock_guard<std::mutex> lock(retire_->mutex);
    retire_->current_frame = current_frame_;
    retire_->last_retired = last_retired_;
  }

  void RetireAll() {
    std::lock_guard<std::mutex> lock(mutex_);
    while (RetireOldestLocked()) {
    }
  }

  std::string label_;
  CommandLog log_;
  RetireQueuePtr retire_ = std::make_shared<RetireQueue>();

  // Frame model. `mutable` because the observers are const but must lock.
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  uint32_t frames_in_flight_ = 3;
  uint64_t current_frame_ = 0;
  uint64_t last_retired_ = 0;
  std::deque<uint64_t> ended_;  // ended but not yet retired, oldest first
  RetirementMode mode_ = RetirementMode::Immediate;
};

std::unique_ptr<IRhiDevice> CreateNullDevice(const std::string& label,
                                             uint32_t frames_in_flight) {
  if (frames_in_flight == 0) {
    spdlog::error("rhi/null: frames_in_flight must be at least 1");
    return nullptr;
  }
  return std::make_unique<NullDevice>(label, frames_in_flight);
}

namespace {

// Same decorator walk GetCommandLog does: a validated Null device is a
// ValidationDevice wrapping a NullDevice, and a direct cast would miss it.
NullDevice* FindNull(IRhiDevice& device) {
  for (IRhiDevice* d = &device; d != nullptr; d = d->Inner()) {
    if (auto* nd = dynamic_cast<NullDevice*>(d)) return nd;
  }
  return nullptr;
}

}  // namespace

CommandLog* GetCommandLog(IRhiDevice& device) {
  auto* nd = FindNull(device);
  return nd ? &nd->Log() : nullptr;
}

void SetRetirementMode(IRhiDevice& device, RetirementMode mode) {
  auto* nd = FindNull(device);
  if (!nd) {
    spdlog::error(
        "rhi/null: SetRetirementMode on a {} device -- retirement is only "
        "controllable on Null",
        ToString(device.GetBackend()));
    return;
  }
  nd->SetMode(mode);
}

namespace {

// Walks the decorator chain, exactly as FindNull does for devices.
NullSwapchain* FindNullSwapchain(ISwapchain& swapchain) {
  for (ISwapchain* s = &swapchain; s != nullptr; s = s->Inner()) {
    if (auto* ns = dynamic_cast<NullSwapchain*>(s)) return ns;
  }
  return nullptr;
}

}  // namespace

void SetSwapchainFault(ISwapchain& swapchain, SwapchainFault fault) {
  auto* ns = FindNullSwapchain(swapchain);
  if (!ns) {
    spdlog::error(
        "rhi/null: SetSwapchainFault on a swapchain that is not Null's -- "
        "faults are only injectable there");
    return;
  }
  ns->SetFault(fault);
}

uint64_t PresentCount(const ISwapchain& swapchain) {
  auto* ns = FindNullSwapchain(const_cast<ISwapchain&>(swapchain));
  return ns ? ns->PresentCount() : 0;
}

bool RetireOldestFrame(IRhiDevice& device) {
  auto* nd = FindNull(device);
  if (!nd) {
    spdlog::error(
        "rhi/null: RetireOldestFrame on a {} device -- retirement is only "
        "controllable on Null",
        ToString(device.GetBackend()));
    return false;
  }
  if (nd->RetireOldest()) return true;
  spdlog::error("rhi/null: RetireOldestFrame with no frame outstanding");
  return false;
}

}  // namespace badlands::rhi::null
