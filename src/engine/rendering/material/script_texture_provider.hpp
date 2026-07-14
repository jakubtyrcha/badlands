#pragma once

// Ported from sampo's src/rendering/material/script_texture_provider.hpp,
// namespace sampo -> badlands (verbatim otherwise).
//
// noiser (Task B3) is not available in Stage 1 — see
// null_script_texture_provider.hpp for the Stage 1 stub implementation that
// keeps the material path from hard-depending on it.
#include <expected>
#include <string>
#include <unordered_map>

#include <dawn/webgpu_cpp.h>

#include "engine/rendering/material/rendering_material_instance.hpp"

namespace badlands {

// A resolved GPU texture with its view and sampler.
struct ResolvedTexture {
  wgpu::TextureView view;
  wgpu::Sampler sampler;
};

class ScriptTextureProvider {
 public:
  virtual ~ScriptTextureProvider() = default;
  virtual std::expected<ResolvedTexture, std::string> ExecuteScriptToTexture(
      const std::string& source,
      const std::unordered_map<std::string, MaterialParameterValue>& params,
      int resolution, bool is_cubemap) = 0;
};

}  // namespace badlands
