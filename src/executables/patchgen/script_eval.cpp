#include "executables/patchgen/script_eval.hpp"

#include <atomic>
#include <cstdio>
#include <fstream>
#include <sstream>

#include "executables/patchgen/noiser_util.hpp"
#include "mapgen/parallel.hpp"

namespace badlands::mapgen {

using sampo::noiser::ExecutionContext;
using sampo::noiser::NoiserInput;
using sampo::noiser::NoiserProgram;

namespace {

constexpr int kTile = 64;

bool read_file(const std::string& path, std::string& out) {
  std::ifstream f(path);
  if (!f) return false;
  std::ostringstream ss;
  ss << f.rdbuf();
  out = ss.str();
  return true;
}

}  // namespace

bool evaluate_patch_script(const std::string& source, const PatchDomain& domain,
                           const std::vector<ScriptUniform>& uniforms,
                           Field2D<float>& out, std::string& err) {
  if (domain.size <= 0) {
    err = "patch size must be positive";
    return false;
  }

  // The compiler recurses deeply enough to overflow a default stack -- always via
  // compile_big_stack, never NoiserProgram::Compile directly (noiser_util.hpp).
  auto compiled = compile_big_stack(source);
  if (!compiled) {
    err = "script compile failed: " + format_compile_error(compiled.error());
    return false;
  }
  std::shared_ptr<NoiserProgram> program = *compiled;

  // The patch's world placement is handed to the script as uniforms, so the script is a
  // function of world meters rather than of its position within the warp.
  std::vector<ScriptUniform> all = uniforms;
  all.push_back({"origin_x", domain.origin_x});
  all.push_back({"origin_z", domain.origin_z});
  all.push_back({"meters_per_sample", domain.meters_per_sample});

  struct Bound {
    int loc;
    float value;
    bool as_i32;
  };
  std::vector<Bound> bound;
  bound.reserve(all.size());

  // Uniforms must be set through the overload matching their DECLARED type. Pushing a
  // float into an i32-declared uniform (e.g. `@uni.octaves: i32`) does not convert --
  // the script reads garbage, and a loop bounded by it runs until the VM's instruction
  // limit. So dispatch on the metadata rather than assuming everything is f32.
  const std::vector<sampo::noiser::UniformMeta>& metas =
      program->GetUniformMetadata();
  for (const ScriptUniform& u : all) {
    const int loc = program->GetUniformLocationI32(u.name);
    if (loc < 0) {
      // Not an error: the script simply doesn't take this knob, and its own default
      // stands. Worth saying out loud though -- a typo'd --uniform is silently ignored
      // otherwise, and the render would look "fine" but wrong.
      std::fprintf(stderr, "patchgen: script has no uniform '%s' (ignored)\n",
                   u.name.c_str());
      continue;
    }
    bool as_i32 = false;
    for (const auto& m : metas) {
      if (m.name == u.name) {
        as_i32 = m.is_i32 || m.type == sampo::noiser::UniformType::kI32;
        break;
      }
    }
    bound.push_back({loc, u.value, as_i32});
  }

  const int N = domain.size;
  auto make_ctx = [&]() {
    ExecutionContext ctx = program->Prepare(
        NoiserInput{.warp_id = {0, 0, 0}, .warp_size = {N, N, 1}});
    // Set once per worker: Reset preserves uniforms (noiser.hpp:912-923).
    for (const Bound& b : bound) {
      if (b.as_i32) {
        ctx.SetUniform(b.loc, static_cast<int32_t>(b.value));
      } else {
        ctx.SetUniform(b.loc, b.value);
      }
    }
    return ctx;
  };

  // Shape probe before the parallel run, so a wrong return type is one clear message
  // rather than N identical ones.
  {
    ExecutionContext probe = make_ctx();
    probe.Reset(NoiserInput{.warp_id = {0, 0, 0}, .warp_size = {N, N, 1}});
    auto r = program->Resume(probe);
    if (!r) {
      err = "script raised a runtime error on its first sample";
      return false;
    }
    if (!r->IsF32()) {
      err =
          "script must return a single f32 (height in meters, water datum at 0); "
          "got a non-scalar result";
      return false;
    }
  }

  out = Field2D<float>(N, N);
  std::atomic<bool> eval_failed{false};
  parallel_tiles(N, N, kTile, make_ctx,
                 [&](ExecutionContext& ctx, int x0, int y0, int x1, int y1) {
                   for (int y = y0; y < y1; ++y) {
                     for (int x = x0; x < x1; ++x) {
                       ctx.Reset(NoiserInput{.warp_id = {x, y, 0},
                                             .warp_size = {N, N, 1}});
                       auto r = program->Resume(ctx);
                       if (!r) {
                         eval_failed.store(true, std::memory_order_relaxed);
                         continue;
                       }
                       out.at(x, y) = r->AsF32().value_or(0.0f);
                     }
                   }
                 });

  if (eval_failed.load()) {
    err = "script raised a runtime error during evaluation";
    return false;
  }
  return true;
}

bool evaluate_patch_script_file(const std::string& script_path,
                                const PatchDomain& domain,
                                const std::vector<ScriptUniform>& uniforms,
                                Field2D<float>& out, std::string& err) {
  std::string source;
  if (!read_file(script_path, source)) {
    err = "cannot read script: " + script_path;
    return false;
  }
  return evaluate_patch_script(source, domain, uniforms, out, err);
}

}  // namespace badlands::mapgen
