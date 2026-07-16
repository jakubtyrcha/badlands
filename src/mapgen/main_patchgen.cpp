// badlands_patchgen: render one noiser terrain script over a world patch to PNGs.
//
// The authoring loop for terrain scripts. A script is a function of WORLD METERS
// returning a height in meters (water datum at 0) — see scripts/mapgen/biomes/README.md
// — so a 128x128 m patch here is a literal crop of the world that script would build at
// any map size.
//
// Run from the repo root.
//
// Usage:
//   badlands_patchgen --script S.noiser --out DIR [--size 128] [--origin X,Z]
//                     [--seed N] [--range LO,HI] [--uniform name=value]...
//                     [--name STEM]

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "mapgen/field2d.hpp"
#include "mapgen/hillshade.hpp"
#include "mapgen/mapgen_constants.hpp"
#include "mapgen/outputs.hpp"
#include "mapgen/script_eval.hpp"

namespace {

using badlands::mapgen::Field2D;
using badlands::mapgen::PatchDomain;
using badlands::mapgen::ScriptUniform;

std::optional<std::pair<float, float>> parse_pair(const std::string& s) {
  const auto comma = s.find(',');
  if (comma == std::string::npos) return std::nullopt;
  try {
    return std::make_pair(std::stof(s.substr(0, comma)),
                          std::stof(s.substr(comma + 1)));
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

// "name=value"
std::optional<ScriptUniform> parse_uniform(const std::string& s) {
  const auto eq = s.find('=');
  if (eq == std::string::npos || eq == 0) return std::nullopt;
  try {
    return ScriptUniform{s.substr(0, eq), std::stof(s.substr(eq + 1))};
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

std::string stem_of(const std::string& path) {
  return std::filesystem::path(path).stem().string();
}

}  // namespace

int main(int argc, char** argv) {
  std::string script_path;
  std::string out_dir;
  std::string name;
  int size = 128;
  int scale = 4;  // display upscale only; evaluation stays at 1 m samples
  float origin_x = 0.0f, origin_z = 0.0f;
  std::optional<std::pair<float, float>> range;
  std::vector<ScriptUniform> uniforms;

  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&](const char* flag) -> std::optional<std::string> {
      if (i + 1 < argc) return std::string(argv[++i]);
      std::fprintf(stderr, "patchgen: %s needs an argument\n", flag);
      return std::nullopt;
    };
    if (a == "--script") {
      if (auto v = next("--script")) script_path = *v; else return 2;
    } else if (a == "--out") {
      if (auto v = next("--out")) out_dir = *v; else return 2;
    } else if (a == "--name") {
      if (auto v = next("--name")) name = *v; else return 2;
    } else if (a == "--size") {
      auto v = next("--size");
      if (!v) return 2;
      size = std::atoi(v->c_str());
      if (size <= 0) {
        std::fprintf(stderr, "patchgen: bad --size '%s'\n", v->c_str());
        return 2;
      }
    } else if (a == "--origin") {
      auto v = next("--origin");
      if (!v) return 2;
      auto p = parse_pair(*v);
      if (!p) {
        std::fprintf(stderr, "patchgen: bad --origin '%s' (want X,Z)\n",
                     v->c_str());
        return 2;
      }
      origin_x = p->first;
      origin_z = p->second;
    } else if (a == "--range") {
      auto v = next("--range");
      if (!v) return 2;
      range = parse_pair(*v);
      if (!range) {
        std::fprintf(stderr, "patchgen: bad --range '%s' (want LO,HI)\n",
                     v->c_str());
        return 2;
      }
    } else if (a == "--scale") {
      auto v = next("--scale");
      if (!v) return 2;
      scale = std::atoi(v->c_str());
      if (scale <= 0) {
        std::fprintf(stderr, "patchgen: bad --scale '%s'\n", v->c_str());
        return 2;
      }
    } else if (a == "--seed") {
      auto v = next("--seed");
      if (!v) return 2;
      uniforms.push_back({"seed", static_cast<float>(std::atof(v->c_str()))});
    } else if (a == "--uniform") {
      auto v = next("--uniform");
      if (!v) return 2;
      auto u = parse_uniform(*v);
      if (!u) {
        std::fprintf(stderr, "patchgen: bad --uniform '%s' (want name=value)\n",
                     v->c_str());
        return 2;
      }
      uniforms.push_back(*u);
    } else {
      std::fprintf(stderr, "patchgen: unknown arg '%s'\n", a.c_str());
      return 2;
    }
  }

  if (script_path.empty() || out_dir.empty()) {
    std::fprintf(stderr,
                 "usage: badlands_patchgen --script S.noiser --out DIR "
                 "[--size N] [--scale N] [--origin X,Z] [--seed N] [--range LO,HI] "
                 "[--uniform name=value]... [--name STEM]\n");
    return 2;
  }
  if (name.empty()) name = stem_of(script_path);

  std::error_code ec;
  std::filesystem::create_directories(out_dir, ec);
  if (ec) {
    std::fprintf(stderr, "patchgen: cannot create out dir '%s': %s\n",
                 out_dir.c_str(), ec.message().c_str());
    return 1;
  }

  PatchDomain domain;
  domain.size = size;
  domain.origin_x = origin_x;
  domain.origin_z = origin_z;
  domain.meters_per_sample =
      static_cast<float>(badlands::mapgen::kMetersPerSample);

  Field2D<float> height;
  std::string err;
  if (!badlands::mapgen::evaluate_patch_script_file(script_path, domain,
                                                    uniforms, height, err)) {
    std::fprintf(stderr, "patchgen: %s\n", err.c_str());
    return 1;
  }

  // Report the true range: the grayscale is clamped to --range, so without this the
  // reader cannot tell a flat patch from one clipping hard against the range.
  float lo = height.data.empty() ? 0.0f : height.data[0];
  float hi = lo;
  for (float v : height.data) {
    lo = std::min(lo, v);
    hi = std::max(hi, v);
  }
  const float span_m = static_cast<float>(size) * domain.meters_per_sample;
  std::printf("%-24s %.0fx%.0f m @ (%.0f,%.0f)  height %.2f .. %.2f m\n",
              name.c_str(), span_m, span_m, origin_x, origin_z, lo, hi);

  // Shade at the REAL sample density (so slopes are true), then upscale the images for
  // legibility -- a 128 m patch at 1 m samples is only 128 px.
  const Field2D<float> shade =
      badlands::mapgen::compute_hillshade(height, domain.meters_per_sample);
  const Field2D<float> height_img =
      badlands::mapgen::upscale_nearest(height, scale);
  const Field2D<float> shade_img =
      badlands::mapgen::upscale_nearest(shade, scale);

  const std::string height_png = out_dir + "/" + name + "_height.png";
  const std::string shade_png = out_dir + "/" + name + "_shade.png";
  if (range) {
    badlands::mapgen::write_gray_png_range(height_img, height_png, range->first,
                                           range->second);
  } else {
    badlands::mapgen::write_gray_png(height_img, height_png, /*normalize=*/true);
  }
  badlands::mapgen::write_gray_png_range(shade_img, shade_png, 0.0f, 1.0f);
  return 0;
}
