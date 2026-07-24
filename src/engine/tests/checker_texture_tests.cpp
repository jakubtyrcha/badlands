// Pure-CPU tests for the RGBA8 checkerboard fill (engine/rendering/
// checker_texture.hpp), the GPU-free core of MaterialLibrary::CheckerAlbedo.
// Also holds a small GPU-backed test for MaterialLibrary::TranslucentFoliage
// (constructing a real device, mirroring decal_pass_tests.cpp /
// terrain_blend_tests.cpp's TestGpu pattern) -- it only checks the built
// DeferredMaterial's factory + params, not a rendered frame.
#include <catch_amalgamated.hpp>

#include <memory>
#include <variant>

#include <dawn/webgpu_cpp.h>
#include <glm/glm.hpp>

#include "engine/rendering/checker_texture.hpp"
#include "engine/rendering/material_library.hpp"
#include "engine/rendering/shader/gpu_pipeline_generator.hpp"
#include "engine/rendering/texture_loader.hpp"
#include "engine/rendering/util/find_shader_directory.hpp"
#include "gpu_test_helpers.hpp"

using badlands::BuildCheckerboardRgba8;

TEST_CASE("checkerboard has RGBA8 size and opaque alpha") {
  const auto px = BuildCheckerboardRgba8(glm::vec3(1, 0, 0), glm::vec3(0, 1, 0),
                                         /*tiles=*/2, /*texels=*/4);
  REQUIRE(px.size() == 4u * 4u * 4u);
  for (size_t i = 3; i < px.size(); i += 4) REQUIRE(px[i] == 255);
}

TEST_CASE("checkerboard alternates color_a / color_b by tile parity") {
  const auto px = BuildCheckerboardRgba8(glm::vec3(1, 0, 0), glm::vec3(0, 1, 0),
                                         /*tiles=*/2, /*texels=*/4);
  auto rgb_at = [&](int x, int y) {
    const size_t o = (static_cast<size_t>(y) * 4 + x) * 4;
    return glm::ivec3(px[o], px[o + 1], px[o + 2]);
  };
  // tile size = 4/2 = 2 px. (0,0)=A red, (2,0)=B green, (0,2)=B green, (2,2)=A red.
  REQUIRE(rgb_at(0, 0) == glm::ivec3(255, 0, 0));
  REQUIRE(rgb_at(2, 0) == glm::ivec3(0, 255, 0));
  REQUIRE(rgb_at(0, 2) == glm::ivec3(0, 255, 0));
  REQUIRE(rgb_at(2, 2) == glm::ivec3(255, 0, 0));
}

namespace {

// Process-lifetime headless GPU context, same rationale as
// decal_pass_tests.cpp / terrain_blend_tests.cpp's TestGpu: a short-lived
// test binary, and Dawn's ref-counted handles do not enjoy static
// destruction order.
struct TestGpu {
  wgpu::Instance instance;
  wgpu::Device device;
  wgpu::Queue queue;
  std::unique_ptr<badlands::GpuPipelineGenerator> pipeline_gen;
  badlands::MaterialLibrary matlib;
};

TestGpu& GetTestGpu() {
  static TestGpu* instance = [] {
    auto* g = new TestGpu();
    g->instance = wgpu::CreateInstance();
    REQUIRE(g->instance);
    wgpu::Adapter adapter = badlands::test::RequestAdapter(g->instance);
    REQUIRE(adapter);
    g->device = badlands::test::RequestDevice(adapter, "checker_texture_test");
    REQUIRE(g->device);
    g->queue = g->device.GetQueue();
    g->pipeline_gen = std::make_unique<badlands::GpuPipelineGenerator>(
        g->device, badlands::FindShaderDirectory());
    REQUIRE(g->matlib.Initialize(g->device, g->queue, g->pipeline_gen.get()));
    return g;
  }();
  return *instance;
}

}  // namespace

TEST_CASE(
    "MaterialLibrary::TranslucentFoliage builds a transmitting "
    "forward-opaque material") {
  TestGpu& g = GetTestGpu();

  wgpu::TextureView albedo_view =
      badlands::CreateSolidColorTexture(g.device, g.queue, 255, 255, 255, 255);
  wgpu::SamplerDescriptor sampler_desc{};
  sampler_desc.minFilter = wgpu::FilterMode::Linear;
  sampler_desc.magFilter = wgpu::FilterMode::Linear;
  wgpu::Sampler sampler = g.device.CreateSampler(&sampler_desc);

  const glm::vec3 tint(0.2f, 0.6f, 0.1f);
  const glm::vec3 transmission_tint(0.3f, 0.8f, 0.15f);
  const float transmission_strength = 0.6f;

  badlands::DeferredMaterial result = g.matlib.TranslucentFoliage(
      albedo_view, sampler, /*cutoff=*/0.5f, tint, transmission_tint,
      transmission_strength);

  REQUIRE(result.factory != nullptr);

  REQUIRE(result.params.uniform_overrides.count("transmission") == 1u);
  REQUIRE(result.params.uniform_overrides.count("tint") == 1u);
  REQUIRE(result.params.uniform_overrides.count("params") == 1u);

  const auto& transmission_value =
      result.params.uniform_overrides.at("transmission");
  REQUIRE(std::holds_alternative<glm::vec4>(transmission_value));
  const glm::vec4 expected_transmission(transmission_tint,
                                        transmission_strength);
  CHECK(std::get<glm::vec4>(transmission_value) == expected_transmission);

  bool found_albedo = false;
  for (const auto& tex : result.params.texture_overrides) {
    if (tex.param_name == "albedo") {
      found_albedo = true;
      CHECK(tex.view.Get() == albedo_view.Get());
    }
  }
  CHECK(found_albedo);
}
