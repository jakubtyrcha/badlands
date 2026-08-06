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

const char* ToString(BackendKind k) {
  switch (k) {
    case BackendKind::Null: return "Null";
    case BackendKind::Metal: return "Metal";
  }
  return "?";
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
  return r;
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
