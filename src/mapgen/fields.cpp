#include "mapgen/fields.hpp"

#include <glm/glm.hpp>

#include <atomic>
#include <cstdio>
#include <fstream>
#include <sstream>

#include "mapgen/noiser_util.hpp"
#include "mapgen/parallel.hpp"

namespace badlands::mapgen {

using sampo::noiser::ExecutionContext;
using sampo::noiser::NoiserInput;
using sampo::noiser::NoiserProgram;

namespace {

constexpr int kTile = 64;

// Read a whole text file. Returns false if it can't be opened.
bool read_file(const std::string& path, std::string& out) {
  std::ifstream f(path);
  if (!f) return false;
  std::ostringstream ss;
  ss << f.rdbuf();
  out = ss.str();
  return true;
}

}  // namespace

bool evaluate_fields(const MapgenConfig& cfg, const std::string& script_path,
                     Fields& out, std::string& err) {
  std::string source;
  if (!read_file(script_path, source)) {
    err = "cannot read field script: " + script_path;
    return false;
  }

  auto compiled = compile_big_stack(source);
  if (!compiled) {
    err = "field script compile failed: " +
          format_compile_error(compiled.error());
    return false;
  }
  std::shared_ptr<NoiserProgram> program = *compiled;

  const int W = cfg.width;
  const int H = cfg.height;
  out.elevation = Field2D<float>(W, H);
  out.moisture = Field2D<float>(W, H);
  out.ridged = Field2D<float>(W, H);
  out.fine = Field2D<float>(W, H);

  // Uniform locations (cached once). Warn on any that the script doesn't
  // declare — the value would silently fall back to the script default.
  struct Uni {
    const char* name;
    float value;
    int loc;
  };
  Uni unis[] = {
      {"seed", static_cast<float>(cfg.seed), -1},
      {"elev_freq", cfg.elevation_freq, -1},
      {"moist_freq", cfg.moisture_freq, -1},
      {"ridged_freq", cfg.ridged_freq, -1},
      {"fine_freq", cfg.fine_freq, -1},
  };
  for (auto& u : unis) {
    u.loc = program->GetUniformLocationI32(u.name);
    if (u.loc < 0) {
      std::fprintf(stderr,
                   "mapgen: field script has no uniform '%s' (using its "
                   "script default)\n",
                   u.name);
    }
  }

  // Thread-safety: the compiled program is shared (immutable after compile, no
  // host thunks bound) and each worker gets its own ExecutionContext (VM
  // contexts are not shareable) — noiser's sanctioned parallel pattern. Same-
  // seed runs are byte-identical. If a future noiser mutates program state in
  // Resume, switch to one compiled program per worker.
  //
  // The context is created once with the uniforms applied, then Reset per pixel.
  auto make_ctx = [&]() {
    ExecutionContext ctx = program->Prepare(
        NoiserInput{.warp_id = {0, 0, 0}, .warp_size = {W, H, 1}});
    for (const auto& u : unis) {
      if (u.loc >= 0) ctx.SetUniform(u.loc, u.value);
    }
    return ctx;
  };

  // Validate the script returns a 4-tuple once, up front: a clear error beats
  // silently zero-filling every pixel with a wrong-shaped result.
  {
    ExecutionContext probe = make_ctx();
    probe.Reset(NoiserInput{.warp_id = {0, 0, 0}, .warp_size = {W, H, 1}});
    auto r = program->Resume(probe);
    if (!r || !r->IsVec4()) {
      err =
          "field script must return a 4-tuple (elevation, moisture, ridged, "
          "fine)";
      return false;
    }
  }

  std::atomic<bool> eval_failed{false};
  parallel_tiles(
      W, H, kTile, make_ctx,
      [&](ExecutionContext& ctx, int x0, int y0, int x1, int y1) {
        for (int y = y0; y < y1; ++y) {
          for (int x = x0; x < x1; ++x) {
            ctx.Reset(
                NoiserInput{.warp_id = {x, y, 0}, .warp_size = {W, H, 1}});
            auto r = program->Resume(ctx);
            if (!r) {
              // A per-pixel runtime error: record it, don't silently zero-fill.
              eval_failed.store(true, std::memory_order_relaxed);
              continue;
            }
            const glm::vec4 v = r->AsVec4().value_or(glm::vec4(0.0f));
            out.elevation.at(x, y) = v.x;
            out.moisture.at(x, y) = v.y;
            out.ridged.at(x, y) = v.z;
            out.fine.at(x, y) = v.w;
          }
        }
      });

  if (eval_failed.load()) {
    err = "field script raised a runtime error during evaluation";
    return false;
  }
  return true;
}

}  // namespace badlands::mapgen
