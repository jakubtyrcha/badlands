#pragma once

// Stage 1 stub for sampo's ScriptTextureProvider. noiser (Task B3) isn't
// available yet, so this provider always fails script execution:
// `NoiserMaterialScript` recipes are safely unsupported — a factory built
// with this provider errors per-script (StandardMaterialFactory logs a
// warning via GetResolvedScripts and simply omits the resolved texture, so
// callers fall through to the recipe's/factory's default texture) instead of
// hard-depending on noiser. `DefaultTextureView` recipes and the 1x1
// fallback defaults (white/flat_normal/full_roughness/gray) are unaffected —
// they don't go through a ScriptTextureProvider at all.
//
// Not present in sampo (which always has a real noiser-backed provider);
// added for this port per the D1 task brief.
#include "engine/rendering/material/script_texture_provider.hpp"

namespace badlands {

class NullScriptTextureProvider : public ScriptTextureProvider {
 public:
  std::expected<ResolvedTexture, std::string> ExecuteScriptToTexture(
      const std::string& /*source*/,
      const std::unordered_map<std::string, MaterialParameterValue>& /*params*/,
      int /*resolution*/, bool /*is_cubemap*/) override {
    return std::unexpected(
        "NullScriptTextureProvider: noiser is not available in Stage 1; "
        "NoiserMaterialScript recipes are unsupported.");
  }
};

}  // namespace badlands
