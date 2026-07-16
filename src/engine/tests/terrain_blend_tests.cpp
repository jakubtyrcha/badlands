// Verifies the terrain-blend material by rendering a kTerrainBlend quad whose
// per-vertex weights blend a 3-layer red/green/blue "debug material" array, and
// reading back the unlit G-buffer ALBEDO via the engine's albedo debug view
// (GBufferDebugMode::Albedo). The blend is thus checked directly, isolated from
// lighting. Also dumps terrain_blend_albedo.png for eyeballing.

#include <catch_amalgamated.hpp>

#include <array>
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

// Pack one kTerrainBlend vertex (pos3 + normal3 + weights4 + indices4) into the
// flat float buffer; the u32 layer indices are bitcast into 4 float slots.
void PushTerrainVertex(std::vector<float>& out, glm::vec3 pos, glm::vec3 nrm,
                       glm::vec4 w, std::array<uint32_t, 4> idx) {
  out.insert(out.end(), {pos.x, pos.y, pos.z, nrm.x, nrm.y, nrm.z, w.x, w.y,
                         w.z, w.w});
  for (uint32_t i : idx) {
    float f;
    std::memcpy(&f, &i, sizeof(float));
    out.push_back(f);
  }
}

}  // namespace

TEST_CASE("terrain_blend: per-vertex R/G/B blend via the albedo debug view") {
  wgpu::Instance instance = wgpu::CreateInstance();
  wgpu::Adapter adapter = test::RequestAdapter(instance);
  REQUIRE(adapter);
  wgpu::Device device = test::RequestDevice(adapter, "terrain_blend_test");
  REQUIRE(device);
  wgpu::Queue queue = device.GetQueue();

  GpuPipelineGenerator pipeline_gen(device, FindShaderDirectory());

  MaterialLibrary lib;
  REQUIRE(lib.Initialize(device, queue, &pipeline_gen));

  // Three debug materials: solid red, green, blue as array layers 0, 1, 2.
  const std::array<uint8_t, 12> layer_colors = {
      255, 0, 0, 255,  // layer 0 = red
      0, 255, 0, 255,  // layer 1 = green
      0, 0, 255, 255,  // layer 2 = blue
  };
  wgpu::TextureView array_view =
      CreateSolidColorArray(device, queue, layer_colors.data(), 3);
  wgpu::SamplerDescriptor samp_desc = {};
  samp_desc.minFilter = wgpu::FilterMode::Linear;
  samp_desc.magFilter = wgpu::FilterMode::Linear;
  wgpu::Sampler sampler = device.CreateSampler(&samp_desc);

  DeferredMaterial mat = lib.TerrainBlend(array_view, sampler);
  REQUIRE(mat.factory != nullptr);

  // A kTerrainBlend quad in the XZ plane (y=0), normal +Y. Corners carry pure
  // one-hot weights on layers R/G/B (+ a 3-way mix); all vertices share the
  // same layer_indices (flat), so weights carry the blend. Emitted
  // double-sided so the material's default back-face cull can't hide it.
  const float S = 3.0f;
  const glm::vec3 up_normal(0.0f, 1.0f, 0.0f);
  const std::array<uint32_t, 4> idx = {0, 1, 2, 0};
  const glm::vec3 A(-S, 0, -S), B(S, 0, -S), C(-S, 0, S), D(S, 0, S);
  const glm::vec4 wR(1, 0, 0, 0), wG(0, 1, 0, 0), wB(0, 0, 1, 0);
  const glm::vec4 wMix(1.0f / 3, 1.0f / 3, 1.0f / 3, 0);

  std::vector<float> verts;
  auto emit_tri = [&](glm::vec3 p0, glm::vec4 w0, glm::vec3 p1, glm::vec4 w1,
                      glm::vec3 p2, glm::vec4 w2) {
    PushTerrainVertex(verts, p0, up_normal, w0, idx);
    PushTerrainVertex(verts, p1, up_normal, w1, idx);
    PushTerrainVertex(verts, p2, up_normal, w2, idx);
    // reversed winding (double-sided)
    PushTerrainVertex(verts, p0, up_normal, w0, idx);
    PushTerrainVertex(verts, p2, up_normal, w2, idx);
    PushTerrainVertex(verts, p1, up_normal, w1, idx);
  };
  emit_tri(A, wR, B, wG, C, wB);
  emit_tri(B, wG, D, wMix, C, wB);
  const uint32_t vertex_count = static_cast<uint32_t>(verts.size() / 14);

  entt::registry registry;
  entt::entity e = registry.create();
  auto& mesh = registry.emplace<StaticTexturedMeshComponent>(e);
  mesh.vertices = std::move(verts);
  mesh.vertex_count = vertex_count;
  mesh.dirty = true;
  mesh.geometry_type = GeometryType::kTerrainBlend;
  mesh.transform = glm::mat4(1.0f);

  MaterialFactoryComponent fmc;
  fmc.factory = mat.factory;
  fmc.pass_type = MaterialPassType::kDeferred;
  fmc.params = mat.params;
  fmc.config_hash = ComputeFactoryConfigHash(fmc);
  registry.emplace<MaterialFactoryComponent>(e, std::move(fmc));

  // Top-down camera centered on the quad — quad center projects to the image
  // center; the quad fills most of the frame.
  const uint32_t W = 256, H = 256;
  Camera camera;
  camera.position = glm::vec3(0.0f, 10.0f, 0.0f);
  camera.direction = glm::vec3(0.0f, -1.0f, 0.0f);
  camera.up = glm::vec3(0.0f, 0.0f, -1.0f);
  camera.fov = 45.0f;
  camera.aspect = static_cast<float>(W) / static_cast<float>(H);
  camera.near_plane = 0.5f;
  camera.far_plane = 100.0f;

  SceneContext scene;
  scene.registry = &registry;

  wgpu::TextureDescriptor target_desc = {};
  target_desc.size = {W, H, 1};
  target_desc.format = wgpu::TextureFormat::RGBA8Unorm;
  target_desc.usage =
      wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::CopySrc;
  target_desc.mipLevelCount = 1;
  target_desc.sampleCount = 1;
  target_desc.dimension = wgpu::TextureDimension::e2D;
  wgpu::Texture target = device.CreateTexture(&target_desc);

  SceneRenderer renderer;
  renderer.Initialize(device, queue, &pipeline_gen,
                      wgpu::TextureFormat::RGBA8Unorm, W, H,
                      /*has_r8unorm_storage=*/false);
  renderer.SetDebugMode(GBufferDebugMode::Albedo);
  renderer.Render(camera, registry, scene, target.CreateView());

  TextureReadback readback(instance, device, queue);
  CpuImage image;
  REQUIRE(readback.ReadTexture(target).AwaitInto(image));
  REQUIRE(image.GetWidth() == W);
  REQUIRE(image.GetHeight() == H);
  image.WritePng("terrain_blend_albedo.png");

  // The quad center lies on the B(green)->C(blue) triangulation edge, so its
  // albedo is a 50/50 green+blue blend (~0,128,128) — the core 2-layer check.
  const CpuImage::Color center = image.GetPixel(W / 2, H / 2);
  INFO("center RGBA = " << int(center.r) << "," << int(center.g) << ","
                        << int(center.b) << "," << int(center.a));
  CHECK(center.r < 60);
  CHECK(std::abs(int(center.g) - 128) < 45);
  CHECK(std::abs(int(center.b) - 128) < 45);

  // Each pure-weight corner must sample its layer: some pixel is red-dominant,
  // some green-dominant, some blue-dominant (proves layer lookup + one-hot).
  bool saw_red = false, saw_green = false, saw_blue = false;
  for (uint32_t y = 0; y < H; ++y) {
    for (uint32_t x = 0; x < W; ++x) {
      const CpuImage::Color c = image.GetPixel(x, y);
      if (c.r > 200 && c.g < 80 && c.b < 80) saw_red = true;
      if (c.g > 200 && c.r < 80 && c.b < 80) saw_green = true;
      if (c.b > 200 && c.r < 80 && c.g < 80) saw_blue = true;
    }
  }
  CHECK(saw_red);
  CHECK(saw_green);
  CHECK(saw_blue);
}
