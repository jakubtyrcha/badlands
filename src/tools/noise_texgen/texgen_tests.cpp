// Catch2 tests for the noise-texgen core. Pure CPU; run from the repo root so
// the assets/noiser/texgen/*.noiser scripts resolve.

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <catch_amalgamated.hpp>

#include "tools/noise_texgen/texgen_core.hpp"

using badlands::texgen::Generate;
using badlands::texgen::Mode;
using badlands::texgen::Options;

namespace {

const std::string kTexgenDir = "assets/noiser/texgen";

Options PerlinGray(uint32_t w, uint32_t h) {
  Options o;
  o.script_path = kTexgenDir + "/perlin_gray.noiser";
  o.width = w;
  o.height = h;
  o.mode = Mode::kGrayscale;
  o.win_min_x = 0.0f;
  o.win_min_y = 0.0f;
  o.win_max_x = 8.0f;
  o.win_max_y = 8.0f;
  return o;
}

// Writes a self-contained (import-free) script to a temp file, returns path.
std::string WriteTempScript(const std::string& tag, const std::string& body) {
  auto path = std::filesystem::temp_directory_path() /
              ("badlands_texgen_" + tag + ".noiser");
  std::ofstream f(path);
  f << body << "\n";
  f.close();
  return path.string();
}

}  // namespace

TEST_CASE("texgen output is deterministic", "[texgen]") {
  auto a = Generate(PerlinGray(128, 128));
  auto b = Generate(PerlinGray(128, 128));
  REQUIRE(a.has_value());
  REQUIRE(b.has_value());
  REQUIRE(*a == *b);
}

TEST_CASE("8x8 patch tiling covers every pixel at non-multiple sizes",
          "[texgen]") {
  // 100x70 -> partial edge patches in both axes; every pixel must be written.
  auto img = Generate(PerlinGray(100, 70));
  REQUIRE(img.has_value());
  REQUIRE(img->size() == static_cast<size_t>(100) * 70 * 4);

  bool all_opaque = true;
  bool all_same = true;
  uint8_t first = (*img)[0];
  for (size_t px = 0; px < static_cast<size_t>(100) * 70; ++px) {
    if ((*img)[px * 4 + 3] != 255) all_opaque = false;
    if ((*img)[px * 4] != first) all_same = false;
  }
  REQUIRE(all_opaque);       // no unwritten (zero-alpha) holes
  REQUIRE_FALSE(all_same);   // real noise, not a blank fill
}

TEST_CASE("perlin grayscale is real noise centered near mid-gray", "[texgen]") {
  auto img = Generate(PerlinGray(256, 256));
  REQUIRE(img.has_value());

  int min_v = 255, max_v = 0;
  double sum = 0.0;
  const size_t count = static_cast<size_t>(256) * 256;
  for (size_t px = 0; px < count; ++px) {
    int v = (*img)[px * 4];
    min_v = std::min(min_v, v);
    max_v = std::max(max_v, v);
    sum += v;
  }
  const double mean = sum / static_cast<double>(count);
  REQUIRE(min_v < 64);              // has dark regions
  REQUIRE(max_v > 192);             // has bright regions
  REQUIRE(mean > 96.0);             // perlin is centered on 0.5 -> ~127
  REQUIRE(mean < 160.0);
}

TEST_CASE("the domain window changes the output", "[texgen]") {
  auto here = Generate(PerlinGray(64, 64));
  Options elsewhere = PerlinGray(64, 64);
  elsewhere.win_min_x = 100.0f;
  elsewhere.win_min_y = 100.0f;
  elsewhere.win_max_x = 108.0f;
  elsewhere.win_max_y = 108.0f;
  auto other = Generate(elsewhere);
  REQUIRE(here.has_value());
  REQUIRE(other.has_value());
  REQUIRE(*here != *other);  // uniforms are actually bound & used
}

TEST_CASE("mode must match the script's return type", "[texgen]") {
  Options as_rgb = PerlinGray(16, 16);
  as_rgb.mode = Mode::kRgb;  // perlin_gray returns f32, not vec3
  auto r = Generate(as_rgb);
  REQUIRE_FALSE(r.has_value());
  REQUIRE(r.error().find("match mode") != std::string::npos);

  Options grad_as_gray;
  grad_as_gray.script_path = kTexgenDir + "/perlin_gradient_rgb.noiser";
  grad_as_gray.width = 16;
  grad_as_gray.height = 16;
  grad_as_gray.mode = Mode::kGrayscale;  // returns vec3, not f32
  auto g = Generate(grad_as_gray);
  REQUIRE_FALSE(g.has_value());
}

TEST_CASE("explicit --window on a script without window uniforms errors",
          "[texgen]") {
  // A self-contained script that never references the window uniforms.
  const std::string path = WriteTempScript("no_window", "0.5");

  Options strict;
  strict.script_path = path;
  strict.width = 4;
  strict.height = 4;
  strict.mode = Mode::kGrayscale;
  strict.window_explicit = true;  // caller asked for a window it can't apply
  auto err = Generate(strict);
  REQUIRE_FALSE(err.has_value());
  REQUIRE(err.error().find("window") != std::string::npos);

  Options lenient = strict;
  lenient.window_explicit = false;  // default window is fine to ignore
  auto ok = Generate(lenient);
  REQUIRE(ok.has_value());
}

TEST_CASE("grayscale is encoded linearly", "[texgen]") {
  Options o;
  o.script_path = WriteTempScript("const_gray", "0.5");
  o.width = 4;
  o.height = 4;
  o.mode = Mode::kGrayscale;
  auto img = Generate(o);
  REQUIRE(img.has_value());
  // 0.5 linear -> round(0.5*255+0.5) = 128, NOT the sRGB value (~188).
  for (size_t px = 0; px < 16; ++px) {
    REQUIRE(static_cast<int>((*img)[px * 4]) == 128);
    REQUIRE(static_cast<int>((*img)[px * 4 + 3]) == 255);
  }
}

TEST_CASE("rgb color channels are sRGB-encoded", "[texgen]") {
  // Reference points from the sRGB standard, independent of the tool's own
  // formula: linear 1.0 -> 255, linear 0.0 -> 0, linear 0.5 -> ~188 (the
  // canonical 8-bit sRGB value; a linear encode would give 128). The 0.5 case
  // sits ~0.02 above a quantization boundary, so allow the last-bit rounding.
  Options o;
  o.script_path = WriteTempScript("const_rgb", "vec3(1.0, 0.0, 0.5)");
  o.width = 4;
  o.height = 4;
  o.mode = Mode::kRgb;
  auto img = Generate(o);
  REQUIRE(img.has_value());
  for (size_t px = 0; px < 16; ++px) {
    REQUIRE(static_cast<int>((*img)[px * 4 + 0]) == 255);        // 1.0 -> 255
    REQUIRE(static_cast<int>((*img)[px * 4 + 1]) == 0);          // 0.0 -> 0
    const int b = (*img)[px * 4 + 2];
    REQUIRE(b >= 187);  // 0.5 linear -> ~188 sRGB, NOT 128 (linear)
    REQUIRE(b <= 188);
    REQUIRE(static_cast<int>((*img)[px * 4 + 3]) == 255);
  }
}

TEST_CASE("rgba keeps alpha linear while color is sRGB", "[texgen]") {
  Options o;
  o.script_path = WriteTempScript("const_rgba", "vec4(0.5, 0.5, 0.5, 0.5)");
  o.width = 4;
  o.height = 4;
  o.mode = Mode::kRgba;
  auto img = Generate(o);
  REQUIRE(img.has_value());
  for (size_t px = 0; px < 16; ++px) {
    const int c = (*img)[px * 4];
    REQUIRE(c >= 187);  // 0.5 linear -> ~188 sRGB color
    REQUIRE(c <= 188);
    REQUIRE(static_cast<int>((*img)[px * 4 + 3]) == 128);  // 0.5 linear alpha
  }
}
