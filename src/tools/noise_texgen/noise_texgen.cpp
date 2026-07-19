// badlands_noise_texgen — render a noiser script over a windowed 2D domain to
// a PNG. Headless CPU tool; see src/tools/noise_texgen/texgen_core.hpp.
//
//   badlands_noise_texgen <script.noiser> \
//       --window <minx,miny,maxx,maxy>   (default 0,0,1,1)
//       --res    <WxH>                    e.g. 512x512
//       --mode   <grayscale|rgb|rgba>     (default grayscale)
//       --out    <path.png>
//       [--include <dir>] ...             extra module search paths

#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

#include "tools/noise_texgen/texgen_core.hpp"

namespace {

using badlands::texgen::Mode;
using badlands::texgen::Options;

void PrintUsage(const char* prog) {
  std::fprintf(
      stderr,
      "Usage: %s <script.noiser> --res <WxH> --out <path.png>\n"
      "           [--window <minx,miny,maxx,maxy>] [--mode <grayscale|rgb|rgba>]\n"
      "           [--include <dir>]...\n\n"
      "  --window   domain rectangle mapped across the texture (default 0,0,1,1)\n"
      "  --res      texture resolution, e.g. 512x512\n"
      "  --mode     grayscale (f32, linear) | rgb (vec3) | rgba (vec4); sRGB color\n"
      "  --out      output PNG path\n"
      "  --include  extra module search path for `import` (repeatable)\n",
      prog);
}

// Parse "a,b,c,d" into four floats. Returns false on malformed input.
bool ParseWindow(const std::string& s, Options& opts) {
  float v[4];
  int n = std::sscanf(s.c_str(), "%f,%f,%f,%f", &v[0], &v[1], &v[2], &v[3]);
  if (n != 4) return false;
  opts.win_min_x = v[0];
  opts.win_min_y = v[1];
  opts.win_max_x = v[2];
  opts.win_max_y = v[3];
  return true;
}

// Parse "WxH" into width/height. Returns false on malformed input.
bool ParseRes(const std::string& s, Options& opts) {
  unsigned w = 0, h = 0;
  if (std::sscanf(s.c_str(), "%ux%u", &w, &h) != 2 || w == 0 || h == 0) {
    return false;
  }
  opts.width = w;
  opts.height = h;
  return true;
}

std::optional<Mode> ParseMode(const std::string& s) {
  if (s == "grayscale") return Mode::kGrayscale;
  if (s == "rgb") return Mode::kRgb;
  if (s == "rgba") return Mode::kRgba;
  return std::nullopt;
}

}  // namespace

int main(int argc, char* argv[]) {
  Options opts;
  std::string out_path;
  bool have_res = false;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    auto next = [&](const char* name) -> std::string {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "error: %s requires a value\n", name);
        std::exit(1);
      }
      return argv[++i];
    };
    if (arg == "--window") {
      if (!ParseWindow(next("--window"), opts)) {
        std::fprintf(stderr, "error: --window expects minx,miny,maxx,maxy\n");
        return 1;
      }
      opts.window_explicit = true;
    } else if (arg == "--res") {
      if (!ParseRes(next("--res"), opts)) {
        std::fprintf(stderr, "error: --res expects WxH (e.g. 512x512)\n");
        return 1;
      }
      have_res = true;
    } else if (arg == "--mode") {
      auto m = ParseMode(next("--mode"));
      if (!m) {
        std::fprintf(stderr, "error: --mode expects grayscale|rgb|rgba\n");
        return 1;
      }
      opts.mode = *m;
    } else if (arg == "--out") {
      out_path = next("--out");
    } else if (arg == "--include" || arg == "-I") {
      opts.include_paths.push_back(next("--include"));
    } else if (arg == "-h" || arg == "--help") {
      PrintUsage(argv[0]);
      return 0;
    } else if (!arg.empty() && arg[0] == '-') {
      std::fprintf(stderr, "error: unknown option '%s'\n", arg.c_str());
      PrintUsage(argv[0]);
      return 1;
    } else if (opts.script_path.empty()) {
      opts.script_path = arg;
    } else {
      std::fprintf(stderr, "error: unexpected argument '%s'\n", arg.c_str());
      return 1;
    }
  }

  if (opts.script_path.empty() || !have_res || out_path.empty()) {
    std::fprintf(stderr, "error: <script>, --res, and --out are required\n\n");
    PrintUsage(argv[0]);
    return 1;
  }

  auto result = badlands::texgen::GenerateToPng(opts, out_path);
  if (!result) {
    std::fprintf(stderr, "error: %s\n", result.error().c_str());
    return 1;
  }
  std::printf("wrote %ux%u %s -> %s\n", opts.width, opts.height,
              opts.script_path.c_str(), out_path.c_str());
  return 0;
}
