// Phase A of the GPU-driven instanced-rendering feature: a de-risk primitives
// spike. Proves three primitives that DO NOT EXIST elsewhere in this engine —
// atomics in a compute shader, indirect draws, and a buffer-readback test
// helper — in isolation, before later phases build on them. See
// .superpowers/sdd/phase-A-brief.md / phase-A-report.md. Later phases extend
// this same `badlands_gpu_instance_tests` target.
//
// Test 1 (THE GATE): does the WESL->WGSL->naga reflection path accept a
// top-level `atomic<u32>` storage binding, and does atomicAdd actually
// serialize concurrent appends correctly on real GPU hardware?
// Test 2: does DrawIndexedIndirect honor the instanceCount the GPU reads out
// of an indirect-args buffer, not whatever the CPU passed at record time?
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <numeric>
#include <string>
#include <vector>

#include <catch_amalgamated.hpp>
#include <dawn/webgpu_cpp.h>
#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "core/geometry_type.hpp"
#include "core/util/cpu_image.hpp"
#include "engine/core/camera.hpp"
#include "engine/rendering/color_render_target.hpp"
#include "engine/rendering/context/frame_context.hpp"
#include "engine/rendering/context/render_pass_context.hpp"
#include "engine/rendering/frustum.hpp"
#include "engine/rendering/gbuffer.hpp"
#include "engine/rendering/gpu_instance_renderer.hpp"
#include "engine/rendering/material/material_instance_cache.hpp"
#include "engine/rendering/material/material_instance_factory.hpp"
#include "engine/rendering/material/rendering_material_instance.hpp"
#include "engine/rendering/shader/gpu_pipeline_generator.hpp"
#include "engine/rendering/texture_readback.hpp"
#include "engine/rendering/util/find_shader_directory.hpp"
#include "engine/tests/buffer_readback.hpp"
#include "gpu_test_helpers.hpp"

using namespace badlands;

namespace {

struct TestGpu {
  wgpu::Instance instance;
  wgpu::Device device;
  wgpu::Queue queue;
  std::unique_ptr<GpuPipelineGenerator> gen;
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
    return t;
  }();
  return *g;
}

// A zero-initialized Storage|CopyDst|CopySrc buffer (CopySrc so the test can
// read it back via BufferReadback).
wgpu::Buffer MakeZeroedStorageBuffer(wgpu::Device device, uint64_t size) {
  wgpu::BufferDescriptor bd{};
  bd.size = size;
  bd.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst |
             wgpu::BufferUsage::CopySrc;
  bd.mappedAtCreation = true;
  wgpu::Buffer buf = device.CreateBuffer(&bd);
  std::memset(buf.GetMappedRange(0, size), 0, size);
  buf.Unmap();
  return buf;
}

}  // namespace

// ===========================================================================
// Test 1 (THE GATE): atomic<u32> storage binding — shaders/tests/
// atomic_append_test.wesl. Dispatches N threads that each atomicAdd a unique
// slot in `counter` and record their global_invocation_id at that slot in
// `out_indices`. If every thread appended exactly once: counter == N and
// out_indices[0..N) is a permutation of 0..N-1.
// ===========================================================================
TEST_CASE("atomic<u32> storage binding compiles, reflects, and serializes "
          "concurrent appends",
          "[gpu_instance][gpu]") {
  TestGpu& g = GetTestGpu();

  auto pipeline = g.gen->GetComputePipeline("tests/atomic_append_test");
  REQUIRE(pipeline != nullptr);
  REQUIRE(pipeline->pipeline != nullptr);

  // Reflection: both bindings must show up as read_write STORAGE at group 0
  // (not e.g. silently falling back to UNIFORM — see wesl_ffi's classify_type
  // fallback arm for why an unrecognised naga::TypeInner needs the outer
  // AddressSpace::Storage refinement to still land on STORAGE).
  REQUIRE(pipeline->reflected_bindings.size() == 2);
  auto find_binding = [&](uint32_t binding) -> const ReflectedBinding* {
    for (const auto& b : pipeline->reflected_bindings) {
      if (b.group == 0 && b.binding == binding) return &b;
    }
    return nullptr;
  };
  const ReflectedBinding* counter_binding = find_binding(0);
  const ReflectedBinding* indices_binding = find_binding(1);
  REQUIRE(counter_binding != nullptr);
  REQUIRE(indices_binding != nullptr);
  CHECK(counter_binding->name == "counter");
  CHECK(counter_binding->buffer_type == wgpu::BufferBindingType::Storage);
  CHECK(indices_binding->name == "out_indices");
  CHECK(indices_binding->buffer_type == wgpu::BufferBindingType::Storage);
  CHECK(pipeline->workgroup_size[0] == 64);

  // Hardcode the dispatch math to the shader's literal @workgroup_size(64)
  // rather than trusting the reflected workgroup_size — this test gates
  // atomics correctness, not the workgroup-size reflection (checked above).
  constexpr uint32_t kWorkgroupSize = 64;
  constexpr uint32_t N = 256;
  static_assert(N % kWorkgroupSize == 0);

  wgpu::Buffer counter_buf = MakeZeroedStorageBuffer(g.device, sizeof(uint32_t));
  wgpu::Buffer out_indices_buf =
      MakeZeroedStorageBuffer(g.device, uint64_t{N} * sizeof(uint32_t));

  std::array<wgpu::BindGroupEntry, 2> entries{};
  entries[0].binding = 0;
  entries[0].buffer = counter_buf;
  entries[0].size = sizeof(uint32_t);
  entries[1].binding = 1;
  entries[1].buffer = out_indices_buf;
  entries[1].size = uint64_t{N} * sizeof(uint32_t);
  wgpu::BindGroup bg = CreateComputeBindGroup(g.device, *pipeline, entries);
  REQUIRE(bg != nullptr);

  wgpu::CommandEncoder enc = g.device.CreateCommandEncoder();
  wgpu::ComputePassEncoder cp = enc.BeginComputePass();
  cp.SetPipeline(pipeline->pipeline);
  cp.SetBindGroup(0, bg, 0, nullptr);
  cp.DispatchWorkgroups(N / kWorkgroupSize, 1, 1);
  cp.End();
  wgpu::CommandBuffer cmd = enc.Finish();
  g.queue.Submit(1, &cmd);
  test::WaitForGpu(g.instance, g.device, g.queue);

  std::vector<uint32_t> counter = test::ReadBufferSync<uint32_t>(
      g.instance, g.device, g.queue, counter_buf, 0, 1);
  REQUIRE(counter.size() == 1);
  CHECK(counter[0] == N);

  std::vector<uint32_t> indices = test::ReadBufferSync<uint32_t>(
      g.instance, g.device, g.queue, out_indices_buf, 0, N);
  REQUIRE(indices.size() == N);
  std::vector<uint32_t> sorted = indices;
  std::sort(sorted.begin(), sorted.end());
  std::vector<uint32_t> expected(N);
  std::iota(expected.begin(), expected.end(), 0u);
  CHECK(sorted == expected);
}

// ===========================================================================
// Test 2: DrawIndexedIndirect honors the GPU-read instanceCount —
// shaders/tests/indirect_draw_test.wesl (self-contained: no bind groups, no
// camera/frame import) drawing a fullscreen unit quad.
// ===========================================================================
namespace {

constexpr uint32_t kSize = 8;
constexpr wgpu::TextureFormat kColorFormat = wgpu::TextureFormat::BGRA8Unorm;

// {indexCount, instanceCount, firstIndex, baseVertex, firstInstance} — the
// standard 20-byte DrawIndexedIndirect args layout.
struct IndirectArgs {
  uint32_t index_count;
  uint32_t instance_count;
  uint32_t first_index;
  int32_t base_vertex;
  uint32_t first_instance;
};
static_assert(sizeof(IndirectArgs) == 20);

wgpu::Buffer UploadIndirectArgs(wgpu::Device device, uint32_t instance_count) {
  IndirectArgs args{.index_count = 6,
                    .instance_count = instance_count,
                    .first_index = 0,
                    .base_vertex = 0,
                    .first_instance = 0};
  wgpu::BufferDescriptor bd{};
  bd.size = sizeof(IndirectArgs);
  bd.usage = wgpu::BufferUsage::Indirect | wgpu::BufferUsage::CopyDst;
  bd.mappedAtCreation = true;
  wgpu::Buffer buf = device.CreateBuffer(&bd);
  std::memcpy(buf.GetMappedRange(0, sizeof(IndirectArgs)), &args,
              sizeof(IndirectArgs));
  buf.Unmap();
  return buf;
}

// Renders the fullscreen unit quad via DrawIndexedIndirect with a fresh
// indirect-args buffer holding `instance_count`; returns the target's centre
// pixel.
CpuImage::Color RenderIndirect(TestGpu& g, wgpu::Buffer vbuf, wgpu::Buffer ibuf,
                               const CompiledPipeline& pipeline,
                               uint32_t instance_count) {
  wgpu::Buffer indirect_buf = UploadIndirectArgs(g.device, instance_count);

  ColorRenderTarget target(g.device, kSize, kSize, kColorFormat);
  REQUIRE(target.IsValid());

  wgpu::CommandEncoder enc = g.device.CreateCommandEncoder();
  {
    wgpu::RenderPassColorAttachment ca{};
    ca.view = target.GetView();
    ca.loadOp = wgpu::LoadOp::Clear;
    ca.storeOp = wgpu::StoreOp::Store;
    ca.clearValue = {0.0, 0.0, 0.0, 1.0};
    ca.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
    wgpu::RenderPassDescriptor desc{};
    desc.colorAttachmentCount = 1;
    desc.colorAttachments = &ca;
    RenderPassContext pass(enc.BeginRenderPass(&desc));
    pass.SetPipeline(pipeline.pipeline);
    pass.SetVertexBuffer(0, vbuf);
    pass.SetIndexBuffer(ibuf, wgpu::IndexFormat::Uint32);
    pass.DrawIndexedIndirect(indirect_buf, 0);
    pass.End();
  }
  wgpu::CommandBuffer cmd = enc.Finish();
  g.queue.Submit(1, &cmd);
  test::WaitForGpu(g.instance, g.device, g.queue);

  TextureReadback rb(g.instance, g.device, g.queue);
  CpuImage img =
      rb.ReadTextureSync(target.GetTexture(), kSize, kSize, kColorFormat);
  return img.GetPixel(kSize / 2, kSize / 2);
}

}  // namespace

TEST_CASE("DrawIndexedIndirect honors the GPU-read instanceCount",
          "[gpu_instance][gpu]") {
  TestGpu& g = GetTestGpu();

  auto pipeline = g.gen->GetPipeline(
      RenderPipelineDeclaration{.shader_path = "tests/indirect_draw_test",
                                .vertex_layout = VertexLayout::kPos2d},
      {kColorFormat});
  REQUIRE(pipeline != nullptr);

  // A single unit quad covering the whole clip-space viewport (kPos2d: pos(vec2)
  // only, already in clip space per the shader's vs_main).
  constexpr std::array<float, 8> kQuadVerts = {
      -1.0f, -1.0f, 1.0f, -1.0f, 1.0f, 1.0f, -1.0f, 1.0f,
  };
  wgpu::BufferDescriptor vbd{};
  vbd.size = kQuadVerts.size() * sizeof(float);
  vbd.usage = wgpu::BufferUsage::Vertex | wgpu::BufferUsage::CopyDst;
  vbd.mappedAtCreation = true;
  wgpu::Buffer vbuf = g.device.CreateBuffer(&vbd);
  std::memcpy(vbuf.GetMappedRange(0, vbd.size), kQuadVerts.data(), vbd.size);
  vbuf.Unmap();

  constexpr std::array<uint32_t, 6> kQuadIndices = {0, 1, 2, 0, 2, 3};
  wgpu::BufferDescriptor ibd{};
  ibd.size = kQuadIndices.size() * sizeof(uint32_t);
  ibd.usage = wgpu::BufferUsage::Index | wgpu::BufferUsage::CopyDst;
  ibd.mappedAtCreation = true;
  wgpu::Buffer ibuf = g.device.CreateBuffer(&ibd);
  std::memcpy(ibuf.GetMappedRange(0, ibd.size), kQuadIndices.data(), ibd.size);
  ibuf.Unmap();

  // instanceCount=1, read from the indirect buffer (not a CPU Draw call
  // argument): the quad IS drawn.
  CpuImage::Color drawn = RenderIndirect(g, vbuf, ibuf, *pipeline, 1);
  INFO("instanceCount=1 rgb = " << (int)drawn.r << "," << (int)drawn.g << ","
                                << (int)drawn.b);
  CHECK(drawn.r > 250);
  CHECK(drawn.g > 250);
  CHECK(drawn.b > 250);

  // instanceCount=0: nothing is drawn — pixel stays the clear colour. Proves
  // the GPU reads the count from the buffer, not the CPU.
  CpuImage::Color not_drawn = RenderIndirect(g, vbuf, ibuf, *pipeline, 0);
  INFO("instanceCount=0 rgb = " << (int)not_drawn.r << ","
                                << (int)not_drawn.g << "," << (int)not_drawn.b);
  CHECK(not_drawn.r < 5);
  CHECK(not_drawn.g < 5);
  CHECK(not_drawn.b < 5);
}

// ===========================================================================
// Phase B: instanced materials, resolved through MaterialInstanceCache. Two
// materials (an instanced G-buffer material + an instanced forward material)
// each read their per-object transform from a group-1 storage array indexed
// by @builtin(instance_index), with material constants in a group-0 UBO. Both
// are rendered with a plain instanced DrawIndexed(indexCount, instanceCount=N)
// and verified by GPU readback: N instances land at N distinct screen
// positions.
// ===========================================================================
namespace {

constexpr uint32_t kInstTarget = 64;
constexpr uint32_t kInstCount = 4;

// A trivial orthographic frame: clip.x = 0.5*worldX, clip.y = 0.5*worldY,
// clip.z = 0.5 (const, never Z-clipped; the renders use no depth attachment),
// with camera at the origin (view = identity, cameraWorldPos = 0), so a world
// XY of +/-2 maps to the full [-1,1] clip range. Ambient SH L0 is set bright so
// the forward material shades to a clearly non-clear color regardless of the
// sun/shadow/view angle.
UniformData MakeOrthoFrame() {
  UniformData u{};
  glm::mat4 proj(0.0f);
  proj[0][0] = 0.5f;
  proj[1][1] = 0.5f;
  proj[3][2] = 0.5f;  // clip.z = 0.5 for every vertex
  proj[3][3] = 1.0f;
  u.view = glm::mat4(1.0f);
  u.proj = proj;
  u.view_prev = glm::mat4(1.0f);
  u.proj_prev = proj;
  u.light_view_proj = glm::mat4(1.0f);
  u.camera_world_pos = glm::vec4(0.0f);
  u.sunDir = glm::vec4(glm::normalize(glm::vec3(0.3f, 0.8f, 0.5f)), 0.0f);
  u.sunColor = glm::vec4(1.0f, 1.0f, 1.0f, 0.0f);
  u.ambient_sh[0] = glm::vec4(30.0f, 30.0f, 30.0f, 0.0f);
  u.near_plane = 0.1f;
  u.far_plane = 100.0f;
  u.screen_size = glm::vec2(float(kInstTarget), float(kInstTarget));
  u.output_is_linear = 1u;
  return u;
}

// N instance world transforms at the 4 quadrant centers, each a 0.35-scaled
// quad pushed to z=-8 (so the forward material's view vector aligns with the
// +Z quad normal). Clip centers land at (+/-0.5, +/-0.5) -> the 4 quadrants.
std::vector<glm::mat4> MakeInstanceTransforms() {
  const glm::vec2 centers[kInstCount] = {
      {-1.0f, -1.0f}, {1.0f, -1.0f}, {1.0f, 1.0f}, {-1.0f, 1.0f}};
  std::vector<glm::mat4> out;
  for (const auto& c : centers) {
    out.push_back(glm::translate(glm::mat4(1.0f), glm::vec3(c.x, c.y, -8.0f)) *
                  glm::scale(glm::mat4(1.0f), glm::vec3(0.35f)));
  }
  return out;
}

// Expected framebuffer center of each instance's quad (WebGPU y-down):
//   fb = ((clip.xy * 0.5) + 0.5) * size, with the y axis flipped.
std::array<std::pair<uint32_t, uint32_t>, kInstCount> ExpectedPixels() {
  const glm::vec2 centers[kInstCount] = {
      {-1.0f, -1.0f}, {1.0f, -1.0f}, {1.0f, 1.0f}, {-1.0f, 1.0f}};
  std::array<std::pair<uint32_t, uint32_t>, kInstCount> px{};
  for (uint32_t i = 0; i < kInstCount; ++i) {
    float clip_x = 0.5f * centers[i].x;
    float clip_y = 0.5f * centers[i].y;
    px[i].first = uint32_t((clip_x * 0.5f + 0.5f) * float(kInstTarget));
    px[i].second = uint32_t((1.0f - (clip_y * 0.5f + 0.5f)) * float(kInstTarget));
  }
  return px;
}

// One kTexturedMesh quad (pos3+uv2+normal3+tangent3), z=0, +Z normal.
std::vector<float> QuadVertices() {
  return {
      -1, -1, 0, 0, 0, 0, 0, 1, 1, 0, 0,  //
      1,  -1, 0, 1, 0, 0, 0, 1, 1, 0, 0,  //
      1,  1,  0, 1, 1, 0, 0, 1, 1, 0, 0,  //
      -1, 1,  0, 0, 1, 0, 0, 1, 1, 0, 0,  //
  };
}

wgpu::Buffer UploadBuffer(wgpu::Device device, const void* data, uint64_t size,
                          wgpu::BufferUsage usage) {
  wgpu::BufferDescriptor bd{};
  bd.size = size;
  bd.usage = usage | wgpu::BufferUsage::CopyDst;
  bd.mappedAtCreation = true;
  wgpu::Buffer buf = device.CreateBuffer(&bd);
  std::memcpy(buf.GetMappedRange(0, size), data, size);
  buf.Unmap();
  return buf;
}

wgpu::TextureView SolidCube1x1(wgpu::Device device, wgpu::Queue queue,
                               uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  wgpu::TextureDescriptor desc{};
  desc.size = {1, 1, 6};
  desc.format = wgpu::TextureFormat::RGBA8Unorm;
  desc.usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;
  desc.dimension = wgpu::TextureDimension::e2D;
  wgpu::Texture tex = device.CreateTexture(&desc);
  uint8_t px[] = {r, g, b, a};
  for (uint32_t face = 0; face < 6; ++face) {
    wgpu::TexelCopyBufferLayout layout{};
    layout.bytesPerRow = 4;
    layout.rowsPerImage = 1;
    wgpu::TexelCopyTextureInfo dst{};
    dst.texture = tex;
    dst.origin = {0, 0, face};
    wgpu::Extent3D extent = {1, 1, 1};
    queue.WriteTexture(&dst, px, sizeof(px), &layout, &extent);
  }
  wgpu::TextureViewDescriptor vd{};
  vd.dimension = wgpu::TextureViewDimension::Cube;
  vd.arrayLayerCount = 6;
  vd.format = wgpu::TextureFormat::RGBA8Unorm;
  return tex.CreateView(&vd);
}

// Build the instanced factory for one MaterialPassType. `depth_format`
// defaults to Undefined (no depth attachment) for the render tests below,
// which stand up only the color-only pass they draw. `casts_shadow` defaults
// to false (no kShadow variant); the shadow-compile tests further down pass
// casts_shadow=true + depth_format=Depth32Float (matching the real
// GBuffer::kDepthFormat / SceneRenderer::kDepthFormat / shadow_map.cpp depth
// format the engine actually uses) so the kShadow variant's pipeline gets a
// real depth-stencil attachment instead of the zero-attachment descriptor
// Dawn rejects — see standard_material_factory.cpp's kVariants gating of
// RenderPassType::kShadow on FactoryDescriptor::casts_shadow.
std::unique_ptr<MaterialInstanceFactory> MakeInstancedFactory(
    TestGpu& g, const std::string& shader_name, MaterialPassType pass,
    RenderTargetFormats color_formats, std::vector<std::string> extra_features,
    bool casts_shadow = false,
    wgpu::TextureFormat depth_format = wgpu::TextureFormat::Undefined) {
  FactoryDescriptor desc;
  desc.shader_name = shader_name;
  desc.shader_path = "material/" + shader_name;
  desc.supported_geometry_types = {GeometryType::kInstancedMesh};
  desc.supported_pass_types = {pass};
  desc.color_formats = std::move(color_formats);
  desc.depth_format = depth_format;
  desc.casts_shadow = casts_shadow;
  desc.cull_mode = wgpu::CullMode::None;  // double-sided; winding-independent
  desc.extra_features = std::move(extra_features);
  return BuildMaterialInstanceFactory(desc, g.device, g.queue, g.gen.get());
}

wgpu::RenderPassColorAttachment ClearAttachment(wgpu::TextureView view) {
  wgpu::RenderPassColorAttachment ca{};
  ca.view = view;
  ca.loadOp = wgpu::LoadOp::Clear;
  ca.storeOp = wgpu::StoreOp::Store;
  ca.clearValue = {0.0, 0.0, 0.0, 1.0};
  ca.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
  return ca;
}

}  // namespace

TEST_CASE("instanced forward material renders N instances at N screen positions",
          "[gpu_instance][gpu]") {
  TestGpu& g = GetTestGpu();

  auto factory = MakeInstancedFactory(g, "instanced_forward",
                                      MaterialPassType::kForwardOpaque,
                                      {wgpu::TextureFormat::BGRA8Unorm},
                                      {"translucency"});
  REQUIRE(factory != nullptr);

  MaterialInstanceCache cache;
  InstanceParams params;
  params.uniform_overrides["tint"] =
      MaterialParameterValue(glm::vec4(1.0f, 1.0f, 1.0f, 1.0f));
  params.uniform_overrides["params"] =
      MaterialParameterValue(glm::vec4(0.0f, 1.0f, 0.0f, 0.0f));  // cutoff/rough
  params.uniform_overrides["transmission"] =
      MaterialParameterValue(glm::vec4(0.0f, 0.0f, 0.0f, 0.0f));

  entt::id_type key = ComposeMaterialCacheKey(
      entt::hashed_string{"instanced_forward"}.value(),
      GeometryType::kInstancedMesh, RenderPassType::kForward, 0);
  auto handle = cache.GetOrCreate(key, *factory, GeometryType::kInstancedMesh,
                                  MaterialPassType::kForwardOpaque,
                                  RenderPassType::kForward, params);
  REQUIRE(handle);
  REQUIRE(handle->IsValid());
  auto* inst = handle.operator->();

  // Material-instance integration: instanced geometry, a group-1 storage
  // binding, the group-2 engine set, and a settable group-0 params UBO.
  CHECK(inst->GetGeometryType() == GeometryType::kInstancedMesh);
  CHECK(inst->DeclaresBindGroup(1));
  CHECK(inst->DeclaresBindGroup(2));
  CHECK(inst->GetParameterId("tint").IsValid());

  // Mesh + per-instance storage.
  std::vector<float> verts = QuadVertices();
  std::array<uint32_t, 6> idx = {0, 1, 2, 0, 2, 3};
  std::vector<glm::mat4> xforms = MakeInstanceTransforms();
  wgpu::Buffer vbuf = UploadBuffer(g.device, verts.data(),
                                   verts.size() * sizeof(float),
                                   wgpu::BufferUsage::Vertex);
  wgpu::Buffer ibuf = UploadBuffer(g.device, idx.data(), sizeof(idx),
                                   wgpu::BufferUsage::Index);
  wgpu::Buffer instbuf =
      UploadBuffer(g.device, xforms.data(), xforms.size() * sizeof(glm::mat4),
                   wgpu::BufferUsage::Storage);

  // Trivial group-2 engine resources (shadow map + IBL).
  wgpu::TextureDescriptor sd{};
  sd.size = {1, 1, 1};
  sd.format = wgpu::TextureFormat::Depth32Float;
  sd.usage = wgpu::TextureUsage::TextureBinding;
  wgpu::Texture shadow_tex = g.device.CreateTexture(&sd);
  wgpu::TextureViewDescriptor shadow_vd{};
  shadow_vd.aspect = wgpu::TextureAspect::DepthOnly;
  wgpu::TextureView shadow_view = shadow_tex.CreateView(&shadow_vd);
  wgpu::SamplerDescriptor cmp{};
  cmp.compare = wgpu::CompareFunction::LessEqual;
  wgpu::Sampler shadow_sampler = g.device.CreateSampler(&cmp);
  wgpu::TextureView ibl_cube = SolidCube1x1(g.device, g.queue, 255, 255, 255, 255);
  wgpu::Sampler linear_sampler = g.device.CreateSampler(nullptr);
  wgpu::TextureView brdf_lut =
      test::CreateRgbaTexture(g.device, g.queue, 1, 1, {255, 255, 0, 255})
          .CreateView();

  UniformData frame_uniforms = MakeOrthoFrame();
  FrameContext frame;
  frame.Begin(g.device, g.queue, frame_uniforms);

  ColorRenderTarget target(g.device, kInstTarget, kInstTarget,
                           wgpu::TextureFormat::BGRA8Unorm);
  REQUIRE(target.IsValid());

  {
    wgpu::RenderPassColorAttachment ca = ClearAttachment(target.GetView());
    wgpu::RenderPassDescriptor desc{};
    desc.colorAttachmentCount = 1;
    desc.colorAttachments = &ca;
    RenderPassContext pass = frame.BeginRenderPass(desc);

    REQUIRE(inst->Bind(pass, frame));

    std::array<wgpu::BindGroupEntry, 6> g2{};
    g2[0].binding = 0; g2[0].textureView = shadow_view;
    g2[1].binding = 1; g2[1].sampler = shadow_sampler;
    g2[2].binding = 2; g2[2].textureView = ibl_cube;
    g2[3].binding = 3; g2[3].sampler = linear_sampler;
    g2[4].binding = 4; g2[4].textureView = brdf_lut;
    g2[5].binding = 5; g2[5].sampler = linear_sampler;
    wgpu::BindGroup g2bg =
        frame.CreateBindGroup(inst->GetPipeline().GetBindGroupLayout(2), g2);
    pass.SetBindGroup(2, g2bg);

    REQUIRE(inst->BindInstanceData(pass, frame, instbuf, 0));

    pass.SetVertexBuffer(0, vbuf);
    pass.SetIndexBuffer(ibuf, wgpu::IndexFormat::Uint32);
    pass.DrawIndexed(6, kInstCount);
    pass.End();
  }

  wgpu::CommandBuffer cmd = frame.End();
  g.queue.Submit(1, &cmd);
  test::WaitForGpu(g.instance, g.device, g.queue);

  TextureReadback rb(g.instance, g.device, g.queue);
  CpuImage img = rb.ReadTextureSync(target.GetTexture(), kInstTarget,
                                    kInstTarget, wgpu::TextureFormat::BGRA8Unorm);

  auto expected = ExpectedPixels();
  for (uint32_t i = 0; i < kInstCount; ++i) {
    CpuImage::Color c = img.GetPixel(expected[i].first, expected[i].second);
    INFO("instance " << i << " at (" << expected[i].first << ","
                     << expected[i].second << ") rgb = " << (int)c.r << ","
                     << (int)c.g << "," << (int)c.b);
    CHECK(c.r > 180);
    CHECK(c.g > 180);
    CHECK(c.b > 180);
  }
  // The gap between the four quadrant quads stays the clear color.
  CpuImage::Color center = img.GetPixel(kInstTarget / 2, kInstTarget / 2);
  INFO("center rgb = " << (int)center.r << "," << (int)center.g << ","
                       << (int)center.b);
  CHECK(center.r < 40);
  CHECK(center.g < 40);
  CHECK(center.b < 40);
}

TEST_CASE("instanced G-buffer material renders N instances at N screen positions",
          "[gpu_instance][gpu]") {
  TestGpu& g = GetTestGpu();

  auto factory = MakeInstancedFactory(
      g, "instanced_gbuffer", MaterialPassType::kDeferred,
      {GBuffer::kNormalsFormat, GBuffer::kAlbedoFormat, GBuffer::kMaterialFormat},
      {});
  REQUIRE(factory != nullptr);

  MaterialInstanceCache cache;
  InstanceParams params;
  params.uniform_overrides["tint"] =
      MaterialParameterValue(glm::vec4(1.0f, 1.0f, 1.0f, 1.0f));

  entt::id_type key = ComposeMaterialCacheKey(
      entt::hashed_string{"instanced_gbuffer"}.value(),
      GeometryType::kInstancedMesh, RenderPassType::kGBuffer, 0);
  auto handle = cache.GetOrCreate(key, *factory, GeometryType::kInstancedMesh,
                                  MaterialPassType::kDeferred,
                                  RenderPassType::kGBuffer, params);
  REQUIRE(handle);
  REQUIRE(handle->IsValid());
  auto* inst = handle.operator->();

  CHECK(inst->GetGeometryType() == GeometryType::kInstancedMesh);
  CHECK(inst->DeclaresBindGroup(1));       // per-instance storage array
  CHECK_FALSE(inst->DeclaresBindGroup(2));  // deferred variant has no group 2
  CHECK(inst->GetParameterId("tint").IsValid());

  std::vector<float> verts = QuadVertices();
  std::array<uint32_t, 6> idx = {0, 1, 2, 0, 2, 3};
  std::vector<glm::mat4> xforms = MakeInstanceTransforms();
  wgpu::Buffer vbuf = UploadBuffer(g.device, verts.data(),
                                   verts.size() * sizeof(float),
                                   wgpu::BufferUsage::Vertex);
  wgpu::Buffer ibuf = UploadBuffer(g.device, idx.data(), sizeof(idx),
                                   wgpu::BufferUsage::Index);
  wgpu::Buffer instbuf =
      UploadBuffer(g.device, xforms.data(), xforms.size() * sizeof(glm::mat4),
                   wgpu::BufferUsage::Storage);

  UniformData frame_uniforms = MakeOrthoFrame();
  FrameContext frame;
  frame.Begin(g.device, g.queue, frame_uniforms);

  // G-buffer targets. Only the albedo target (BGRA8) is read back; its value is
  // deterministic (albedo texture * tint, no lighting).
  ColorRenderTarget normals_t(g.device, kInstTarget, kInstTarget,
                              GBuffer::kNormalsFormat);
  ColorRenderTarget albedo_t(g.device, kInstTarget, kInstTarget,
                             GBuffer::kAlbedoFormat);
  ColorRenderTarget material_t(g.device, kInstTarget, kInstTarget,
                               GBuffer::kMaterialFormat);
  REQUIRE(albedo_t.IsValid());

  {
    std::array<wgpu::RenderPassColorAttachment, 3> ca = {
        ClearAttachment(normals_t.GetView()),
        ClearAttachment(albedo_t.GetView()),
        ClearAttachment(material_t.GetView())};
    wgpu::RenderPassDescriptor desc{};
    desc.colorAttachmentCount = ca.size();
    desc.colorAttachments = ca.data();
    RenderPassContext pass = frame.BeginRenderPass(desc);

    REQUIRE(inst->Bind(pass, frame));
    REQUIRE(inst->BindInstanceData(pass, frame, instbuf, 0));
    pass.SetVertexBuffer(0, vbuf);
    pass.SetIndexBuffer(ibuf, wgpu::IndexFormat::Uint32);
    pass.DrawIndexed(6, kInstCount);
    pass.End();
  }

  wgpu::CommandBuffer cmd = frame.End();
  g.queue.Submit(1, &cmd);
  test::WaitForGpu(g.instance, g.device, g.queue);

  TextureReadback rb(g.instance, g.device, g.queue);
  CpuImage img = rb.ReadTextureSync(albedo_t.GetTexture(), kInstTarget,
                                    kInstTarget, GBuffer::kAlbedoFormat);

  auto expected = ExpectedPixels();
  for (uint32_t i = 0; i < kInstCount; ++i) {
    CpuImage::Color c = img.GetPixel(expected[i].first, expected[i].second);
    INFO("instance " << i << " at (" << expected[i].first << ","
                     << expected[i].second << ") rgb = " << (int)c.r << ","
                     << (int)c.g << "," << (int)c.b);
    CHECK(c.r > 180);
    CHECK(c.g > 180);
    CHECK(c.b > 180);
  }
  CpuImage::Color center = img.GetPixel(kInstTarget / 2, kInstTarget / 2);
  INFO("center rgb = " << (int)center.r << "," << (int)center.g << ","
                       << (int)center.b);
  CHECK(center.r < 40);
  CHECK(center.g < 40);
  CHECK(center.b < 40);
}

// ===========================================================================
// Phase B, shadow-compile proof: the render tests above build their factories
// with casts_shadow=false, so the kShadow variant of instanced_forward.wesl /
// instanced_gbuffer.wesl is never compiled by them — a WGSL error in either
// shader's `@if(shadow_pass)` VS (e.g. still reading a removed per-object
// uniform instead of `instances[instance_index]`) would ship undetected.
// These two tests build the factory with casts_shadow=true and resolve a
// RenderPassType::kShadow instance through MaterialInstanceCache.
//
// A non-null/IsValid() handle alone does NOT prove this: on this Dawn build,
// a WGSL validation failure (confirmed by deliberately breaking the shadow VS
// during development of this test) still yields a non-null wgpu::ShaderModule
// / wgpu::RenderPipeline — Dawn's synchronous Create* calls return an
// internally-invalid "error object" rather than null, surfacing the failure
// only via the device's error-reporting channel — so
// GpuPipelineGenerator::GetPipeline's `if (!shader_module)` / `if (!pipeline)`
// null checks don't catch it, and CreateInstance/GetOrCreate happily hand
// back a "valid" instance wrapping a broken pipeline. The real proof is a
// pushed Dawn validation error scope around the compile: NoError observed
// means the shadow pipeline genuinely compiled + reflected.
// ===========================================================================

namespace {

struct CapturedError {
  wgpu::ErrorType type = wgpu::ErrorType::NoError;
  std::string message;
};

// Runs `fn` (expected to trigger lazy pipeline compilation) inside a Dawn
// validation-error scope and returns what it observed. See the file comment
// above for why this — not a non-null/IsValid() check alone — is what
// actually proves shader compilation succeeded.
CapturedError RunCapturingValidationErrors(TestGpu& g,
                                           const std::function<void()>& fn) {
  g.device.PushErrorScope(wgpu::ErrorFilter::Validation);
  fn();

  CapturedError result;
  bool done = false;
  g.device.PopErrorScope(
      wgpu::CallbackMode::AllowProcessEvents,
      [&](wgpu::PopErrorScopeStatus status, wgpu::ErrorType type,
          wgpu::StringView message) {
        if (status == wgpu::PopErrorScopeStatus::Success) {
          result.type = type;
          result.message = message.length > 0
                                ? std::string(message.data, message.length)
                                : std::string();
        }
        done = true;
      });
  while (!done) {
    g.instance.ProcessEvents();
  }
  return result;
}

}  // namespace

TEST_CASE("instanced forward material's shadow-pass variant compiles",
          "[gpu_instance][gpu]") {
  TestGpu& g = GetTestGpu();

  auto factory = MakeInstancedFactory(g, "instanced_forward",
                                      MaterialPassType::kForwardOpaque,
                                      {wgpu::TextureFormat::BGRA8Unorm}, {},
                                      /*casts_shadow=*/true,
                                      wgpu::TextureFormat::Depth32Float);
  REQUIRE(factory != nullptr);

  MaterialInstanceCache cache;
  entt::id_type key = ComposeMaterialCacheKey(
      entt::hashed_string{"instanced_forward_shadow"}.value(),
      GeometryType::kInstancedMesh, RenderPassType::kShadow, 0);

  entt::resource<RenderingMaterialInstance> handle;
  CapturedError err = RunCapturingValidationErrors(g, [&] {
    handle = cache.GetOrCreate(key, *factory, GeometryType::kInstancedMesh,
                              MaterialPassType::kForwardOpaque,
                              RenderPassType::kShadow, InstanceParams{});
  });
  INFO("Dawn validation error: " << err.message);
  CHECK(err.type == wgpu::ErrorType::NoError);
  REQUIRE(handle);
  REQUIRE(handle->IsValid());
}

TEST_CASE("instanced G-buffer material's shadow-pass variant compiles",
          "[gpu_instance][gpu]") {
  TestGpu& g = GetTestGpu();

  auto factory = MakeInstancedFactory(
      g, "instanced_gbuffer", MaterialPassType::kDeferred,
      {GBuffer::kNormalsFormat, GBuffer::kAlbedoFormat, GBuffer::kMaterialFormat},
      {}, /*casts_shadow=*/true, wgpu::TextureFormat::Depth32Float);
  REQUIRE(factory != nullptr);

  MaterialInstanceCache cache;
  entt::id_type key = ComposeMaterialCacheKey(
      entt::hashed_string{"instanced_gbuffer_shadow"}.value(),
      GeometryType::kInstancedMesh, RenderPassType::kShadow, 0);

  entt::resource<RenderingMaterialInstance> handle;
  CapturedError err = RunCapturingValidationErrors(g, [&] {
    handle = cache.GetOrCreate(key, *factory, GeometryType::kInstancedMesh,
                              MaterialPassType::kDeferred,
                              RenderPassType::kShadow, InstanceParams{});
  });
  INFO("Dawn validation error: " << err.message);
  CHECK(err.type == wgpu::ErrorType::NoError);
  REQUIRE(handle);
  REQUIRE(handle->IsValid());
}

// ===========================================================================
// Phase C: compute frustum-cull + compaction feeding one indirect instanced
// draw (GpuInstanceRenderer, shaders/compute/instance_cull.wesl). Building on
// Phase A's atomics/indirect-draw/BufferReadback primitives and Phase B's
// instanced materials: a compute pass tests each instance's world-space
// bounding sphere against the camera frustum, atomically compacts survivors'
// transforms, and one DrawIndexedIndirect renders exactly those survivors
// with the Phase-B instanced_gbuffer material.
//
// Test 1 (gate): does compute/instance_cull compile and reflect the expected
// group-0 uniform + group-1 storage bindings (read-only instances, writable
// compacted + args)?
// Test 2: cull correctness via buffer readback -- the compacted transforms +
// count exactly match an independently-computed (CPU) in-frustum set,
// including a sphere straddling a plane.
// Test 3-5: end-to-end + edge cases via framebuffer readback -- only
// surviving instances actually draw.
// ===========================================================================

TEST_CASE("compute/instance_cull compiles and reflects the expected bindings",
          "[gpu_instance][gpu]") {
  TestGpu& g = GetTestGpu();

  // A private pipeline generator (NOT the shared TestGpu::gen) so this
  // compile is guaranteed to be a genuine first-time compile under the Dawn
  // validation-error scope below, regardless of what other tests in this
  // binary already cached against the shared generator (Catch2's default
  // test order is lexicographic by name, not declaration order, so another
  // test's GpuInstanceRenderer may otherwise have compiled + cached this
  // pipeline first, unwrapped). A non-null/IsValid() check alone does NOT
  // prove a WGSL compile succeeded on this Dawn build -- see
  // RunCapturingValidationErrors' comment above (established in Phase B).
  GpuPipelineGenerator fresh_gen(g.device, FindShaderDirectory());

  std::shared_ptr<const CompiledComputePipeline> pipeline;
  CapturedError err = RunCapturingValidationErrors(g, [&] {
    pipeline = fresh_gen.GetComputePipeline("compute/instance_cull");
  });
  INFO("Dawn validation error: " << err.message);
  CHECK(err.type == wgpu::ErrorType::NoError);
  REQUIRE(pipeline != nullptr);
  REQUIRE(pipeline->pipeline != nullptr);
  CHECK(pipeline->workgroup_size[0] == 64);
  REQUIRE(pipeline->bind_group_layouts.size() == 2);

  auto find_binding = [&](uint32_t group,
                          uint32_t binding) -> const ReflectedBinding* {
    for (const auto& b : pipeline->reflected_bindings) {
      if (b.group == group && b.binding == binding) return &b;
    }
    return nullptr;
  };
  const ReflectedBinding* frustum_b = find_binding(0, 0);
  const ReflectedBinding* instances_b = find_binding(1, 0);
  const ReflectedBinding* compacted_b = find_binding(1, 1);
  const ReflectedBinding* args_b = find_binding(1, 2);
  REQUIRE(frustum_b != nullptr);
  REQUIRE(instances_b != nullptr);
  REQUIRE(compacted_b != nullptr);
  REQUIRE(args_b != nullptr);

  CHECK(frustum_b->name == "frustum");
  CHECK(frustum_b->buffer_type == wgpu::BufferBindingType::Uniform);
  CHECK(instances_b->name == "instances");
  CHECK(instances_b->buffer_type == wgpu::BufferBindingType::ReadOnlyStorage);
  CHECK(compacted_b->name == "compacted");
  CHECK(compacted_b->buffer_type == wgpu::BufferBindingType::Storage);
  CHECK(args_b->name == "args");
  CHECK(args_b->buffer_type == wgpu::BufferBindingType::Storage);
}

namespace {

// Sphere-vs-frustum test mirroring instance_cull.wesl's `sphereCulled`
// EXACTLY (see the phase-C report for the derivation from
// Frustum::Intersects' AABB "positive vertex" test): a sphere is culled iff
// it is fully outside any single plane's half-space,
// dot(plane.xyz, center) + plane.w < -radius. Used to compute the expected
// in/out set independently of the GPU.
bool ExpectedCulled(const Frustum& f, glm::vec3 center, float radius) {
  for (const glm::vec4& p : f.planes) {
    if (glm::dot(glm::vec3(p), center) + p.w < -radius) {
      return true;
    }
  }
  return false;
}

bool Mat4Equal(const glm::mat4& a, const glm::mat4& b) {
  return std::memcmp(&a, &b, sizeof(glm::mat4)) == 0;
}

}  // namespace

TEST_CASE(
    "GPU cull correctness: compacted buffer + count match the in-frustum set",
    "[gpu_instance][gpu]") {
  TestGpu& g = GetTestGpu();

  // Camera at the origin looking down -Z (fov 90, aspect 1, near 0.5, far
  // 100) -- the same setup frustum_tests.cpp's CPU-only test uses, so the
  // expected in/out set below can be cross-checked against that known-good
  // CPU behavior.
  Camera camera;
  camera.position = glm::vec3(0.0f);
  camera.direction = glm::vec3(0.0f, 0.0f, -1.0f);
  camera.up = glm::vec3(0.0f, 1.0f, 0.0f);
  camera.fov = 90.0f;
  camera.aspect = 1.0f;
  camera.near_plane = 0.5f;
  camera.far_plane = 100.0f;
  const Frustum frustum =
      Frustum::FromViewProj(camera.GetProj() * camera.GetView());

  struct Case {
    glm::vec3 center;
    float radius;
    const char* label;
  };
  const std::vector<Case> cases = {
      {{0.0f, 0.0f, -10.0f}, 1.0f, "straight ahead"},
      {{-100.0f, 0.0f, -10.0f}, 1.0f, "far to the left"},
      {{100.0f, 0.0f, -10.0f}, 1.0f, "far to the right"},
      {{0.0f, 0.0f, 10.0f}, 1.0f, "behind the camera"},
      {{0.0f, 0.0f, -1000.0f}, 1.0f, "beyond the far plane"},
      {{0.0f, 5.0f, -20.0f}, 2.0f, "up and ahead"},
      {{-11.0f, 0.0f, -10.0f}, 2.0f, "straddling the left plane"},
  };

  // Sanity-check the straddling case actually straddles: a bare point there
  // (radius 0) is outside, but the sphere's radius reaches back into the
  // frustum -- exercising the "< -radius", not "< 0", comparison.
  REQUIRE(ExpectedCulled(frustum, cases[6].center, 0.0f));
  REQUIRE_FALSE(ExpectedCulled(frustum, cases[6].center, cases[6].radius));

  std::vector<glm::mat4> expected_visible_transforms;
  std::vector<GpuInstanceRenderer::InstanceInput> inputs;
  uint32_t expected_count = 0;
  for (const Case& c : cases) {
    GpuInstanceRenderer::InstanceInput in;
    in.transform = glm::translate(glm::mat4(1.0f), c.center);
    in.bounds_sphere = glm::vec4(c.center, c.radius);
    inputs.push_back(in);
    const bool culled = ExpectedCulled(frustum, c.center, c.radius);
    INFO("case '" << c.label << "' culled=" << culled);
    if (!culled) {
      expected_visible_transforms.push_back(in.transform);
      ++expected_count;
    }
  }

  GpuInstanceRenderer renderer(g.device, g.queue, *g.gen,
                              static_cast<uint32_t>(cases.size()));
  REQUIRE(renderer.IsValid());
  renderer.UploadInstances(inputs);

  FrameContext frame;
  frame.Begin(g.device, g.queue, UniformData{});
  renderer.Cull(frame, camera);
  wgpu::CommandBuffer cmd = frame.End();
  g.queue.Submit(1, &cmd);
  test::WaitForGpu(g.instance, g.device, g.queue);

  std::vector<uint32_t> gpu_count = test::ReadBufferSync<uint32_t>(
      g.instance, g.device, g.queue, renderer.GetArgsBuffer(),
      GpuInstanceRenderer::kArgsInstanceCountOffset, 1);
  REQUIRE(gpu_count.size() == 1);
  INFO("expected count = " << expected_count << ", GPU count = " << gpu_count[0]);
  CHECK(gpu_count[0] == expected_count);

  std::vector<glm::mat4> compacted = test::ReadBufferSync<glm::mat4>(
      g.instance, g.device, g.queue, renderer.GetCompactedBuffer(), 0,
      gpu_count[0]);
  REQUIRE(compacted.size() == gpu_count[0]);

  // Order-independent set match: every expected survivor's transform appears
  // exactly once among the compacted transforms (atomic append order isn't
  // fixed across dispatches).
  std::vector<bool> matched(compacted.size(), false);
  for (const glm::mat4& expected : expected_visible_transforms) {
    bool found = false;
    for (size_t i = 0; i < compacted.size(); ++i) {
      if (!matched[i] && Mat4Equal(compacted[i], expected)) {
        matched[i] = true;
        found = true;
        break;
      }
    }
    CHECK(found);
  }
}

// ---------------------------------------------------------------------------
// End-to-end (framebuffer readback) + edge cases: render the culled/compacted
// survivors via GpuInstanceRenderer::Draw with the Phase-B instanced_gbuffer
// material, and read back the albedo target.
// ---------------------------------------------------------------------------
namespace {

constexpr uint32_t kCullQuadCount = 4;
// World-space quad centers along X at a fixed depth, all comfortably within
// the ortho test frame's on-screen range (|x| <= 2 -- see MakeOrthoFrame
// above) so any missing from the rendered image were genuinely removed by
// the cull compute, not simply off-screen.
const std::array<glm::vec2, kCullQuadCount> kCullQuadCenters = {
    glm::vec2{-1.5f, 0.0f}, glm::vec2{-0.5f, 0.0f}, glm::vec2{0.5f, 0.0f},
    glm::vec2{1.5f, 0.0f}};
constexpr float kCullQuadZ = -10.0f;
constexpr float kCullQuadScale = 0.2f;
constexpr float kCullQuadRadius = 0.3f;

// A camera at the origin looking down -Z; `aspect` alone controls the
// visible half-width at kCullQuadZ (= |z| * aspect * tan(fovy/2), and
// fovy=90 here makes tan(fovy/2)=1) so each test picks a single aspect to
// get a mixed / all-in / all-out cull outcome against kCullQuadCenters.
Camera MakeCullTestCamera(float aspect) {
  Camera camera;
  camera.position = glm::vec3(0.0f);
  camera.direction = glm::vec3(0.0f, 0.0f, -1.0f);
  camera.up = glm::vec3(0.0f, 1.0f, 0.0f);
  camera.fov = 90.0f;
  camera.aspect = aspect;
  camera.near_plane = 0.1f;
  camera.far_plane = 100.0f;
  return camera;
}

// Expected framebuffer center for a world XY under MakeOrthoFrame's trivial
// projection (clip = 0.5*world), WebGPU y-down -- same formula as Phase B's
// ExpectedPixels above, generalized to an arbitrary world XY.
std::pair<uint32_t, uint32_t> ExpectedPixelForWorldXY(glm::vec2 xy) {
  const float clip_x = 0.5f * xy.x;
  const float clip_y = 0.5f * xy.y;
  const uint32_t px = uint32_t((clip_x * 0.5f + 0.5f) * float(kInstTarget));
  const uint32_t py =
      uint32_t((1.0f - (clip_y * 0.5f + 0.5f)) * float(kInstTarget));
  return {px, py};
}

struct CullRenderResult {
  uint32_t survivor_count = 0;
  CpuImage albedo;
};

// Builds kCullQuadCount instances (one quad per kCullQuadCenters entry),
// culls them against `cull_camera` via GpuInstanceRenderer, and renders the
// survivors with the instanced_gbuffer material (Phase B) fed by the
// renderer's compacted buffer + indirect args. Returns the GPU-read survivor
// count and the albedo target for pixel assertions.
CullRenderResult RunCullAndRender(TestGpu& g, const Camera& cull_camera) {
  auto factory = MakeInstancedFactory(
      g, "instanced_gbuffer", MaterialPassType::kDeferred,
      {GBuffer::kNormalsFormat, GBuffer::kAlbedoFormat, GBuffer::kMaterialFormat},
      {});
  REQUIRE(factory != nullptr);

  MaterialInstanceCache cache;
  InstanceParams params;
  params.uniform_overrides["tint"] =
      MaterialParameterValue(glm::vec4(1.0f, 1.0f, 1.0f, 1.0f));
  entt::id_type key = ComposeMaterialCacheKey(
      entt::hashed_string{"instanced_gbuffer_cull"}.value(),
      GeometryType::kInstancedMesh, RenderPassType::kGBuffer, 0);
  auto handle = cache.GetOrCreate(key, *factory, GeometryType::kInstancedMesh,
                                  MaterialPassType::kDeferred,
                                  RenderPassType::kGBuffer, params);
  REQUIRE(handle);
  REQUIRE(handle->IsValid());
  auto* inst = handle.operator->();

  std::vector<float> verts = QuadVertices();
  std::array<uint32_t, 6> idx = {0, 1, 2, 0, 2, 3};
  wgpu::Buffer vbuf = UploadBuffer(g.device, verts.data(),
                                   verts.size() * sizeof(float),
                                   wgpu::BufferUsage::Vertex);
  wgpu::Buffer ibuf = UploadBuffer(g.device, idx.data(), sizeof(idx),
                                   wgpu::BufferUsage::Index);

  GpuInstanceRenderer renderer(g.device, g.queue, *g.gen, kCullQuadCount);
  REQUIRE(renderer.IsValid());
  renderer.SetMesh(vbuf, ibuf, wgpu::IndexFormat::Uint32, 6);

  std::vector<GpuInstanceRenderer::InstanceInput> instances;
  for (glm::vec2 c : kCullQuadCenters) {
    GpuInstanceRenderer::InstanceInput in;
    in.transform =
        glm::translate(glm::mat4(1.0f), glm::vec3(c.x, c.y, kCullQuadZ)) *
        glm::scale(glm::mat4(1.0f), glm::vec3(kCullQuadScale));
    in.bounds_sphere = glm::vec4(c.x, c.y, kCullQuadZ, kCullQuadRadius);
    instances.push_back(in);
  }
  renderer.UploadInstances(instances);

  UniformData frame_uniforms = MakeOrthoFrame();
  FrameContext frame;
  frame.Begin(g.device, g.queue, frame_uniforms);

  // Cull() dispatches a compute pass -- must run before BeginRenderPass on
  // this same encoder (a render pass can't be active while a compute pass
  // runs, and vice versa).
  renderer.Cull(frame, cull_camera);

  ColorRenderTarget normals_t(g.device, kInstTarget, kInstTarget,
                              GBuffer::kNormalsFormat);
  ColorRenderTarget albedo_t(g.device, kInstTarget, kInstTarget,
                             GBuffer::kAlbedoFormat);
  ColorRenderTarget material_t(g.device, kInstTarget, kInstTarget,
                               GBuffer::kMaterialFormat);
  REQUIRE(albedo_t.IsValid());

  {
    std::array<wgpu::RenderPassColorAttachment, 3> ca = {
        ClearAttachment(normals_t.GetView()),
        ClearAttachment(albedo_t.GetView()),
        ClearAttachment(material_t.GetView())};
    wgpu::RenderPassDescriptor desc{};
    desc.colorAttachmentCount = ca.size();
    desc.colorAttachments = ca.data();
    RenderPassContext pass = frame.BeginRenderPass(desc);

    REQUIRE(inst->Bind(pass, frame));
    REQUIRE(renderer.Draw(pass, frame, *inst));
    pass.End();
  }

  wgpu::CommandBuffer cmd = frame.End();
  g.queue.Submit(1, &cmd);
  test::WaitForGpu(g.instance, g.device, g.queue);

  CullRenderResult result;
  std::vector<uint32_t> count = test::ReadBufferSync<uint32_t>(
      g.instance, g.device, g.queue, renderer.GetArgsBuffer(),
      GpuInstanceRenderer::kArgsInstanceCountOffset, 1);
  REQUIRE(count.size() == 1);
  result.survivor_count = count[0];

  TextureReadback rb(g.instance, g.device, g.queue);
  result.albedo = rb.ReadTextureSync(albedo_t.GetTexture(), kInstTarget,
                                     kInstTarget, GBuffer::kAlbedoFormat);
  return result;
}

}  // namespace

TEST_CASE(
    "GPU cull end-to-end: indirect draw shows only in-frustum instances",
    "[gpu_instance][gpu]") {
  TestGpu& g = GetTestGpu();

  // aspect=0.1 -> visible half-width at kCullQuadZ is 1.0: the inner two
  // quad centers (|x|=0.5) survive, the outer two (|x|=1.5) are culled.
  CullRenderResult result = RunCullAndRender(g, MakeCullTestCamera(0.1f));
  CHECK(result.survivor_count == 2);

  auto px_outer_left = ExpectedPixelForWorldXY(kCullQuadCenters[0]);   // culled
  auto px_inner_left = ExpectedPixelForWorldXY(kCullQuadCenters[1]);   // visible
  auto px_inner_right = ExpectedPixelForWorldXY(kCullQuadCenters[2]);  // visible
  auto px_outer_right = ExpectedPixelForWorldXY(kCullQuadCenters[3]);  // culled

  CpuImage::Color outer_left =
      result.albedo.GetPixel(px_outer_left.first, px_outer_left.second);
  CpuImage::Color inner_left =
      result.albedo.GetPixel(px_inner_left.first, px_inner_left.second);
  CpuImage::Color inner_right =
      result.albedo.GetPixel(px_inner_right.first, px_inner_right.second);
  CpuImage::Color outer_right =
      result.albedo.GetPixel(px_outer_right.first, px_outer_right.second);

  INFO("outer_left rgb = " << (int)outer_left.r << "," << (int)outer_left.g
                           << "," << (int)outer_left.b);
  CHECK(outer_left.r < 40);
  INFO("inner_left rgb = " << (int)inner_left.r << "," << (int)inner_left.g
                           << "," << (int)inner_left.b);
  CHECK(inner_left.r > 180);
  INFO("inner_right rgb = " << (int)inner_right.r << "," << (int)inner_right.g
                            << "," << (int)inner_right.b);
  CHECK(inner_right.r > 180);
  INFO("outer_right rgb = " << (int)outer_right.r << "," << (int)outer_right.g
                            << "," << (int)outer_right.b);
  CHECK(outer_right.r < 40);
}

TEST_CASE(
    "GPU cull edge case: all instances culled -> indirect draw renders "
    "nothing",
    "[gpu_instance][gpu]") {
  TestGpu& g = GetTestGpu();

  // aspect=0.001 -> visible half-width at kCullQuadZ is 0.01, well inside
  // even the closest quad center (|x|=0.5): every instance is culled.
  CullRenderResult result = RunCullAndRender(g, MakeCullTestCamera(0.001f));
  CHECK(result.survivor_count == 0);

  for (glm::vec2 c : kCullQuadCenters) {
    auto px = ExpectedPixelForWorldXY(c);
    CpuImage::Color color = result.albedo.GetPixel(px.first, px.second);
    INFO("world (" << c.x << "," << c.y << ") rgb = " << (int)color.r << ","
                   << (int)color.g << "," << (int)color.b);
    CHECK(color.r < 40);
  }
}

TEST_CASE(
    "GPU cull edge case: all instances visible -> indirect draw renders all N",
    "[gpu_instance][gpu]") {
  TestGpu& g = GetTestGpu();

  // aspect=1.0 -> visible half-width at kCullQuadZ is 10.0, comfortably past
  // even the farthest quad center (|x|=1.5): every instance survives.
  CullRenderResult result = RunCullAndRender(g, MakeCullTestCamera(1.0f));
  CHECK(result.survivor_count == kCullQuadCount);

  for (glm::vec2 c : kCullQuadCenters) {
    auto px = ExpectedPixelForWorldXY(c);
    CpuImage::Color color = result.albedo.GetPixel(px.first, px.second);
    INFO("world (" << c.x << "," << c.y << ") rgb = " << (int)color.r << ","
                   << (int)color.g << "," << (int)color.b);
    CHECK(color.r > 180);
  }
}
