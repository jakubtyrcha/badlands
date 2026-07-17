// GPU cross-check: render the water material's G-buffer variant and read the
// shader-computed surface normal back from the G-buffer normals target
// (RG16Float, octahedron-encoded), decode it CPU-side, and assert it matches
// src/engine/rendering/water_waves.hpp::WaveNormal. This guards that
// shaders/common/water_waves.wesl actually implements the same wave math as the
// C++ core (which badlands_water_tests unit-tests).
//
// Trick: an ODD-sized (65x65) target puts NDC (0,0) exactly at the centre
// pixel's centre, so with the camera at (x0, H, z0) looking straight down, the
// centre pixel's ray hits world XZ exactly (x0, z0) — no half-pixel offset — so
// the expected normal is WaveNormal((x0,z0), t) with no reconstruction.

#include <catch_amalgamated.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cstring>
#include <vector>

#include "core/util/cpu_image.hpp"
#include "engine/core/camera.hpp"
#include "engine/rendering/context/frame_context.hpp"
#include "engine/rendering/context/render_pass_context.hpp"
#include "engine/rendering/gbuffer.hpp"
#include "engine/rendering/geometry/textured_mesh_builders.hpp"
#include "engine/rendering/material/material_instance_factory.hpp"
#include "engine/rendering/octahedron.hpp"
#include "engine/rendering/shader/gpu_pipeline_generator.hpp"
#include "engine/rendering/texture_readback.hpp"
#include "engine/rendering/util/find_shader_directory.hpp"
#include "engine/rendering/water_material.hpp"
#include "engine/rendering/water_waves.hpp"
#include "engine/tests/gpu_test_helpers.hpp"

using namespace badlands;

namespace {

constexpr uint32_t kSize = 65;  // odd -> centre pixel == NDC (0,0)
constexpr uint32_t kCenter = kSize / 2;

struct TestGpu {
  wgpu::Instance instance;
  wgpu::Device device;
  wgpu::Queue queue;
  std::unique_ptr<GpuPipelineGenerator> pipeline_gen;
  std::unique_ptr<MaterialInstanceFactory> water_gbuffer;
};

TestGpu& GetTestGpu() {
  static TestGpu* g = [] {
    auto* t = new TestGpu();
    wgpu::InstanceDescriptor idesc = {};
    t->instance = wgpu::CreateInstance(&idesc);
    REQUIRE(t->instance);
    wgpu::Adapter adapter = test::RequestAdapter(t->instance);
    REQUIRE(adapter);
    t->device = test::RequestDevice(adapter);
    REQUIRE(t->device);
    t->queue = t->device.GetQueue();
    t->pipeline_gen =
        std::make_unique<GpuPipelineGenerator>(t->device, FindShaderDirectory());
    t->water_gbuffer =
        BuildWaterGBufferFactory(t->device, t->queue, t->pipeline_gen.get());
    REQUIRE(t->water_gbuffer != nullptr);
    return t;
  }();
  return *g;
}

float HalfToFloat(uint16_t h) {
  uint32_t sign = static_cast<uint32_t>(h & 0x8000u) << 16;
  uint32_t exp = (h >> 10) & 0x1Fu;
  uint32_t mant = h & 0x3FFu;
  uint32_t f;
  if (exp == 0u) {
    if (mant == 0u) {
      f = sign;
    } else {
      exp = 127u - 15u + 1u;
      while ((mant & 0x400u) == 0u) {
        mant <<= 1;
        exp--;
      }
      mant &= 0x3FFu;
      f = sign | (exp << 23) | (mant << 13);
    }
  } else if (exp == 0x1Fu) {
    f = sign | 0x7F800000u | (mant << 13);
  } else {
    f = sign | ((exp - 15u + 127u) << 23) | (mant << 13);
  }
  float out;
  std::memcpy(&out, &f, sizeof(out));
  return out;
}

// Render the water G-buffer variant with the camera above (x0, z0) looking
// straight down at time t; return the decoded centre-pixel world normal.
glm::vec3 RenderWaterNormalAt(float x0, float z0, float t) {
  TestGpu& g = GetTestGpu();
  wgpu::Device device = g.device;
  wgpu::Queue queue = g.queue;

  // Large flat water quad in world XZ (waves displace Y in the vertex shader;
  // the G-buffer normal is recomputed per-fragment from interpolated world XZ).
  static const TexturedMeshResult mesh = GenerateHeightmapMesh(
      800.0f, 1, [](float, float) { return 0.0f; });
  wgpu::BufferDescriptor vb_desc;
  vb_desc.size = mesh.mesh.vertices.size() * sizeof(float);
  vb_desc.usage = wgpu::BufferUsage::Vertex | wgpu::BufferUsage::CopyDst;
  vb_desc.mappedAtCreation = true;
  wgpu::Buffer vbuf = device.CreateBuffer(&vb_desc);
  std::memcpy(vbuf.GetMappedRange(0, vb_desc.size), mesh.mesh.vertices.data(),
              vb_desc.size);
  vbuf.Unmap();

  // G-buffer targets (normals read back; albedo/material required by the MRT).
  auto make_target = [&](wgpu::TextureFormat fmt, wgpu::TextureUsage extra) {
    wgpu::TextureDescriptor d;
    d.size = {kSize, kSize, 1};
    d.format = fmt;
    d.usage = wgpu::TextureUsage::RenderAttachment | extra;
    return device.CreateTexture(&d);
  };
  wgpu::Texture normals =
      make_target(GBuffer::kNormalsFormat, wgpu::TextureUsage::CopySrc);
  wgpu::Texture albedo = make_target(GBuffer::kAlbedoFormat, wgpu::TextureUsage::None);
  wgpu::Texture material =
      make_target(GBuffer::kMaterialFormat, wgpu::TextureUsage::None);
  wgpu::Texture depth =
      make_target(GBuffer::kDepthFormat, wgpu::TextureUsage::None);

  // Frame uniforms: camera at (x0, H, z0) looking straight down.
  Camera camera;
  camera.position = {x0, 50.0f, z0};
  camera.direction = {0.0f, -1.0f, 0.0f};
  camera.up = {0.0f, 0.0f, -1.0f};
  camera.fov = 45.0f;
  camera.aspect = 1.0f;
  camera.near_plane = 0.1f;
  camera.far_plane = 1000.0f;

  UniformData u{};
  u.view = glm::lookAt(glm::vec3(0.0f), camera.direction, camera.up);
  u.proj = camera.GetProj();
  u.view_prev = u.view;
  u.proj_prev = u.proj;
  u.camera_world_pos = glm::vec4(camera.position, 0.0f);
  u.sunDir = glm::vec4(0.0f, 1.0f, 0.0f, 0.0f);
  u.sunColor = glm::vec4(1.0f);
  u.near_plane = camera.near_plane;
  u.far_plane = camera.far_plane;
  u.screen_size = glm::vec2(kSize, kSize);
  u.output_is_linear = 1u;

  auto instance = g.water_gbuffer->CreateInstance(
      GeometryType::kTexturedMesh, MaterialPassType::kDeferred,
      RenderPassType::kGBuffer, DefaultWaterParams());
  REQUIRE(instance != nullptr);
  REQUIRE(instance->IsValid());

  glm::mat4 model = glm::translate(glm::mat4(1.0f), -camera.position);
  instance->SetParameterByName("modelMatrix", MaterialParameterValue(model));
  instance->SetParameterByName("time", MaterialParameterValue(t));

  FrameContext frame;
  frame.Begin(device, queue, u);
  {
    std::array<wgpu::RenderPassColorAttachment, 3> color{};
    wgpu::Texture texs[3] = {normals, albedo, material};
    for (int i = 0; i < 3; ++i) {
      color[i].view = texs[i].CreateView();
      color[i].loadOp = wgpu::LoadOp::Clear;
      color[i].storeOp = wgpu::StoreOp::Store;
      color[i].clearValue = {0, 0, 0, 0};
      color[i].depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
    }
    wgpu::RenderPassDepthStencilAttachment depth_att{};
    depth_att.view = depth.CreateView();
    depth_att.depthLoadOp = wgpu::LoadOp::Clear;
    depth_att.depthStoreOp = wgpu::StoreOp::Store;
    depth_att.depthClearValue = 0.0f;  // reversed-Z far

    wgpu::RenderPassDescriptor desc;
    desc.colorAttachmentCount = 3;
    desc.colorAttachments = color.data();
    desc.depthStencilAttachment = &depth_att;

    RenderPassContext pass = frame.BeginRenderPass(desc);
    REQUIRE(instance->Bind(pass, frame));
    REQUIRE(instance->BindPerObject(pass, frame));
    pass.SetVertexBuffer(0, vbuf);
    pass.Draw(mesh.mesh.vertex_count);
    pass.End();
  }
  wgpu::CommandBuffer commands = frame.End();
  queue.Submit(1, &commands);

  // Copy the normals texture to a mappable buffer (RG16Float = 4 bytes/pixel).
  const uint32_t bytes_per_pixel = 4;
  uint32_t bpr = ((kSize * bytes_per_pixel + 255) / 256) * 256;
  wgpu::BufferDescriptor rb;
  rb.size = static_cast<uint64_t>(bpr) * kSize;
  rb.usage = wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst;
  wgpu::Buffer readback = device.CreateBuffer(&rb);
  {
    wgpu::CommandEncoder enc = device.CreateCommandEncoder();
    wgpu::TexelCopyTextureInfo src{};
    src.texture = normals;
    wgpu::TexelCopyBufferInfo dst{};
    dst.buffer = readback;
    dst.layout.bytesPerRow = bpr;
    dst.layout.rowsPerImage = kSize;
    wgpu::Extent3D ext = {kSize, kSize, 1};
    enc.CopyTextureToBuffer(&src, &dst, &ext);
    wgpu::CommandBuffer c = enc.Finish();
    queue.Submit(1, &c);
  }
  test::WaitForGpu(g.instance, device, queue);

  bool mapped = false;
  readback.MapAsync(wgpu::MapMode::Read, 0, rb.size,
                    wgpu::CallbackMode::AllowProcessEvents,
                    [&](wgpu::MapAsyncStatus, wgpu::StringView) { mapped = true; });
  while (!mapped) {
    g.instance.ProcessEvents();
    device.Tick();
  }
  const auto* base = static_cast<const uint8_t*>(
      readback.GetConstMappedRange(0, rb.size));
  const uint16_t* row =
      reinterpret_cast<const uint16_t*>(base + static_cast<size_t>(kCenter) * bpr);
  glm::vec2 oct(HalfToFloat(row[kCenter * 2 + 0]), HalfToFloat(row[kCenter * 2 + 1]));
  readback.Unmap();
  return DecodeOctahedron(oct);
}

}  // namespace

TEST_CASE("water GPU wave normal matches the CPU core", "[water][gpu]") {
  struct Sample {
    float x, z, t;
  };
  const std::array<Sample, 4> samples = {{
      {0.0f, 0.0f, 0.0f},
      {5.3f, -2.1f, 0.0f},
      {12.0f, 7.5f, 1.25f},
      {-8.0f, 3.0f, 4.0f},
  }};
  for (const auto& s : samples) {
    glm::vec3 gpu = RenderWaterNormalAt(s.x, s.z, s.t);
    glm::vec3 cpu = water::WaveNormal(glm::vec2(s.x, s.z), s.t, water::DefaultWaves());
    CAPTURE(s.x, s.z, s.t, gpu.x, gpu.y, gpu.z, cpu.x, cpu.y, cpu.z);
    CHECK(glm::distance(gpu, cpu) < 0.02f);
  }
}

// === Refraction foreground-rejection (red/green for the depth guard) ========
namespace {

// A flat quad in world XZ at height `y` spanning [x0,x1] x [z0,z1], wound CCW
// as seen from +Y (so it is not back-face culled by a top-down camera).
std::vector<float> BandQuad(float x0, float x1, float z0, float z1, float y) {
  const glm::vec3 n(0, 1, 0), t(1, 0, 0);
  const glm::vec3 a(x0, y, z0), b(x1, y, z0), c(x1, y, z1), d(x0, y, z1);
  std::vector<float> v;
  auto push = [&](glm::vec3 p) {
    v.insert(v.end(), {p.x, p.y, p.z, 0.0f, 0.0f, n.x, n.y, n.z, t.x, t.y, t.z});
  };
  push(a); push(c); push(b);
  push(a); push(d); push(c);
  return v;
}

wgpu::Buffer UploadVerts(wgpu::Device device, const std::vector<float>& v) {
  wgpu::BufferDescriptor bd;
  bd.size = v.size() * sizeof(float);
  bd.usage = wgpu::BufferUsage::Vertex | wgpu::BufferUsage::CopyDst;
  bd.mappedAtCreation = true;
  wgpu::Buffer b = device.CreateBuffer(&bd);
  std::memcpy(b.GetMappedRange(0, bd.size), v.data(), bd.size);
  b.Unmap();
  return b;
}

wgpu::Texture MakeTex(wgpu::Device d, uint32_t w, uint32_t h,
                      wgpu::TextureFormat fmt, wgpu::TextureUsage usage) {
  wgpu::TextureDescriptor td;
  td.size = {w, h, 1};
  td.format = fmt;
  td.usage = usage;
  return d.CreateTexture(&td);
}

// Nearest sampler: no bilinear fringe at the hard colour boundary, so the only
// way green reaches the water is a refraction offset landing on the occluder —
// which is exactly what the depth guard rejects. Isolates the guard's effect.
wgpu::Sampler NearestClamp(wgpu::Device d) {
  wgpu::SamplerDescriptor s;
  s.magFilter = wgpu::FilterMode::Nearest;
  s.minFilter = wgpu::FilterMode::Nearest;
  s.addressModeU = wgpu::AddressMode::ClampToEdge;
  s.addressModeV = wgpu::AddressMode::ClampToEdge;
  s.addressModeW = wgpu::AddressMode::ClampToEdge;
  return d.CreateSampler(&s);
}

}  // namespace

TEST_CASE("water refraction rejects foreground bleed", "[water][gpu][refraction]") {
  TestGpu& g = GetTestGpu();
  wgpu::Device device = g.device;
  wgpu::Queue queue = g.queue;
  constexpr uint32_t W = 64;

  // Water forward material targeting BGRA8 (so CpuImage can read it back).
  FactoryDescriptor wd;
  wd.shader_name = "water";
  wd.shader_path = "material/water.wesl";
  wd.supported_pass_types = {MaterialPassType::kForwardTransparent};
  wd.color_formats = {wgpu::TextureFormat::BGRA8Unorm};
  wd.depth_format = wgpu::TextureFormat::Depth32Float;
  wd.depth_write = false;
  auto water = BuildMaterialInstanceFactory(wd, device, queue, g.pipeline_gen.get());
  REQUIRE(water != nullptr);

  // Green occluder material (writes depth), for the foreground band.
  FactoryDescriptor od;
  od.shader_name = "forward_debug";
  od.shader_path = "tests/forward_debug.wesl";
  od.supported_pass_types = {MaterialPassType::kForwardOpaque};
  od.color_formats = {wgpu::TextureFormat::BGRA8Unorm};
  od.depth_format = wgpu::TextureFormat::Depth32Float;
  od.depth_write = true;
  auto occ = BuildMaterialInstanceFactory(od, device, queue, g.pipeline_gen.get());
  REQUIRE(occ != nullptr);

  // Targets: the scene colour + depth the water samples, and the water output.
  wgpu::Texture sceneColor = MakeTex(
      device, W, W, wgpu::TextureFormat::BGRA8Unorm,
      wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding);
  wgpu::Texture sceneDepth = MakeTex(
      device, W, W, wgpu::TextureFormat::Depth32Float,
      wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding);
  wgpu::Texture waterOut = MakeTex(
      device, W, W, wgpu::TextureFormat::BGRA8Unorm,
      wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::CopySrc);

  // 1x1 black IBL cube + 1x1 BRDF LUT so the water's reflection bindings are
  // valid but contribute ~nothing (isolates the refraction/bg term).
  wgpu::Texture cube;
  {
    wgpu::TextureDescriptor cd;
    cd.size = {1, 1, 6};  // cube = 6 array layers
    cd.format = wgpu::TextureFormat::RGBA16Float;
    cd.usage =
        wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;
    cube = device.CreateTexture(&cd);
  }
  {
    std::array<uint16_t, 6 * 4> zeros{};
    wgpu::TexelCopyTextureInfo dst;
    dst.texture = cube;
    wgpu::TexelCopyBufferLayout layout;
    layout.bytesPerRow = 4 * sizeof(uint16_t);
    layout.rowsPerImage = 1;
    wgpu::Extent3D ext = {1, 1, 6};
    queue.WriteTexture(&dst, zeros.data(), zeros.size() * sizeof(uint16_t),
                       &layout, &ext);
  }
  wgpu::TextureViewDescriptor cvd;
  cvd.dimension = wgpu::TextureViewDimension::Cube;
  cvd.arrayLayerCount = 6;
  wgpu::TextureView cubeView = cube.CreateView(&cvd);
  wgpu::Texture brdf = MakeTex(device, 1, 1, wgpu::TextureFormat::RG16Float,
                               wgpu::TextureUsage::TextureBinding |
                                   wgpu::TextureUsage::CopyDst);
  {
    std::array<uint16_t, 2> lut = {0x3C00, 0};  // (1.0, 0.0) in half
    wgpu::TexelCopyTextureInfo dst;
    dst.texture = brdf;
    wgpu::TexelCopyBufferLayout layout;
    layout.bytesPerRow = 2 * sizeof(uint16_t);
    layout.rowsPerImage = 1;
    wgpu::Extent3D ext = {1, 1, 1};
    queue.WriteTexture(&dst, lut.data(), lut.size() * sizeof(uint16_t), &layout,
                       &ext);
  }
  wgpu::Sampler nearest = NearestClamp(device);

  // Camera looking straight down; no sun (isolate refraction).
  Camera cam;
  cam.position = {0.0f, 10.0f, 0.0f};
  cam.direction = {0.0f, -1.0f, 0.0f};
  cam.up = {0.0f, 0.0f, -1.0f};
  cam.fov = 60.0f;
  cam.aspect = 1.0f;
  cam.near_plane = 0.1f;
  cam.far_plane = 1000.0f;
  UniformData u{};
  u.view = glm::lookAt(glm::vec3(0.0f), cam.direction, cam.up);
  u.proj = cam.GetProj();
  u.camera_world_pos = glm::vec4(cam.position, 0.0f);
  u.sunDir = glm::vec4(0.0f, 1.0f, 0.0f, 0.0f);
  u.sunColor = glm::vec4(0.0f);  // no sun glints
  u.near_plane = cam.near_plane;
  u.far_plane = cam.far_plane;
  u.screen_size = glm::vec2(W, W);
  u.output_is_linear = 1u;

  const glm::mat4 model = glm::translate(glm::mat4(1.0f), -cam.position);

  // Occluder band: green, at y=5 (closer than the water at y=0), covering world
  // z in [0.5, 40]. At that depth the camera's window is ~+/-2.9 m, so the band
  // fills the lower part of the frame with its top edge (z=0.5) visible — water
  // above the edge can refract-offset down into it.
  wgpu::Buffer occVerts = UploadVerts(device, BandQuad(-40, 40, 0.5f, 40, 5.0f));
  auto occInst = occ->CreateInstance(
      GeometryType::kTexturedMesh, MaterialPassType::kForwardOpaque,
      RenderPassType::kForward,
      InstanceParams{.uniform_overrides = {{"color", glm::vec4(0, 1, 0, 1)}}});
  REQUIRE(occInst != nullptr);
  occInst->SetParameterByName("modelMatrix", MaterialParameterValue(model));

  // Water: flat, full-frame; absorption 0 (so refracted == background), strong
  // refraction offset, detail on. Group-2 engine resources supply scene colour
  // + depth + (black) IBL.
  static const TexturedMeshResult waterMesh =
      GenerateHeightmapMesh(200.0f, 1, [](float, float) { return 0.0f; });
  wgpu::Buffer waterVerts = UploadVerts(device, waterMesh.mesh.vertices);
  InstanceParams wp;
  wp.uniform_overrides = {
      {"deepColor", glm::vec4(0.0f, 0.0f, 1.0f, 1.0f)},
      {"shallowColor", glm::vec4(0.0f, 0.0f, 1.0f, 1.0f)},
      {"params", glm::vec4(0.0f, 0.4f, 0.5f, 0.9f)},  // absorb=0, refract=0.4
      {"params2", glm::vec4(0.6f, 0.0f, 0.0f, 0.0f)},
      {"time", 3.0f},
  };
  auto waterInst = water->CreateInstance(GeometryType::kTexturedMesh,
                                         MaterialPassType::kForwardTransparent,
                                         RenderPassType::kForward, wp);
  REQUIRE(waterInst != nullptr);
  waterInst->SetParameterByName("modelMatrix", MaterialParameterValue(model));

  FrameContext frame;
  frame.Begin(device, queue, u);
  // Pass A: draw the occluder into scene colour (blue clear) + scene depth.
  {
    wgpu::RenderPassColorAttachment ca{};
    ca.view = sceneColor.CreateView();
    ca.loadOp = wgpu::LoadOp::Clear;
    ca.storeOp = wgpu::StoreOp::Store;
    ca.clearValue = {0, 0, 1, 1};  // blue background
    ca.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
    wgpu::RenderPassDepthStencilAttachment da{};
    da.view = sceneDepth.CreateView();
    da.depthLoadOp = wgpu::LoadOp::Clear;
    da.depthStoreOp = wgpu::StoreOp::Store;
    da.depthClearValue = 0.0f;
    wgpu::RenderPassDescriptor desc;
    desc.colorAttachmentCount = 1;
    desc.colorAttachments = &ca;
    desc.depthStencilAttachment = &da;
    RenderPassContext pass = frame.BeginRenderPass(desc);
    REQUIRE(occInst->Bind(pass, frame));
    REQUIRE(occInst->BindPerObject(pass, frame));
    pass.SetVertexBuffer(0, occVerts);
    pass.Draw(6);
    pass.End();
  }
  // Pass B: water into waterOut, sampling scene colour/depth (depth read-only).
  {
    wgpu::RenderPassColorAttachment ca{};
    ca.view = waterOut.CreateView();
    ca.loadOp = wgpu::LoadOp::Clear;
    ca.storeOp = wgpu::StoreOp::Store;
    ca.clearValue = {0, 0, 0, 1};
    ca.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
    wgpu::RenderPassDepthStencilAttachment da{};
    da.view = sceneDepth.CreateView();
    da.depthLoadOp = wgpu::LoadOp::Undefined;
    da.depthStoreOp = wgpu::StoreOp::Undefined;
    da.depthReadOnly = true;
    wgpu::RenderPassDescriptor desc;
    desc.colorAttachmentCount = 1;
    desc.colorAttachments = &ca;
    desc.depthStencilAttachment = &da;
    RenderPassContext pass = frame.BeginRenderPass(desc);
    REQUIRE(waterInst->Bind(pass, frame));
    REQUIRE(waterInst->BindPerObject(pass, frame));
    std::array<wgpu::BindGroupEntry, 7> e{};
    e[0].binding = 0;
    e[0].textureView = sceneDepth.CreateView();
    e[1].binding = 1;
    e[1].textureView = sceneColor.CreateView();
    e[2].binding = 2;
    e[2].sampler = nearest;
    e[3].binding = 3;
    e[3].textureView = cubeView;
    e[4].binding = 4;
    e[4].sampler = nearest;
    e[5].binding = 5;
    e[5].textureView = brdf.CreateView();
    e[6].binding = 6;
    e[6].sampler = nearest;
    pass.SetBindGroup(2, frame.CreateBindGroup(
                             waterInst->GetPipeline().GetBindGroupLayout(2), e));
    pass.SetVertexBuffer(0, waterVerts);
    pass.Draw(waterMesh.mesh.vertex_count);
    pass.End();
  }
  wgpu::CommandBuffer cmd = frame.End();
  queue.Submit(1, &cmd);

  TextureReadback rb(g.instance, device, queue);
  CpuImage img = rb.ReadTextureSync(waterOut, W, W, wgpu::TextureFormat::BGRA8Unorm);
  int max_green = 0;
  for (uint32_t y = 0; y < W; ++y) {
    for (uint32_t x = 0; x < W; ++x) {
      max_green = std::max<int>(max_green, img.GetPixel(x, y).g);
    }
  }
  // Water samples blue background or (rejected) foreground -> never the green
  // occluder. Without the depth guard, the band bleeds and max green spikes.
  INFO("max green in water output = " << max_green);
  CHECK(max_green < 60);
}
