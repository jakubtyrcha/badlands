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
