#include <SDL3/SDL.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <spdlog/spdlog.h>

#include "badlands_assets.h"
#include "core/geometry_type.hpp"
#include "engine/core/camera.hpp"
#include "engine/rendering/context/scene_context.hpp"
#include "engine/rendering/geometry/textured_mesh_builders.hpp"
#include "engine/rendering/gpu_context.hpp"
#include "engine/rendering/material/material_instance_factory.hpp"
#include "engine/rendering/scene_renderer.hpp"
#include "engine/rendering/shader/gpu_pipeline_generator.hpp"
#include "engine/rendering/texture_loader.hpp"
#include "engine/rendering/util/find_shader_directory.hpp"
#include "engine/scene/scene_graph.hpp"

// The ONLY game-specific seam. Stage 2 replaces this body with world->scene.
// Builds a single lit sphere (untextured -> 1x1 white default, flat-lit by
// textured_mesh.wesl's directional sun + flat ambient) so the forward +
// tonemap render path (Task D3) has something to draw. E2 swaps in a real
// albedo texture; later game-layer tasks replace this whole function with a
// world/scene translation.
static void build_test_scene(badlands::SceneGraph& scene,
                             badlands::MaterialInstanceFactory* mat,
                             const badlands::InstanceParams& params) {
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
      .params = params,
  });

  scene.SetSunDirection(glm::normalize(glm::vec3(0.3f, 1.0f, 0.4f)));
  scene.SetSunColor(glm::vec3(1.0f));
}

namespace {

// 256-byte row alignment WebGPU requires for CopyTextureToBuffer
// destinations (WGPU_COPY_BYTES_PER_ROW_ALIGNMENT).
constexpr uint32_t kCopyBytesPerRowAlignment = 256;

uint32_t AlignUp(uint32_t value, uint32_t alignment) {
  return ((value + alignment - 1) / alignment) * alignment;
}

// Renders one frame of `scene`/`camera` into an offscreen
// Rgba8Unorm texture (instead of the SDL surface), reads it back, and
// writes it to `path` as a PNG via the `assets` crate. Blocks until the
// GPU work + readback complete. Returns false (after logging) on failure.
//
// The offscreen texture uses a non-sRGB format (RGBA8Unorm) to match the
// window surface's BGRA8Unorm (also non-sRGB): the tonemap shader manually
// applies linear_to_srgb encoding when output_is_linear==0. An sRGB target
// format (e.g. RGBA8UnormSrgb) would have the hardware sRGB-encode the
// already-encoded output a second time on store, producing a too-bright
// image.
bool RenderScreenshot(badlands::GpuContext& gpu,
                      badlands::GpuPipelineGenerator& pipeline_gen,
                      badlands::SceneGraph& scene,
                      entt::registry& registry,
                      badlands::SceneContext& scene_context,
                      const badlands::Camera& camera,
                      uint32_t width, uint32_t height,
                      const std::string& path) {
  wgpu::Device device = gpu.GetDevice();
  wgpu::Queue queue = gpu.GetQueue();

  // Offscreen tonemap target: same convention the window surface uses
  // (non-sRGB 8-bit color, manually sRGB-encoded by the tonemap shader),
  // but with COPY_SRC instead of presentation usage so we can read it back.
  wgpu::TextureDescriptor offscreen_desc;
  offscreen_desc.size = {width, height, 1};
  offscreen_desc.format = wgpu::TextureFormat::RGBA8Unorm;
  offscreen_desc.usage =
      wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::CopySrc;
  offscreen_desc.mipLevelCount = 1;
  offscreen_desc.sampleCount = 1;
  offscreen_desc.dimension = wgpu::TextureDimension::e2D;
  wgpu::Texture offscreen_texture = device.CreateTexture(&offscreen_desc);
  wgpu::TextureView offscreen_view = offscreen_texture.CreateView();
  if (!offscreen_view) {
    spdlog::error("RenderScreenshot: failed to create offscreen texture");
    return false;
  }

  badlands::SceneRenderer renderer;
  renderer.Initialize(device, queue, &pipeline_gen,
                      wgpu::TextureFormat::RGBA8Unorm, width, height);

  scene.SyncToRegistry(registry, scene_context);
  renderer.Render(camera, registry, scene_context, offscreen_view);

  // Read the offscreen texture back: CopyTextureToBuffer requires
  // bytesPerRow to be a multiple of 256, which the tightly-packed RGBA8
  // buffer badlands_write_png expects generally isn't -- strip the padding
  // per row after mapping.
  const uint32_t unpadded_bytes_per_row = width * 4;
  const uint32_t padded_bytes_per_row =
      AlignUp(unpadded_bytes_per_row, kCopyBytesPerRowAlignment);
  const uint64_t buffer_size =
      static_cast<uint64_t>(padded_bytes_per_row) * height;

  wgpu::BufferDescriptor readback_desc;
  readback_desc.label = "screenshot readback";
  readback_desc.size = buffer_size;
  readback_desc.usage =
      wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst;
  wgpu::Buffer readback_buffer = device.CreateBuffer(&readback_desc);

  wgpu::CommandEncoder encoder = device.CreateCommandEncoder();

  wgpu::TexelCopyTextureInfo copy_src;
  copy_src.texture = offscreen_texture;
  copy_src.mipLevel = 0;
  copy_src.origin = {0, 0, 0};
  copy_src.aspect = wgpu::TextureAspect::All;

  wgpu::TexelCopyBufferInfo copy_dst;
  copy_dst.buffer = readback_buffer;
  copy_dst.layout.offset = 0;
  copy_dst.layout.bytesPerRow = padded_bytes_per_row;
  copy_dst.layout.rowsPerImage = height;

  wgpu::Extent3D copy_size = {width, height, 1};
  encoder.CopyTextureToBuffer(&copy_src, &copy_dst, &copy_size);

  wgpu::CommandBuffer commands = encoder.Finish();
  queue.Submit(1, &commands);

  bool map_done = false;
  wgpu::MapAsyncStatus map_status = wgpu::MapAsyncStatus::Error;
  readback_buffer.MapAsync(
      wgpu::MapMode::Read, 0, buffer_size, wgpu::CallbackMode::AllowSpontaneous,
      [&map_done, &map_status](wgpu::MapAsyncStatus status, wgpu::StringView) {
        map_status = status;
        map_done = true;
      });
  while (!map_done) SDL_Delay(5);

  if (map_status != wgpu::MapAsyncStatus::Success) {
    spdlog::error("RenderScreenshot: buffer map failed (status={})",
                  static_cast<int>(map_status));
    return false;
  }

  const auto* mapped = static_cast<const uint8_t*>(
      readback_buffer.GetConstMappedRange(0, buffer_size));
  if (!mapped) {
    spdlog::error("RenderScreenshot: GetConstMappedRange returned null");
    readback_buffer.Unmap();
    return false;
  }

  std::vector<uint8_t> packed(static_cast<size_t>(unpadded_bytes_per_row) *
                              height);
  for (uint32_t y = 0; y < height; ++y) {
    std::memcpy(packed.data() + static_cast<size_t>(y) * unpadded_bytes_per_row,
               mapped + static_cast<size_t>(y) * padded_bytes_per_row,
               unpadded_bytes_per_row);
  }
  readback_buffer.Unmap();

  badlands_write_png(path.c_str(), packed.data(), width, height);
  spdlog::info("RenderScreenshot: wrote {} ({}x{})", path, width, height);
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  std::string screenshot_path;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc) {
      screenshot_path = argv[++i];
    }
  }
  const bool screenshot_mode = !screenshot_path.empty();

  if (!SDL_Init(SDL_INIT_VIDEO)) return 1;
  // In screenshot mode we render into an offscreen texture, not the SDL
  // surface, but a window is still needed: GpuContext::Initialize requires
  // one to create the WebGPU surface an adapter is requested as compatible
  // with.
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
  spdlog::info("main: window surface format = {}",
               static_cast<int>(gpu.GetSurfaceFormat()));

  int width = 0, height = 0;
  SDL_GetWindowSizeInPixels(window, &width, &height);

  badlands::GpuPipelineGenerator pipeline_gen(gpu.GetDevice(),
                                              badlands::FindShaderDirectory());

  // Sphere albedo (E1 loader): JPEG -> RGBA8 Dawn texture with a GPU-
  // generated mip chain. Kept alive for the whole render loop -- the bind
  // group ref-keeps the view+texture, but the owning handle stays in scope
  // regardless.
  badlands::LoadedTexture albedo = badlands::LoadTexture2D(
      gpu.GetDevice(), gpu.GetQueue(), pipeline_gen,
      "assets/materials/rocky_trail_1k.gltf/textures/rocky_trail_diff_1k.jpg");
  if (!albedo.texture) {
    spdlog::error("main: failed to load sphere albedo texture");
    gpu.Shutdown();
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 1;
  }
  spdlog::info("main: sphere albedo mip_level_count={}",
               albedo.texture.GetMipLevelCount());  // expect 11

  // Trilinear + anisotropic sampler: the material factory's default sampler
  // uses mipmapFilter=Nearest, which would defeat the GPU mip chain above.
  wgpu::SamplerDescriptor samp_desc = {};
  samp_desc.minFilter = wgpu::FilterMode::Linear;
  samp_desc.magFilter = wgpu::FilterMode::Linear;
  samp_desc.mipmapFilter = wgpu::MipmapFilterMode::Linear;  // trilinear
  samp_desc.addressModeU = wgpu::AddressMode::Repeat;  // UV sphere wraps in u
  samp_desc.addressModeV = wgpu::AddressMode::Repeat;
  samp_desc.maxAnisotropy = 16;  // grazing-angle sharpness
  wgpu::Sampler albedo_sampler = gpu.GetDevice().CreateSampler(&samp_desc);

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

  badlands::InstanceParams sphere_params;
  sphere_params.texture_overrides.push_back(badlands::DefaultTextureView{
      .param_name = "mesh_texture",
      .view = albedo.view,
      .sampler = albedo_sampler,
      .type = badlands::TextureType::k2D,
  });

  badlands::SceneGraph scene;
  build_test_scene(scene, textured_mesh_factory.get(), sphere_params);

  entt::registry registry;
  badlands::SceneContext scene_context;

  badlands::Camera camera;
  camera.position = glm::vec3(0.0f, 1.5f, 4.0f);
  camera.LookAt(glm::vec3(0.0f));

  if (screenshot_mode) {
    // Modest fixed size — this mode exists for visual verification, not to
    // match the (possibly HiDPI) window's pixel size.
    constexpr uint32_t kScreenshotWidth = 800;
    constexpr uint32_t kScreenshotHeight = 600;
    camera.aspect = static_cast<float>(kScreenshotWidth) /
                    static_cast<float>(kScreenshotHeight);

    bool ok = RenderScreenshot(gpu, pipeline_gen, scene, registry,
                               scene_context, camera, kScreenshotWidth,
                               kScreenshotHeight, screenshot_path);

    gpu.Shutdown();
    SDL_DestroyWindow(window);
    SDL_Quit();
    return ok ? 0 : 1;
  }

  camera.aspect =
      height > 0 ? static_cast<float>(width) / static_cast<float>(height) : 1.0f;

  badlands::SceneRenderer renderer;
  renderer.Initialize(gpu.GetDevice(), gpu.GetQueue(), &pipeline_gen,
                      gpu.GetSurfaceFormat(), static_cast<uint32_t>(width),
                      static_cast<uint32_t>(height));

  bool render_ok_logged = false;

  bool running = true;
  while (running) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      if (e.type == SDL_EVENT_QUIT) running = false;
      if (e.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
        int w = 0, h = 0;
        SDL_GetWindowSizeInPixels(window, &w, &h);
        if (w > 0 && h > 0) {
          gpu.Configure(static_cast<uint32_t>(w), static_cast<uint32_t>(h));
          renderer.Resize(static_cast<uint32_t>(w), static_cast<uint32_t>(h));
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
