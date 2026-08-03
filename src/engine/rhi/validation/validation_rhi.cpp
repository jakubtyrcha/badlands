#include "engine/rhi/validation/validation_rhi.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

namespace badlands::rhi::validation {
namespace {

// ---------------------------------------------------------------------------
// Violation sink, shared by the device and everything it hands out
// ---------------------------------------------------------------------------

class Recorder {
 public:
  void Report(const std::string& what) {
    spdlog::warn("rhi validation: {}", what);
    if (scope_depth_ > 0) {
      if (!scope_message_.empty()) scope_message_ += "; ";
      scope_message_ += what;
    }
    ++total_;
  }

  void BeginScope() {
    if (scope_depth_++ == 0) scope_message_.clear();
  }

  std::optional<std::string> EndScope() {
    if (scope_depth_ == 0) return std::nullopt;
    if (--scope_depth_ > 0) return std::nullopt;
    if (scope_message_.empty()) return std::nullopt;
    return scope_message_;
  }

  uint64_t TotalViolations() const { return total_; }

 private:
  std::string scope_message_;
  int scope_depth_ = 0;
  uint64_t total_ = 0;
};

// Shadow resource states for one encoder. Purely bookkeeping -- no GPU, no
// backend involvement.
class StateTracker {
 public:
  // State belongs to the underlying resource, not to a view of it: callers
  // transition a texture but bind its view, and those must be the same thing
  // as far as tracking is concerned.
  static IResource* Canonical(IResource* r) {
    if (auto* view = dynamic_cast<ITextureView*>(r)) {
      if (auto* tex = view->GetTexture()) return tex;
    }
    return r;
  }

  void Declare(IResource* r, ResourceState s) {
    if (r) states_[Canonical(r)] = s;
  }

  // Checks `r` is in `want`. Reports through `rec` if not.
  void Expect(Recorder& rec, IResource* r, ResourceState want,
              const char* context) {
    if (!r) return;
    r = Canonical(r);
    auto it = states_.find(r);
    const ResourceState have =
        it == states_.end() ? ResourceState::Undefined : it->second;
    if (have == want) return;
    rec.Report(fmt::format(
        "{}: resource '{}' is in state {} but {} requires {}. Declare it with "
        "ICommandEncoder::Transition before use.",
        context, r->GetLabel().empty() ? "<unlabelled>" : r->GetLabel(),
        ToString(have), context, ToString(want)));
  }

 private:
  std::unordered_map<IResource*, ResourceState> states_;
};

// Shared state handed down from the device to encoders and passes.
struct Context {
  Recorder recorder;
};

// ---------------------------------------------------------------------------
// Binding table wrapper
//
// The only resource kind the decorator wraps. It needs the table's entries to
// check them against reflection at creation and to state-check the resources
// they reference at bind time, and IBindingTable deliberately does not expose
// them -- that would be interface surface existing only for validation.
// ---------------------------------------------------------------------------

class ValidationBindingTable final : public IBindingTable {
 public:
  ValidationBindingTable(BindingTablePtr inner, std::vector<BindingEntry> entries)
      : inner_(std::move(inner)), entries_(std::move(entries)) {}

  uint32_t GetGroup() const override { return inner_->GetGroup(); }
  void Destroy() override { inner_->Destroy(); }
  bool IsDestroyed() const override { return inner_->IsDestroyed(); }
  const std::string& GetLabel() const override { return inner_->GetLabel(); }

  IBindingTable* Inner() const { return inner_.get(); }
  const std::vector<BindingEntry>& Entries() const { return entries_; }

 private:
  BindingTablePtr inner_;
  std::vector<BindingEntry> entries_;
};

IBindingTable* Unwrap(IBindingTable* t) {
  if (auto* v = dynamic_cast<ValidationBindingTable*>(t)) return v->Inner();
  return t;
}

// ---------------------------------------------------------------------------
// Passes
// ---------------------------------------------------------------------------

class ValidationRenderPass final : public IRenderPass {
 public:
  ValidationRenderPass(IRenderPass* inner, Context* ctx, StateTracker* states,
                       std::string label)
      : inner_(inner), ctx_(ctx), states_(states), label_(std::move(label)) {}

  void SetPipeline(IRenderPipeline* p) override {
    if (Ended("SetPipeline")) return;
    pipeline_ = p;
    inner_->SetPipeline(p);
  }

  void SetBindingTable(uint32_t group, IBindingTable* table) override {
    if (Ended("SetBindingTable")) return;
    CheckTableUsable(group, table);
    inner_->SetBindingTable(group, Unwrap(table));
  }

  void SetIndexBuffer(IBuffer* b, IndexFormat f, uint64_t off) override {
    if (Ended("SetIndexBuffer")) return;
    if (b && b->IsDestroyed()) {
      ctx_->recorder.Report("SetIndexBuffer: buffer '" + b->GetLabel() +
                            "' was destroyed");
    }
    index_bound_ = b != nullptr;
    inner_->SetIndexBuffer(b, f, off);
  }

  void SetViewport(float x, float y, float w, float h) override {
    if (Ended("SetViewport")) return;
    inner_->SetViewport(x, y, w, h);
  }
  void SetScissor(uint32_t x, uint32_t y, uint32_t w, uint32_t h) override {
    if (Ended("SetScissor")) return;
    inner_->SetScissor(x, y, w, h);
  }

  void Draw(uint32_t vc, uint32_t ic, uint32_t fv, uint32_t fi) override {
    if (Ended("Draw") || NoPipeline("Draw")) return;
    inner_->Draw(vc, ic, fv, fi);
  }

  void DrawIndexed(uint32_t ic, uint32_t inst, uint32_t fi, int32_t bv,
                   uint32_t finst) override {
    if (Ended("DrawIndexed") || NoPipeline("DrawIndexed")) return;
    if (!index_bound_) {
      ctx_->recorder.Report("DrawIndexed: no index buffer bound");
    }
    inner_->DrawIndexed(ic, inst, fi, bv, finst);
  }

  void DrawIndexedIndirect(IBuffer* args, uint64_t offset) override {
    if (Ended("DrawIndexedIndirect") || NoPipeline("DrawIndexedIndirect")) return;
    if (!index_bound_) {
      ctx_->recorder.Report("DrawIndexedIndirect: no index buffer bound");
    }
    if (args && !Has(args->GetUsage(), BufferUsage::Indirect)) {
      ctx_->recorder.Report("DrawIndexedIndirect: buffer '" + args->GetLabel() +
                            "' lacks BufferUsage::Indirect");
    }
    // The intent check that Metal cannot make for us.
    states_->Expect(ctx_->recorder, args, ResourceState::IndirectArg,
                    "DrawIndexedIndirect");
    inner_->DrawIndexedIndirect(args, offset);
  }

  void End() override {
    if (ended_) return;
    ended_ = true;
    inner_->End();
  }
  bool IsEnded() const override { return ended_; }

 private:
  bool Ended(const char* what) {
    if (!ended_) return false;
    ctx_->recorder.Report(std::string(what) + ": render pass '" + label_ +
                          "' has already ended");
    return true;
  }
  bool NoPipeline(const char* what) {
    if (pipeline_) return false;
    ctx_->recorder.Report(std::string(what) + ": no pipeline bound");
    return true;
  }
  void CheckTableUsable(uint32_t group, IBindingTable* table);

  IRenderPass* inner_;
  Context* ctx_;
  StateTracker* states_;
  std::string label_;
  IRenderPipeline* pipeline_ = nullptr;
  bool index_bound_ = false;
  bool ended_ = false;
};

class ValidationComputePass final : public IComputePass {
 public:
  ValidationComputePass(IComputePass* inner, Context* ctx, StateTracker* states,
                        std::string label)
      : inner_(inner), ctx_(ctx), states_(states), label_(std::move(label)) {}

  void SetPipeline(IComputePipeline* p) override {
    if (Ended("SetPipeline")) return;
    pipeline_ = p;
    inner_->SetPipeline(p);
  }
  void SetBindingTable(uint32_t group, IBindingTable* table) override {
    if (Ended("SetBindingTable")) return;
    CheckTableUsable(group, table);
    inner_->SetBindingTable(group, Unwrap(table));
  }
  void Dispatch(uint32_t x, uint32_t y, uint32_t z) override {
    if (Ended("Dispatch")) return;
    if (!pipeline_) {
      ctx_->recorder.Report("Dispatch: no pipeline bound");
      return;
    }
    if (x == 0 || y == 0 || z == 0) {
      ctx_->recorder.Report("Dispatch: zero workgroup count");
    }
    inner_->Dispatch(x, y, z);
  }
  void End() override {
    if (ended_) return;
    ended_ = true;
    inner_->End();
  }
  bool IsEnded() const override { return ended_; }

 private:
  bool Ended(const char* what) {
    if (!ended_) return false;
    ctx_->recorder.Report(std::string(what) + ": compute pass '" + label_ +
                          "' has already ended");
    return true;
  }
  void CheckTableUsable(uint32_t group, IBindingTable* table);

  IComputePass* inner_;
  Context* ctx_;
  StateTracker* states_;
  std::string label_;
  IComputePipeline* pipeline_ = nullptr;
  bool ended_ = false;
};

// Shared by both pass kinds: a bound table must be alive, must match the
// group it is bound at, and everything it references must be alive and in a
// shader-readable state.
void CheckBoundTable(Recorder& rec, StateTracker& states, uint32_t group,
                     IBindingTable* table, const char* context) {
  if (!table) {
    rec.Report(std::string(context) + ": null binding table at group " +
               std::to_string(group));
    return;
  }
  if (table->IsDestroyed()) {
    rec.Report(std::string(context) + ": binding table '" + table->GetLabel() +
               "' was destroyed");
    return;
  }
  if (table->GetGroup() != group) {
    rec.Report(fmt::format(
        "{}: binding table '{}' was created for group {} but bound at {}",
        context, table->GetLabel(), table->GetGroup(), group));
  }

  auto* wrapped = dynamic_cast<ValidationBindingTable*>(table);
  if (!wrapped) return;
  for (const auto& e : wrapped->Entries()) {
    IResource* res = nullptr;
    if (e.buffer) res = e.buffer;
    else if (e.texture_view) res = e.texture_view;
    else if (e.sampler) res = e.sampler;
    if (!res) continue;

    if (res->IsDestroyed()) {
      rec.Report(fmt::format("{}: binding table '{}' slot {} references "
                             "destroyed resource '{}'",
                             context, table->GetLabel(), e.slot,
                             res->GetLabel()));
      continue;
    }
    // Samplers carry no state; buffers and textures do.
    if (e.kind == BindingKind::Sampler) continue;
    const ResourceState want = e.kind == BindingKind::StorageBuffer
                                   ? ResourceState::ShaderWrite
                                   : ResourceState::ShaderRead;
    states.Expect(rec, res, want, context);
  }
}

void ValidationRenderPass::CheckTableUsable(uint32_t group,
                                            IBindingTable* table) {
  CheckBoundTable(ctx_->recorder, *states_, group, table, "SetBindingTable");
}
void ValidationComputePass::CheckTableUsable(uint32_t group,
                                             IBindingTable* table) {
  CheckBoundTable(ctx_->recorder, *states_, group, table, "SetBindingTable");
}

// ---------------------------------------------------------------------------
// Encoder
// ---------------------------------------------------------------------------

class ValidationEncoder final : public ICommandEncoder {
 public:
  ValidationEncoder(std::unique_ptr<ICommandEncoder> inner, Context* ctx,
                    std::string label)
      : inner_(std::move(inner)), ctx_(ctx), label_(std::move(label)) {}

  void Transition(IResource* r, ResourceState s) override {
    if (Finished("Transition")) return;
    if (!r) {
      ctx_->recorder.Report("Transition: null resource");
      return;
    }
    if (r->IsDestroyed()) {
      ctx_->recorder.Report("Transition: resource '" + r->GetLabel() +
                            "' was destroyed");
      return;
    }
    states_.Declare(r, s);
    inner_->Transition(r, s);
  }

  void TransitionMany(std::span<const ResourceTransition> batch) override {
    for (const auto& t : batch) Transition(t.resource, t.state);
  }

  IRenderPass* BeginRenderPass(const RenderPassDesc& desc) override {
    if (Finished("BeginRenderPass")) return nullptr;
    if (HasOpenPass()) {
      ctx_->recorder.Report("BeginRenderPass: a pass is already open");
    }
    CheckAttachments(desc);

    IRenderPass* inner = inner_->BeginRenderPass(desc);
    if (!inner) return nullptr;
    render_passes_.push_back(std::make_unique<ValidationRenderPass>(
        inner, ctx_, &states_, desc.label));
    return render_passes_.back().get();
  }

  IComputePass* BeginComputePass(const std::string& label) override {
    if (Finished("BeginComputePass")) return nullptr;
    if (HasOpenPass()) {
      ctx_->recorder.Report("BeginComputePass: a pass is already open");
    }
    IComputePass* inner = inner_->BeginComputePass(label);
    if (!inner) return nullptr;
    compute_passes_.push_back(
        std::make_unique<ValidationComputePass>(inner, ctx_, &states_, label));
    return compute_passes_.back().get();
  }

  void CopyBufferToBuffer(IBuffer* src, uint64_t so, IBuffer* dst, uint64_t dof,
                          uint64_t size) override {
    if (Finished("CopyBufferToBuffer")) return;
    states_.Expect(ctx_->recorder, src, ResourceState::CopySrc,
                   "CopyBufferToBuffer(src)");
    states_.Expect(ctx_->recorder, dst, ResourceState::CopyDst,
                   "CopyBufferToBuffer(dst)");
    if (src && !Has(src->GetUsage(), BufferUsage::CopySrc)) {
      ctx_->recorder.Report("CopyBufferToBuffer: src '" + src->GetLabel() +
                            "' lacks BufferUsage::CopySrc");
    }
    if (dst && !Has(dst->GetUsage(), BufferUsage::CopyDst)) {
      ctx_->recorder.Report("CopyBufferToBuffer: dst '" + dst->GetLabel() +
                            "' lacks BufferUsage::CopyDst");
    }
    inner_->CopyBufferToBuffer(src, so, dst, dof, size);
  }

  void CopyTextureToBuffer(ITexture* src, uint32_t mip, uint32_t layer,
                           IBuffer* dst, uint64_t off) override {
    if (Finished("CopyTextureToBuffer")) return;
    states_.Expect(ctx_->recorder, src, ResourceState::CopySrc,
                   "CopyTextureToBuffer(src)");
    states_.Expect(ctx_->recorder, dst, ResourceState::CopyDst,
                   "CopyTextureToBuffer(dst)");
    if (src && !Has(src->GetUsage(), TextureUsage::CopySrc)) {
      ctx_->recorder.Report("CopyTextureToBuffer: texture '" +
                            src->GetLabel() + "' lacks TextureUsage::CopySrc");
    }
    inner_->CopyTextureToBuffer(src, mip, layer, dst, off);
  }

  void Finish() override {
    if (finished_) {
      ctx_->recorder.Report("Finish: encoder '" + label_ + "' already finished");
      return;
    }
    for (const auto& p : render_passes_) {
      if (!p->IsEnded()) {
        ctx_->recorder.Report("Finish: a render pass was never ended");
      }
    }
    for (const auto& p : compute_passes_) {
      if (!p->IsEnded()) {
        ctx_->recorder.Report("Finish: a compute pass was never ended");
      }
    }
    finished_ = true;
    inner_->Finish();
  }

  bool IsFinished() const override { return finished_; }
  ICommandEncoder* Inner() const { return inner_.get(); }

 private:
  // Derived rather than tracked with a flag: the flag version was set on
  // Begin and never cleared on End, so every second pass on an encoder was
  // wrongly reported. Asking the passes is impossible to get out of sync.
  bool HasOpenPass() const {
    for (const auto& p : render_passes_) {
      if (!p->IsEnded()) return true;
    }
    for (const auto& p : compute_passes_) {
      if (!p->IsEnded()) return true;
    }
    return false;
  }

  bool Finished(const char* what) {
    if (!finished_) return false;
    ctx_->recorder.Report(std::string(what) + ": encoder '" + label_ +
                          "' has already finished");
    return true;
  }

  void CheckAttachments(const RenderPassDesc& desc) {
    for (size_t i = 0; i < desc.color_attachments.size(); ++i) {
      const auto& a = desc.color_attachments[i];
      if (!a.view) {
        ctx_->recorder.Report("BeginRenderPass: color attachment " +
                              std::to_string(i) + " has no view");
        continue;
      }
      ITexture* tex = a.view->GetTexture();
      if (tex && !Has(tex->GetUsage(), TextureUsage::RenderTarget)) {
        ctx_->recorder.Report("BeginRenderPass: color attachment '" +
                              tex->GetLabel() +
                              "' lacks TextureUsage::RenderTarget");
      }
      if (tex && IsDepthFormat(tex->GetFormat())) {
        ctx_->recorder.Report("BeginRenderPass: color attachment '" +
                              tex->GetLabel() + "' has a depth format");
      }
      states_.Expect(ctx_->recorder, tex, ResourceState::RenderTarget,
                     "BeginRenderPass(color)");
    }

    if (const auto& d = desc.depth_attachment; d.view) {
      ITexture* tex = d.view->GetTexture();
      if (tex && !Has(tex->GetUsage(), TextureUsage::DepthStencil)) {
        ctx_->recorder.Report("BeginRenderPass: depth attachment '" +
                              tex->GetLabel() +
                              "' lacks TextureUsage::DepthStencil");
      }
      if (tex && !IsDepthFormat(tex->GetFormat())) {
        ctx_->recorder.Report("BeginRenderPass: depth attachment '" +
                              tex->GetLabel() + "' has a color format");
      }
      states_.Expect(ctx_->recorder, tex,
                     d.read_only ? ResourceState::DepthRead
                                 : ResourceState::DepthWrite,
                     "BeginRenderPass(depth)");
    }
  }

  std::unique_ptr<ICommandEncoder> inner_;
  Context* ctx_;
  std::string label_;
  StateTracker states_;
  std::vector<std::unique_ptr<ValidationRenderPass>> render_passes_;
  std::vector<std::unique_ptr<ValidationComputePass>> compute_passes_;
  bool finished_ = false;
};

// ---------------------------------------------------------------------------
// Device
// ---------------------------------------------------------------------------

class ValidationDevice final : public IRhiDevice {
 public:
  explicit ValidationDevice(std::unique_ptr<IRhiDevice> inner)
      : inner_(std::move(inner)) {}

  BackendKind GetBackend() const override { return inner_->GetBackend(); }

  BufferPtr CreateBuffer(const BufferDesc& d) override {
    if (d.size == 0) ctx_.recorder.Report("CreateBuffer: zero size");
    if (!Any(d.usage)) {
      ctx_.recorder.Report("CreateBuffer: '" + d.label + "' has no usage flags");
    }
    return inner_->CreateBuffer(d);
  }

  TexturePtr CreateTexture(const TextureDesc& d) override {
    if (d.width == 0 || d.height == 0) {
      ctx_.recorder.Report("CreateTexture: '" + d.label + "' has a zero extent");
    }
    if (d.format == Format::Undefined) {
      ctx_.recorder.Report("CreateTexture: '" + d.label + "' has no format");
    }
    if (IsDepthFormat(d.format) && Has(d.usage, TextureUsage::RenderTarget)) {
      ctx_.recorder.Report("CreateTexture: '" + d.label +
                           "' is depth but asks for RenderTarget; use "
                           "DepthStencil");
    }
    return inner_->CreateTexture(d);
  }

  SamplerPtr CreateSampler(const SamplerDesc& d) override {
    return inner_->CreateSampler(d);
  }

  ShaderModulePtr CreateShaderModule(const std::string& src,
                                     const ShaderReflection& refl,
                                     const std::string& label) override {
    return inner_->CreateShaderModule(src, refl, label);
  }

  RenderPipelinePtr CreateRenderPipeline(const RenderPipelineDesc& d) override {
    if (!d.vertex_shader) {
      ctx_.recorder.Report("CreateRenderPipeline: '" + d.label +
                           "' has no vertex shader");
    }
    if (d.color_formats.empty() && d.depth.format == Format::Undefined) {
      ctx_.recorder.Report("CreateRenderPipeline: '" + d.label +
                           "' has neither color targets nor depth");
    }
    if (d.depth.test_enabled && d.depth.format == Format::Undefined) {
      ctx_.recorder.Report("CreateRenderPipeline: '" + d.label +
                           "' enables depth test with no depth format");
    }
    return inner_->CreateRenderPipeline(d);
  }

  ComputePipelinePtr CreateComputePipeline(
      const ComputePipelineDesc& d) override {
    if (!d.shader) {
      ctx_.recorder.Report("CreateComputePipeline: '" + d.label +
                           "' has no shader");
    }
    return inner_->CreateComputePipeline(d);
  }

  // The "unbound binding slot" check happens here, at creation, because that is
  // where both the reflection and the full entry list are in hand.
  BindingTablePtr CreateBindingTable(const BindingTableDesc& d) override {
    const ShaderReflection* refl = nullptr;
    if (d.render_pipeline) refl = &d.render_pipeline->GetReflection();
    else if (d.compute_pipeline) refl = &d.compute_pipeline->GetReflection();
    else ctx_.recorder.Report("CreateBindingTable: no pipeline given");

    if (refl) {
      for (const auto& rb : refl->bindings) {
        if (rb.group != d.group) continue;
        const bool bound = std::any_of(
            d.entries.begin(), d.entries.end(),
            [&](const BindingEntry& e) { return e.slot == rb.slot; });
        if (!bound) {
          ctx_.recorder.Report(fmt::format(
              "CreateBindingTable '{}': group {} slot {} ('{}') is declared by "
              "the shader but not bound",
              d.label, d.group, rb.slot, rb.name));
          continue;
        }
        const auto& e = *std::find_if(
            d.entries.begin(), d.entries.end(),
            [&](const BindingEntry& x) { return x.slot == rb.slot; });
        if (e.kind != rb.kind) {
          ctx_.recorder.Report(fmt::format(
              "CreateBindingTable '{}': group {} slot {} ('{}') bound as kind "
              "{} but the shader declares kind {}",
              d.label, d.group, rb.slot, rb.name, int(e.kind), int(rb.kind)));
        }
      }
    }

    for (const auto& e : d.entries) {
      IResource* res = e.buffer   ? static_cast<IResource*>(e.buffer)
                       : e.texture_view
                           ? static_cast<IResource*>(e.texture_view)
                           : static_cast<IResource*>(e.sampler);
      if (!res) {
        ctx_.recorder.Report(fmt::format(
            "CreateBindingTable '{}': slot {} has no resource", d.label, e.slot));
      } else if (res->IsDestroyed()) {
        ctx_.recorder.Report(fmt::format(
            "CreateBindingTable '{}': slot {} references destroyed resource "
            "'{}'", d.label, e.slot, res->GetLabel()));
      }
    }

    auto inner = inner_->CreateBindingTable(d);
    if (!inner) return nullptr;
    return std::make_shared<ValidationBindingTable>(std::move(inner),
                                                    d.entries);
  }

  std::unique_ptr<ICommandEncoder> CreateCommandEncoder(
      const std::string& label) override {
    auto inner = inner_->CreateCommandEncoder(label);
    if (!inner) return nullptr;
    return std::make_unique<ValidationEncoder>(std::move(inner), &ctx_, label);
  }

  void Submit(ICommandEncoder& encoder) override {
    auto* v = dynamic_cast<ValidationEncoder*>(&encoder);
    if (!v) {
      ctx_.recorder.Report("Submit: encoder did not come from this device");
      inner_->Submit(encoder);
      return;
    }
    if (!v->IsFinished()) {
      ctx_.recorder.Report("Submit: encoder was not finished");
    }
    inner_->Submit(*v->Inner());
  }

  void WaitIdle() override { inner_->WaitIdle(); }

  void BeginValidationScope() override { ctx_.recorder.BeginScope(); }
  std::optional<std::string> EndValidationScope() override {
    return ctx_.recorder.EndScope();
  }
  bool IsValidationEnabled() const override { return true; }

 private:
  std::unique_ptr<IRhiDevice> inner_;
  Context ctx_;
};

}  // namespace

std::unique_ptr<IRhiDevice> MakeValidationDevice(
    std::unique_ptr<IRhiDevice> inner) {
  if (!inner) return nullptr;
  return std::make_unique<ValidationDevice>(std::move(inner));
}

}  // namespace badlands::rhi::validation
