// Small shared bits of the RHI: enum names, reflection lookups, and the
// device factory. Kept in one translation unit so the headers stay pure
// interface.

#include <algorithm>
#include <atomic>

#include <spdlog/spdlog.h>

#include "engine/rhi/null/null_rhi.hpp"
#include "engine/rhi/rhi_device.hpp"
#include "engine/rhi/rhi_pipeline.hpp"

// Backends land in later steps; each defines its symbol when it does.
#if defined(BADLANDS_RHI_METAL)
#include "engine/rhi/metal/metal_rhi.hpp"
#endif
#if defined(BADLANDS_RHI_VALIDATION)
#include "engine/rhi/validation/validation_rhi.hpp"
#endif

namespace badlands::rhi {

const char* ToString(ResourceState s) {
  switch (s) {
    case ResourceState::Undefined: return "Undefined";
    case ResourceState::ShaderRead: return "ShaderRead";
    case ResourceState::ShaderWrite: return "ShaderWrite";
    case ResourceState::RenderTarget: return "RenderTarget";
    case ResourceState::DepthWrite: return "DepthWrite";
    case ResourceState::DepthRead: return "DepthRead";
    case ResourceState::IndirectArg: return "IndirectArg";
    case ResourceState::CopySrc: return "CopySrc";
    case ResourceState::CopyDst: return "CopyDst";
  }
  return "?";
}

const char* ToString(AcquireStatus s) {
  switch (s) {
    case AcquireStatus::Ok: return "Ok";
    case AcquireStatus::Skip: return "Skip";
    case AcquireStatus::Lost: return "Lost";
  }
  return "?";
}

// No default case, deliberately: a new Format is then a compile error here
// rather than a "?" in the one log line that was supposed to explain a refusal.
const char* ToString(Format f) {
  switch (f) {
    case Format::Undefined: return "Undefined";
    case Format::R8Unorm: return "R8Unorm";
    case Format::RGBA8Unorm: return "RGBA8Unorm";
    case Format::RGBA8UnormSrgb: return "RGBA8UnormSrgb";
    case Format::BGRA8Unorm: return "BGRA8Unorm";
    case Format::BGRA8UnormSrgb: return "BGRA8UnormSrgb";
    case Format::RG16Float: return "RG16Float";
    case Format::RGBA16Float: return "RGBA16Float";
    case Format::R32Float: return "R32Float";
    case Format::R32Uint: return "R32Uint";
    case Format::RG32Uint: return "RG32Uint";
    case Format::RGBA32Float: return "RGBA32Float";
    case Format::Depth32Float: return "Depth32Float";
  }
  return "?";
}

const char* ToString(ColorSpace s) {
  switch (s) {
    case ColorSpace::Srgb: return "Srgb";
    case ColorSpace::DisplayP3: return "DisplayP3";
    case ColorSpace::ExtendedLinearDisplayP3: return "ExtendedLinearDisplayP3";
  }
  return "?";
}

bool ValidateSwapchainDesc(const SwapchainDesc& d) {
  // Headless is exempt: with no layer nothing is presented, so there is no
  // transfer for the compositor to apply and an extended-range texture is just
  // a texture. That is also what lets a test render the EDR path with no display.
  if (!d.native_window) return true;
  if (IsExtendedRangeFormat(d.format) && d.color_space == ColorSpace::Srgb) {
    spdlog::error(
        "rhi: swapchain '{}' asks for extended-range format {} with colour "
        "space {} -- linear values in an untagged surface have no defined "
        "transfer. Use ExtendedLinearDisplayP3, or an 8-bit format.",
        d.label, ToString(d.format), ToString(d.color_space));
    return false;
  }
  // THE MIRROR IMAGE, and just as wrong. An 8-bit surface holds sRGB-ENCODED
  // bytes; tagging it extended-LINEAR tells the compositor to read those bytes
  // as linear intensities and turns on EDR for a format that cannot carry it.
  // The result is a visibly wrong image with no diagnostic anywhere, which is
  // exactly what the other direction was refused for.
  if (!IsExtendedRangeFormat(d.format) &&
      d.color_space == ColorSpace::ExtendedLinearDisplayP3) {
    spdlog::error(
        "rhi: swapchain '{}' asks for colour space {} with format {} -- an "
        "8-bit surface holds encoded bytes, so tagging it extended-linear "
        "would have the compositor read them as linear. Use DisplayP3, or a "
        "float format.",
        d.label, ToString(d.color_space), ToString(d.format));
    return false;
  }
  return true;
}

const char* ToString(DeviceFeature f) {
  switch (f) {
    case DeviceFeature::Atomic64MinMax: return "Atomic64MinMax";
  }
  return "?";
}

const char* ToString(BackendKind k) {
  switch (k) {
    case BackendKind::Null: return "Null";
    case BackendKind::Metal: return "Metal";
  }
  return "?";
}

const char* ToString(TextureDimension d) {
  switch (d) {
    case TextureDimension::Tex2D: return "Tex2D";
    case TextureDimension::Tex2DArray: return "Tex2DArray";
    case TextureDimension::Cube: return "Cube";
  }
  return "?";
}

const char* ToString(TextureViewDimension d) {
  switch (d) {
    case TextureViewDimension::Auto: return "Auto";
    case TextureViewDimension::Tex2D: return "Tex2D";
    case TextureViewDimension::Tex2DArray: return "Tex2DArray";
    case TextureViewDimension::Cube: return "Cube";
  }
  return "?";
}

const char* ToString(BlendFactor f) {
  switch (f) {
    case BlendFactor::Zero: return "Zero";
    case BlendFactor::One: return "One";
    case BlendFactor::Src: return "Src";
    case BlendFactor::OneMinusSrc: return "OneMinusSrc";
    case BlendFactor::SrcAlpha: return "SrcAlpha";
    case BlendFactor::OneMinusSrcAlpha: return "OneMinusSrcAlpha";
    case BlendFactor::SrcAlphaSaturated: return "SrcAlphaSaturated";
    case BlendFactor::Dst: return "Dst";
    case BlendFactor::OneMinusDst: return "OneMinusDst";
    case BlendFactor::DstAlpha: return "DstAlpha";
    case BlendFactor::OneMinusDstAlpha: return "OneMinusDstAlpha";
  }
  return "?";
}

const char* ToString(BlendOp op) {
  switch (op) {
    case BlendOp::Add: return "Add";
    case BlendOp::Subtract: return "Subtract";
    case BlendOp::ReverseSubtract: return "ReverseSubtract";
    case BlendOp::Min: return "Min";
    case BlendOp::Max: return "Max";
  }
  return "?";
}

bool IndirectArgsInBounds(const IBuffer* args, uint64_t offset,
                          uint64_t struct_size, const char* what) {
  if (!args) {
    spdlog::error("rhi: {} has no argument buffer", what);
    return false;
  }
  const uint64_t size = args->GetSize();
  // By SUBTRACTION, having first established that the struct fits at all:
  // `offset + struct_size` wraps, and a huge offset then sums to something
  // small and passes the very check it exists to fail (rule 8).
  if (size < struct_size || offset > size - struct_size) {
    spdlog::error(
        "rhi: {} reads {}-byte args at offset {} of buffer '{}', which is only "
        "{} bytes -- the GPU would read past the end",
        what, struct_size, offset, args->GetLabel(), size);
    return false;
  }
  return true;
}

bool ValidateBlendStates(const RenderPipelineDesc& d) {
  // Empty is the opaque default and always legal. Anything else must line up
  // one-to-one with the attachments, because the alternative -- padding with
  // defaults or dropping the excess -- blends some attachments and not others
  // and reports nothing (rule 13: an object that cannot be encoded must not be
  // constructed, in release builds as much as in debug ones).
  if (d.blend_states.empty()) return true;
  if (d.blend_states.size() != d.color_formats.size()) {
    spdlog::error(
        "rhi: render pipeline '{}' declares {} blend state(s) for {} colour "
        "attachment(s) -- supply one per attachment, or none at all for opaque",
        d.label, d.blend_states.size(), d.color_formats.size());
    return false;
  }
  return true;
}

const ReflectedBinding* ShaderReflection::FindBinding(
    std::string_view name) const {
  auto it = std::find_if(bindings.begin(), bindings.end(),
                         [name](const auto& b) { return b.name == name; });
  return it == bindings.end() ? nullptr : &*it;
}

const ReflectedUniformBlock* ShaderReflection::FindUniformBlock(
    std::string_view name) const {
  auto it = std::find_if(uniform_blocks.begin(), uniform_blocks.end(),
                         [name](const auto& b) { return b.name == name; });
  return it == uniform_blocks.end() ? nullptr : &*it;
}

// Never reused, so a freed resource's id cannot be inherited by whatever lands
// at its address next. Atomic because resources are created from whatever
// thread the caller is on.
IResource::IResource() {
  static std::atomic<uint64_t> next{1};
  id_ = next.fetch_add(1, std::memory_order_relaxed);
}

namespace {

// The resource an entry's ownership hangs off. A texture VIEW is owned by its
// texture rather than by a shared_ptr, so the TEXTURE is what gets retained --
// keeping the owner alive keeps the view alive, which is the whole point.
IResource* OwnerOf(const BindingEntry& e) {
  if (e.buffer) return e.buffer;
  if (e.texture_view) return e.texture_view->GetTexture();
  return e.sampler;
}

const ShaderReflection* ReflectionOf(const BindingTableDesc& d) {
  if (d.render_pipeline) return &d.render_pipeline->GetReflection();
  if (d.compute_pipeline) return &d.compute_pipeline->GetReflection();
  return nullptr;
}

}  // namespace

std::vector<uint32_t> DynamicEntryOrder(
    const std::vector<BindingEntry>& entries) {
  std::vector<uint32_t> out;
  for (uint32_t i = 0; i < entries.size(); ++i) {
    if (entries[i].dynamic_offset) out.push_back(i);
  }
  std::stable_sort(out.begin(), out.end(), [&entries](uint32_t a, uint32_t b) {
    return entries[a].slot < entries[b].slot;
  });
  return out;
}

std::optional<ResolvedBindingTable> ResolveBindingTable(
    const BindingTableDesc& d, uint64_t min_buffer_offset_alignment) {
  const ShaderReflection* refl = ReflectionOf(d);
  if (!refl) {
    spdlog::error(
        "rhi: binding table '{}' has no pipeline, so its slots cannot be "
        "resolved against anything", d.label);
    return std::nullopt;
  }

  ResolvedBindingTable out;
  out.entries = d.entries;
  out.indices.reserve(d.entries.size());
  out.retained.reserve(d.entries.size());

  for (size_t i = 0; i < d.entries.size(); ++i) {
    const BindingEntry& e = d.entries[i];

    // --- Resolve. Guessing an index is not an option: Slang numbers bindings
    // per category, so a constant buffer, a texture and a sampler can all
    // report index 0, and the slot number lands on whichever the shader
    // happens to declare there.
    const ReflectedBinding* b = nullptr;
    for (const auto& cand : refl->bindings) {
      if (cand.group == d.group && cand.slot == e.slot) { b = &cand; break; }
    }
    if (!b) {
      spdlog::error(
          "rhi: binding table '{}' entry {}: group {} slot {} is absent from "
          "the pipeline's reflection, so it has no target index",
          d.label, i, d.group, e.slot);
      return std::nullopt;
    }
    if (b->kind != e.kind) {
      spdlog::error(
          "rhi: binding table '{}' entry {}: slot {} ('{}') is bound as kind "
          "{} but the shader declares kind {}",
          d.label, i, e.slot, b->name, int(e.kind), int(b->kind));
      return std::nullopt;
    }

    // --- Retain. An entry that cannot be retained would dangle the moment the
    // caller drops its handle, which rhi_types.hpp explicitly invites it to do.
    IResource* owner = OwnerOf(e);
    if (!owner) {
      // Distinguish "you bound nothing" from "you bound a view whose texture
      // is gone" -- the second reads as a caller bug against the first's
      // message and wastes the reader's time.
      if (e.texture_view) {
        spdlog::error(
            "rhi: binding table '{}' entry {}: slot {} view '{}' has no owning "
            "texture, so nothing can keep it alive",
            d.label, i, e.slot, e.texture_view->GetLabel());
      } else {
        spdlog::error(
            "rhi: binding table '{}' entry {}: slot {} has no resource",
            d.label, i, e.slot);
      }
      return std::nullopt;
    }
    if (owner->IsDestroyed()) {
      spdlog::error(
          "rhi: binding table '{}' entry {}: slot {} references destroyed "
          "resource '{}'", d.label, i, e.slot, owner->GetLabel());
      return std::nullopt;
    }
    auto owned = owner->Share();
    if (!owned) {
      spdlog::error(
          "rhi: binding table '{}' entry {}: slot {} resource '{}' is not "
          "shared_ptr-owned, so the table cannot retain it",
          d.label, i, e.slot, owner->GetLabel());
      return std::nullopt;
    }

    // The BASE offset, checked here because it is fixed for the table's life
    // (rule 13). Nothing checked it before: the validation layer only ever
    // looked at the dynamic part, so `buffer_offset = 16` plus a correctly
    // aligned dynamic offset produced a final address that satisfied neither.
    if (e.buffer) {
      const uint64_t size = e.buffer->GetSize();
      if (e.buffer_offset % min_buffer_offset_alignment != 0) {
        spdlog::error(
            "rhi: binding table '{}' entry {}: slot {} buffer_offset {} is "
            "not a multiple of the backend's {}-byte alignment",
            d.label, i, e.slot, e.buffer_offset, min_buffer_offset_alignment);
        return std::nullopt;
      }
      if (e.buffer_offset > size) {
        spdlog::error(
            "rhi: binding table '{}' entry {}: slot {} buffer_offset {} is "
            "past the end of buffer '{}' ({} bytes)",
            d.label, i, e.slot, e.buffer_offset, e.buffer->GetLabel(), size);
        return std::nullopt;
      }
      // buffer_size is ACCEPTED AND IGNORED by every backend -- Metal's
      // setBuffer takes no length -- so a caller setting it would believe in a
      // bound it does not have. Refuse it rather than silently drop it
      // (rule 4); implementing it means a real bounds-checked view, not a
      // field the encoder never reads.
      if (e.buffer_size != 0) {
        spdlog::error(
            "rhi: binding table '{}' entry {}: slot {} sets buffer_size {}, "
            "which no backend implements -- leave it 0 until it does",
            d.label, i, e.slot, e.buffer_size);
        return std::nullopt;
      }
    }

    // A dynamic offset re-points a BUFFER binding. There is nothing to
    // re-point on a texture or a sampler, and accepting the flag there would
    // silently consume a value from the caller's span and shift every
    // subsequent offset onto the wrong binding.
    if (e.dynamic_offset && !e.buffer) {
      spdlog::error(
          "rhi: binding table '{}' entry {}: slot {} is marked dynamic_offset "
          "but binds no buffer -- only buffer bindings can take one",
          d.label, i, e.slot);
      return std::nullopt;
    }

    out.indices.push_back(b->location.index);
    out.retained.push_back(std::move(owned));
  }

  out.dynamic_entries = DynamicEntryOrder(d.entries);
  if (out.dynamic_entries.size() > kMaxDynamicOffsetsPerTable) {
    spdlog::error(
        "rhi: binding table '{}' declares {} dynamic offsets, above the "
        "cross-platform maximum of {} -- a DX12 root signature has a 64-DWORD "
        "budget and would reject it",
        d.label, out.dynamic_entries.size(), kMaxDynamicOffsetsPerTable);
    return std::nullopt;
  }
  return out;
}

void RecordReadbackCopy(ICommandEncoder& encoder, ITextureView* src,
                        IBuffer* staging) {
  if (!src || !staging) return;
  // STATED, not assumed. Without these the validation layer reports the copy's
  // source as whatever the last pass left it in -- and on DX12 the copy really
  // does read a resource still in RENDER_TARGET.
  encoder.Transition(src->GetTexture(), ResourceState::CopySrc);
  encoder.Transition(staging, ResourceState::CopyDst);
  // The view's RESOLVED range says which subresource; ValidateReadbackSource
  // has already established it is exactly one.
  const TextureViewDesc& d = src->GetDesc();
  encoder.CopyTextureToBuffer(src->GetTexture(), d.base_mip, d.base_layer,
                              staging, 0);
}

bool ValidateReadbackSource(const ITextureView* src, size_t& out_bytes,
                            uint32_t& out_width, uint32_t& out_height) {
  out_bytes = 0;
  out_width = 0;
  out_height = 0;
  if (!src) {
    spdlog::error("rhi: ReadTexture was given no source view");
    return false;
  }
  const ITexture* tex = src->GetTexture();
  if (!tex) {
    spdlog::error("rhi: ReadTexture on '{}': the view has no texture",
                  src->GetLabel());
    return false;
  }
  if (!Has(tex->GetUsage(), TextureUsage::CopySrc)) {
    spdlog::error(
        "rhi: ReadTexture on '{}': the texture lacks TextureUsage::CopySrc, so "
        "the copy could never be encoded",
        tex->GetLabel());
    return false;
  }

  // ONE SUBRESOURCE. A readback produces one tightly packed image, so a view
  // spanning several mips or layers has no single answer -- and silently
  // reading its base would be the accepted-and-ignored trap (rule 4). The
  // range itself needs no bounds check: ResolveViewDesc validated it at
  // CreateView, and a resolved desc never carries a 0 count.
  const TextureViewDesc& d = src->GetDesc();
  if (d.mip_count != 1 || d.layer_count != 1) {
    spdlog::error(
        "rhi: ReadTexture on '{}': a readback names ONE subresource, but this "
        "view covers {} mip(s) and {} layer(s)",
        src->GetLabel(), d.mip_count, d.layer_count);
    return false;
  }

  const uint32_t texel = FormatByteSize(tex->GetFormat());
  if (texel == 0) {
    spdlog::error("rhi: ReadTexture on '{}': format {} has no byte size",
                  src->GetLabel(), ToString(tex->GetFormat()));
    return false;
  }
  out_width = std::max(1u, tex->GetWidth() >> d.base_mip);
  out_height = std::max(1u, tex->GetHeight() >> d.base_mip);
  out_bytes = size_t(out_width) * out_height * texel;
  return true;
}

void ReadbackCompletion::Signal() {
  ReadbackCallback to_run;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (ready_) return;  // idempotent: a second signal must not re-run anything
    ready_ = true;
    to_run = std::move(callback_);
    callback_ = nullptr;
  }
  // NOTIFY BEFORE RUNNING, and run OUTSIDE the lock. A callback that blocks --
  // a PNG encode, say -- would otherwise hold the mutex a waiter needs, so
  // Wait() would not return until the callback it does not care about is done.
  cv_.notify_all();
  if (to_run) to_run();
}

bool ReadbackCompletion::Wait(std::chrono::milliseconds timeout,
                              std::string_view label) {
  std::unique_lock<std::mutex> lock(mutex_);
  if (cv_.wait_for(lock, timeout, [this] { return ready_; })) return true;
  spdlog::error(
      "rhi: readback '{}' did not complete within {} ms -- the GPU may be hung, "
      "which an unbounded wait would have turned into a process that never "
      "returns and never says why",
      label, timeout.count());
  return false;
}

void ReadbackCompletion::Register(ReadbackCallback callback) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ready_) {
      callback_ = std::move(callback);
      return;
    }
  }
  // ALREADY DONE: run it here, on the caller's thread, rather than storing a
  // callback nothing will ever fire. A backend that only notified from its
  // completion handler would silently drop this case.
  if (callback) callback();
}

bool ReadbackCompletion::IsReady() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return ready_;
}

std::optional<TextureViewDesc> ResolveViewDesc(const TextureViewDesc& requested,
                                               const TextureDesc& texture,
                                               std::string_view texture_label) {
  const uint32_t mips = std::max(1u, texture.mip_levels);
  const uint32_t layers = std::max(1u, texture.array_layers);

  if (requested.base_mip >= mips) {
    spdlog::error(
        "rhi: CreateView on '{}': base_mip {} is out of range (texture has {} "
        "mip level(s))",
        texture_label, requested.base_mip, mips);
    return std::nullopt;
  }
  if (requested.base_layer >= layers) {
    spdlog::error(
        "rhi: CreateView on '{}': base_layer {} is out of range (texture has "
        "{} layer(s))",
        texture_label, requested.base_layer, layers);
    return std::nullopt;
  }

  TextureViewDesc r = requested;
  if (r.mip_count == 0) r.mip_count = mips - r.base_mip;
  if (r.layer_count == 0) r.layer_count = layers - r.base_layer;

  // Subtraction, not addition. `base + count` is uint32 arithmetic and wraps,
  // so a count of 0xFFFFFFFF sums to something small and passes the very check
  // it has to fail. The bases are already known in range, so the subtractions
  // cannot underflow.
  if (r.mip_count > mips - r.base_mip) {
    spdlog::error(
        "rhi: CreateView on '{}': {} mips from {} exceed the texture's {}",
        texture_label, r.mip_count, r.base_mip, mips);
    return std::nullopt;
  }
  if (r.layer_count > layers - r.base_layer) {
    spdlog::error(
        "rhi: CreateView on '{}': {} layers from {} exceed the texture's {}",
        texture_label, r.layer_count, r.base_layer, layers);
    return std::nullopt;
  }

  // Auto becomes concrete HERE, for the same reason a mip_count of 0 does: a
  // resolved descriptor must not carry a value that means "ask again".
  if (r.dimension == TextureViewDimension::Auto) {
    switch (texture.dimension) {
      case TextureDimension::Tex2D:
        r.dimension = TextureViewDimension::Tex2D;
        break;
      case TextureDimension::Tex2DArray:
        r.dimension = TextureViewDimension::Tex2DArray;
        break;
      case TextureDimension::Cube:
        // A partial range cannot be a cube, and silently promoting it to one
        // would be the accepted-and-ignored trap. A single face defaults to the
        // flat 2D view that a render target wants; anything else in between
        // stays an array slice.
        r.dimension = (r.base_layer == 0 && r.layer_count == 6)
                          ? TextureViewDimension::Cube
                          : (r.layer_count == 1
                                 ? TextureViewDimension::Tex2D
                                 : TextureViewDimension::Tex2DArray);
        break;
    }
  }

  // A cube view is not "six layers you may sample by direction" -- the backend
  // encodes the texture's own type, so both the resource and the range have to
  // agree with the request. Refusing here rather than letting the backend
  // silently hand back an array view is the difference between a diagnosable
  // mistake and a shader sampling garbage.
  if (r.dimension == TextureViewDimension::Cube) {
    if (texture.dimension != TextureDimension::Cube) {
      spdlog::error(
          "rhi: CreateView on '{}': a Cube view needs a Cube texture, but this "
          "one is {}",
          texture_label, ToString(texture.dimension));
      return std::nullopt;
    }
    if (r.base_layer != 0 || r.layer_count != 6) {
      spdlog::error(
          "rhi: CreateView on '{}': a Cube view covers all six faces, not {} "
          "from {}",
          texture_label, r.layer_count, r.base_layer);
      return std::nullopt;
    }
  }
  // The converse: a Tex2D view addresses ONE layer. Left unchecked, a
  // whole-resource request on a cube would resolve to six layers and still
  // claim to be a flat 2D image, which is the render-target-per-face case
  // silently targeting all six.
  if (r.dimension == TextureViewDimension::Tex2D && r.layer_count != 1) {
    spdlog::error(
        "rhi: CreateView on '{}': a Tex2D view covers one layer, not {}",
        texture_label, r.layer_count);
    return std::nullopt;
  }
  return r;
}

bool ValidateTextureDesc(const TextureDesc& desc) {
  const uint32_t layers = std::max(1u, desc.array_layers);
  switch (desc.dimension) {
    case TextureDimension::Tex2D:
      if (layers != 1) {
        spdlog::error(
            "rhi: CreateTexture '{}': a Tex2D texture has one layer, not {} -- "
            "say Tex2DArray if that is what was meant",
            desc.label, layers);
        return false;
      }
      return true;
    case TextureDimension::Tex2DArray:
      return true;
    case TextureDimension::Cube:
      if (layers != 6) {
        spdlog::error(
            "rhi: CreateTexture '{}': a Cube texture has exactly six faces, "
            "not {}",
            desc.label, layers);
        return false;
      }
      if (desc.width != desc.height) {
        spdlog::error(
            "rhi: CreateTexture '{}': a Cube face is square, but this is {}x{}",
            desc.label, desc.width, desc.height);
        return false;
      }
      return true;
  }
  spdlog::error("rhi: CreateTexture '{}': unknown texture dimension {}",
                desc.label, uint32_t(desc.dimension));
  return false;
}

bool ValidateTextureWrite(const TextureDesc& desc, std::string_view label,
                          uint32_t mip, uint32_t layer, size_t byte_count) {
  const uint32_t mips = std::max(1u, desc.mip_levels);
  const uint32_t layers = std::max(1u, desc.array_layers);
  if (mip >= mips) {
    spdlog::error("rhi: Write to '{}': mip {} is out of range ({} level(s))",
                  label, mip, mips);
    return false;
  }
  if (layer >= layers) {
    spdlog::error("rhi: Write to '{}': layer {} is out of range ({} layer(s))",
                  label, layer, layers);
    return false;
  }
  const uint32_t w = std::max(1u, desc.width >> mip);
  const uint32_t h = std::max(1u, desc.height >> mip);
  const size_t needed = size_t(w) * FormatByteSize(desc.format) * h;
  if (byte_count < needed) {
    spdlog::error("rhi: Write to '{}' is short ({} < {} bytes for mip {})",
                  label, byte_count, needed, mip);
    return false;
  }
  return true;
}

std::unique_ptr<IRhiDevice> CreateDevice(const DeviceDesc& desc) {
  std::unique_ptr<IRhiDevice> device;

  switch (desc.backend) {
    case BackendKind::Null:
      device = null::CreateNullDevice(desc.label, desc.frames_in_flight);
      break;
    case BackendKind::Metal:
#if defined(BADLANDS_RHI_METAL)
      device = metal::CreateMetalDevice(desc.label, desc.frames_in_flight);
#else
      spdlog::error("rhi: Metal backend not compiled in");
      return nullptr;
#endif
      break;
  }

  if (!device) {
    spdlog::error("rhi: failed to create {} device", ToString(desc.backend));
    return nullptr;
  }

  if (desc.enable_validation) {
#if defined(BADLANDS_RHI_VALIDATION)
    device = validation::MakeValidationDevice(std::move(device));
#else
    // Loud rather than silent: a caller that asked for validation and got a
    // bare device would read a clean run as proof of correctness.
    spdlog::warn("rhi: validation requested but not compiled in");
#endif
  }
  return device;
}

}  // namespace badlands::rhi
