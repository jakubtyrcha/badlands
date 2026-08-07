// The editor's RHI renderer, on the Null backend wherever the property allows.
//
// CATCH2, not the doctest the rest of editors/shapeshifter uses. This target
// links badlands_rhi and the render graph, whose own suites are Catch2, and two
// frameworks in one binary is worse than two in one repo. The editor's pure-CPU
// core keeps its doctest suite next door, untouched.

#include <catch_amalgamated.hpp>

#include <string>

#include "engine/rhi/rhi_device.hpp"
#include "engine/slang/slang_compiler.hpp"

#include "rhi_pipelines.h"

using namespace badlands::rhi;

namespace {

// Both paths: the entry points live in shaders/slang/shapeshifter, and the
// headers they include (shared_types.h, sdf_scene.h, ground_grid.h) live one
// level up in shaders/. Relative to the repo root, which is where ctest runs.
std::unique_ptr<badlands::slang::SlangCompiler> MakeCompiler() {
  const std::string paths[] = {
      "editors/shapeshifter/shaders/slang/shapeshifter",
      "editors/shapeshifter/shaders"};
  return badlands::slang::CreateSlangCompiler(paths);
}

}  // namespace

TEST_CASE("shapeshifter: the core can create an RHI device", "[ss-rhi]") {
  // The whole of task 1: proves shapeshifter_core links the RHI and that a
  // device can be made from inside the editor's own target, before anything is
  // built on top of it.
  auto device = CreateDevice({.backend = BackendKind::Null,
                              .enable_validation = true,
                              .label = "shapeshifter_tests"});
  REQUIRE(device != nullptr);
}

TEST_CASE("shapeshifter: all seven pipelines build", "[ss-rhi]") {
  // SEVEN where the metal-cpp path had five PSOs. Metal let each draw pick its
  // depth-stencil state and primitive type; the RHI folds both into the
  // pipeline, so the one blend PSO becomes three -- lines and triangles,
  // depth-tested and depth-ignored.
  auto device = CreateDevice({.backend = BackendKind::Null,
                              .enable_validation = true, .label = "pipelines"});
  REQUIRE(device);
  auto compiler = MakeCompiler();
  REQUIRE(compiler);

  auto p = sq::RhiPipelines::Create(*device, *compiler, Format::RGBA16Float,
                                    Format::Depth32Float);
  REQUIRE(p != nullptr);
  CHECK(p->raymarch);
  CHECK(p->mesh);
  CHECK(p->ground);
  CHECK(p->origin);
  CHECK(p->lines);
  CHECK(p->blend_lines);
  CHECK(p->blend_tris);
}

TEST_CASE("shapeshifter: all seven pipelines build ON METAL", "[ss-rhi][metal]") {
  // The Null case above proves Slang compiled and the descriptors are
  // structurally valid. It cannot prove more: Null "runs no shaders" by its own
  // comment, so a pipeline it accepts may still be refused by a real backend --
  // an attachment format that cannot blend, a depth format the pass disagrees
  // with, an entry point name that does not exist in the emitted MSL. Those only
  // surface where the MSL is actually compiled.
  auto device = CreateDevice({.backend = BackendKind::Metal,
                              .enable_validation = true, .label = "pipelines"});
  if (!device) {
    SUCCEED("no Metal device on this host");
    return;
  }
  auto compiler = MakeCompiler();
  REQUIRE(compiler);

  auto p = sq::RhiPipelines::Create(*device, *compiler, Format::RGBA16Float,
                                    Format::Depth32Float);
  REQUIRE(p != nullptr);
  CHECK(p->raymarch);
  CHECK(p->mesh);
  CHECK(p->ground);
  CHECK(p->origin);
  CHECK(p->lines);
  CHECK(p->blend_lines);
  CHECK(p->blend_tris);
}
