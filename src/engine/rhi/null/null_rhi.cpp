#include "engine/rhi/null/null_rhi.hpp"

#include <algorithm>
#include <cstring>
#include <map>
#include <tuple>

#include <spdlog/spdlog.h>

namespace badlands::rhi::null {
namespace {

// ---------------------------------------------------------------------------
// Resources
// ---------------------------------------------------------------------------

class NullResource : public virtual IResource {
 public:
  explicit NullResource(std::string label) : label_(std::move(label)) {}
  void Destroy() override { destroyed_ = true; }
  bool IsDestroyed() const override { return destroyed_; }
  const std::string& GetLabel() const override { return label_; }

 private:
  std::string label_;
  bool destroyed_ = false;
};

class NullBuffer final : public IBuffer, public NullResource {
 public:
  explicit NullBuffer(const BufferDesc& d)
      : NullResource(d.label), usage_(d.usage), data_(d.size, 0) {}

  uint64_t GetSize() const override { return data_.size(); }
  BufferUsage GetUsage() const override { return usage_; }

  void Write(uint64_t offset, std::span<const uint8_t> data) override {
    if (offset + data.size() > data_.size()) return;
    std::memcpy(data_.data() + offset, data.data(), data.size());
  }

  bool Read(uint64_t offset, std::span<uint8_t> out) override {
    if (offset + out.size() > data_.size()) return false;
    std::memcpy(out.data(), data_.data() + offset, out.size());
    return true;
  }

  // Real bytes, so an indirect draw can resolve its args without a GPU.
  const std::vector<uint8_t>& Bytes() const { return data_; }

 private:
  BufferUsage usage_;
  std::vector<uint8_t> data_;
};

class NullTexture;

class NullTextureView final : public ITextureView, public NullResource {
 public:
  NullTextureView(NullTexture* tex, Format fmt, std::string label,
                  const TextureViewDesc& resolved)
      : NullResource(std::move(label)), texture_(tex), format_(fmt),
        desc_(resolved) {}
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
  explicit NullTexture(const TextureDesc& d) : NullResource(d.label), desc_(d) {}

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
        this, desc_.format, desc_.label + ".view", *r);
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
  explicit NullSampler(const SamplerDesc& d)
      : NullResource(d.label), desc_(d) {}
  const SamplerDesc& GetDesc() const override { return desc_; }

 private:
  SamplerDesc desc_;
};

// Built only from an already-resolved table, exactly as Metal is: the shared
// resolver is what stops the two backends disagreeing about which tables are
// constructible (rule 6).
class NullBindingTable final : public IBindingTable, public NullResource {
 public:
  NullBindingTable(ResolvedBindingTable r, uint32_t group, std::string label)
      : NullResource(std::move(label)), group_(group), resolved_(std::move(r)) {}
  uint32_t GetGroup() const override { return group_; }
  const std::vector<BindingEntry>& Entries() const { return resolved_.entries; }
  const std::vector<uint32_t>& Indices() const { return resolved_.indices; }

 private:
  uint32_t group_;
  ResolvedBindingTable resolved_;
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

  void SetPipeline(IRenderPipeline* p) override {
    log_->Record({.kind = RecordedCommand::Kind::SetRenderPipeline,
                  .label = label_, .object = p});
  }
  void SetBindingTable(uint32_t group, IBindingTable* t) override {
    log_->Record({.kind = RecordedCommand::Kind::SetBindingTable,
                  .label = label_, .object = t, .group = group});
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
    RecordedCommand c{.kind = RecordedCommand::Kind::DrawIndexed,
                      .label = label_, .object = index_buffer_};
    c.draw_args = {index_count, instance_count, first_index, base_vertex,
                   first_instance};
    log_->Record(std::move(c));
  }

  // Resolves the args from the indirect buffer's real bytes. This is what lets
  // a GPU-driven draw be asserted on without a GPU.
  void DrawIndexedIndirect(IBuffer* args, uint64_t offset) override {
    RecordedCommand c{.kind = RecordedCommand::Kind::DrawIndexedIndirect,
                      .label = label_, .object = args};
    if (auto* nb = dynamic_cast<NullBuffer*>(args)) {
      DrawIndexedIndirectArgs resolved{};
      std::span<uint8_t> dst(reinterpret_cast<uint8_t*>(&resolved),
                             sizeof(resolved));
      if (nb->Read(offset, dst)) c.draw_args = resolved;
    }
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
  IBuffer* index_buffer_ = nullptr;
  bool ended_ = false;
};

class NullComputePass final : public IComputePass {
 public:
  NullComputePass(CommandLog* log, std::string label)
      : log_(log), label_(std::move(label)) {}

  void SetPipeline(IComputePipeline* p) override {
    log_->Record({.kind = RecordedCommand::Kind::SetComputePipeline,
                  .label = label_, .object = p});
  }
  void SetBindingTable(uint32_t group, IBindingTable* t) override {
    log_->Record({.kind = RecordedCommand::Kind::SetBindingTable,
                  .label = label_, .object = t, .group = group});
  }
  void Dispatch(uint32_t x, uint32_t y, uint32_t z) override {
    RecordedCommand c{.kind = RecordedCommand::Kind::Dispatch, .label = label_};
    c.dispatch[0] = x; c.dispatch[1] = y; c.dispatch[2] = z;
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
  explicit NullDevice(std::string label) : label_(std::move(label)) {}

  BackendKind GetBackend() const override { return BackendKind::Null; }

  BufferPtr CreateBuffer(const BufferDesc& d) override {
    return std::make_shared<NullBuffer>(d);
  }
  TexturePtr CreateTexture(const TextureDesc& d) override {
    return std::make_shared<NullTexture>(d);
  }
  SamplerPtr CreateSampler(const SamplerDesc& d) override {
    return std::make_shared<NullSampler>(d);
  }

  ShaderModulePtr CreateShaderModule(const std::string& source,
                                     const ShaderReflection& reflection,
                                     const std::string& label) override {
    (void)source;  // nothing to compile
    return std::make_shared<NullShaderModule>(reflection, label);
  }
  RenderPipelinePtr CreateRenderPipeline(
      const RenderPipelineDesc& d) override {
    return std::make_shared<NullRenderPipeline>(d);
  }
  ComputePipelinePtr CreateComputePipeline(
      const ComputePipelineDesc& d) override {
    return std::make_shared<NullComputePipeline>(d);
  }
  BindingTablePtr CreateBindingTable(const BindingTableDesc& d) override {
    auto resolved = ResolveBindingTable(d);
    if (!resolved) return nullptr;  // ResolveBindingTable logged why
    return std::make_shared<NullBindingTable>(std::move(*resolved), d.group,
                                              d.label);
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
  void WaitIdle() override {}
  // Null executes on Submit, so nothing is ever in flight. This is a real
  // answer, not a stub -- which is why the base declares it pure.
  size_t InFlightCount() override { return 0; }

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
  std::string label_;
  CommandLog log_;
};

std::unique_ptr<IRhiDevice> CreateNullDevice(const std::string& label) {
  return std::make_unique<NullDevice>(label);
}

CommandLog* GetCommandLog(IRhiDevice& device) {
  // Walk the decorator chain: a validated Null device is a ValidationDevice
  // wrapping a NullDevice, and a direct cast would miss it.
  for (IRhiDevice* d = &device; d != nullptr; d = d->Inner()) {
    if (auto* nd = dynamic_cast<NullDevice*>(d)) return &nd->Log();
  }
  return nullptr;
}

}  // namespace badlands::rhi::null
