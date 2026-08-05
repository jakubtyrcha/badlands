// Small shared bits of the RHI: enum names, reflection lookups, and the
// device factory. Kept in one translation unit so the headers stay pure
// interface.

#include <algorithm>

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

std::vector<std::shared_ptr<IResource>> RetainBindingResources(
    const std::vector<BindingEntry>& entries, std::string_view owner_label) {
  std::vector<std::shared_ptr<IResource>> retained;
  retained.reserve(entries.size());
  for (size_t i = 0; i < entries.size(); ++i) {
    const BindingEntry& e = entries[i];
    IResource* r = nullptr;
    if (e.buffer) {
      r = e.buffer;
    } else if (e.texture_view) {
      r = e.texture_view->GetTexture();  // retain the owner, not the view
      if (!r) {
        spdlog::error(
            "rhi: binding table '{}' entry {} (slot {}): view '{}' has no "
            "owning texture, so it cannot be retained -- it will dangle if the "
            "caller drops its handle",
            owner_label, i, e.slot, e.texture_view->GetLabel());
        continue;
      }
    } else if (e.sampler) {
      r = e.sampler;
    }
    // A null entry is the caller's business (SetBindingTable reports it); an
    // entry that exists but cannot be retained is ours.
    if (!r) continue;

    auto owned = r->Share();
    if (!owned) {
      spdlog::error(
          "rhi: binding table '{}' entry {} (slot {}): resource '{}' is not "
          "shared_ptr-owned, so the table cannot retain it -- it will dangle "
          "if the caller drops its handle",
          owner_label, i, e.slot, r->GetLabel());
      continue;
    }
    retained.push_back(std::move(owned));
  }
  return retained;
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

  if (r.base_mip + r.mip_count > mips) {
    spdlog::error(
        "rhi: CreateView on '{}': mips [{}, {}) exceed the texture's {}",
        texture_label, r.base_mip, r.base_mip + r.mip_count, mips);
    return std::nullopt;
  }
  if (r.base_layer + r.layer_count > layers) {
    spdlog::error(
        "rhi: CreateView on '{}': layers [{}, {}) exceed the texture's {}",
        texture_label, r.base_layer, r.base_layer + r.layer_count, layers);
    return std::nullopt;
  }
  return r;
}

std::unique_ptr<IRhiDevice> CreateDevice(const DeviceDesc& desc) {
  std::unique_ptr<IRhiDevice> device;

  switch (desc.backend) {
    case BackendKind::Null:
      device = null::CreateNullDevice(desc.label);
      break;
    case BackendKind::Metal:
#if defined(BADLANDS_RHI_METAL)
      device = metal::CreateMetalDevice(desc.label);
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
