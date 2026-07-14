#include <SDL3/SDL.h>

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <spdlog/spdlog.h>

#include "core/geometry_type.hpp"
#include "engine/core/camera.hpp"
#include "engine/rendering/context/scene_context.hpp"
#include "engine/rendering/geometry/textured_mesh_builders.hpp"
#include "engine/rendering/gpu_context.hpp"
#include "engine/rendering/material/material_instance_factory.hpp"
#include "engine/rendering/scene_renderer.hpp"
#include "engine/rendering/shader/gpu_pipeline_generator.hpp"
#include "engine/rendering/util/find_shader_directory.hpp"
#include "engine/scene/scene_graph.hpp"

// The ONLY game-specific seam. Stage 2 replaces this body with world->scene.
// Builds a single lit sphere (untextured -> 1x1 white default, flat-lit by
// textured_mesh.wesl's directional sun + flat ambient) so the forward +
// tonemap render path (Task D3) has something to draw. E2 swaps in a real
// albedo texture; later game-layer tasks replace this whole function with a
// world/scene translation.
static void build_test_scene(badlands::SceneGraph& scene,
                             badlands::MaterialInstanceFactory* mat) {
  auto sphere = badlands::GenerateSphereTexturedMesh(1.0f, 48);

  badlands::ResolvedMesh resolved_mesh{
      .vertices = std::move(sphere.mesh.vertices),
      .vertex_count = sphere.mesh.vertex_count,
      .geometry_type = sphere.mesh.geometry_type,
      .local_bounds = sphere.local_bounds,
  };

  auto node = scene.CreateNode("test_sphere");
  scene.AddAttachment(node, badlands::MeshAttachment{
      .mesh = std::move(resolved_mesh),
      .factory = mat,
      .pass_type = badlands::MaterialPassType::kForwardOpaque,
      .params = {},
  });

  scene.SetSunDirection(glm::normalize(glm::vec3(0.3f, 1.0f, 0.4f)));
  scene.SetSunColor(glm::vec3(1.0f));
}

int main(int, char**) {
  if (!SDL_Init(SDL_INIT_VIDEO)) return 1;
  SDL_Window* window = SDL_CreateWindow("badlands", 1600, 900, SDL_WINDOW_RESIZABLE);
  if (!window) {
    SDL_Quit();
    return 1;
  }

  badlands::GpuContext gpu;
  if (!gpu.Initialize(window)) {
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 1;
  }

  int width = 0, height = 0;
  SDL_GetWindowSizeInPixels(window, &width, &height);

  badlands::GpuPipelineGenerator pipeline_gen(gpu.GetDevice(),
                                              badlands::FindShaderDirectory());

  badlands::SceneRenderer renderer;
  renderer.Initialize(gpu.GetDevice(), gpu.GetQueue(), &pipeline_gen,
                      gpu.GetSurfaceFormat(), static_cast<uint32_t>(width),
                      static_cast<uint32_t>(height));

  // Build the textured_mesh material factory (D1), targeting the forward
  // pass's render-target formats (SceneRenderer's fixed HDR + reversed-Z
  // depth formats) so pipelines compiled through it match what the forward
  // pass actually renders into.
  badlands::FactoryDescriptor textured_mesh_desc;
  textured_mesh_desc.shader_name = "textured_mesh";
  textured_mesh_desc.shader_path = "material/textured_mesh.wesl";
  textured_mesh_desc.supported_pass_types = {
      badlands::MaterialPassType::kForwardOpaque};
  textured_mesh_desc.supported_geometry_types = {
      badlands::GeometryType::kTexturedMesh};
  textured_mesh_desc.color_formats = {badlands::SceneRenderer::kAccumulationFormat};
  textured_mesh_desc.depth_format = badlands::SceneRenderer::kDepthFormat;

  auto textured_mesh_factory = badlands::BuildMaterialInstanceFactory(
      textured_mesh_desc, gpu.GetDevice(), gpu.GetQueue(), &pipeline_gen,
      /*script_provider=*/nullptr);  // no NoiserMaterialScript recipes here
  if (!textured_mesh_factory) {
    spdlog::error("main: failed to build textured_mesh material factory");
    gpu.Shutdown();
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 1;
  }

  // Verify the (kTexturedMesh, kForwardOpaque, kForward) combination
  // actually compiles a valid pipeline before entering the render loop —
  // the discarded instance below is only a probe (the real render path
  // resolves its own instance per entity via MaterialInstanceCache).
  {
    auto probe = textured_mesh_factory->CreateInstance(
        badlands::GeometryType::kTexturedMesh,
        badlands::MaterialPassType::kForwardOpaque,
        badlands::RenderPassType::kForward, badlands::InstanceParams{});
    if (!probe || !probe->IsValid() || !probe->GetPipeline()) {
      spdlog::error(
          "main: textured_mesh material instance/pipeline is invalid");
      gpu.Shutdown();
      SDL_DestroyWindow(window);
      SDL_Quit();
      return 1;
    }
  }

  badlands::SceneGraph scene;
  build_test_scene(scene, textured_mesh_factory.get());

  entt::registry registry;
  badlands::SceneContext scene_context;

  badlands::Camera camera;
  camera.position = glm::vec3(0.0f, 1.5f, 4.0f);
  camera.LookAt(glm::vec3(0.0f));
  camera.aspect =
      height > 0 ? static_cast<float>(width) / static_cast<float>(height) : 1.0f;

  bool render_ok_logged = false;

  bool running = true;
  while (running) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      if (e.type == SDL_EVENT_QUIT) running = false;
      if (e.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
        int w = 0, h = 0;
        SDL_GetWindowSizeInPixels(window, &w, &h);
        gpu.Configure(static_cast<uint32_t>(w), static_cast<uint32_t>(h));
        renderer.Resize(static_cast<uint32_t>(w), static_cast<uint32_t>(h));
        if (h > 0) {
          camera.aspect = static_cast<float>(w) / static_cast<float>(h);
        }
      }
    }

    wgpu::TextureView view = gpu.AcquireSurfaceTexture();
    if (!view) continue;

    scene.SyncToRegistry(registry, scene_context);
    renderer.Render(camera, registry, scene_context, view);

    gpu.Present();

    if (!render_ok_logged) {
      render_ok_logged = true;
      if (badlands::g_gpu_error_flag) {
        spdlog::error("D3 render FAILED: Dawn validation error(s) occurred");
      } else {
        spdlog::info("D3 render OK");
      }
    }
  }

  gpu.Shutdown();
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}
