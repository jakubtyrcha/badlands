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

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

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

// ===========================================================================
// standard_forward material tests: the general forward-opaque, standard-lit
// material that "follows the G-buffer" (shared shadeStandard) + shadow-map PCF
// + IBL. Two behaviours are locked in here:
//   (A) the per-material shadow-cast DECISION (FactoryDescriptor::casts_shadow):
//       casts_shadow=true builds a kShadow pipeline, false does not.
//   (B) the forward-opaque material RECEIVES the standard sun + shadow through
//       the real 6-entry @group(2) path (shadow map + IBL), rendered on the GPU
//       and read back.
// Exact forward==deferred equivalence is guaranteed BY CONSTRUCTION (both call
// shadeStandard), so these target BEHAVIOUR, not a CPU re-derivation of the BRDF.
// ===========================================================================

namespace {

// Build a `standard_forward` kForwardOpaque factory (matching MaterialLibrary::
// AlphaCutout's descriptor shape — cull None / double-sided foliage), toggling
// only casts_shadow. Rendered here against BGRA8Unorm + Depth32Float (the test
// harness targets) rather than the HDR/reversed-Z scene targets.
std::unique_ptr<MaterialInstanceFactory> BuildStandardForwardFactory(
    TestGpu& g, bool casts_shadow,
    std::vector<std::string> extra_features = {}) {
  FactoryDescriptor desc;
  desc.shader_name = "standard_forward";
  desc.shader_path = "material/standard_forward.wesl";
  desc.supported_pass_types = {MaterialPassType::kForwardOpaque};
  desc.supported_geometry_types = {GeometryType::kTexturedMesh};
  desc.color_formats = {kColorFormat};
  desc.depth_format = kDepthFormat;
  desc.depth_write = true;
  desc.cull_mode = wgpu::CullMode::None;
  desc.casts_shadow = casts_shadow;
  desc.extra_features = std::move(extra_features);
  return BuildMaterialInstanceFactory(desc, g.device, g.queue, g.gen.get());
}

}  // namespace

// Test A (requirements #1/#2 guard): FactoryDescriptor::casts_shadow gates
// whether a kShadow pipeline is built for the kForwardOpaque variant. Both
// values still build a valid kForward pipeline; only casts_shadow=true yields a
// creatable kShadow instance. The kShadow-absent signal is a null CreateInstance
// return (StandardMaterialFactory::CreateInstance -> MeshRenderingMaterial::
// GetCompiledPipeline: pass_targets_ has no kShadow entry -> nullptr).
TEST_CASE("standard_forward casts_shadow gates the kShadow pipeline",
          "[forward][gpu][shadow]") {
  TestGpu& g = GetTestGpu();
  auto caster = BuildStandardForwardFactory(g, /*casts_shadow=*/true);
  auto noncaster = BuildStandardForwardFactory(g, /*casts_shadow=*/false);
  REQUIRE(caster != nullptr);
  REQUIRE(noncaster != nullptr);

  // Both build a valid FORWARD pipeline.
  auto fwd_caster =
      caster->CreateInstance(GeometryType::kTexturedMesh,
                             MaterialPassType::kForwardOpaque,
                             RenderPassType::kForward);
  auto fwd_noncaster =
      noncaster->CreateInstance(GeometryType::kTexturedMesh,
                                MaterialPassType::kForwardOpaque,
                                RenderPassType::kForward);
  REQUIRE(fwd_caster != nullptr);
  CHECK(fwd_caster->IsValid());
  REQUIRE(fwd_noncaster != nullptr);
  CHECK(fwd_noncaster->IsValid());

  // Only casts_shadow=true builds a SHADOW pipeline (depth-only alpha-test).
  auto shadow_caster =
      caster->CreateInstance(GeometryType::kTexturedMesh,
                             MaterialPassType::kForwardOpaque,
                             RenderPassType::kShadow);
  auto shadow_noncaster =
      noncaster->CreateInstance(GeometryType::kTexturedMesh,
                                MaterialPassType::kForwardOpaque,
                                RenderPassType::kShadow);
  REQUIRE(shadow_caster != nullptr);
  CHECK(shadow_caster->IsValid());
  CHECK(shadow_noncaster == nullptr);  // no kShadow pipeline was built
}

// FactoryDescriptor::extra_features (first real exercise of Task 1's
// @if(translucency) shader path): a standard_forward factory built with
// extra_features={"translucency"} still compiles a valid kForwardOpaque
// instance with the real 6-entry @group(2) layout, and casts_shadow still
// yields a creatable kShadow pipeline (the feature flag doesn't disturb it).
TEST_CASE("standard_forward translucency feature compiles the group-2 variant",
          "[forward][gpu][shadow]") {
  TestGpu& g = GetTestGpu();
  auto fac = BuildStandardForwardFactory(g, /*casts_shadow=*/true,
                                         /*extra_features=*/{"translucency"});
  REQUIRE(fac != nullptr);

  auto instance =
      fac->CreateInstance(GeometryType::kTexturedMesh,
                          MaterialPassType::kForwardOpaque,
                          RenderPassType::kForward);
  REQUIRE(instance != nullptr);
  CHECK(instance->IsValid());
  CHECK(instance->DeclaresBindGroup(2));

  auto shadow_instance =
      fac->CreateInstance(GeometryType::kTexturedMesh,
                          MaterialPassType::kForwardOpaque,
                          RenderPassType::kShadow);
  REQUIRE(shadow_instance != nullptr);
  CHECK(shadow_instance->IsValid());
}

namespace {

// --- Test B group-2 resources (mirror render_forward.cpp's 6-entry opaque
// bind: shadow map + comparison sampler, then IBL prefiltered cube + BRDF LUT).

wgpu::Sampler MakeComparisonSampler(wgpu::Device d) {
  wgpu::SamplerDescriptor s{};
  s.addressModeU = wgpu::AddressMode::ClampToEdge;
  s.addressModeV = wgpu::AddressMode::ClampToEdge;
  s.addressModeW = wgpu::AddressMode::ClampToEdge;
  s.magFilter = wgpu::FilterMode::Linear;
  s.minFilter = wgpu::FilterMode::Linear;
  s.maxAnisotropy = 1;
  s.compare = wgpu::CompareFunction::LessEqual;  // matches shadow_map conv-Z
  return d.CreateSampler(&s);
}

wgpu::Sampler MakeLinearSampler(wgpu::Device d) {
  wgpu::SamplerDescriptor s{};
  s.addressModeU = wgpu::AddressMode::ClampToEdge;
  s.addressModeV = wgpu::AddressMode::ClampToEdge;
  s.addressModeW = wgpu::AddressMode::ClampToEdge;
  s.magFilter = wgpu::FilterMode::Linear;
  s.minFilter = wgpu::FilterMode::Linear;
  s.maxAnisotropy = 1;
  return d.CreateSampler(&s);
}

// 1x1x6 BLACK cube (RGBA8Unorm): a single mip so textureNumLevels==1 ->
// maxMip==0 -> textureSampleLevel(..., roughness*0) reads the black texel ->
// the IBL ambient-specular term is exactly 0, isolating the direct-sun +
// SH-ambient path for Test B's shadow assertions.
wgpu::Texture MakeBlackCube(wgpu::Device d, wgpu::Queue q) {
  wgpu::TextureDescriptor td{};
  td.size = {1, 1, 6};
  td.dimension = wgpu::TextureDimension::e2D;
  td.format = wgpu::TextureFormat::RGBA8Unorm;
  td.usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;
  td.mipLevelCount = 1;
  wgpu::Texture t = d.CreateTexture(&td);
  std::vector<uint8_t> zero(6u * 4u, 0u);  // 6 faces * 1 px * RGBA
  wgpu::TexelCopyTextureInfo dst{};
  dst.texture = t;
  wgpu::TexelCopyBufferLayout layout{};
  layout.bytesPerRow = 4u;
  layout.rowsPerImage = 1u;
  wgpu::Extent3D ext = {1, 1, 6};
  q.WriteTexture(&dst, zero.data(), zero.size(), &layout, &ext);
  return t;
}

wgpu::TextureView CubeView(wgpu::Texture cube) {
  wgpu::TextureViewDescriptor vd{};
  vd.dimension = wgpu::TextureViewDimension::Cube;
  vd.arrayLayerCount = 6;
  vd.format = wgpu::TextureFormat::RGBA8Unorm;
  return cube.CreateView(&vd);
}

// A small Depth32Float texture usable both as a render attachment (to clear it
// to a chosen depth) and as a sampled shadow map (texture_depth_2d) — same dual
// usage as ShadowMap's real depth texture.
wgpu::Texture MakeShadowDepth(wgpu::Device d) {
  wgpu::TextureDescriptor td{};
  td.size = {16, 16, 1};
  td.format = kDepthFormat;  // Depth32Float
  td.usage =
      wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding;
  return d.CreateTexture(&td);
}

// Orthographic light view-proj looking straight down (-Y) at the quad centre.
// glm::ortho under GLM_FORCE_DEPTH_ZERO_TO_ONE maps near->0, far->1 (conv-Z),
// matching sampleShadowMapPCF's ndc.z and the LessEqual comparison sampler. The
// quad centre (world (0,0,0)) sits ~mid-frustum -> receiverDepth ~= 0.49, so
// clearing the shadow map to 1.0 leaves it fully LIT and clearing to 0.0 fully
// OCCLUDED. Frustum (+/-30) covers the whole +/-20 quad so every pixel agrees.
glm::mat4 DownLightViewProj() {
  glm::mat4 view = glm::lookAt(glm::vec3(0.0f, 50.0f, 0.0f),
                               glm::vec3(0.0f, 0.0f, 0.0f),
                               glm::vec3(0.0f, 0.0f, 1.0f));
  glm::mat4 proj = glm::ortho(-30.0f, 30.0f, -30.0f, 30.0f, 1.0f, 100.0f);
  return proj * view;
}

// Frame UBO for Test B: top-down camera (as the other tests) plus a sun toward
// the quad's viewer-facing normal (NdotL>0), the down-looking lightViewProj, an
// optional constant SH ambient, and shadowParams=0 (no normal-offset bias / a
// degenerate PCF tap radius -> every tap reads receiverDepth exactly, keeping
// the lit<->occluded toggle deterministic).
UniformData StandardForwardUniforms(bool ambient_on, bool sun_toward_viewer) {
  UniformData u = TopDownUniforms();
  // Viewer is above (+Y); the double-sided material flips the geometric normal
  // to face the viewer, so the shaded N ~= +Y. sun = +Y => NdotL ~= 1.
  u.sunDir = glm::vec4(0.0f, sun_toward_viewer ? 1.0f : -1.0f, 0.0f, 0.0f);
  u.sunColor = glm::vec4(3.0f, 3.0f, 3.0f, 0.0f);
  for (auto& c : u.ambient_sh) c = glm::vec4(0.0f);
  if (ambient_on) u.ambient_sh[0] = glm::vec4(0.3f, 0.3f, 0.3f, 0.0f);
  u.light_view_proj = DownLightViewProj();
  u.shadow_params = glm::vec4(0.0f);
  return u;
}

// Transmission tint for the translucency uniform override: deliberately
// green-dominant so the transmitted channel is unambiguous against the
// grey/white reflection terms.
constexpr glm::vec3 kTransTint{0.2f, 1.0f, 0.2f};

struct ForwardShadeParams {
  float shadow_clear_depth;  // 1.0 = lit texel, 0.0 = occluding texel
  bool ambient_on;
  bool sun_toward_viewer;
  float transmission_strength = 0.0f;
};

// Render one standard_forward quad through the REAL forward-opaque path:
// CreateInstance -> Bind (group 0: frame + albedo) -> BindPerObject (group 1:
// object UBO) -> manual 6-entry @group(2) bind (shadow map + comparison sampler
// + black IBL cube + BRDF LUT), mirroring render_forward.cpp's
// BuildForwardOpaqueEngineBindGroup. A depth-only pre-pass clears the shadow map
// to `shadow_clear_depth`. Returns the centre pixel.
CpuImage::Color RenderStandardForward(TestGpu& g, MaterialInstanceFactory* fac,
                                      wgpu::Buffer vbuf,
                                      wgpu::TextureView albedo_view,
                                      wgpu::Sampler albedo_sampler,
                                      wgpu::TextureView shadow_view,
                                      wgpu::Sampler shadow_sampler,
                                      wgpu::TextureView cube_view,
                                      wgpu::Sampler ibl_sampler,
                                      wgpu::TextureView brdf_view,
                                      wgpu::Sampler brdf_sampler,
                                      const ForwardShadeParams& p) {
  wgpu::Texture color =
      MakeTarget(g.device, kColorFormat, wgpu::TextureUsage::CopySrc);
  wgpu::Texture depth = MakeTarget(g.device, kDepthFormat, wgpu::TextureUsage::None);

  auto instance = fac->CreateInstance(
      GeometryType::kTexturedMesh, MaterialPassType::kForwardOpaque,
      RenderPassType::kForward,
      InstanceParams{
          .texture_overrides = {DefaultTextureView{.param_name = "albedo",
                                                    .view = albedo_view,
                                                    .sampler = albedo_sampler,
                                                    .type = TextureType::k2D}},
          .uniform_overrides = {
              {"tint", glm::vec4(1.0f, 1.0f, 1.0f, 1.0f)},
              // params.x = alpha cutoff (0 = opaque), params.y = roughness.
              {"params", glm::vec4(0.0f, 0.9f, 0.0f, 0.0f)},
              // rgb = transmission tint, a = translucency strength (0 = off;
              // the non-translucency opaque factory's shader ignores this
              // regardless, so existing callers passing the default 0 are
              // unaffected).
              {"transmission", glm::vec4(kTransTint, p.transmission_strength)}}});
  REQUIRE(instance != nullptr);
  REQUIRE(instance->DeclaresBindGroup(2));  // the real 6-entry @group(2) path
  instance->SetParameterByName(
      "modelMatrix",
      MaterialParameterValue(
          glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, -10.0f, 0.0f))));

  FrameContext frame;
  frame.Begin(g.device, g.queue,
              StandardForwardUniforms(p.ambient_on, p.sun_toward_viewer));

  // Pass 1: clear the shadow map to the chosen depth (lit vs occluding).
  {
    wgpu::RenderPassDepthStencilAttachment da{};
    da.view = shadow_view;
    da.depthLoadOp = wgpu::LoadOp::Clear;
    da.depthStoreOp = wgpu::StoreOp::Store;
    da.depthClearValue = p.shadow_clear_depth;
    wgpu::RenderPassDescriptor desc;
    desc.colorAttachmentCount = 0;
    desc.colorAttachments = nullptr;
    desc.depthStencilAttachment = &da;
    frame.BeginRenderPass(desc).End();
  }

  // Pass 2: forward-opaque draw sampling the shadow map + IBL at @group(2).
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
    da.depthClearValue = 0.0f;  // reversed-Z far
    wgpu::RenderPassDescriptor desc;
    desc.colorAttachmentCount = 1;
    desc.colorAttachments = &ca;
    desc.depthStencilAttachment = &da;
    RenderPassContext pass = frame.BeginRenderPass(desc);
    REQUIRE(instance->Bind(pass, frame));
    REQUIRE(instance->BindPerObject(pass, frame));

    std::array<wgpu::BindGroupEntry, 6> e{};
    e[0].binding = 0; e[0].textureView = shadow_view;
    e[1].binding = 1; e[1].sampler = shadow_sampler;
    e[2].binding = 2; e[2].textureView = cube_view;
    e[3].binding = 3; e[3].sampler = ibl_sampler;
    e[4].binding = 4; e[4].textureView = brdf_view;
    e[5].binding = 5; e[5].sampler = brdf_sampler;
    wgpu::BindGroup g2 = frame.CreateBindGroup(
        instance->GetPipeline().GetBindGroupLayout(2), e);
    pass.SetBindGroup(2, g2);

    pass.SetVertexBuffer(0, vbuf);
    pass.Draw(Quad().mesh.vertex_count);
    pass.End();
  }

  wgpu::CommandBuffer cmd = frame.End();
  g.queue.Submit(1, &cmd);

  TextureReadback rb(g.instance, g.device, g.queue);
  CpuImage img = rb.ReadTextureSync(color, kSize, kSize, kColorFormat);
  return img.GetPixel(kSize / 2, kSize / 2);
}

int MaxChannel(CpuImage::Color c) {
  return std::max({(int)c.r, (int)c.g, (int)c.b});
}

}  // namespace

// Test B: a standard_forward quad RECEIVES the standard sun + shadow through the
// real 6-entry @group(2) bind (shadow map + IBL), rendered + read back.
//   (1) Lit + ambient: shadow map = LIT, small SH ambient, black IBL -> clearly
//       non-black (the forward material compiles, the 6-entry @group(2) layout
//       matches the pass's bind with no Dawn error, and lighting is applied).
//   (2) Shadow received: with SH ambient=0 and the black IBL (so the direct sun
//       is the ONLY term), toggling the shadow map LIT->OCCLUDED drops the
//       output to ~black: lit_direct > occluded and occluded ~= 0.
TEST_CASE("standard_forward receives the standard sun + shadow via @group(2)",
          "[forward][gpu][shadow]") {
  TestGpu& g = GetTestGpu();
  auto fac = BuildStandardForwardFactory(g, /*casts_shadow=*/true);
  REQUIRE(fac != nullptr);

  wgpu::Buffer vbuf = UploadQuad(g.device);
  // Known non-black albedo (opaque mid-grey), alpha=255 so cutoff=0 never
  // discards.
  wgpu::Texture albedo =
      test::CreateRgbaTexture(g.device, g.queue, 1, 1, {200, 200, 200, 255});
  wgpu::Sampler albedo_sampler = MakeLinearSampler(g.device);

  wgpu::Texture shadow = MakeShadowDepth(g.device);
  wgpu::TextureView shadow_view = shadow.CreateView();
  wgpu::Sampler shadow_sampler = MakeComparisonSampler(g.device);

  wgpu::Texture cube = MakeBlackCube(g.device, g.queue);
  wgpu::TextureView cube_view = CubeView(cube);
  wgpu::Sampler ibl_sampler = MakeLinearSampler(g.device);

  wgpu::Texture brdf =
      test::CreateRgbaTexture(g.device, g.queue, 1, 1, {0, 0, 0, 255});
  wgpu::TextureView brdf_view = brdf.CreateView();
  wgpu::Sampler brdf_sampler = MakeLinearSampler(g.device);

  auto render = [&](const ForwardShadeParams& p) {
    return RenderStandardForward(g, fac.get(), vbuf, albedo.CreateView(),
                                 albedo_sampler, shadow_view, shadow_sampler,
                                 cube_view, ibl_sampler, brdf_view, brdf_sampler,
                                 p);
  };

  // (1) Lit + ambient: whole real path applied -> clearly non-black.
  CpuImage::Color lit_ambient = render(
      {.shadow_clear_depth = 1.0f, .ambient_on = true, .sun_toward_viewer = true});
  INFO("lit+ambient rgb = " << (int)lit_ambient.r << "," << (int)lit_ambient.g
                            << "," << (int)lit_ambient.b);
  CHECK(MaxChannel(lit_ambient) > 40);

  // (2) Shadow received: ambient off + black IBL => direct sun is the ONLY
  // term. Toggle the shadow map LIT -> OCCLUDED.
  CpuImage::Color lit_direct = render(
      {.shadow_clear_depth = 1.0f, .ambient_on = false, .sun_toward_viewer = true});
  CpuImage::Color occluded = render(
      {.shadow_clear_depth = 0.0f, .ambient_on = false, .sun_toward_viewer = true});
  INFO("lit_direct rgb = " << (int)lit_direct.r << "," << (int)lit_direct.g
                          << "," << (int)lit_direct.b);
  INFO("occluded rgb = " << (int)occluded.r << "," << (int)occluded.g << ","
                        << (int)occluded.b);
  CHECK(MaxChannel(lit_direct) > 40);           // direct sun reaches the surface
  CHECK(MaxChannel(occluded) < 6);              // shadowed -> ~black
  CHECK(MaxChannel(lit_direct) - MaxChannel(occluded) > 30);  // shadow received
}

// Test C: standard_forward's translucency term (Task 1's evaluateTranslucency,
// wired through the "transmission" uniform: rgb=tint, a=strength). Model under
// test: Lo += strength * T * (direct + ambient), direct = shadow * sunColor *
// backLit * (1+flare) with backLit = max(-dot(N,L),0) (only nonzero when the
// sun is BEHIND the leaf), ambient = ao * evaluateAmbientSHL2(-N, ambientSH).
// So: transmission needs back-lighting; `direct` is shadow-gated; `ambient`
// (the AO/IBL "glare") is only ao-gated, not shadow-gated. Differential
// (ON @ strength 0.8 vs OFF @ strength 0) assertions are self-validating: a
// no-op transmission collapses ON==OFF and fails every check below.
TEST_CASE(
    "standard_forward translucency: back-lit transmission (shadow-gated "
    "direct + AO/IBL glare)",
    "[forward][gpu][shadow]") {
  TestGpu& g = GetTestGpu();
  auto fac = BuildStandardForwardFactory(g, /*casts_shadow=*/true,
                                         /*extra_features=*/{"translucency"});
  REQUIRE(fac != nullptr);

  wgpu::Buffer vbuf = UploadQuad(g.device);
  wgpu::Texture albedo =
      test::CreateRgbaTexture(g.device, g.queue, 1, 1, {200, 200, 200, 255});
  wgpu::Sampler albedo_sampler = MakeLinearSampler(g.device);

  wgpu::Texture shadow = MakeShadowDepth(g.device);
  wgpu::TextureView shadow_view = shadow.CreateView();
  wgpu::Sampler shadow_sampler = MakeComparisonSampler(g.device);

  wgpu::Texture cube = MakeBlackCube(g.device, g.queue);
  wgpu::TextureView cube_view = CubeView(cube);
  wgpu::Sampler ibl_sampler = MakeLinearSampler(g.device);

  wgpu::Texture brdf =
      test::CreateRgbaTexture(g.device, g.queue, 1, 1, {0, 0, 0, 255});
  wgpu::TextureView brdf_view = brdf.CreateView();
  wgpu::Sampler brdf_sampler = MakeLinearSampler(g.device);

  auto render = [&](const ForwardShadeParams& p) {
    return RenderStandardForward(g, fac.get(), vbuf, albedo.CreateView(),
                                 albedo_sampler, shadow_view, shadow_sampler,
                                 cube_view, ibl_sampler, brdf_view,
                                 brdf_sampler, p);
  };

  // (1) Back-lit glow: back-lit, shadow=lit, ambient=off. ON(0.8) vs OFF(0).
  CpuImage::Color backlit_on = render({.shadow_clear_depth = 1.0f,
                                       .ambient_on = false,
                                       .sun_toward_viewer = false,
                                       .transmission_strength = 0.8f});
  CpuImage::Color backlit_off = render({.shadow_clear_depth = 1.0f,
                                        .ambient_on = false,
                                        .sun_toward_viewer = false,
                                        .transmission_strength = 0.0f});
  INFO("backlit_on rgb = " << (int)backlit_on.r << "," << (int)backlit_on.g
                           << "," << (int)backlit_on.b);
  INFO("backlit_off rgb = " << (int)backlit_off.r << "," << (int)backlit_off.g
                            << "," << (int)backlit_off.b);
  CHECK(MaxChannel(backlit_on) - MaxChannel(backlit_off) > 20);

  // (2) Front-lit unchanged: sun_toward_viewer=true (front-lit), shadow=lit,
  // ambient=off. ON vs OFF should be indistinguishable -- no back
  // transmission when the sun is in front of the leaf.
  CpuImage::Color frontlit_on = render({.shadow_clear_depth = 1.0f,
                                        .ambient_on = false,
                                        .sun_toward_viewer = true,
                                        .transmission_strength = 0.8f});
  CpuImage::Color frontlit_off = render({.shadow_clear_depth = 1.0f,
                                         .ambient_on = false,
                                         .sun_toward_viewer = true,
                                         .transmission_strength = 0.0f});
  INFO("frontlit_on rgb = " << (int)frontlit_on.r << "," << (int)frontlit_on.g
                            << "," << (int)frontlit_on.b);
  INFO("frontlit_off rgb = " << (int)frontlit_off.r << ","
                             << (int)frontlit_off.g << ","
                             << (int)frontlit_off.b);
  CHECK(std::abs(MaxChannel(frontlit_on) - MaxChannel(frontlit_off)) < 8);

  // (3) Direct is shadow-gated: back-lit, ambient=off, strength=0.8: shadow
  // lit vs occluded. No ambient => a shadowed back-lit leaf is ~black.
  CpuImage::Color backlit_occluded = render({.shadow_clear_depth = 0.0f,
                                             .ambient_on = false,
                                             .sun_toward_viewer = false,
                                             .transmission_strength = 0.8f});
  INFO("backlit_occluded rgb = " << (int)backlit_occluded.r << ","
                                 << (int)backlit_occluded.g << ","
                                 << (int)backlit_occluded.b);
  CHECK(MaxChannel(backlit_on) - MaxChannel(backlit_occluded) > 20);
  CHECK(MaxChannel(backlit_occluded) < 6);

  // (4) Glare survives shadow: back-lit, ambient=on, OCCLUDED (shadow=0),
  // ON(0.8) vs OFF(0). Isolates the SH(-N) transmission glare: it's
  // ao-gated, not shadow-gated, while the reflection-side ambient term is
  // identical for ON/OFF and cancels out of the differential.
  CpuImage::Color glare_on = render({.shadow_clear_depth = 0.0f,
                                     .ambient_on = true,
                                     .sun_toward_viewer = false,
                                     .transmission_strength = 0.8f});
  CpuImage::Color glare_off = render({.shadow_clear_depth = 0.0f,
                                      .ambient_on = true,
                                      .sun_toward_viewer = false,
                                      .transmission_strength = 0.0f});
  INFO("glare_on rgb = " << (int)glare_on.r << "," << (int)glare_on.g << ","
                         << (int)glare_on.b);
  INFO("glare_off rgb = " << (int)glare_off.r << "," << (int)glare_off.g
                          << "," << (int)glare_off.b);
  CHECK((int)glare_on.g - (int)glare_off.g > 10);
}

namespace {

// Add a ForwardOpaqueRenderable quad backed by a `standard_forward` factory
// (which DECLARES @group(2)) to the registry, ready for RenderForwardMeshes.
// Albedo falls back to the factory's default "white" slot texture (group 0),
// so no texture override is needed to make the draw bind + record.
entt::entity AddStandardForwardEntity(entt::registry& reg,
                                      MaterialInstanceFactory* fac) {
  entt::entity e = reg.create();
  StaticTexturedMeshComponent mesh;
  mesh.vertices = Quad().mesh.vertices;
  mesh.vertex_count = Quad().mesh.vertex_count;
  mesh.geometry_type = GeometryType::kTexturedMesh;
  mesh.dirty = true;
  reg.emplace<StaticTexturedMeshComponent>(e, std::move(mesh));

  MaterialFactoryComponent fmc;
  fmc.factory = fac;
  fmc.pass_type = MaterialPassType::kForwardOpaque;
  fmc.params.uniform_overrides = {
      {"tint", glm::vec4(1.0f, 1.0f, 1.0f, 1.0f)},
      {"params", glm::vec4(0.0f, 0.9f, 0.0f, 0.0f)}};  // x=cutoff, y=roughness
  fmc.config_hash = ComputeFactoryConfigHash(fmc);
  reg.emplace<MaterialFactoryComponent>(e, std::move(fmc));
  reg.emplace<ForwardOpaqueRenderable>(e);
  return e;
}

}  // namespace

// Robustness (findings #1/#2): a `standard_forward` material REQUIRES @group(2),
// so RenderForwardMeshes must never issue its draw when the engine forward
// resources it binds (shadow map + IBL) are absent — a declared-but-unbound
// group 2 is a Dawn validation error. This drives RenderForwardMeshes with a
// registry entity that declares @group(2) but an EMPTY ForwardEngineResources{},
// captured under a validation error scope. Before the fix the opaque gate keyed
// on scene_depth presence (never bound in the opaque path) and left group 2
// unbound while the pipeline required it -> validation error (RED). After the
// fix the entity is skipped when the opaque resources are unavailable -> no
// invalid draw (GREEN).
TEST_CASE("RenderForwardMeshes skips a group-2 material when engine resources "
          "are absent (no unbound draw)",
          "[forward][gpu][shadow]") {
  TestGpu& g = GetTestGpu();
  auto fac = BuildStandardForwardFactory(g, /*casts_shadow=*/true);
  REQUIRE(fac != nullptr);

  entt::registry reg;
  AddStandardForwardEntity(reg, fac.get());

  wgpu::Texture color =
      MakeTarget(g.device, kColorFormat, wgpu::TextureUsage::CopySrc);
  wgpu::Texture depth =
      MakeTarget(g.device, kDepthFormat, wgpu::TextureUsage::None);
  MaterialInstanceCache cache;

  // Capture any validation error the draw provokes rather than letting it hit
  // the device uncaptured-error callback.
  g.device.PushErrorScope(wgpu::ErrorFilter::Validation);

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
    // Empty resources: standard_forward declares @group(2) but the opaque
    // engine bind (shadow map + IBL) is unavailable this frame.
    RenderForwardMeshes(pass, frame, reg, glm::vec3(0.0f, 10.0f, 0.0f), cache,
                        ForwardEngineResources{});
    pass.End();
  }
  wgpu::CommandBuffer cmd = frame.End();
  g.queue.Submit(1, &cmd);

  bool scope_done = false;
  bool validation_error = false;
  g.device.PopErrorScope(
      wgpu::CallbackMode::AllowProcessEvents,
      [&](wgpu::PopErrorScopeStatus, wgpu::ErrorType type, wgpu::StringView msg) {
        if (type != wgpu::ErrorType::NoError) {
          validation_error = true;
          INFO("captured error: " << (msg.length > 0
                                           ? std::string(msg.data, msg.length)
                                           : std::string("(no message)")));
        }
        scope_done = true;
      });
  while (!scope_done) {
    g.instance.ProcessEvents();
    g.device.Tick();
  }

  CHECK_FALSE(validation_error);  // must not emit a declared-but-unbound draw
}
