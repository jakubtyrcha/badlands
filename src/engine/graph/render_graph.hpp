#pragma once

// The render graph: declare passes and what they touch, then let it order them
// and derive the resource transitions.
//
// THREE THINGS IT DELIBERATELY DOES NOT DO, because each one would make it
// impossible to test or to reuse:
//
//   1. It does not own the frame loop. Execute() records into a caller-owned
//      encoder; it never begins a frame, never submits, never presents.
//   2. It never sees a swapchain. Its output is an IMPORTED texture, and the
//      caller decides where that came from -- an acquired backbuffer when
//      windowed, a plain texture when headless. That is the whole of the sink
//      abstraction, and it is why the same graph with the same passes runs
//      both ways rather than having a special headless mode.
//   3. It never compiles a shader. Passes are given pipelines; the Slang layer
//      lives above the RHI and beside this, never underneath it.
//
// WHY THIS IS TESTABLE WITHOUT A GPU. Transitions are a no-op on Metal, which
// tracks hazards itself -- but the validation decorator CHECKS the declared
// intent as bookkeeping over the command stream. So the graph's hardest
// property, "did it declare the right transitions in the right order?", fails
// a validation scope on the Null backend, in the fast suite, on any machine.
// It does not wait for a DX12 box to surface as corrupted output.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "engine/rhi/rhi_commands.hpp"
#include "engine/rhi/rhi_device.hpp"

namespace badlands::graph {

// A resource inside the graph. An index, not a pointer: the graph may not have
// created the resource yet when a pass declares it.
//
// Deliberately not implicitly constructible from an integer -- a bare index
// would let a caller pass a pass number, a slot, or an uninitialised int, and
// every one of those resolves to *some* resource.
struct ResourceHandle {
  static constexpr uint32_t kInvalid = 0xFFFFFFFFu;
  uint32_t id = kInvalid;

  bool IsValid() const { return id != kInvalid; }
  bool operator==(const ResourceHandle&) const = default;
};

// How a pass touches a resource. Maps onto rhi::ResourceState, but only the
// subset a pass can legitimately declare -- there is no "CopySrc" here because
// the graph does not record copies.
enum class Access : uint8_t {
  Read,           // sampled or read-only storage
  Write,          // read-write storage
  ColorTarget,    // written as a colour attachment
  DepthTarget,    // written as a depth attachment
  DepthReadOnly,  // tested as a depth attachment, never written
};

const char* ToString(Access a);

// What a pass records, given the encoder's pass object. The graph has already
// begun the pass and set nothing else -- the pipeline, the tables and the draws
// are the callback's business.
struct RasterContext {
  rhi::IRenderPass* pass = nullptr;
  uint32_t width = 0;
  uint32_t height = 0;
};

struct ComputeContext {
  rhi::IComputePass* pass = nullptr;
};

class RenderGraph;

// Declares one raster pass. Returned by AddRasterPass and used fluently; the
// pass is only complete once Execute is given a callback.
class RasterPassBuilder {
 public:
  // A colour attachment. `clear` is used only with LoadOp::Clear.
  RasterPassBuilder& ColorTarget(ResourceHandle target,
                                 rhi::LoadOp load = rhi::LoadOp::Clear,
                                 rhi::StoreOp store = rhi::StoreOp::Store,
                                 const float clear[4] = nullptr);
  // The depth attachment, written: depth-tested geometry that also updates the
  // buffer. `clear` applies only with LoadOp::Clear and defaults to the
  // REVERSED-Z far value -- 0, not 1 -- matching rhi::DepthAttachment and the
  // project-wide invariant.
  RasterPassBuilder& DepthTarget(ResourceHandle target,
                                 rhi::LoadOp load = rhi::LoadOp::Clear,
                                 rhi::StoreOp store = rhi::StoreOp::Store,
                                 float clear = 0.0f);
  // The depth attachment, tested but never written -- an overlay that must sit
  // behind the geometry already drawn without occluding the next overlay.
  //
  // A SEPARATE METHOD rather than a flag on DepthTarget: read-only depth never
  // clears and never stores, so a shared signature would carry two parameters
  // it must ignore, and the flag would silently pick which barrier is emitted.
  // That is the ambiguity rhi's rule 5 exists to prevent.
  RasterPassBuilder& DepthReadOnly(ResourceHandle target);
  // Declares that this pass reads or writes `handle`. This is what the
  // transition is derived from, so declaring a binding and declaring a barrier
  // are the SAME act and cannot drift apart.
  RasterPassBuilder& Reads(ResourceHandle handle);
  RasterPassBuilder& Writes(ResourceHandle handle);
  void Execute(std::function<void(const RasterContext&)> fn);

 private:
  friend class RenderGraph;
  RasterPassBuilder(RenderGraph* g, uint32_t pass) : graph_(g), pass_(pass) {}
  RenderGraph* graph_;
  uint32_t pass_;
};

class ComputePassBuilder {
 public:
  ComputePassBuilder& Reads(ResourceHandle handle);
  ComputePassBuilder& Writes(ResourceHandle handle);
  void Execute(std::function<void(const ComputeContext&)> fn);

 private:
  friend class RenderGraph;
  ComputePassBuilder(RenderGraph* g, uint32_t pass) : graph_(g), pass_(pass) {}
  RenderGraph* graph_;
  uint32_t pass_;
};

class RenderGraph {
 public:
  explicit RenderGraph(rhi::IRhiDevice& device) : device_(&device) {}

  // --- Resources ---

  // A resource the graph did not create. `state` is what it is in ON ENTRY, so
  // the first transition is derived rather than assumed -- a freshly acquired
  // backbuffer is Undefined every frame, and a texture the previous graph left
  // as a render target is not.
  ResourceHandle ImportTexture(rhi::ITexture* texture, rhi::ResourceState state,
                               std::string_view name = {});
  ResourceHandle ImportBuffer(rhi::IBuffer* buffer, rhi::ResourceState state,
                              std::string_view name = {});

  // Graph-owned, created at Compile() and released with the graph.
  //
  // NOT POOLED AND NOT ALIASED, deliberately and for now: transient-resource
  // aliasing is a memory optimisation that needs lifetime analysis to be
  // correct, and shipping the interface without the analysis would be a field
  // that looks like it saves memory and does not.
  ResourceHandle CreateTexture(const rhi::TextureDesc& desc);

  // --- Passes ---
  RasterPassBuilder AddRasterPass(std::string_view name);
  ComputePassBuilder AddComputePass(std::string_view name);

  // Resolves resources, orders passes, and derives the transitions.
  //
  // Returns false, after logging what and why, if a pass reads a resource
  // nothing writes, if a declared resource was never imported or created, or if
  // a transient texture could not be created. Refusing here rather than at
  // record time is the same principle as rule 13: what cannot be executed must
  // not reach the encoder.
  bool Compile();

  // Records every pass into `encoder`, in order, emitting the transitions
  // Compile() derived. Does nothing (after logging) if Compile() has not run or
  // did not succeed.
  void Execute(rhi::ICommandEncoder& encoder);

  // Diagnostics, and what the tests assert on.
  size_t PassCount() const { return passes_.size(); }
  size_t ResourceCount() const { return resources_.size(); }
  // The order Compile() settled on, as pass indices.
  const std::vector<uint32_t>& Order() const { return order_; }

 private:
  friend class RasterPassBuilder;
  friend class ComputePassBuilder;

  struct Resource {
    std::string name;
    rhi::ITexture* texture = nullptr;  // imported or transient
    rhi::IBuffer* buffer = nullptr;
    rhi::TexturePtr owned;             // non-null for transient textures
    rhi::TextureDesc desc;             // transient only
    // What the resource is in on ENTRY to the graph, and the value `state` is
    // reset to at the top of every Execute. Without the reset, a graph compiled
    // once and executed per frame emits its transitions on the first execution
    // and NONE afterwards -- every later frame then uses an imported backbuffer
    // that was never declared a render target.
    rhi::ResourceState entry_state = rhi::ResourceState::Undefined;
    rhi::ResourceState state = rhi::ResourceState::Undefined;
    bool transient = false;
    // True for anything a caller supplied from outside. A transient becomes
    // readable only once a pass EARLIER IN THE ORDER has written it, which is
    // tracked during Compile rather than stored here -- see Compile().
    bool imported = false;
  };

  struct Attachment {
    ResourceHandle target;
    rhi::LoadOp load = rhi::LoadOp::Clear;
    rhi::StoreOp store = rhi::StoreOp::Store;
    float clear[4] = {0, 0, 0, 1};
  };

  // Separate from Attachment because a depth clear is one float, not four, and
  // read_only has no colour equivalent. An invalid `target` means the pass has
  // no depth at all, which is what every existing pass declares by omission.
  struct DepthSlot {
    ResourceHandle target;
    rhi::LoadOp load = rhi::LoadOp::Clear;
    rhi::StoreOp store = rhi::StoreOp::Store;
    float clear = 0.0f;   // reversed-Z far
    bool read_only = false;
  };

  struct Pass {
    std::string name;
    bool raster = false;
    std::vector<Attachment> colors;
    DepthSlot depth;
    std::vector<std::pair<ResourceHandle, Access>> accesses;
    std::function<void(const RasterContext&)> raster_fn;
    std::function<void(const ComputeContext&)> compute_fn;
  };

  void Declare(uint32_t pass, ResourceHandle handle, Access access);

  rhi::IRhiDevice* device_ = nullptr;
  std::vector<Resource> resources_;
  std::vector<Pass> passes_;
  std::vector<uint32_t> order_;
  bool compiled_ = false;
};

}  // namespace badlands::graph
