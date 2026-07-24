// Forward-pass render tests (adapted from sampo's scene_graph_rendering_tests
// "Forward material binding test"). Exercises the forward material variants
// through the real factory path (BuildMaterialInstanceFactory -> CreateInstance
// -> Bind/BindPerObject -> draw):
//   - forward OPAQUE: a solid colour renders as that colour.
//   - forward TRANSPARENT: premultiplied-alpha blend over a known background,
//     rendered against a DEPTH-READ-ONLY attachment. This only validates if the
//     transparent material's pipeline does NOT write depth — i.e. it exercises
//     depth_write as a per-material property (FactoryDescriptor::depth_write),
//     which a depth-writing pipeline against a read-only attachment would reject.
//
// Adapted to badlands' GetTestGpu() harness (no RenderStateProvider — badlands
// carries blend/depth-write on the material variant directly).

#include <catch_amalgamated.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cstring>

#include "core/util/cpu_image.hpp"
#include "engine/core/camera.hpp"
#include "engine/rendering/components/forward_component.hpp"
#include "engine/rendering/components/material_factory_component.hpp"
#include "engine/rendering/components/mesh_components.hpp"
#include "engine/rendering/context/frame_context.hpp"
#include "engine/rendering/context/render_pass_context.hpp"
#include "engine/rendering/geometry/textured_mesh_builders.hpp"
#include "engine/rendering/material/material_instance_cache.hpp"
#include "engine/rendering/material/material_instance_factory.hpp"
#include "engine/rendering/passes/render_forward.hpp"
#include "engine/rendering/shader/gpu_pipeline_generator.hpp"
#include "engine/rendering/texture_readback.hpp"
#include "engine/rendering/util/find_shader_directory.hpp"
#include "engine/tests/gpu_test_helpers.hpp"

using namespace badlands;

namespace {

constexpr uint32_t kSize = 32;
constexpr wgpu::TextureFormat kColorFormat = wgpu::TextureFormat::BGRA8Unorm;
constexpr wgpu::TextureFormat kDepthFormat = wgpu::TextureFormat::Depth32Float;

struct TestGpu {
  wgpu::Instance instance;
  wgpu::Device device;
  wgpu::Queue queue;
  std::unique_ptr<GpuPipelineGenerator> gen;
  std::unique_ptr<MaterialInstanceFactory> opaque;       // depth_write = true
  std::unique_ptr<MaterialInstanceFactory> transparent;  // depth_write = false
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
    t->gen = std::make_unique<GpuPipelineGenerator>(t->device,
                                                    FindShaderDirectory());

    FactoryDescriptor base;
    base.shader_name = "forward_debug";
    base.shader_path = "tests/forward_debug.wesl";
    base.color_formats = {kColorFormat};
    base.depth_format = kDepthFormat;

    FactoryDescriptor od = base;
    od.supported_pass_types = {MaterialPassType::kForwardOpaque};
    od.depth_write = true;
    t->opaque = BuildMaterialInstanceFactory(od, t->device, t->queue, t->gen.get());
    REQUIRE(t->opaque != nullptr);

    FactoryDescriptor td = base;
    td.supported_pass_types = {MaterialPassType::kForwardTransparent};
    td.depth_write = false;  // tests but does not write depth
    t->transparent =
        BuildMaterialInstanceFactory(td, t->device, t->queue, t->gen.get());
    REQUIRE(t->transparent != nullptr);
    return t;
  }();
  return *g;
}

// A flat quad in the XZ plane (y=0), large enough to fill a top-down view.
const TexturedMeshResult& Quad() {
  static const TexturedMeshResult mesh =
      GenerateHeightmapMesh(40.0f, 1, [](float, float) { return 0.0f; });
  return mesh;
}

wgpu::Buffer UploadQuad(wgpu::Device device) {
  const auto& mesh = Quad();
  wgpu::BufferDescriptor bd;
  bd.size = mesh.mesh.vertices.size() * sizeof(float);
  bd.usage = wgpu::BufferUsage::Vertex | wgpu::BufferUsage::CopyDst;
  bd.mappedAtCreation = true;
  wgpu::Buffer b = device.CreateBuffer(&bd);
  std::memcpy(b.GetMappedRange(0, bd.size), mesh.mesh.vertices.data(), bd.size);
  b.Unmap();
  return b;
}

UniformData TopDownUniforms() {
  Camera cam;
  cam.position = {0.0f, 10.0f, 0.0f};
  cam.direction = {0.0f, -1.0f, 0.0f};
  cam.up = {0.0f, 0.0f, -1.0f};
  cam.fov = 45.0f;
  cam.aspect = 1.0f;
  cam.near_plane = 0.1f;
  cam.far_plane = 1000.0f;
  UniformData u{};
  u.view = glm::lookAt(glm::vec3(0.0f), cam.direction, cam.up);
  u.proj = cam.GetProj();
  u.camera_world_pos = glm::vec4(cam.position, 0.0f);
  u.near_plane = cam.near_plane;
  u.far_plane = cam.far_plane;
  u.screen_size = glm::vec2(kSize, kSize);
  u.output_is_linear = 1u;
  return u;
}

wgpu::Texture MakeTarget(wgpu::Device device, wgpu::TextureFormat fmt,
                         wgpu::TextureUsage extra) {
  wgpu::TextureDescriptor d;
  d.size = {kSize, kSize, 1};
  d.format = fmt;
  d.usage = wgpu::TextureUsage::RenderAttachment | extra;
  return device.CreateTexture(&d);
}

}  // namespace

TEST_CASE("forward-opaque material renders its solid colour", "[forward][gpu]") {
  TestGpu& g = GetTestGpu();
  wgpu::Buffer vbuf = UploadQuad(g.device);
  wgpu::Texture color = MakeTarget(g.device, kColorFormat, wgpu::TextureUsage::CopySrc);
  wgpu::Texture depth = MakeTarget(g.device, kDepthFormat, wgpu::TextureUsage::None);

  auto instance = g.opaque->CreateInstance(
      GeometryType::kTexturedMesh, MaterialPassType::kForwardOpaque,
      RenderPassType::kForward,
      InstanceParams{.uniform_overrides = {{"color", glm::vec4(1.0f, 0.0f, 0.0f, 1.0f)}}});
  REQUIRE(instance != nullptr);
  instance->SetParameterByName("modelMatrix",
      MaterialParameterValue(glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, -10.0f, 0.0f))));

  FrameContext frame;
  frame.Begin(g.device, g.queue, TopDownUniforms());
  {
    wgpu::RenderPassColorAttachment ca{};
    ca.view = color.CreateView();
    ca.loadOp = wgpu::LoadOp::Clear;
    ca.storeOp = wgpu::StoreOp::Store;
    ca.clearValue = {0, 0, 0, 1};
    ca.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
    wgpu::RenderPassDepthStencilAttachment da{};
    da.view = depth.CreateView();
    da.depthLoadOp = wgpu::LoadOp::Clear;
    da.depthStoreOp = wgpu::StoreOp::Store;
    da.depthClearValue = 0.0f;
    wgpu::RenderPassDescriptor desc;
    desc.colorAttachmentCount = 1;
    desc.colorAttachments = &ca;
    desc.depthStencilAttachment = &da;
    RenderPassContext pass = frame.BeginRenderPass(desc);
    REQUIRE(instance->Bind(pass, frame));
    REQUIRE(instance->BindPerObject(pass, frame));
    pass.SetVertexBuffer(0, vbuf);
    pass.Draw(Quad().mesh.vertex_count);
    pass.End();
  }
  wgpu::CommandBuffer cmd = frame.End();
  g.queue.Submit(1, &cmd);

  TextureReadback rb(g.instance, g.device, g.queue);
  CpuImage img = rb.ReadTextureSync(color, kSize, kSize, kColorFormat);
  CpuImage::Color px = img.GetPixel(kSize / 2, kSize / 2);
  CHECK(px.r > 250);
  CHECK(px.g < 5);
  CHECK(px.b < 5);
}

TEST_CASE("forward-transparent material premultiplied-blends over the background",
          "[forward][gpu]") {
  TestGpu& g = GetTestGpu();
  wgpu::Buffer vbuf = UploadQuad(g.device);
  wgpu::Texture color = MakeTarget(g.device, kColorFormat, wgpu::TextureUsage::CopySrc);
  wgpu::Texture depth = MakeTarget(g.device, kDepthFormat, wgpu::TextureUsage::None);

  // Premultiplied red at 50% coverage: out = src + dst*(1-srcA).
  //   dst = grey 0.25; src = (0.5, 0, 0, 0.5)
  //   out = (0.5 + 0.25*0.5, 0.25*0.5, 0.25*0.5) = (0.625, 0.125, 0.125)
  auto instance = g.transparent->CreateInstance(
      GeometryType::kTexturedMesh, MaterialPassType::kForwardTransparent,
      RenderPassType::kForward,
      InstanceParams{.uniform_overrides = {{"color", glm::vec4(0.5f, 0.0f, 0.0f, 0.5f)}}});
  REQUIRE(instance != nullptr);
  instance->SetParameterByName("modelMatrix",
      MaterialParameterValue(glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, -10.0f, 0.0f))));

  FrameContext frame;
  frame.Begin(g.device, g.queue, TopDownUniforms());
  wgpu::TextureView color_view = color.CreateView();
  // Pass 1: clear the colour to grey and the depth (writable).
  {
    wgpu::RenderPassColorAttachment ca{};
    ca.view = color_view;
    ca.loadOp = wgpu::LoadOp::Clear;
    ca.storeOp = wgpu::StoreOp::Store;
    ca.clearValue = {0.25, 0.25, 0.25, 1.0};
    ca.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
    wgpu::RenderPassDepthStencilAttachment da{};
    da.view = depth.CreateView();
    da.depthLoadOp = wgpu::LoadOp::Clear;
    da.depthStoreOp = wgpu::StoreOp::Store;
    da.depthClearValue = 0.0f;
    wgpu::RenderPassDescriptor desc;
    desc.colorAttachmentCount = 1;
    desc.colorAttachments = &ca;
    desc.depthStencilAttachment = &da;
    frame.BeginRenderPass(desc).End();
  }
  // Pass 2: draw the transparent quad against a DEPTH-READ-ONLY attachment.
  {
    wgpu::RenderPassColorAttachment ca{};
    ca.view = color_view;
    ca.loadOp = wgpu::LoadOp::Load;
    ca.storeOp = wgpu::StoreOp::Store;
    ca.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
    wgpu::RenderPassDepthStencilAttachment da{};
    da.view = depth.CreateView();
    da.depthLoadOp = wgpu::LoadOp::Undefined;
    da.depthStoreOp = wgpu::StoreOp::Undefined;
    da.depthReadOnly = true;
    wgpu::RenderPassDescriptor desc;
    desc.colorAttachmentCount = 1;
    desc.colorAttachments = &ca;
    desc.depthStencilAttachment = &da;
    RenderPassContext pass = frame.BeginRenderPass(desc);
    REQUIRE(instance->Bind(pass, frame));
    REQUIRE(instance->BindPerObject(pass, frame));
    pass.SetVertexBuffer(0, vbuf);
    pass.Draw(Quad().mesh.vertex_count);
    pass.End();
  }
  wgpu::CommandBuffer cmd = frame.End();
  g.queue.Submit(1, &cmd);

  TextureReadback rb(g.instance, g.device, g.queue);
  CpuImage img = rb.ReadTextureSync(color, kSize, kSize, kColorFormat);
  CpuImage::Color px = img.GetPixel(kSize / 2, kSize / 2);
  // Blended: reddish grey, brighter red than the 0.25 background, r > g == b.
  CHECK(px.r > 140);           // ~0.625*255 = 159
  CHECK(px.r < 180);
  CHECK(px.g > 20);            // ~0.125*255 = 32
  CHECK(px.g < 50);
  CHECK(std::abs((int)px.g - (int)px.b) <= 4);
  CHECK((int)px.r - (int)px.g > 80);  // clearly red-tinted by the blend
}

namespace {

// Add a forward mesh entity (quad at world origin) with the given factory,
// pass type, colour, and pass-routing marker, ready for the pass functions.
template <typename Marker>
entt::entity AddForwardEntity(entt::registry& reg, MaterialInstanceFactory* fac,
                              MaterialPassType pass, glm::vec4 color) {
  entt::entity e = reg.create();
  StaticTexturedMeshComponent mesh;
  mesh.vertices = Quad().mesh.vertices;
  mesh.vertex_count = Quad().mesh.vertex_count;
  mesh.geometry_type = GeometryType::kTexturedMesh;
  mesh.dirty = true;
  reg.emplace<StaticTexturedMeshComponent>(e, std::move(mesh));

  MaterialFactoryComponent fmc;
  fmc.factory = fac;
  fmc.pass_type = pass;
  fmc.params.uniform_overrides = {{"color", color}};
  fmc.config_hash = ComputeFactoryConfigHash(fmc);
  reg.emplace<MaterialFactoryComponent>(e, std::move(fmc));
  reg.emplace<Marker>(e);
  return e;
}

CpuImage RenderRegistry(TestGpu& g, entt::registry& reg, bool transparent,
                        const ForwardEngineResources& engine) {
  wgpu::Texture color = MakeTarget(g.device, kColorFormat, wgpu::TextureUsage::CopySrc);
  wgpu::Texture depth = MakeTarget(g.device, kDepthFormat, wgpu::TextureUsage::None);
  MaterialInstanceCache cache;
  FrameContext frame;
  frame.Begin(g.device, g.queue, TopDownUniforms());
  {
    wgpu::RenderPassColorAttachment ca{};
    ca.view = color.CreateView();
    ca.loadOp = wgpu::LoadOp::Clear;
    ca.storeOp = wgpu::StoreOp::Store;
    ca.clearValue = {0.25, 0.25, 0.25, 1.0};
    ca.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
    wgpu::RenderPassDepthStencilAttachment da{};
    da.view = depth.CreateView();
    da.depthLoadOp = wgpu::LoadOp::Clear;
    da.depthStoreOp = wgpu::StoreOp::Store;
    da.depthClearValue = 0.0f;
    wgpu::RenderPassDescriptor desc;
    desc.colorAttachmentCount = 1;
    desc.colorAttachments = &ca;
    desc.depthStencilAttachment = &da;
    RenderPassContext pass = frame.BeginRenderPass(desc);
    if (transparent) {
      RenderForwardTransparentMeshes(pass, frame, reg,
                                     glm::vec3(0.0f, 10.0f, 0.0f), cache, engine);
    } else {
      RenderForwardMeshes(pass, frame, reg, glm::vec3(0.0f, 10.0f, 0.0f), cache,
                          engine);
    }
    pass.End();
  }
  wgpu::CommandBuffer cmd = frame.End();
  g.queue.Submit(1, &cmd);
  TextureReadback rb(g.instance, g.device, g.queue);
  return rb.ReadTextureSync(color, kSize, kSize, kColorFormat);
}

}  // namespace

TEST_CASE("RenderForwardMeshes draws ForwardOpaqueRenderable entities",
          "[forward][gpu]") {
  TestGpu& g = GetTestGpu();
  entt::registry reg;
  AddForwardEntity<ForwardOpaqueRenderable>(reg, g.opaque.get(),
                                            MaterialPassType::kForwardOpaque,
                                            glm::vec4(0.0f, 1.0f, 0.0f, 1.0f));
  CpuImage img = RenderRegistry(g, reg, /*transparent=*/false, {});
  CpuImage::Color px = img.GetPixel(kSize / 2, kSize / 2);
  CHECK(px.g > 250);  // green quad drawn by the forward-opaque pass function
  CHECK(px.r < 5);
}

TEST_CASE("RenderForwardTransparentMeshes skips group 2 for a material that "
          "doesn't declare it",
          "[forward][gpu]") {
  TestGpu& g = GetTestGpu();
  entt::registry reg;
  AddForwardEntity<ForwardTransparentRenderable>(
      reg, g.transparent.get(), MaterialPassType::kForwardTransparent,
      glm::vec4(0.5f, 0.0f, 0.0f, 0.5f));

  // Engine resources are "present" (scene_depth set -> have_engine true), but
  // forward_debug declares no @group(2). The guard must skip the group-2 bind;
  // otherwise Dawn validation rejects binding a non-existent group and the draw
  // never lands. A successful reddish blend over grey proves the guard worked.
  wgpu::Texture dummy_depth =
      MakeTarget(g.device, kDepthFormat, wgpu::TextureUsage::None);
  ForwardEngineResources engine;
  engine.scene_depth = dummy_depth.CreateView();  // only this makes have_engine true

  CpuImage img = RenderRegistry(g, reg, /*transparent=*/true, engine);
  CpuImage::Color px = img.GetPixel(kSize / 2, kSize / 2);
  CHECK((int)px.r - (int)px.g > 80);  // premultiplied red blended over grey
  CHECK(px.r > 140);
}
