#include "shadow_test_harness.hpp"

#include <memory>
#include <string>

#include <dawn/webgpu_cpp.h>
#include <entt/entt.hpp>
#include <spdlog/spdlog.h>

#include "engine/rendering/color_render_target.hpp"
#include "engine/rendering/context/scene_context.hpp"
#include "engine/rendering/geometry/primitive_mesh_builders.hpp"
#include "engine/rendering/geometry/textured_mesh_builders.hpp"
#include "engine/rendering/material_library.hpp"
#include "engine/rendering/scene_build.hpp"
#include "engine/rendering/shader/gpu_pipeline_generator.hpp"
#include "engine/rendering/texture_readback.hpp"
#include "engine/rendering/util/find_shader_directory.hpp"
#include "engine/scene/scene_graph.hpp"
#include "gpu_test_helpers.hpp"

namespace badlands::shadowtest {

namespace {

// Process-lifetime headless GPU context: an instance/adapter/device/queue
// from badlands::test's headless helpers (no SDL window -- unlike
// GpuContext, these don't need a real surface), the shared pipeline
// generator, and a MaterialLibrary (for the flat-gray debug material).
// Deliberately leaked (never destroyed): this is a test binary that exits
// shortly after use, and avoids any static-destruction-order pitfalls with
// Dawn's ref-counted wgpu:: handles.
struct TestGpu {
  wgpu::Instance instance;
  wgpu::Device device;
  wgpu::Queue queue;
  std::unique_ptr<GpuPipelineGenerator> pipeline_gen;
  MaterialLibrary matlib;
};

TestGpu& GetTestGpu() {
  static TestGpu* instance = [] {
    auto* g = new TestGpu();

    wgpu::InstanceDescriptor instance_desc = {};
    g->instance = wgpu::CreateInstance(&instance_desc);
    if (!g->instance) {
      spdlog::error("shadow_test_harness: wgpu::CreateInstance failed");
      std::abort();
    }

    wgpu::Adapter adapter = badlands::test::RequestAdapter(g->instance);
    if (!adapter) {
      spdlog::error("shadow_test_harness: RequestAdapter failed");
      std::abort();
    }

    g->device = badlands::test::RequestDevice(adapter);
    if (!g->device) {
      spdlog::error("shadow_test_harness: RequestDevice failed");
      std::abort();
    }
    g->queue = g->device.GetQueue();

    g->pipeline_gen =
        std::make_unique<GpuPipelineGenerator>(g->device, FindShaderDirectory());
    if (!g->matlib.Initialize(g->device, g->queue, g->pipeline_gen.get())) {
      spdlog::error("shadow_test_harness: MaterialLibrary::Initialize failed");
      std::abort();
    }
    return g;
  }();
  return *instance;
}

}  // namespace

CpuImage RenderShadowFrame(const ShadowTestConfig& config, const Scene& world_scene,
                           const Camera& camera) {
  TestGpu& test_gpu = GetTestGpu();
  wgpu::Device device = test_gpu.device;
  wgpu::Queue queue = test_gpu.queue;

  // Build the render scene from `world_scene` alone (ground_point/normal +
  // caster model matrices/half_extents) so it is always geometrically
  // consistent with the oracle, which reads the same fields.
  SceneGraph graph;
  graph.SetSunDirection(world_scene.sun_toward);
  graph.SetSunColor(glm::vec3(3.0f));
  graph.SetClearColor(glm::vec4(0.1f, 0.1f, 0.1f, 1.0f));

  DeferredMaterial gray = test_gpu.matlib.SolidColor(glm::vec3(0.881f), 1.0f);

  // Floor: a large quad centered at ground_point, oriented so its normal is
  // ground_normal -- GenerateQuadTexturedMesh spans local X/Y at Z=0 with
  // normal +Z (scene_build.cpp), so map local X->basis_u, local Y->basis_v,
  // local Z->ground_normal directly via the transform's column basis.
  glm::vec3 basis_u, basis_v;
  PlaneBasis(world_scene.ground_normal, basis_u, basis_v);
  glm::mat4 floor_transform(1.0f);
  floor_transform[0] = glm::vec4(basis_u, 0.0f);
  floor_transform[1] = glm::vec4(basis_v, 0.0f);
  floor_transform[2] = glm::vec4(world_scene.ground_normal, 0.0f);
  floor_transform[3] = glm::vec4(world_scene.ground_point, 1.0f);
  AddMeshEntity(graph, "floor", GenerateQuadTexturedMesh(2.0f * kFloorHalfSize, 1, 1.0f),
               gray, floor_transform);

  int caster_index = 0;
  for (const CasterMesh& caster : world_scene.casters) {
    const std::string name = "caster_" + std::to_string(caster_index++);
    AddMeshEntity(graph, name.c_str(), GenerateCube(caster.half_extents), gray,
                 caster.model_matrix);
  }

  entt::registry registry;
  SceneContext scene_context;
  scene_context.registry = &registry;
  graph.SyncToRegistry(registry, scene_context);

  // Offscreen R32Float target: TextureReadback supports R32Float (not
  // RGBA16Float), and R32Float is exactly a linear single-channel float --
  // scene_renderer.cpp's output_is_linear predicate treats it as linear
  // (like RGBA16Float), so the tonemap pass's `return hdr;` passes the
  // shadow debug modes' vec4(v,v,v,1) straight through with R = v, no
  // sRGB/clamp distortion. See task-8-brief.md.
  ColorRenderTarget rt(device, kFrameWidth, kFrameHeight, wgpu::TextureFormat::R32Float);

  SceneRenderer renderer;
  renderer.Initialize(device, queue, test_gpu.pipeline_gen.get(),
                      wgpu::TextureFormat::R32Float, kFrameWidth, kFrameHeight,
                      device.HasFeature(wgpu::FeatureName::TextureFormatsTier1));

  ShadowConfig shadow_config;
  shadow_config.coverage_dmax = config.d_max;
  shadow_config.resolution = config.r_sm;
  shadow_config.backward_extension = 100.0f;
  shadow_config.enable_shadow_map = config.enable_shadow_map;
  shadow_config.enable_contact_shadows = config.enable_contact_shadows;
  shadow_config.hard_shadow_debug = config.hard_shadow_debug;
  renderer.SetShadowConfig(shadow_config);
  renderer.SetShadowDebugMode(config.mode);

  renderer.Render(camera, registry, scene_context, rt.GetView());
  badlands::test::WaitForGpu(test_gpu.instance, device, queue);

  TextureReadback readback(test_gpu.instance, device, queue);
  return readback.ReadTextureSync(rt.GetTexture(), kFrameWidth, kFrameHeight,
                                  wgpu::TextureFormat::R32Float);
}

}  // namespace badlands::shadowtest
