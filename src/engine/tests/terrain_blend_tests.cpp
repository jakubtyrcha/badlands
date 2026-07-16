// GPU tests for the terrain-blend material. Renders a kTerrainBlend quad and
// reads back the unlit G-buffer ALBEDO via GBufferDebugMode::Albedo (so the
// blend is checked directly, isolated from lighting):
//   1. per-vertex red/green/blue blend of a 3-layer debug array;
//   2. a kTerrainBlend material with NO albedo_array override renders the
//      factory's default (a neutral gray array), not a Dawn view-dimension error.

#include <catch_amalgamated.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include "core/geometry_type.hpp"
#include "core/util/cpu_image.hpp"
#include "engine/core/camera.hpp"
#include "engine/rendering/components/material_factory_component.hpp"
#include "engine/rendering/components/mesh_components.hpp"
#include "engine/rendering/context/scene_context.hpp"
#include "engine/rendering/material_library.hpp"
#include "engine/rendering/scene_renderer.hpp"
#include "engine/rendering/shader/gpu_pipeline_generator.hpp"
#include "engine/rendering/texture_loader.hpp"
#include "engine/rendering/texture_readback.hpp"
#include "engine/rendering/util/find_shader_directory.hpp"
#include "gpu_test_helpers.hpp"

using namespace badlands;

namespace {

constexpr uint32_t kW = 256;
constexpr uint32_t kH = 256;

// Pack one kTerrainBlend vertex (8 floats): pos3 + normal3 + layer_indices
// (Uint8x4 packed as 1 float) + blend_weights (Unorm8x4 packed as 1 float).
void PushTerrainVertex(std::vector<float>& out, glm::vec3 pos, glm::vec3 nrm,
                       glm::vec4 w, std::array<uint32_t, 4> idx) {
  out.insert(out.end(), {pos.x, pos.y, pos.z, nrm.x, nrm.y, nrm.z});
  const uint32_t pi = (idx[0] & 0xffu) | ((idx[1] & 0xffu) << 8) |
                      ((idx[2] & 0xffu) << 16) | ((idx[3] & 0xffu) << 24);
  auto b = [](float f) {
    return static_cast<uint32_t>(std::lround(std::clamp(f, 0.0f, 1.0f) * 255.0f));
  };
  const uint32_t pw =
      b(w.x) | (b(w.y) << 8) | (b(w.z) << 16) | (b(w.w) << 24);
  float fi, fw;
  std::memcpy(&fi, &pi, sizeof(float));
  std::memcpy(&fw, &pw, sizeof(float));
  out.push_back(fi);
  out.push_back(fw);
}

// Add a double-sided, INDEXED kTerrainBlend quad (XZ plane, +Y normal) whose
// four corners (A,B,C,D) carry the given per-vertex weights. Indexed so the
// test also exercises the DrawIndexed path.
void AddTerrainQuad(entt::registry& reg, MaterialInstanceFactory* factory,
                    InstanceParams params,
                    const std::array<glm::vec4, 4>& corner_w,
                    const std::array<uint32_t, 4>& idx) {
  const float S = 3.0f;
  const glm::vec3 nY(0, 1, 0);
  const std::array<glm::vec3, 4> corners = {
      glm::vec3(-S, 0, -S), glm::vec3(S, 0, -S), glm::vec3(-S, 0, S),
      glm::vec3(S, 0, S)};  // A, B, C, D
  std::vector<float> v;
  for (int i = 0; i < 4; ++i)
    PushTerrainVertex(v, corners[i], nY, corner_w[i], idx);
  // Front tris (A,B,C),(B,D,C) + reversed for double-sidedness.
  std::vector<uint32_t> indices = {0, 1, 2, 1, 3, 2, 0, 2, 1, 1, 2, 3};

  entt::entity e = reg.create();
  auto& mesh = reg.emplace<StaticTexturedMeshComponent>(e);
  mesh.vertices = std::move(v);
  mesh.vertex_count = 4;
  mesh.indices = std::move(indices);
  mesh.dirty = true;
  mesh.geometry_type = GeometryType::kTerrainBlend;
  mesh.transform = glm::mat4(1.0f);

  MaterialFactoryComponent fmc;
  fmc.factory = factory;
  fmc.pass_type = MaterialPassType::kDeferred;
  fmc.params = std::move(params);
  fmc.config_hash = ComputeFactoryConfigHash(fmc);
  reg.emplace<MaterialFactoryComponent>(e, std::move(fmc));
}

// Render the registry top-down and read back the unlit albedo debug view.
CpuImage RenderAlbedo(wgpu::Instance instance, wgpu::Device device,
                      wgpu::Queue queue, GpuPipelineGenerator& pipeline_gen,
                      entt::registry& registry) {
  Camera camera;  // top-down, centred on the quad, quad fills most of the frame
  camera.position = glm::vec3(0.0f, 10.0f, 0.0f);
  camera.direction = glm::vec3(0.0f, -1.0f, 0.0f);
  camera.up = glm::vec3(0.0f, 0.0f, -1.0f);
  camera.fov = 45.0f;
  camera.aspect = static_cast<float>(kW) / static_cast<float>(kH);
  camera.near_plane = 0.5f;
  camera.far_plane = 100.0f;

  SceneContext scene;
  scene.registry = &registry;

  wgpu::TextureDescriptor td = {};
  td.size = {kW, kH, 1};
  td.format = wgpu::TextureFormat::RGBA8Unorm;
  td.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::CopySrc;
  td.mipLevelCount = 1;
  td.sampleCount = 1;
  td.dimension = wgpu::TextureDimension::e2D;
  wgpu::Texture target = device.CreateTexture(&td);

  SceneRenderer renderer;
  renderer.Initialize(device, queue, &pipeline_gen,
                      wgpu::TextureFormat::RGBA8Unorm, kW, kH,
                      /*has_r8unorm_storage=*/false);
  renderer.SetDebugMode(GBufferDebugMode::Albedo);
  renderer.Render(camera, registry, scene, target.CreateView());

  TextureReadback readback(instance, device, queue);
  CpuImage image;
  REQUIRE(readback.ReadTexture(target).AwaitInto(image));
  REQUIRE(image.GetWidth() == kW);
  REQUIRE(image.GetHeight() == kH);
  return image;
}

struct TestGpu {
  wgpu::Instance instance;
  wgpu::Device device;
  wgpu::Queue queue;
};

TestGpu MakeGpu() {
  TestGpu g;
  g.instance = wgpu::CreateInstance();
  wgpu::Adapter adapter = test::RequestAdapter(g.instance);
  REQUIRE(adapter);
  g.device = test::RequestDevice(adapter, "terrain_blend_test");
  REQUIRE(g.device);
  g.queue = g.device.GetQueue();
  return g;
}

wgpu::Sampler LinearSampler(wgpu::Device device) {
  wgpu::SamplerDescriptor sd = {};
  sd.minFilter = wgpu::FilterMode::Linear;
  sd.magFilter = wgpu::FilterMode::Linear;
  return device.CreateSampler(&sd);
}

}  // namespace

TEST_CASE("terrain_blend: per-vertex R/G/B blend via the albedo debug view") {
  TestGpu g = MakeGpu();
  GpuPipelineGenerator pipeline_gen(g.device, FindShaderDirectory());
  MaterialLibrary lib;
  REQUIRE(lib.Initialize(g.device, g.queue, &pipeline_gen));

  // Three debug materials: solid red / green / blue as array layers 0, 1, 2.
  const std::array<uint8_t, 12> layer_colors = {
      255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255};
  wgpu::TextureView array_view =
      CreateSolidColorArray(g.device, g.queue, layer_colors.data(), 3);
  DeferredMaterial mat = lib.TerrainBlend(array_view, LinearSampler(g.device));
  REQUIRE(mat.factory != nullptr);

  entt::registry registry;
  AddTerrainQuad(registry, mat.factory, mat.params,
                 {glm::vec4(1, 0, 0, 0), glm::vec4(0, 1, 0, 0),
                  glm::vec4(0, 0, 1, 0),
                  glm::vec4(1.0f / 3, 1.0f / 3, 1.0f / 3, 0)},
                 {0, 1, 2, 0});

  CpuImage image = RenderAlbedo(g.instance, g.device, g.queue, pipeline_gen,
                                registry);
  image.WritePng("terrain_blend_albedo.png");

  // Center lies on the B(green)->C(blue) triangulation edge: a 50/50 blend.
  const CpuImage::Color c = image.GetPixel(kW / 2, kH / 2);
  INFO("center RGBA = " << int(c.r) << "," << int(c.g) << "," << int(c.b));
  CHECK(c.r < 60);
  CHECK(std::abs(int(c.g) - 128) < 45);
  CHECK(std::abs(int(c.b) - 128) < 45);

  // Each pure-weight corner samples its layer (proves layer lookup + one-hot).
  bool saw_red = false, saw_green = false, saw_blue = false;
  for (uint32_t y = 0; y < kH; ++y) {
    for (uint32_t x = 0; x < kW; ++x) {
      const CpuImage::Color p = image.GetPixel(x, y);
      if (p.r > 200 && p.g < 80 && p.b < 80) saw_red = true;
      if (p.g > 200 && p.r < 80 && p.b < 80) saw_green = true;
      if (p.b > 200 && p.r < 80 && p.g < 80) saw_blue = true;
    }
  }
  CHECK(saw_red);
  CHECK(saw_green);
  CHECK(saw_blue);
}

TEST_CASE("terrain_blend: missing albedo_array binds the default (no override)") {
  // Regression: a kTerrainBlend instance built WITHOUT an albedo_array override
  // must fall back to a valid texture_2d_array default (a neutral gray), not a
  // 2D 1x1 view that fails Dawn's e2DArray bind-group validation (which would
  // skip the draw and leave the quad unrendered).
  TestGpu g = MakeGpu();
  GpuPipelineGenerator pipeline_gen(g.device, FindShaderDirectory());
  MaterialLibrary lib;
  REQUIRE(lib.Initialize(g.device, g.queue, &pipeline_gen));

  entt::registry registry;
  AddTerrainQuad(registry, lib.terrain_factory(), InstanceParams{},
                 {glm::vec4(1, 0, 0, 0), glm::vec4(1, 0, 0, 0),
                  glm::vec4(1, 0, 0, 0), glm::vec4(1, 0, 0, 0)},
                 {0, 0, 0, 0});

  CpuImage image = RenderAlbedo(g.instance, g.device, g.queue, pipeline_gen,
                                registry);

  // The quad rendered the default gray albedo (not the background clear).
  const CpuImage::Color c = image.GetPixel(kW / 2, kH / 2);
  INFO("center RGBA = " << int(c.r) << "," << int(c.g) << "," << int(c.b));
  CHECK(std::abs(int(c.r) - 128) < 45);
  CHECK(std::abs(int(c.g) - 128) < 45);
  CHECK(std::abs(int(c.b) - 128) < 45);
}
