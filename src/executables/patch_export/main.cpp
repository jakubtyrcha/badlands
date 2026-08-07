// badlands_patch_export: the stage-2 iteration loop's offline half.
//
// Cuts one or more windows out of a cached stage-1 world (tools/protogen's
// world.txt dump), runs them through the SAME stage-2 path badlands_mapview
// fetches with (mapgen::CoarseWorldPatchSource::Fetch -- resample + relief
// filter + water reconstruction), and writes each as images. The point is that
// looking at stage 2 offline stops being a throwaway script: the same window,
// the same code, either as PNGs here or in 3D over there.
//
// Run from the repo root.
//
// Usage: badlands_patch_export --load DIR [--tag NAME]
//                              [--patch-size M] [--patch-res N]
//                              (--patch-origin X,Y | --window NAME=X,Y ...
//                               | --windows FILE)
//                              --out DIR
//                              [--layers height,biome,hillshade]
//                              [--height-range LO,HI] [--dump-patch]
//
//   --load DIR         a coarse world directory (world.txt). Unlike mapview
//                      this takes ONLY a coarse world: a finished patch
//                      directory is already the thing this tool produces.
//   --tag NAME         which snapshot to read (default: the latest written).
//   --patch-size M     window extent in metres (default 256).
//   --patch-res N      window resolution in texels (default 256).
//   --patch-origin X,Y a single window's origin in metres, named `patch`.
//   --window NAME=X,Y  a named window; repeatable.
//   --windows FILE     a window list, `name x y` per line, `#` comments.
//   --out DIR          where the images go (created if absent).
//   --layers a,b,c     any of height, biome, hillshade (default: all three).
//   --height-range LO,HI
//                      force the metre range the height channel maps over.
//                      Default is the UNION across every window in the run, so
//                      windows of one world are comparable by construction --
//                      per-image autoscale silently makes them not.
//   --dump-patch       also write each window as a patch directory (patch_io),
//                      which is the shareable and `mapview --load`-able form.
//
// Every window's exports carry a `<name>-export.txt` sidecar naming the metre
// range its codes decode against. Without it the PNGs are pictures, not data.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include <glm/glm.hpp>

#include "badlands_assets.h"
#include "mapgen/coarse_world_patch_source.hpp"
#include "mapgen/patch_data.hpp"
#include "mapgen/patch_export.hpp"
#include "mapgen/patch_io.hpp"

namespace {

using badlands::mapgen::ExportRange;
using badlands::mapgen::PatchData;

constexpr const char* kUsage =
    "usage: badlands_patch_export --load DIR [--tag NAME] "
    "[--patch-size M] [--patch-res N] "
    "(--patch-origin X,Y | --window NAME=X,Y ... | --windows FILE) --out DIR "
    "[--layers height,biome,hillshade] [--height-range LO,HI] [--dump-patch]\n";

struct Window {
  std::string name;
  glm::dvec2 origin_m{0.0};
};

// What one window turned into: kept so the batch can agree on a shared range
// before any of it is encoded.
struct Fetched {
  Window window;
  PatchData patch;
  double fetch_ms = 0.0;
  float height_min_m = 0.0f, height_max_m = 0.0f;
  float water_max_m = 0.0f;
  float wet_fraction = 0.0f;
};

bool parse_pair(const std::string& text, double& a, double& b) {
  const size_t comma = text.find(',');
  if (comma == std::string::npos) return false;
  try {
    a = std::stod(text.substr(0, comma));
    b = std::stod(text.substr(comma + 1));
  } catch (const std::exception&) {
    return false;
  }
  return true;
}

// `name x y` per line; blank lines and `#` comments skipped. A malformed line
// is an ERROR, not a skip -- a silently dropped window is a window you think
// you looked at.
bool read_window_file(const std::string& path, std::vector<Window>& out) {
  std::ifstream in(path);
  if (!in) {
    std::fprintf(stderr, "patch_export: cannot open --windows '%s'\n",
                 path.c_str());
    return false;
  }
  std::string line;
  int lineno = 0;
  while (std::getline(in, line)) {
    ++lineno;
    const size_t hash = line.find('#');
    if (hash != std::string::npos) line.resize(hash);
    Window w;
    double x = 0.0, y = 0.0;
    std::istringstream fields(line);
    if (!(fields >> w.name)) continue;  // blank or comment-only
    if (!(fields >> x >> y)) {
      std::fprintf(stderr, "patch_export: %s:%d: want `name x y`, got '%s'\n",
                   path.c_str(), lineno, line.c_str());
      return false;
    }
    w.origin_m = {x, y};
    out.push_back(std::move(w));
  }
  return true;
}

void summarize(Fetched& f) {
  const auto& h = f.patch.height;
  const badlands::mapgen::ElevationRange r =
      badlands::mapgen::compute_elevation_range(h);
  f.height_min_m = r.min_m;
  f.height_max_m = r.max_m;

  const auto& d = f.patch.water_depth;
  size_t wet = 0;
  for (float v : d.data) {
    if (v > 0.0f) {
      ++wet;
      f.water_max_m = std::max(f.water_max_m, v);
    }
  }
  f.wet_fraction = d.data.empty()
                       ? 0.0f
                       : static_cast<float>(wet) / static_cast<float>(d.data.size());
}

bool write_sidecar(const std::filesystem::path& path, const Fetched& f,
                   const badlands::mapgen::PatchRequest& req, ExportRange range,
                   float water_max_m, const std::string& source) {
  std::ofstream out(path);
  if (!out) return false;
  out.setf(std::ios::fixed);
  out << "resolution " << req.resolution << "\n";
  out << "world_size_m " << std::setprecision(9) << req.world_size_m << "\n";
  out << "origin_m " << std::setprecision(17) << req.origin_m.x << " "
      << req.origin_m.y << "\n";
  out << "texel_m " << std::setprecision(9) << f.patch.texel_m << "\n";
  out << "height_range_m " << range.lo_m << " " << range.hi_m << "\n";
  out << "water_max_m " << water_max_m << "\n";
  out << "source " << source << "\n";
  return static_cast<bool>(out);
}

}  // namespace

int main(int argc, char** argv) {
  std::string load_dir, tag, out_dir;
  float patch_size_m = 256.0f;
  int patch_res = 256;
  std::vector<Window> windows;
  bool want_height = true, want_biome = true, want_hillshade = true;
  std::optional<ExportRange> forced_range;
  bool dump_patch = false;

  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&](const char* flag) -> std::optional<std::string> {
      if (i + 1 < argc) return std::string(argv[++i]);
      std::fprintf(stderr, "patch_export: %s needs an argument\n", flag);
      return std::nullopt;
    };
    auto parse_num = [&](const char* flag, const char* want, auto conv,
                         auto& dst) -> bool {
      auto v = next(flag);
      if (!v) return false;
      try {
        dst = conv(*v);
        return true;
      } catch (const std::exception&) {
        std::fprintf(stderr, "patch_export: bad %s '%s' (want %s)\n", flag,
                     v->c_str(), want);
        return false;
      }
    };

    if (a == "--load") {
      if (auto v = next("--load")) load_dir = *v; else return 2;
    } else if (a == "--tag") {
      if (auto v = next("--tag")) tag = *v; else return 2;
    } else if (a == "--out") {
      if (auto v = next("--out")) out_dir = *v; else return 2;
    } else if (a == "--dump-patch") {
      dump_patch = true;
    } else if (a == "--patch-size") {
      if (!parse_num("--patch-size", "metres",
                     [](const std::string& s) { return std::stof(s); },
                     patch_size_m))
        return 2;
    } else if (a == "--patch-res") {
      if (!parse_num("--patch-res", "texels",
                     [](const std::string& s) { return std::stoi(s); },
                     patch_res))
        return 2;
    } else if (a == "--patch-origin") {
      auto v = next("--patch-origin");
      if (!v) return 2;
      double x = 0.0, y = 0.0;
      if (!parse_pair(*v, x, y)) {
        std::fprintf(stderr, "patch_export: bad --patch-origin '%s' (want X,Y)\n",
                     v->c_str());
        return 2;
      }
      windows.push_back({"patch", {x, y}});
    } else if (a == "--window") {
      auto v = next("--window");
      if (!v) return 2;
      const size_t eq = v->find('=');
      double x = 0.0, y = 0.0;
      if (eq == std::string::npos || eq == 0 ||
          !parse_pair(v->substr(eq + 1), x, y)) {
        std::fprintf(stderr, "patch_export: bad --window '%s' (want NAME=X,Y)\n",
                     v->c_str());
        return 2;
      }
      windows.push_back({v->substr(0, eq), {x, y}});
    } else if (a == "--windows") {
      auto v = next("--windows");
      if (!v) return 2;
      if (!read_window_file(*v, windows)) return 2;
    } else if (a == "--layers") {
      auto v = next("--layers");
      if (!v) return 2;
      want_height = want_biome = want_hillshade = false;
      std::string item;
      std::istringstream items(*v);
      while (std::getline(items, item, ',')) {
        if (item == "height") want_height = true;
        else if (item == "biome") want_biome = true;
        else if (item == "hillshade") want_hillshade = true;
        else {
          std::fprintf(stderr,
                       "patch_export: unknown layer '%s' (want height, biome, "
                       "or hillshade)\n",
                       item.c_str());
          return 2;
        }
      }
    } else if (a == "--height-range") {
      auto v = next("--height-range");
      if (!v) return 2;
      double lo = 0.0, hi = 0.0;
      if (!parse_pair(*v, lo, hi) || hi <= lo) {
        std::fprintf(stderr,
                     "patch_export: bad --height-range '%s' (want LO,HI with "
                     "HI > LO)\n",
                     v->c_str());
        return 2;
      }
      forced_range = ExportRange{static_cast<float>(lo), static_cast<float>(hi)};
    } else {
      std::fprintf(stderr, "patch_export: unknown arg '%s'\n%s", a.c_str(),
                   kUsage);
      return 2;
    }
  }

  if (load_dir.empty() || out_dir.empty() || windows.empty()) {
    std::fputs(kUsage, stderr);
    return 2;
  }
  if (patch_res <= 0 || patch_size_m <= 0.0f) {
    std::fprintf(stderr, "patch_export: --patch-size and --patch-res must be "
                         "positive (got %.3f m, %d texels)\n",
                 patch_size_m, patch_res);
    return 2;
  }

  std::string err;
  std::unique_ptr<badlands::mapgen::CoarseWorldPatchSource> source =
      badlands::mapgen::LoadCoarseWorldPatchSource(load_dir, tag, &err);
  if (!source) {
    std::fprintf(stderr, "patch_export: %s\n", err.c_str());
    return 1;
  }
  const badlands::mapgen::CoarseManifest& man = source->manifest();
  std::printf("patch_export: %s (%dx%d texels, %.0f m, %.2f m/texel) -> %s\n",
              load_dir.c_str(), man.resolution, man.resolution, man.world_size_m,
              man.texel_m, out_dir.c_str());

  std::error_code ec;
  std::filesystem::create_directories(out_dir, ec);
  if (!std::filesystem::is_directory(out_dir)) {
    std::fprintf(stderr, "patch_export: cannot create --out '%s'\n",
                 out_dir.c_str());
    return 1;
  }

  // One window at a time: fetch, encode, write, drop. Nothing here needs the
  // batch, because the height range is PER IMAGE by default.
  //
  // A shared range was the first design and measurement killed it: over these
  // eight windows the union spans 279 m, so one code step is 1.09 m, while the
  // stage-2 relief being judged averages 0.116 m per texel -- the whole signal
  // falls under one step. A single window spans ~41 m, which is the 0.16 m step
  // the 8-bit format was chosen for. Comparability is still available, but it
  // has to be asked for (--height-range), and the sidecar always records what
  // was used so an autoscaled image is never ambiguous.
  const float water_scale_m = badlands::mapgen::kExportWaterDepthM;
  std::printf("%-14s %16s %9s %18s %7s %10s\n", "window", "origin_m", "fetch_ms",
              "height_m", "wet%", "rivers");
  bool all_ok = true;
  ExportRange batch_union{0.0f, 0.0f};
  bool have_union = false;

  for (const Window& window : windows) {
    badlands::mapgen::PatchRequest req;
    req.origin_m = window.origin_m;
    req.world_size_m = patch_size_m;
    req.resolution = patch_res;

    const auto t0 = std::chrono::steady_clock::now();
    PatchData patch = source->Fetch(req);
    const auto t1 = std::chrono::steady_clock::now();
    if (badlands::mapgen::empty(patch)) {
      std::fprintf(stderr, "patch_export: window '%s' fetched empty\n",
                   window.name.c_str());
      return 1;
    }
    Fetched f;
    f.window = window;
    f.patch = std::move(patch);
    f.fetch_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    summarize(f);

    if (!have_union) {
      batch_union = {f.height_min_m, f.height_max_m};
      have_union = true;
    } else {
      batch_union.lo_m = std::min(batch_union.lo_m, f.height_min_m);
      batch_union.hi_m = std::max(batch_union.hi_m, f.height_max_m);
    }
    const ExportRange range =
        forced_range.value_or(ExportRange{f.height_min_m, f.height_max_m});

    const std::filesystem::path base = std::filesystem::path(out_dir);
    const std::string stem = f.window.name;
    const uint32_t w = static_cast<uint32_t>(f.patch.height.width);
    const uint32_t h = static_cast<uint32_t>(f.patch.height.height);

    if (want_height) {
      const std::vector<uint8_t> rgba = badlands::mapgen::encode_height_water_rgba(
          f.patch.height, f.patch.water_depth, range, water_scale_m);
      badlands_write_png((base / (stem + "-height.png")).string().c_str(),
                         rgba.data(), w, h);
    }
    if (want_biome) {
      const std::vector<uint8_t> rgba =
          badlands::mapgen::encode_biome_rgba(f.patch.biome);
      badlands_write_png((base / (stem + "-biome.png")).string().c_str(),
                         rgba.data(), w, h);
    }
    if (want_hillshade) {
      const std::vector<uint8_t> rgba =
          badlands::mapgen::encode_hillshade_rgba(f.patch);
      badlands_write_png((base / (stem + "-hillshade.png")).string().c_str(),
                         rgba.data(), w, h);
    }

    const std::string source_note =
        load_dir + (tag.empty() ? "" : " " + tag) + " @" +
        std::to_string(static_cast<long long>(f.window.origin_m.x)) + "," +
        std::to_string(static_cast<long long>(f.window.origin_m.y));
    if (!write_sidecar(base / (stem + "-export.txt"), f, req, range,
                       water_scale_m, source_note)) {
      std::fprintf(stderr, "patch_export: cannot write sidecar for '%s'\n",
                   stem.c_str());
      all_ok = false;
    }
    if (dump_patch) {
      std::string patch_err;
      if (!badlands::mapgen::write_patch((base / stem).string(), f.patch,
                                         source_note, &patch_err)) {
        std::fprintf(stderr, "patch_export: %s\n", patch_err.c_str());
        all_ok = false;
      }
    }

    char origin[32];
    std::snprintf(origin, sizeof(origin), "%.0f,%.0f", f.window.origin_m.x,
                  f.window.origin_m.y);
    char heights[32];
    std::snprintf(heights, sizeof(heights), "%.1f .. %.1f", f.height_min_m,
                  f.height_max_m);
    char rivers[32];
    std::snprintf(rivers, sizeof(rivers), "%zun/%zue", f.patch.rivers.nodes.size(),
                  f.patch.rivers.edges.size());
    std::printf("%-14s %16s %9.1f %18s %7.1f %10s\n", stem.c_str(), origin,
                f.fetch_ms, heights, 100.0f * f.wet_fraction, rivers);
  }

  if (forced_range) {
    std::printf("patch_export: height range forced to %.2f .. %.2f m\n",
                forced_range->lo_m, forced_range->hi_m);
  } else if (have_union && windows.size() > 1) {
    std::printf(
        "patch_export: each image autoscaled to its own range (see the "
        "sidecars).\n              for images comparable across the run: "
        "--height-range %.2f,%.2f\n",
        batch_union.lo_m, batch_union.hi_m);
  }
  return all_ok ? 0 : 1;
}
