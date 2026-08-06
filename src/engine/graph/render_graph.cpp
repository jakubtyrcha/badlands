#include "engine/graph/render_graph.hpp"

#include <algorithm>

#include <spdlog/spdlog.h>

namespace badlands::graph {

using namespace badlands::rhi;

const char* ToString(Access a) {
  switch (a) {
    case Access::Read: return "Read";
    case Access::Write: return "Write";
    case Access::ColorTarget: return "ColorTarget";
  }
  return "?";
}

namespace {

ResourceState StateFor(Access a) {
  switch (a) {
    case Access::Read: return ResourceState::ShaderRead;
    case Access::Write: return ResourceState::ShaderWrite;
    case Access::ColorTarget: return ResourceState::RenderTarget;
  }
  return ResourceState::Undefined;
}

}  // namespace

// ---------------------------------------------------------------------------
// Builders
// ---------------------------------------------------------------------------

RasterPassBuilder& RasterPassBuilder::ColorTarget(ResourceHandle target,
                                                  LoadOp load, StoreOp store,
                                                  const float clear[4]) {
  RenderGraph::Attachment a{.target = target, .load = load, .store = store};
  if (clear) {
    a.clear[0] = clear[0];
    a.clear[1] = clear[1];
    a.clear[2] = clear[2];
    a.clear[3] = clear[3];
  }
  graph_->passes_[pass_].colors.push_back(a);
  // An attachment IS a write. Declaring it twice -- once as a target and once
  // as a barrier -- is how the two drift apart.
  graph_->Declare(pass_, target, Access::ColorTarget);
  return *this;
}

RasterPassBuilder& RasterPassBuilder::Reads(ResourceHandle h) {
  graph_->Declare(pass_, h, Access::Read);
  return *this;
}
RasterPassBuilder& RasterPassBuilder::Writes(ResourceHandle h) {
  graph_->Declare(pass_, h, Access::Write);
  return *this;
}
void RasterPassBuilder::Execute(std::function<void(const RasterContext&)> fn) {
  graph_->passes_[pass_].raster_fn = std::move(fn);
}

ComputePassBuilder& ComputePassBuilder::Reads(ResourceHandle h) {
  graph_->Declare(pass_, h, Access::Read);
  return *this;
}
ComputePassBuilder& ComputePassBuilder::Writes(ResourceHandle h) {
  graph_->Declare(pass_, h, Access::Write);
  return *this;
}
void ComputePassBuilder::Execute(std::function<void(const ComputeContext&)> fn) {
  graph_->passes_[pass_].compute_fn = std::move(fn);
}

// ---------------------------------------------------------------------------
// Resources
// ---------------------------------------------------------------------------

ResourceHandle RenderGraph::ImportTexture(ITexture* texture,
                                          ResourceState state,
                                          std::string_view name) {
  if (!texture) {
    spdlog::error("graph: ImportTexture('{}') was given no texture", name);
    return {};
  }
  Resource r;
  r.name = name.empty() ? texture->GetLabel() : std::string(name);
  r.texture = texture;
  r.state = state;
  r.entry_state = state;
  // Imported means "somebody else produced this", so a pass may read it
  // without any pass in THIS graph having written it.
  r.imported = true;
  resources_.push_back(std::move(r));
  return {uint32_t(resources_.size() - 1)};
}

ResourceHandle RenderGraph::ImportBuffer(IBuffer* buffer, ResourceState state,
                                         std::string_view name) {
  if (!buffer) {
    spdlog::error("graph: ImportBuffer('{}') was given no buffer", name);
    return {};
  }
  Resource r;
  r.name = name.empty() ? buffer->GetLabel() : std::string(name);
  r.buffer = buffer;
  r.state = state;
  r.entry_state = state;
  r.imported = true;
  resources_.push_back(std::move(r));
  return {uint32_t(resources_.size() - 1)};
}

ResourceHandle RenderGraph::CreateTexture(const TextureDesc& desc) {
  Resource r;
  r.name = desc.label.empty() ? "transient" : desc.label;
  r.desc = desc;
  r.transient = true;
  r.state = ResourceState::Undefined;
  r.entry_state = ResourceState::Undefined;
  resources_.push_back(std::move(r));
  return {uint32_t(resources_.size() - 1)};
}

RasterPassBuilder RenderGraph::AddRasterPass(std::string_view name) {
  passes_.push_back({.name = std::string(name), .raster = true});
  return RasterPassBuilder(this, uint32_t(passes_.size() - 1));
}

ComputePassBuilder RenderGraph::AddComputePass(std::string_view name) {
  passes_.push_back({.name = std::string(name), .raster = false});
  return ComputePassBuilder(this, uint32_t(passes_.size() - 1));
}

void RenderGraph::Declare(uint32_t pass, ResourceHandle h, Access access) {
  // Records the access and NOTHING else. Marking the resource "written" here
  // made the read-before-write check in Compile order-BLIND: every Declare runs
  // before Compile, so a pass reading a transient that only a LATER pass writes
  // passed the one guard that exists for it. Producedness is now decided in
  // Compile, walking the passes in execution order.
  passes_[pass].accesses.emplace_back(h, access);
}

// ---------------------------------------------------------------------------
// Compile
// ---------------------------------------------------------------------------

bool RenderGraph::Compile() {
  compiled_ = false;
  order_.clear();

  // Declaration order IS execution order for now, and that is a deliberate
  // limitation rather than an oversight: with three passes there is nothing to
  // reorder, and a topological sort whose ordering nothing exercises is a
  // sorting bug waiting to happen. What Compile() DOES do is validate the
  // declarations, which is the part that has teeth today.
  // Which resources are readable at each point in the ORDER. Imported ones
  // arrive readable; a transient becomes readable only after a pass that writes
  // it has run.
  std::vector<bool> produced(resources_.size(), false);
  for (size_t i = 0; i < resources_.size(); ++i) {
    produced[i] = resources_[i].imported;
  }

  for (size_t p = 0; p < passes_.size(); ++p) {
    const Pass& pass = passes_[p];
    if (pass.raster && !pass.raster_fn) {
      spdlog::error("graph: raster pass '{}' has no Execute callback",
                    pass.name);
      return false;
    }
    if (!pass.raster && !pass.compute_fn) {
      spdlog::error("graph: compute pass '{}' has no Execute callback",
                    pass.name);
      return false;
    }
    if (pass.raster && pass.colors.empty()) {
      spdlog::error(
          "graph: raster pass '{}' declares no colour target -- a pass that "
          "renders nowhere is a mistake, not an optimisation",
          pass.name);
      return false;
    }
    for (const auto& [h, access] : pass.accesses) {
      if (!h.IsValid() || h.id >= resources_.size()) {
        spdlog::error(
            "graph: pass '{}' declares {} on a resource that was never "
            "imported or created",
            pass.name, ToString(access));
        return false;
      }
      // A read of something not yet produced is the classic graph bug: it
      // renders whatever the memory happened to hold, which is usually the
      // previous frame and occasionally garbage. "Not yet" is the load-bearing
      // word -- a pass that writes it LATER does not help this pass.
      if (access == Access::Read && !produced[h.id]) {
        spdlog::error(
            "graph: pass '{}' reads '{}', which nothing has written by this "
            "point in the order and which was not imported -- it would read "
            "undefined contents",
            pass.name, resources_[h.id].name);
        return false;
      }
    }
    // Marked only AFTER this pass's own reads are checked, so a pass that both
    // reads and writes one resource still has to have it produced beforehand.
    for (const auto& [h, access] : pass.accesses) {
      if (access != Access::Read) produced[h.id] = true;
    }
    order_.push_back(uint32_t(p));
  }

  // Transient textures exist only once the graph knows they are needed.
  for (Resource& r : resources_) {
    if (!r.transient || r.owned) continue;
    r.owned = device_->CreateTexture(r.desc);
    if (!r.owned) {
      spdlog::error("graph: could not create transient texture '{}'", r.name);
      return false;
    }
    r.texture = r.owned.get();
  }

  compiled_ = true;
  return true;
}

// ---------------------------------------------------------------------------
// Execute
// ---------------------------------------------------------------------------

void RenderGraph::Execute(ICommandEncoder& encoder) {
  if (!compiled_) {
    spdlog::error(
        "graph: Execute called without a successful Compile -- refusing to "
        "record passes whose declarations were never checked");
    return;
  }

  // Back to the states the graph was handed, so a compiled graph can be
  // executed more than once. Without this the second Execute sees every
  // resource already in the state its first pass wants and emits no transitions
  // at all -- correct on Metal, which tracks hazards itself, and a
  // use-before-barrier on DX12.
  for (Resource& r : resources_) r.state = r.entry_state;

  for (uint32_t index : order_) {
    Pass& pass = passes_[index];

    // Transitions FIRST, all of them, before the pass opens. A transition
    // recorded inside a render pass is not a barrier, it is a validation
    // error -- and on DX12 it would be a hazard.
    for (const auto& [h, access] : pass.accesses) {
      Resource& r = resources_[h.id];
      const ResourceState want = StateFor(access);
      if (r.state == want) continue;  // already there; nothing to declare
      IResource* res = r.texture ? static_cast<IResource*>(r.texture)
                                 : static_cast<IResource*>(r.buffer);
      if (!res) continue;
      encoder.Transition(res, want);
      r.state = want;
    }

    if (pass.raster) {
      RenderPassDesc desc;
      desc.label = pass.name;
      for (const Attachment& a : pass.colors) {
        Resource& r = resources_[a.target.id];
        if (!r.texture) continue;
        desc.color_attachments.push_back(
            {.view = r.texture->GetDefaultView(),
             .load_op = a.load,
             .store_op = a.store,
             .clear_color = {a.clear[0], a.clear[1], a.clear[2], a.clear[3]}});
      }
      auto* rp = encoder.BeginRenderPass(desc);
      if (!rp) {
        spdlog::error("graph: pass '{}' could not begin", pass.name);
        continue;
      }
      const Resource& first = resources_[pass.colors.front().target.id];
      RasterContext ctx{.pass = rp,
                        .width = first.texture ? first.texture->GetWidth() : 0,
                        .height = first.texture ? first.texture->GetHeight() : 0};
      // The viewport is set HERE rather than left to the callback: a pass whose
      // viewport does not match its attachment is always a bug, and every
      // caller writing the same two lines is how one of them gets it wrong.
      rp->SetViewport(0, 0, float(ctx.width), float(ctx.height));
      pass.raster_fn(ctx);
      rp->End();
    } else {
      auto* cp = encoder.BeginComputePass(pass.name);
      if (!cp) {
        spdlog::error("graph: pass '{}' could not begin", pass.name);
        continue;
      }
      pass.compute_fn(ComputeContext{.pass = cp});
      cp->End();
    }
  }
}

}  // namespace badlands::graph
