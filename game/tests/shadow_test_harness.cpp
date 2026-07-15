#include "shadow_test_harness.hpp"

#include <cstring>
#include <memory>
#include <string>

#include <SDL3/SDL.h>
#include <dawn/webgpu_cpp.h>
#include <entt/entt.hpp>
#include <glm/gtc/packing.hpp>
#include <spdlog/spdlog.h>

#include "engine/rendering/context/scene_context.hpp"
#include "engine/rendering/geometry/primitive_mesh_builders.hpp"
#include "engine/rendering/geometry/textured_mesh_builders.hpp"
#include "engine/rendering/gpu_context.hpp"
#include "engine/rendering/material_library.hpp"
#include "engine/rendering/scene_build.hpp"
#include "engine/rendering/shader/gpu_pipeline_generator.hpp"
#include "engine/rendering/util/find_shader_directory.hpp"
#include "engine/scene/scene_graph.hpp"

namespace badlands::shadowtest {

namespace {

// 256-byte row alignment WebGPU requires for CopyTextureToBuffer
// destinations (WGPU_COPY_BYTES_PER_ROW_ALIGNMENT) -- same constraint
// screenshot.cpp's WriteTextureToPng works around.
constexpr uint32_t kCopyBytesPerRowAlignment = 256;

uint32_t AlignUp(uint32_t value, uint32_t alignment) {
  return ((value + alignment - 1) / alignment) * alignment;
}

// Process-lifetime headless GPU context: a hidden SDL window (GpuContext
// needs a real SDL_Window to create a WebGPU surface/adapter, even though
// this harness never presents to it -- see gpu_context.hpp), the shared
// pipeline generator, and a MaterialLibrary (for the flat-gray debug
// material). Deliberately leaked (never destroyed): this is a test binary
// that exits shortly after use, and avoids any static-destruction-order
// pitfalls with Dawn's ref-counted wgpu:: handles.
struct TestGpu {
  SDL_Window* window = nullptr;
  GpuContext gpu;
  std::unique_ptr<GpuPipelineGenerator> pipeline_gen;
  MaterialLibrary matlib;
};

TestGpu& GetTestGpu() {
  static TestGpu* instance = [] {
    auto* g = new TestGpu();

    if (!SDL_Init(SDL_INIT_VIDEO)) {
      spdlog::error("shadow_test_harness: SDL_Init failed: {}", SDL_GetError());
      std::abort();
    }
    // Hidden: this harness renders offscreen only and never presents to the
    // window -- it exists purely so GpuContext::Initialize can create a
    // WebGPU surface/adapter (see gpu_context.hpp's Initialize doc comment).
    g->window = SDL_CreateWindow("badlands_shadow_test", 64, 64, SDL_WINDOW_HIDDEN);
    if (!g->window) {
      spdlog::error("shadow_test_harness: SDL_CreateWindow failed: {}", SDL_GetError());
      std::abort();
    }
    if (!g->gpu.Initialize(g->window)) {
      spdlog::error("shadow_test_harness: GpuContext::Initialize failed");
      std::abort();
    }
    g->pipeline_gen =
        std::make_unique<GpuPipelineGenerator>(g->gpu.GetDevice(), FindShaderDirectory());
    if (!g->matlib.Initialize(g->gpu.GetDevice(), g->gpu.GetQueue(), g->pipeline_gen.get())) {
      spdlog::error("shadow_test_harness: MaterialLibrary::Initialize failed");
      std::abort();
    }
    return g;
  }();
  return *instance;
}

// Reads an RGBA16Float `texture` (RenderAttachment|CopySrc, size
// width*height) back to CPU and decodes its R channel to linear float via
// glm::unpackHalf1x16 -- the shadow debug modes write vec4(v,v,v,1), so R
// alone carries the value Test 1/Test 2 need.
Image ReadRgba16FloatImage(wgpu::Device device, wgpu::Queue queue,
                           const wgpu::Texture& texture, uint32_t width,
                           uint32_t height) {
  constexpr uint32_t kBytesPerPixel = 8;  // RGBA16Float
  const uint32_t unpadded_bytes_per_row = width * kBytesPerPixel;
  const uint32_t padded_bytes_per_row =
      AlignUp(unpadded_bytes_per_row, kCopyBytesPerRowAlignment);
  const uint64_t buffer_size = static_cast<uint64_t>(padded_bytes_per_row) * height;

  wgpu::BufferDescriptor readback_desc;
  readback_desc.label = "shadow test readback";
  readback_desc.size = buffer_size;
  readback_desc.usage = wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst;
  wgpu::Buffer readback_buffer = device.CreateBuffer(&readback_desc);

  wgpu::CommandEncoder encoder = device.CreateCommandEncoder();

  wgpu::TexelCopyTextureInfo copy_src;
  copy_src.texture = texture;
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

  Image img;
  img.width = width;
  img.height = height;
  img.pixels.assign(static_cast<size_t>(width) * height, 0.0f);

  if (map_status != wgpu::MapAsyncStatus::Success) {
    spdlog::error("ReadRgba16FloatImage: buffer map failed (status={})",
                  static_cast<int>(map_status));
    return img;
  }

  const auto* mapped =
      static_cast<const uint8_t*>(readback_buffer.GetConstMappedRange(0, buffer_size));
  for (uint32_t y = 0; y < height; ++y) {
    const uint8_t* row = mapped + static_cast<size_t>(y) * padded_bytes_per_row;
    for (uint32_t x = 0; x < width; ++x) {
      uint16_t r_raw = 0;
      std::memcpy(&r_raw, row + static_cast<size_t>(x) * kBytesPerPixel, sizeof(uint16_t));
      img.pixels[static_cast<size_t>(y) * width + x] = glm::unpackHalf1x16(r_raw);
    }
  }
  readback_buffer.Unmap();
  return img;
}

}  // namespace

Image RenderShadowFrame(const ShadowTestConfig& config, const Scene& world_scene,
                        const Camera& camera) {
  TestGpu& test_gpu = GetTestGpu();
  wgpu::Device device = test_gpu.gpu.GetDevice();
  wgpu::Queue queue = test_gpu.gpu.GetQueue();

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

  // Offscreen RGBA16Float target: keeps the shadow debug modes' vec4(v,v,v,1)
  // output LINEAR all the way to readback -- SceneRenderer's tonemap pass
  // only applies sRGB encode when output_is_linear==0 (surface_format !=
  // RGBA16Float), see scene_renderer.cpp / shaders/passes/tonemapping.wesl.
  wgpu::TextureDescriptor offscreen_desc;
  offscreen_desc.size = {kFrameWidth, kFrameHeight, 1};
  offscreen_desc.format = wgpu::TextureFormat::RGBA16Float;
  offscreen_desc.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::CopySrc;
  offscreen_desc.mipLevelCount = 1;
  offscreen_desc.sampleCount = 1;
  offscreen_desc.dimension = wgpu::TextureDimension::e2D;
  wgpu::Texture offscreen_texture = device.CreateTexture(&offscreen_desc);
  wgpu::TextureView offscreen_view = offscreen_texture.CreateView();

  SceneRenderer renderer;
  renderer.Initialize(device, queue, test_gpu.pipeline_gen.get(),
                      wgpu::TextureFormat::RGBA16Float, kFrameWidth, kFrameHeight,
                      test_gpu.gpu.HasR8UnormStorage());

  ShadowConfig shadow_config;
  shadow_config.coverage_dmax = config.d_max;
  shadow_config.resolution = config.r_sm;
  shadow_config.backward_extension = 100.0f;
  shadow_config.enable_shadow_map = config.enable_shadow_map;
  shadow_config.enable_contact_shadows = config.enable_contact_shadows;
  renderer.SetShadowConfig(shadow_config);
  renderer.SetShadowDebugMode(config.mode);

  renderer.Render(camera, registry, scene_context, offscreen_view);

  return ReadRgba16FloatImage(device, queue, offscreen_texture, kFrameWidth, kFrameHeight);
}

}  // namespace badlands::shadowtest
