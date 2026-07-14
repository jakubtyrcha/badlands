#pragma once

// Ported from sampo's src/core/util/find_shader_directory.hpp, namespace
// sampo -> badlands, candidate paths adjusted: sampo's shaders live under
// `src/shaders` (relative to sampo's repo root / build cwd); badlands'
// shaders live at the repo root (`shaders/`, not `src/shaders/`), so the
// candidate list is updated accordingly. Tries, in order: the binary's cwd
// (repo root), one level up (cwd == build/), two levels up (cwd == a
// build/<config>/ subdirectory).

#include <filesystem>
#include <string>

namespace badlands {

inline std::string FindShaderDirectory() {
  static const std::string cached = [] {
    for (const char* p : {"shaders", "../shaders", "../../shaders"}) {
      if (std::filesystem::is_directory(p)) return std::string(p);
    }
    return std::string("shaders");
  }();
  return cached;
}

}  // namespace badlands
