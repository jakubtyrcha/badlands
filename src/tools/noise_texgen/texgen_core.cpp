#include "tools/noise_texgen/texgen_core.hpp"

#include <pthread.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>

#include <glm/glm.hpp>

#include "core/parallel.hpp"

#include <badlands_assets.h>
#include <noiser.hpp>

namespace badlands::texgen {
namespace {

using sampo::noiser::CompileError;
using sampo::noiser::ExecutionContext;
using sampo::noiser::FileModuleResolver;
using sampo::noiser::NoiserInput;
using sampo::noiser::NoiserProgram;

constexpr uint32_t kPatch = 8;  // 8x8 pixel patches, one per parallel task.

// Returns nullopt if the file cannot be opened (distinct from an empty file,
// which returns an empty string and is surfaced later as a compile error).
std::optional<std::string> ReadFile(const std::string& path) {
  std::ifstream file(path);
  if (!file) return std::nullopt;
  std::stringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

std::string FormatError(const CompileError& err) {
  std::string out = err.message;
  if (err.line > 0) {
    out += " at " + (err.module.empty() ? std::string("main") : err.module) +
           ":" + std::to_string(err.line) + ":" + std::to_string(err.column);
  }
  return out;
}

// The noiser compiler recurses deeply and assumes a large (64 MiB) stack, so
// compilation runs on a dedicated big-stack thread (mirrors game/src/brain.cpp).
std::expected<std::shared_ptr<NoiserProgram>, std::string> CompileBigStack(
    const std::string& source, const std::string& main_path,
    const std::vector<std::filesystem::path>& search_paths) {
  struct Job {
    const std::string* source;
    const std::string* main_path;
    const std::vector<std::filesystem::path>* search_paths;
    std::expected<std::shared_ptr<NoiserProgram>, std::string> result;
  } job{&source, &main_path, &search_paths,
        std::unexpected(std::string("compile thread did not run"))};

  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 64u << 20);
  pthread_t thread;
  auto entry = [](void* raw) -> void* {
    auto* j = static_cast<Job*>(raw);
    FileModuleResolver resolver(*j->search_paths);
    CompileError err;
    auto prog = resolver.Compile(*j->source, *j->main_path, &err);
    if (!prog.has_value()) {
      j->result = std::unexpected(FormatError(err));
    } else {
      j->result = std::make_shared<NoiserProgram>(std::move(prog.value()));
    }
    return nullptr;
  };
  if (pthread_create(&thread, &attr, entry, &job) != 0) {
    pthread_attr_destroy(&attr);
    return std::unexpected(std::string("failed to spawn compile thread"));
  }
  pthread_join(thread, nullptr);
  pthread_attr_destroy(&attr);
  return std::move(job.result);
}

float LinearToSrgb(float linear) {
  linear = std::clamp(linear, 0.0f, 1.0f);
  return linear <= 0.0031308f ? linear * 12.92f
                              : 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
}

uint8_t FloatToByte(float value) {
  return static_cast<uint8_t>(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f);
}

// Reads the mode's expected return type from an evaluated result and writes the
// four encoded bytes. Returns an error string on a type mismatch.
std::expected<void, std::string> EncodePixel(
    const sampo::noiser::EvaluationResult& res, Mode mode, uint8_t* out) {
  switch (mode) {
    case Mode::kGrayscale: {
      auto v = res.AsF32();
      if (!v) return std::unexpected(v.error());
      uint8_t g = FloatToByte(*v);  // linear: no gamma for grayscale data
      out[0] = g;
      out[1] = g;
      out[2] = g;
      out[3] = 255;
      return {};
    }
    case Mode::kRgb: {
      auto c = res.AsVec3();
      if (!c) return std::unexpected(c.error());
      out[0] = FloatToByte(LinearToSrgb(c->r));
      out[1] = FloatToByte(LinearToSrgb(c->g));
      out[2] = FloatToByte(LinearToSrgb(c->b));
      out[3] = 255;
      return {};
    }
    case Mode::kRgba: {
      auto c = res.AsVec4();
      if (!c) return std::unexpected(c.error());
      out[0] = FloatToByte(LinearToSrgb(c->r));
      out[1] = FloatToByte(LinearToSrgb(c->g));
      out[2] = FloatToByte(LinearToSrgb(c->b));
      out[3] = FloatToByte(c->a);  // alpha is linear
      return {};
    }
  }
  return std::unexpected(std::string("invalid mode"));
}

const char* ModeName(Mode mode) {
  switch (mode) {
    case Mode::kGrayscale: return "grayscale (expected f32)";
    case Mode::kRgb: return "rgb (expected vec3)";
    case Mode::kRgba: return "rgba (expected vec4)";
  }
  return "?";
}

}  // namespace

std::expected<std::vector<uint8_t>, std::string> Generate(const Options& opts) {
  if (opts.width == 0 || opts.height == 0) {
    return std::unexpected(std::string("width and height must be > 0"));
  }

  const auto source = ReadFile(opts.script_path);
  if (!source) {
    return std::unexpected("failed to read script: " + opts.script_path);
  }

  std::vector<std::filesystem::path> search_paths;
  search_paths.push_back(
      std::filesystem::path(opts.script_path).parent_path());
  for (const auto& p : opts.include_paths) search_paths.emplace_back(p);

  auto program = CompileBigStack(*source, opts.script_path, search_paths);
  if (!program) return std::unexpected(program.error());
  const std::shared_ptr<NoiserProgram>& prog = *program;

  // Bind the domain window as program-level uniform defaults; every context
  // Prepare()d below inherits them. SetUniform returns false for a uniform the
  // script does not declare. A script may legitimately ignore the window, but
  // if the caller asked for one explicitly and none of the uniforms exist,
  // that is a mismatch worth surfacing rather than silently ignoring.
  bool bound = false;
  bound |= prog->SetUniform("win_min_x", opts.win_min_x);
  bound |= prog->SetUniform("win_min_y", opts.win_min_y);
  bound |= prog->SetUniform("win_max_x", opts.win_max_x);
  bound |= prog->SetUniform("win_max_y", opts.win_max_y);
  if (opts.window_explicit && !bound) {
    return std::unexpected(
        "--window was given but the script declares no window uniforms "
        "(win_min_x/win_min_y/win_max_x/win_max_y); does it `import ... from "
        "texgen`?");
  }

  const uint32_t w = opts.width;
  const uint32_t h = opts.height;
  const glm::ivec3 warp_size(static_cast<int>(w), static_cast<int>(h), 1);

  // Pre-flight: evaluate one pixel to validate the script's return type
  // matches the requested mode, so a mismatch fails fast with a clear message
  // rather than per-pixel inside the parallel loop.
  {
    ExecutionContext ctx = prog->Prepare(NoiserInput{.warp_id = glm::ivec3(0),
                                                     .warp_size = warp_size});
    auto res = prog->Resume(ctx);
    if (!res) {
      return std::unexpected(std::string("script produced no result"));
    }
    uint8_t probe[4];
    auto enc = EncodePixel(*res, opts.mode, probe);
    if (!enc) {
      return std::unexpected("script return type does not match mode " +
                             std::string(ModeName(opts.mode)) + ": " +
                             enc.error());
    }
  }

  std::vector<uint8_t> pixels(static_cast<size_t>(w) * h * 4, 0);

  const uint32_t patches_x = (w + kPatch - 1) / kPatch;
  const uint32_t patches_y = (h + kPatch - 1) / kPatch;
  const size_t patch_count = static_cast<size_t>(patches_x) * patches_y;

  std::atomic<bool> had_error{false};
  std::mutex error_mutex;
  std::string first_error;

  // Split the patches into one contiguous range per worker so a single
  // execution context is prepared per task (contexts are not thread-safe),
  // rather than one per 8x8 patch. Each task reuses its context across all its
  // pixels via Reset (which preserves the window uniforms).
  const size_t tasks =
      std::max<size_t>(1, std::min<size_t>(GetWorkerThreadCount(), patch_count));

  ParallelFor(tasks, [&](size_t t) {
    const size_t begin = t * patch_count / tasks;
    const size_t end = (t + 1) * patch_count / tasks;
    if (begin >= end) return;

    ExecutionContext ctx = prog->Prepare(
        NoiserInput{.warp_id = glm::ivec3(0), .warp_size = warp_size});

    for (size_t p = begin; p < end; ++p) {
      if (had_error.load(std::memory_order_relaxed)) return;

      const uint32_t px = static_cast<uint32_t>(p % patches_x);
      const uint32_t py = static_cast<uint32_t>(p / patches_x);
      const uint32_t x0 = px * kPatch;
      const uint32_t y0 = py * kPatch;
      const uint32_t x1 = std::min(x0 + kPatch, w);
      const uint32_t y1 = std::min(y0 + kPatch, h);

      for (uint32_t y = y0; y < y1; ++y) {
        for (uint32_t x = x0; x < x1; ++x) {
          ctx.Reset(NoiserInput{
              .warp_id =
                  glm::ivec3(static_cast<int>(x), static_cast<int>(y), 0),
              .warp_size = warp_size});
          auto res = prog->Resume(ctx);
          if (!res) {
            std::lock_guard<std::mutex> lock(error_mutex);
            had_error.store(true, std::memory_order_relaxed);
            if (first_error.empty()) first_error = "evaluation returned nothing";
            return;
          }
          uint8_t* out = &pixels[(static_cast<size_t>(y) * w + x) * 4];
          if (auto enc = EncodePixel(*res, opts.mode, out); !enc) {
            std::lock_guard<std::mutex> lock(error_mutex);
            had_error.store(true, std::memory_order_relaxed);
            if (first_error.empty()) first_error = enc.error();
            return;
          }
        }
      }
    }
  });

  if (had_error.load()) return std::unexpected(first_error);
  return pixels;
}

std::expected<void, std::string> GenerateToPng(const Options& opts,
                                               const std::string& out_path) {
  auto pixels = Generate(opts);
  if (!pixels) return std::unexpected(pixels.error());
  badlands_write_png(out_path.c_str(), pixels->data(), opts.width, opts.height);
  return {};
}

}  // namespace badlands::texgen
