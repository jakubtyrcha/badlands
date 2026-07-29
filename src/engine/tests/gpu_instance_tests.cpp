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
#include <optional>
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
  // Unified layout, single-bucket case: bucketBase = [0], bucketId = 0 (unset ->
  // zero) makes the vertex shader read instances[0 + instance_index].
  std::array<uint32_t, 1> base0 = {0u};
  wgpu::Buffer basebuf = UploadBuffer(g.device, base0.data(), sizeof(base0),
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

    REQUIRE(inst->BindInstanceData(pass, frame, instbuf, basebuf));

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
  std::array<uint32_t, 1> base0 = {0u};
  wgpu::Buffer basebuf = UploadBuffer(g.device, base0.data(), sizeof(base0),
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
    REQUIRE(inst->BindInstanceData(pass, frame, instbuf, basebuf));
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
// Review fix #1 (correctness hardening) RED/GREEN: BindPerObject must REFUSE
// (return false) for instanced materials, not silently succeed with group 1
// left unbound. The standard render passes gate every draw on
// `if (!Bind() || !BindPerObject()) continue;` (render_forward.cpp,
// render_textured_mesh.cpp) -- if BindPerObject wrongly returned true for
// kInstancedMesh (as it did before this fix), an instanced material routed
// through those passes would pass the gate and DrawIndexed with the required
// group-1 storage binding never set: a Dawn validation error.
//
// This replicates that exact gate under a validation-error scope, using the
// instanced_gbuffer material (no @group(2) -- see the DeclaresBindGroup(2)
// check below -- so any validation error here can only be about the missing
// group-1 binding, not some unrelated unbound group).
//
// RED (recorded before the fix, BindPerObject returning `true` for
// kInstancedMesh): ok == true, the draw WAS issued, and Dawn raised a
// validation error for the unset group-1 bind group -- both CHECK_FALSE(ok)
// and CHECK_FALSE(validation_error) failed, plus the direct contract
// assertion CHECK(...BindPerObject(...) == false) failed. GREEN (after the
// fix): ok == false, the draw is skipped entirely, and no validation error
// fires.
// ===========================================================================
TEST_CASE("BindPerObject refuses an instanced material (no unbound group-1 draw)",
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
      entt::hashed_string{"instanced_gbuffer_bindperobject"}.value(),
      GeometryType::kInstancedMesh, RenderPassType::kGBuffer, 0);
  auto handle = cache.GetOrCreate(key, *factory, GeometryType::kInstancedMesh,
                                  MaterialPassType::kDeferred,
                                  RenderPassType::kGBuffer, params);
  REQUIRE(handle);
  REQUIRE(handle->IsValid());
  auto* inst = handle.operator->();
  REQUIRE(inst->GetGeometryType() == GeometryType::kInstancedMesh);
  REQUIRE_FALSE(inst->DeclaresBindGroup(2));  // isolate: no group-2 confound

  std::vector<float> verts = QuadVertices();
  std::array<uint32_t, 6> idx = {0, 1, 2, 0, 2, 3};
  wgpu::Buffer vbuf = UploadBuffer(g.device, verts.data(),
                                   verts.size() * sizeof(float),
                                   wgpu::BufferUsage::Vertex);
  wgpu::Buffer ibuf = UploadBuffer(g.device, idx.data(), sizeof(idx),
                                   wgpu::BufferUsage::Index);

  UniformData frame_uniforms = MakeOrthoFrame();
  ColorRenderTarget normals_t(g.device, kInstTarget, kInstTarget,
                              GBuffer::kNormalsFormat);
  ColorRenderTarget albedo_t(g.device, kInstTarget, kInstTarget,
                             GBuffer::kAlbedoFormat);
  ColorRenderTarget material_t(g.device, kInstTarget, kInstTarget,
                               GBuffer::kMaterialFormat);
  REQUIRE(albedo_t.IsValid());

  bool ok = false;
  // Capture any validation error the draw provokes rather than letting it hit
  // the device uncaptured-error callback (mirrors
  // game/tests/forward_pass_tests.cpp's validation-scope pattern).
  g.device.PushErrorScope(wgpu::ErrorFilter::Validation);

  FrameContext frame;
  frame.Begin(g.device, g.queue, frame_uniforms);
  {
    std::array<wgpu::RenderPassColorAttachment, 3> ca = {
        ClearAttachment(normals_t.GetView()),
        ClearAttachment(albedo_t.GetView()),
        ClearAttachment(material_t.GetView())};
    wgpu::RenderPassDescriptor desc{};
    desc.colorAttachmentCount = ca.size();
    desc.colorAttachments = ca.data();
    RenderPassContext pass = frame.BeginRenderPass(desc);

    // Exactly the standard passes' gate (render_forward.cpp /
    // render_textured_mesh.cpp): `if (!Bind() || !BindPerObject()) continue;`
    ok = inst->Bind(pass, frame) && inst->BindPerObject(pass, frame);
    if (ok) {
      pass.SetVertexBuffer(0, vbuf);
      pass.SetIndexBuffer(ibuf, wgpu::IndexFormat::Uint32);
      pass.DrawIndexed(6);
    }

    // Direct contract assertion, independent of the gate replay above: an
    // instanced material's BindPerObject must always refuse.
    CHECK(inst->BindPerObject(pass, frame) == false);

    pass.End();
  }
  wgpu::CommandBuffer cmd = frame.End();
  g.queue.Submit(1, &cmd);

  bool scope_done = false;
  bool validation_error = false;
  g.device.PopErrorScope(
      wgpu::CallbackMode::AllowProcessEvents,
      [&](wgpu::PopErrorScopeStatus, wgpu::ErrorType type,
          wgpu::StringView msg) {
        if (type != wgpu::ErrorType::NoError) {
          validation_error = true;
          INFO("captured error: "
               << (msg.length > 0 ? std::string(msg.data, msg.length)
                                  : std::string("(no message)")));
        }
        scope_done = true;
      });
  while (!scope_done) {
    g.instance.ProcessEvents();
    g.device.Tick();
  }

  CHECK_FALSE(ok);                // the gate must refuse an instanced material
  CHECK_FALSE(validation_error);  // ...so no unbound-group-1 draw is ever issued
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
// Phase C/D: GPU-driven multi-bucket cull + LOD classification + prefix-sum
// compaction feeding one indirect draw PER (model,lod) bucket
// (GpuInstanceRenderer + shaders/compute/instance_{classify,scan,scatter}.wesl).
// Builds on Phase A's atomics/indirect-draw/BufferReadback primitives and Phase
// B's unified instanced materials: a 3-pass compute frustum-culls each
// instance, routes survivors into `bucket = modelId*kMaxLods + lod(distance)`,
// prefix-sums the per-bucket counts into disjoint slices of one global compacted
// buffer, and one DrawIndexedIndirect per bucket renders each bucket's survivors
// — the draw's vertex shader reads the GPU-computed base as
// compacted[bucketBase[bucketId] + instance_index] (no per-frame readback).
//
// Test 1 (gate): the three compute shaders compile + reflect their bindings.
// Test 2: single-bucket cull correctness (Phase C carried forward as the
//         trivial 1-bucket case) via buffer readback.
// Test 3: LOD boundary — instances straddling the thresholds land in the
//         correct lod bucket (bucketCount readback, incl. a boundary value).
// Test 4: multi-bucket compaction — prefix-sum bases are exactly tightly
//         packed (non-overlapping, no padding), and each slice holds exactly
//         its bucket's transforms.
// Test 5: end-to-end per-LOD render — each lod drawn with its own mesh + color
//         at its position (framebuffer readback).
// Test 6-7: edges — all culled (empty buckets, no draw/crash); all-in-one-bucket.
// ===========================================================================

TEST_CASE(
    "compute/instance_{classify,scan,scatter} compile and reflect their bindings",
    "[gpu_instance][gpu]") {
  TestGpu& g = GetTestGpu();

  // A private generator so each compile is a genuine first-time compile under
  // the Dawn validation-error scope (see RunCapturingValidationErrors' comment
  // — a non-null/IsValid() check alone does NOT prove a WGSL compile succeeded
  // on this Dawn build).
  GpuPipelineGenerator fresh_gen(g.device, FindShaderDirectory());

  auto find_binding = [](const CompiledComputePipeline& p, uint32_t group,
                         uint32_t binding) -> const ReflectedBinding* {
    for (const auto& b : p.reflected_bindings) {
      if (b.group == group && b.binding == binding) return &b;
    }
    return nullptr;
  };

  std::shared_ptr<const CompiledComputePipeline> classify, scan, scatter;
  CapturedError err = RunCapturingValidationErrors(g, [&] {
    classify = fresh_gen.GetComputePipeline("compute/instance_classify");
    scan = fresh_gen.GetComputePipeline("compute/instance_scan");
    scatter = fresh_gen.GetComputePipeline("compute/instance_scatter");
  });
  INFO("Dawn validation error: " << err.message);
  CHECK(err.type == wgpu::ErrorType::NoError);
  REQUIRE(classify != nullptr);
  REQUIRE(scan != nullptr);
  REQUIRE(scatter != nullptr);
  REQUIRE(classify->pipeline != nullptr);
  REQUIRE(scan->pipeline != nullptr);
  REQUIRE(scatter->pipeline != nullptr);
  CHECK(classify->workgroup_size[0] == 64);
  CHECK(scan->workgroup_size[0] == 1);
  CHECK(scatter->workgroup_size[0] == 64);

  // Classify: config (uniform) + instances (read-only) + perInstanceBucket +
  // bucketCount (both writable storage).
  const ReflectedBinding* c_config = find_binding(*classify, 0, 0);
  const ReflectedBinding* c_instances = find_binding(*classify, 0, 1);
  const ReflectedBinding* c_bucketcount = find_binding(*classify, 0, 3);
  REQUIRE(c_config != nullptr);
  REQUIRE(c_instances != nullptr);
  REQUIRE(c_bucketcount != nullptr);
  CHECK(c_config->name == "config");
  CHECK(c_config->buffer_type == wgpu::BufferBindingType::Uniform);
  CHECK(c_instances->name == "instances");
  CHECK(c_instances->buffer_type == wgpu::BufferBindingType::ReadOnlyStorage);
  CHECK(c_bucketcount->name == "bucketCount");
  CHECK(c_bucketcount->buffer_type == wgpu::BufferBindingType::Storage);

  // Scan: writes bucketBase + indirectArgs.
  const ReflectedBinding* s_base = find_binding(*scan, 0, 2);
  const ReflectedBinding* s_args = find_binding(*scan, 0, 4);
  REQUIRE(s_base != nullptr);
  REQUIRE(s_args != nullptr);
  CHECK(s_base->name == "bucketBase");
  CHECK(s_base->buffer_type == wgpu::BufferBindingType::Storage);
  CHECK(s_args->name == "indirectArgs");

  // Scatter: reads bucketBase (read-only), writes compacted.
  const ReflectedBinding* x_base = find_binding(*scatter, 0, 3);
  const ReflectedBinding* x_compacted = find_binding(*scatter, 0, 5);
  REQUIRE(x_base != nullptr);
  REQUIRE(x_compacted != nullptr);
  CHECK(x_base->name == "bucketBase");
  CHECK(x_base->buffer_type == wgpu::BufferBindingType::ReadOnlyStorage);
  CHECK(x_compacted->name == "compacted");
  CHECK(x_compacted->buffer_type == wgpu::BufferBindingType::Storage);
}

namespace {

// Sphere-vs-frustum test mirroring the classify shader's `sphereCulled` EXACTLY
// (culled iff fully outside any plane: dot(plane.xyz, center)+plane.w < -radius).
bool ExpectedCulled(const Frustum& f, glm::vec3 center, float radius) {
  for (const glm::vec4& p : f.planes) {
    if (glm::dot(glm::vec3(p), center) + p.w < -radius) return true;
  }
  return false;
}

bool Mat4Equal(const glm::mat4& a, const glm::mat4& b) {
  return std::memcmp(&a, &b, sizeof(glm::mat4)) == 0;
}

// Camera at the origin looking down -Z. `aspect` controls the horizontal
// half-width (fov 90 -> tan(fovy/2)=1); the LOD tests use aspect=1 (wide, so
// nothing is side-culled) and rely on distance for the LOD band.
Camera MakeCullCamera(float aspect = 1.0f) {
  Camera camera;
  camera.position = glm::vec3(0.0f);
  camera.direction = glm::vec3(0.0f, 0.0f, -1.0f);
  camera.up = glm::vec3(0.0f, 1.0f, 0.0f);
  camera.fov = 90.0f;
  camera.aspect = aspect;
  camera.near_plane = 0.1f;
  camera.far_plane = 1000.0f;
  return camera;
}

// The bucket an instance is expected to land in (CPU mirror of the classify
// pass: frustum cull -> distance LOD -> modelId*kMaxLods + lod). nullopt =
// culled / out of range.
std::optional<uint32_t> ExpectedBucket(const Frustum& f, glm::vec3 cam_pos,
                                       glm::vec2 thresholds, glm::vec3 center,
                                       float radius, uint32_t model_id,
                                       uint32_t num_buckets) {
  if (ExpectedCulled(f, center, radius)) return std::nullopt;
  const float d = glm::length(center - cam_pos);
  const uint32_t lod = d < thresholds.x ? 0u : (d < thresholds.y ? 1u : 2u);
  const uint32_t b = model_id * GpuInstanceRenderer::kMaxLods + lod;
  if (b >= num_buckets) return std::nullopt;
  return b;
}

// An instance whose bounds center is at `center`, model id `model`, radius
// small. Transform = translate(center) (so the stored transform is unique per
// unique center and matchable via memcmp).
GpuInstanceRenderer::InstanceInput MakeInstance(glm::vec3 center, uint32_t model,
                                                float radius = 0.25f) {
  GpuInstanceRenderer::InstanceInput in;
  in.transform = glm::translate(glm::mat4(1.0f), center);
  in.bounds_sphere = glm::vec4(center, radius);
  in.model_info = glm::uvec4(model, 0u, 0u, 0u);
  return in;
}

}  // namespace

// ---------------------------------------------------------------------------
// Test 2: single-bucket cull correctness (Phase C, as the trivial 1-bucket
// case). Thresholds pushed to +inf so every survivor lands in lod0 (bucket 0);
// the compacted buffer + bucket-0 count must match the independently-computed
// in-frustum set, including a sphere straddling a plane.
// ---------------------------------------------------------------------------
TEST_CASE("GPU cull single-bucket: compacted + count match the in-frustum set",
          "[gpu_instance][gpu]") {
  TestGpu& g = GetTestGpu();

  Camera camera = MakeCullCamera(1.0f);
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
  // The straddling case really straddles: a bare point is out, the sphere reaches
  // back in (exercises "< -radius", not "< 0").
  REQUIRE(ExpectedCulled(frustum, cases[6].center, 0.0f));
  REQUIRE_FALSE(ExpectedCulled(frustum, cases[6].center, cases[6].radius));

  std::vector<glm::mat4> expected_visible;
  std::vector<GpuInstanceRenderer::InstanceInput> inputs;
  for (const Case& c : cases) {
    GpuInstanceRenderer::InstanceInput in = MakeInstance(c.center, 0, c.radius);
    inputs.push_back(in);
    if (!ExpectedCulled(frustum, c.center, c.radius)) {
      expected_visible.push_back(in.transform);
    }
  }

  // thresholds = {1e30, 1e30} -> every survivor is lod0 -> bucket 0.
  GpuInstanceRenderer renderer(g.device, g.queue, *g.gen,
                               static_cast<uint32_t>(cases.size()),
                               /*num_models=*/1, {1e30f, 1e30f});
  REQUIRE(renderer.IsValid());
  renderer.UploadInstances(inputs);

  FrameContext frame;
  frame.Begin(g.device, g.queue, UniformData{});
  renderer.Cull(frame, camera);
  wgpu::CommandBuffer cmd = frame.End();
  g.queue.Submit(1, &cmd);
  test::WaitForGpu(g.instance, g.device, g.queue);

  std::vector<uint32_t> counts = test::ReadBufferSync<uint32_t>(
      g.instance, g.device, g.queue, renderer.GetBucketCountBuffer(), 0,
      renderer.GetNumBuckets());
  REQUIRE(counts.size() == renderer.GetNumBuckets());
  INFO("expected count = " << expected_visible.size()
                           << ", bucket0 = " << counts[0]);
  CHECK(counts[0] == expected_visible.size());
  CHECK(counts[1] == 0);  // no survivor should reach lod1/lod2
  CHECK(counts[2] == 0);

  std::vector<uint32_t> bases = test::ReadBufferSync<uint32_t>(
      g.instance, g.device, g.queue, renderer.GetBucketBaseBuffer(), 0,
      renderer.GetNumBuckets());
  REQUIRE(bases.size() == renderer.GetNumBuckets());
  CHECK(bases[0] == 0);

  std::vector<glm::mat4> compacted = test::ReadBufferSync<glm::mat4>(
      g.instance, g.device, g.queue, renderer.GetCompactedBuffer(), 0,
      counts[0]);
  REQUIRE(compacted.size() == counts[0]);

  // Order-independent set match (atomic append order isn't fixed).
  std::vector<bool> matched(compacted.size(), false);
  for (const glm::mat4& expected : expected_visible) {
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
// Test 3: LOD boundary. Instances on the view axis at distances straddling the
// thresholds land in the correct lod bucket. Includes a value EXACTLY at a
// boundary (dist == t0), which must fall to the coarser side (strict `<`).
// ---------------------------------------------------------------------------
TEST_CASE("GPU LOD selection: distance thresholds route into the right bucket",
          "[gpu_instance][gpu]") {
  TestGpu& g = GetTestGpu();

  const glm::vec2 thresholds(10.0f, 20.0f);
  Camera camera = MakeCullCamera(1.0f);

  // dist -> expected lod: 5->0, 10->1 (boundary, coarser side), 15->1, 20->2
  // (boundary), 25->2. All on -Z axis, tiny radius, well inside the frustum.
  const std::array<float, 5> dists = {5.0f, 10.0f, 15.0f, 20.0f, 25.0f};
  std::vector<GpuInstanceRenderer::InstanceInput> inputs;
  for (float d : dists) {
    inputs.push_back(MakeInstance(glm::vec3(0.0f, 0.0f, -d), 0, 0.1f));
  }

  GpuInstanceRenderer renderer(g.device, g.queue, *g.gen,
                               static_cast<uint32_t>(inputs.size()),
                               /*num_models=*/1,
                               {thresholds.x, thresholds.y});
  REQUIRE(renderer.IsValid());
  renderer.UploadInstances(inputs);

  FrameContext frame;
  frame.Begin(g.device, g.queue, UniformData{});
  renderer.Cull(frame, camera);
  wgpu::CommandBuffer cmd = frame.End();
  g.queue.Submit(1, &cmd);
  test::WaitForGpu(g.instance, g.device, g.queue);

  std::vector<uint32_t> counts = test::ReadBufferSync<uint32_t>(
      g.instance, g.device, g.queue, renderer.GetBucketCountBuffer(), 0,
      renderer.GetNumBuckets());
  REQUIRE(counts.size() == 3);
  INFO("lod counts: " << counts[0] << "," << counts[1] << "," << counts[2]);
  CHECK(counts[0] == 1);  // {5}
  CHECK(counts[1] == 2);  // {10 (boundary), 15}
  CHECK(counts[2] == 2);  // {20 (boundary), 25}
}

// ---------------------------------------------------------------------------
// Test 4: multi-bucket compaction (prefix-sum). Several instances across 2
// models x lods -> the compacted slices are non-overlapping and EXACTLY
// tightly packed (no padding — each bucket's base is exactly the end of the
// previous bucket's slice), and each bucket's [base, base+count) slice holds
// exactly that bucket's transforms (order-independent).
// ---------------------------------------------------------------------------
TEST_CASE("GPU multi-bucket compaction: prefix-sum slices are disjoint + exact",
          "[gpu_instance][gpu]") {
  TestGpu& g = GetTestGpu();

  const glm::vec2 thresholds(10.0f, 20.0f);
  Camera camera = MakeCullCamera(1.0f);
  const Frustum frustum =
      Frustum::FromViewProj(camera.GetProj() * camera.GetView());

  // model 0: two lod0 (dist~5) + one lod1 (dist~15)
  // model 1: one lod0 (dist~5) + two lod1 (dist~15) + one lod2 (dist~25)
  // -> buckets 0:2, 1:1, 2:0(empty), 3:1, 4:2, 5:1. Unique x-offset per instance
  // keeps each transform distinct without changing the LOD band.
  struct Spec {
    float dist;
    uint32_t model;
  };
  const std::vector<Spec> specs = {
      {5.0f, 0}, {5.0f, 0}, {15.0f, 0},          // model 0
      {5.0f, 1}, {15.0f, 1}, {15.0f, 1}, {25.0f, 1}};  // model 1

  std::vector<GpuInstanceRenderer::InstanceInput> inputs;
  float tag = 0.0f;
  for (const Spec& s : specs) {
    tag += 1.0f;
    glm::vec3 center(tag * 0.001f, 0.0f, -s.dist);  // unique center
    inputs.push_back(MakeInstance(center, s.model, 0.1f));
  }

  const uint32_t num_models = 2;
  const uint32_t num_buckets = num_models * GpuInstanceRenderer::kMaxLods;

  GpuInstanceRenderer renderer(g.device, g.queue, *g.gen,
                               static_cast<uint32_t>(inputs.size()), num_models,
                               {thresholds.x, thresholds.y});
  REQUIRE(renderer.IsValid());
  REQUIRE(renderer.GetNumBuckets() == num_buckets);
  renderer.UploadInstances(inputs);

  FrameContext frame;
  frame.Begin(g.device, g.queue, UniformData{});
  renderer.Cull(frame, camera);
  wgpu::CommandBuffer cmd = frame.End();
  g.queue.Submit(1, &cmd);
  test::WaitForGpu(g.instance, g.device, g.queue);

  // Expected per-bucket membership (CPU mirror).
  std::vector<std::vector<glm::mat4>> expected(num_buckets);
  for (size_t i = 0; i < inputs.size(); ++i) {
    auto b = ExpectedBucket(frustum, camera.GetPosition(), thresholds,
                            glm::vec3(inputs[i].bounds_sphere),
                            inputs[i].bounds_sphere.w, specs[i].model,
                            num_buckets);
    REQUIRE(b.has_value());
    expected[*b].push_back(inputs[i].transform);
  }

  std::vector<uint32_t> counts = test::ReadBufferSync<uint32_t>(
      g.instance, g.device, g.queue, renderer.GetBucketCountBuffer(), 0,
      num_buckets);
  std::vector<uint32_t> bases = test::ReadBufferSync<uint32_t>(
      g.instance, g.device, g.queue, renderer.GetBucketBaseBuffer(), 0,
      num_buckets);
  std::vector<glm::mat4> compacted = test::ReadBufferSync<glm::mat4>(
      g.instance, g.device, g.queue, renderer.GetCompactedBuffer(), 0,
      renderer.GetCompactedCapacity());
  REQUIRE(counts.size() == num_buckets);
  REQUIRE(bases.size() == num_buckets);
  REQUIRE(compacted.size() == renderer.GetCompactedCapacity());

  // Counts match; bases are EXACTLY tightly packed (no padding) — each
  // bucket's base is exactly the end of the previous bucket's slice, which
  // proves non-overlap too (a strictly weaker `>=` wouldn't catch padding).
  uint32_t prev_end = 0;
  for (uint32_t b = 0; b < num_buckets; ++b) {
    INFO("bucket " << b << " count=" << counts[b] << " base=" << bases[b]);
    CHECK(counts[b] == expected[b].size());
    CHECK(bases[b] == prev_end);  // exact tight packing: non-overlapping, no padding
    prev_end = bases[b] + counts[b];
  }

  // Each bucket's slice holds exactly its expected transforms (order-independent).
  for (uint32_t b = 0; b < num_buckets; ++b) {
    std::vector<bool> matched(counts[b], false);
    for (const glm::mat4& want : expected[b]) {
      bool found = false;
      for (uint32_t s = 0; s < counts[b]; ++s) {
        if (!matched[s] &&
            Mat4Equal(compacted[bases[b] + s], want)) {
          matched[s] = true;
          found = true;
          break;
        }
      }
      CHECK(found);
    }
  }
}

// ---------------------------------------------------------------------------
// End-to-end render helpers (framebuffer readback): cull + one indirect draw
// per bucket with per-bucket distinct-color instanced_gbuffer materials.
// ---------------------------------------------------------------------------
namespace {

// One distinct-color instanced_gbuffer material per bucket, with `bucketId`
// baked to the bucket index (so the vertex shader reads the right slice). Kept
// alive by the returned handles vector.
struct BucketMaterials {
  std::unique_ptr<MaterialInstanceFactory> factory;
  MaterialInstanceCache cache;
  std::vector<entt::resource<RenderingMaterialInstance>> handles;
  std::vector<RenderingMaterialInstance*> ptrs;  // size num_buckets, may be null
};

std::unique_ptr<BucketMaterials> MakeBucketMaterials(
    TestGpu& g, uint32_t num_buckets, const std::vector<glm::vec4>& tints) {
  auto bm = std::make_unique<BucketMaterials>();
  bm->factory = MakeInstancedFactory(
      g, "instanced_gbuffer", MaterialPassType::kDeferred,
      {GBuffer::kNormalsFormat, GBuffer::kAlbedoFormat, GBuffer::kMaterialFormat},
      {});
  REQUIRE(bm->factory != nullptr);
  bm->ptrs.assign(num_buckets, nullptr);
  for (uint32_t b = 0; b < num_buckets; ++b) {
    InstanceParams params;
    params.uniform_overrides["tint"] = MaterialParameterValue(tints[b]);
    params.uniform_overrides["bucketId"] = MaterialParameterValue(uint32_t(b));
    entt::id_type key = ComposeMaterialCacheKey(
        entt::hashed_string{"instanced_gbuffer_bucket"}.value(),
        GeometryType::kInstancedMesh, RenderPassType::kGBuffer, b);
    auto handle = bm->cache.GetOrCreate(
        key, *bm->factory, GeometryType::kInstancedMesh,
        MaterialPassType::kDeferred, RenderPassType::kGBuffer, params);
    REQUIRE(handle);
    REQUIRE(handle->IsValid());
    bm->handles.push_back(handle);
    bm->ptrs[b] = handle.operator->();
  }
  return bm;
}

// Runs Cull() then one indirect draw per bucket into a G-buffer and returns the
// albedo target. `mats[b]` is bucket b's material (already carries bucketId=b);
// the Draw callback just Bind()s it (group 0) — the renderer binds group 1 +
// mesh + issues the indirect draw.
CpuImage CullAndRenderBuckets(TestGpu& g, GpuInstanceRenderer& renderer,
                              const Camera& cull_camera,
                              const std::vector<RenderingMaterialInstance*>& mats) {
  UniformData frame_uniforms = MakeOrthoFrame();
  FrameContext frame;
  frame.Begin(g.device, g.queue, frame_uniforms);

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

    renderer.Draw(pass, frame,
                  [&](uint32_t bucket, uint32_t /*submesh*/)
                      -> RenderingMaterialInstance* {
                    RenderingMaterialInstance* m = mats[bucket];
                    if (!m) return nullptr;
                    if (!m->Bind(pass, frame)) return nullptr;
                    return m;
                  });
    pass.End();
  }

  wgpu::CommandBuffer cmd = frame.End();
  g.queue.Submit(1, &cmd);
  test::WaitForGpu(g.instance, g.device, g.queue);

  TextureReadback rb(g.instance, g.device, g.queue);
  return rb.ReadTextureSync(albedo_t.GetTexture(), kInstTarget, kInstTarget,
                            GBuffer::kAlbedoFormat);
}

// Framebuffer center for a world XY under MakeOrthoFrame (clip = 0.5*world),
// WebGPU y-down.
std::pair<uint32_t, uint32_t> WorldXyToPixel(glm::vec2 xy) {
  const float clip_x = 0.5f * xy.x;
  const float clip_y = 0.5f * xy.y;
  return {uint32_t((clip_x * 0.5f + 0.5f) * float(kInstTarget)),
          uint32_t((1.0f - (clip_y * 0.5f + 0.5f)) * float(kInstTarget))};
}

// A quad (pos3+uv2+normal3+tangent3, +Z normal) of the given half-extent.
std::vector<float> QuadVerticesSized(float h) {
  return {
      -h, -h, 0, 0, 0, 0, 0, 1, 1, 0, 0,  //
      h,  -h, 0, 1, 0, 0, 0, 1, 1, 0, 0,  //
      h,  h,  0, 1, 1, 0, 0, 1, 1, 0, 0,  //
      -h, h,  0, 0, 1, 0, 0, 1, 1, 0, 0,  //
  };
}

// A quad of the given half-extent, centered at `offset` IN MESH-LOCAL space
// (baked into the vertex positions, not the per-instance transform). Two
// submeshes of the same bucket using different offsets render as distinct
// on-screen shapes per instance while still reading the exact same
// per-instance world transform.
std::vector<float> QuadVerticesOffset(float h, glm::vec2 offset) {
  return {
      offset.x - h, offset.y - h, 0, 0, 0, 0, 0, 1, 1, 0, 0,  //
      offset.x + h, offset.y - h, 0, 1, 0, 0, 0, 1, 1, 0, 0,  //
      offset.x + h, offset.y + h, 0, 1, 1, 0, 0, 1, 1, 0, 0,  //
      offset.x - h, offset.y + h, 0, 0, 1, 0, 0, 1, 1, 0, 0,  //
  };
}

}  // namespace

// ---------------------------------------------------------------------------
// Test 5: end-to-end per-LOD render. Three instances at distinct distances (one
// per lod) and distinct screen positions; each lod bucket gets a DISTINCT mesh
// (quad size) + color. The framebuffer must show each instance drawn with its
// own-lod color at its own position — which only holds if bucketId + the
// GPU-computed base route each bucket's draw to its own compacted slice.
// ---------------------------------------------------------------------------
TEST_CASE("GPU per-LOD render: each lod drawn with its own mesh/color/position",
          "[gpu_instance][gpu]") {
  TestGpu& g = GetTestGpu();

  const glm::vec2 thresholds(10.0f, 20.0f);
  Camera cull_camera = MakeCullCamera(1.0f);

  // One instance per lod: screen X from world x (-1,0,1), LOD from distance
  // (z = -5/-15/-25). All comfortably inside the frustum.
  struct Inst {
    glm::vec2 screen_xy;
    float z;
    uint32_t expected_bucket;  // 1 model -> bucket == lod
  };
  const std::array<Inst, 3> insts = {{
      {{-1.0f, 0.0f}, -5.0f, 0},   // lod0
      {{0.0f, 0.0f}, -15.0f, 1},   // lod1
      {{1.0f, 0.0f}, -25.0f, 2},   // lod2
  }};

  std::vector<GpuInstanceRenderer::InstanceInput> inputs;
  for (const Inst& it : insts) {
    glm::vec3 center(it.screen_xy.x, it.screen_xy.y, it.z);
    GpuInstanceRenderer::InstanceInput in = MakeInstance(center, 0, 0.25f);
    // Small on-screen quad: scale the unit-quad mesh down at the instance level.
    in.transform = glm::translate(glm::mat4(1.0f), center) *
                   glm::scale(glm::mat4(1.0f), glm::vec3(0.2f));
    inputs.push_back(in);
  }

  const uint32_t num_buckets = 1 * GpuInstanceRenderer::kMaxLods;  // 3
  GpuInstanceRenderer renderer(g.device, g.queue, *g.gen, 3, /*num_models=*/1,
                               {thresholds.x, thresholds.y});
  REQUIRE(renderer.IsValid());
  renderer.UploadInstances(inputs);

  // Distinct mesh (half-extent) + distinct color per lod bucket.
  const std::array<float, 3> half_extents = {1.0f, 0.7f, 0.4f};
  const std::vector<glm::vec4> tints = {
      {1.0f, 0.0f, 0.0f, 1.0f},   // lod0 red
      {0.0f, 1.0f, 0.0f, 1.0f},   // lod1 green
      {0.0f, 0.0f, 1.0f, 1.0f}};  // lod2 blue
  std::array<uint32_t, 6> idx = {0, 1, 2, 0, 2, 3};
  wgpu::Buffer ibuf = UploadBuffer(g.device, idx.data(), sizeof(idx),
                                   wgpu::BufferUsage::Index);
  std::vector<wgpu::Buffer> vbufs;
  for (uint32_t b = 0; b < num_buckets; ++b) {
    std::vector<float> verts = QuadVerticesSized(half_extents[b]);
    wgpu::Buffer vbuf = UploadBuffer(g.device, verts.data(),
                                     verts.size() * sizeof(float),
                                     wgpu::BufferUsage::Vertex);
    vbufs.push_back(vbuf);
    renderer.SetBucketSubmesh(b, 0, vbuf, ibuf, wgpu::IndexFormat::Uint32, 6);
  }

  auto mats = MakeBucketMaterials(g, num_buckets, tints);
  CpuImage albedo =
      CullAndRenderBuckets(g, renderer, cull_camera, mats->ptrs);

  // Each instance appears with its own-lod color at its own screen position.
  const std::array<glm::vec3, 3> want_rgb = {
      glm::vec3(255, 0, 0), glm::vec3(0, 255, 0), glm::vec3(0, 0, 255)};
  for (size_t i = 0; i < insts.size(); ++i) {
    auto px = WorldXyToPixel(insts[i].screen_xy);
    CpuImage::Color c = albedo.GetPixel(px.first, px.second);
    INFO("instance " << i << " (bucket " << insts[i].expected_bucket << ") at ("
                     << px.first << "," << px.second << ") rgb = " << (int)c.r
                     << "," << (int)c.g << "," << (int)c.b);
    CHECK(std::abs(int(c.r) - int(want_rgb[i].r)) < 60);
    CHECK(std::abs(int(c.g) - int(want_rgb[i].g)) < 60);
    CHECK(std::abs(int(c.b) - int(want_rgb[i].b)) < 60);
  }
  // A gap between the three quads stays the clear color.
  CpuImage::Color gap = albedo.GetPixel(uint32_t(0.25f * kInstTarget),
                                        uint32_t(0.75f * kInstTarget));
  CHECK(gap.r < 40);
  CHECK(gap.g < 40);
  CHECK(gap.b < 40);
}

// ---------------------------------------------------------------------------
// Test 6: edge — every instance culled. All buckets have count 0, so every
// per-bucket indirect draw renders nothing (instanceCount == 0). No crash, and
// the framebuffer stays the clear color.
// ---------------------------------------------------------------------------
TEST_CASE("GPU edge: all instances culled -> empty buckets draw nothing",
          "[gpu_instance][gpu]") {
  TestGpu& g = GetTestGpu();

  // Cull camera far plane = 1000; placing every instance at z = -2000 puts all
  // of them beyond the far plane -> all culled, independent of screen X (an
  // on-axis instance would survive a narrow frustum, so distance-cull instead).
  Camera cull_camera = MakeCullCamera(1.0f);
  constexpr float kFarBeyond = -2000.0f;

  std::vector<GpuInstanceRenderer::InstanceInput> inputs;
  const std::array<glm::vec2, 3> xy = {glm::vec2{-1.0f, 0.0f},
                                       glm::vec2{0.0f, 0.0f},
                                       glm::vec2{1.0f, 0.0f}};
  for (glm::vec2 c : xy) {
    GpuInstanceRenderer::InstanceInput in =
        MakeInstance(glm::vec3(c.x, c.y, kFarBeyond), 0, 0.2f);
    // The ortho render frame ignores z, so were these NOT culled they'd draw at
    // WorldXyToPixel(c); since they are (beyond the far plane), pixels stay clear.
    in.transform = glm::translate(glm::mat4(1.0f), glm::vec3(c.x, c.y, -10.0f)) *
                   glm::scale(glm::mat4(1.0f), glm::vec3(0.2f));
    inputs.push_back(in);
  }

  const uint32_t num_buckets = GpuInstanceRenderer::kMaxLods;
  GpuInstanceRenderer renderer(g.device, g.queue, *g.gen, 3, 1, {10.0f, 20.0f});
  REQUIRE(renderer.IsValid());
  renderer.UploadInstances(inputs);

  std::array<uint32_t, 6> idx = {0, 1, 2, 0, 2, 3};
  std::vector<float> verts = QuadVerticesSized(1.0f);
  wgpu::Buffer ibuf = UploadBuffer(g.device, idx.data(), sizeof(idx),
                                   wgpu::BufferUsage::Index);
  wgpu::Buffer vbuf = UploadBuffer(g.device, verts.data(),
                                   verts.size() * sizeof(float),
                                   wgpu::BufferUsage::Vertex);
  for (uint32_t b = 0; b < num_buckets; ++b) {
    renderer.SetBucketSubmesh(b, 0, vbuf, ibuf, wgpu::IndexFormat::Uint32, 6);
  }

  const std::vector<glm::vec4> tints(num_buckets,
                                     glm::vec4(1.0f, 1.0f, 1.0f, 1.0f));
  auto mats = MakeBucketMaterials(g, num_buckets, tints);
  CpuImage albedo =
      CullAndRenderBuckets(g, renderer, cull_camera, mats->ptrs);

  for (glm::vec2 c : xy) {
    auto px = WorldXyToPixel(c);
    CpuImage::Color color = albedo.GetPixel(px.first, px.second);
    INFO("world (" << c.x << "," << c.y << ") rgb = " << (int)color.r << ","
                   << (int)color.g << "," << (int)color.b);
    CHECK(color.r < 40);
    CHECK(color.g < 40);
    CHECK(color.b < 40);
  }
}

// ---------------------------------------------------------------------------
// Test 7: edge — all instances in ONE bucket. Every instance is model 0 at the
// same lod (all dist < t0), so bucket 0 holds all N and buckets 1/2 are empty.
// The one non-empty bucket's draw renders all N at their positions; the empty
// buckets' draws render nothing.
// ---------------------------------------------------------------------------
TEST_CASE("GPU edge: all instances in one bucket render together",
          "[gpu_instance][gpu]") {
  TestGpu& g = GetTestGpu();

  const glm::vec2 thresholds(100.0f, 200.0f);  // everything is lod0
  Camera cull_camera = MakeCullCamera(1.0f);

  // Four quads spread across screen X, all at the same (small) distance -> lod0.
  const std::array<glm::vec2, 4> centers = {
      glm::vec2{-1.5f, 0.0f}, glm::vec2{-0.5f, 0.0f}, glm::vec2{0.5f, 0.0f},
      glm::vec2{1.5f, 0.0f}};
  std::vector<GpuInstanceRenderer::InstanceInput> inputs;
  for (glm::vec2 c : centers) {
    glm::vec3 center(c.x, c.y, -5.0f);
    GpuInstanceRenderer::InstanceInput in = MakeInstance(center, 0, 0.3f);
    in.transform = glm::translate(glm::mat4(1.0f), center) *
                   glm::scale(glm::mat4(1.0f), glm::vec3(0.2f));
    inputs.push_back(in);
  }

  const uint32_t num_buckets = GpuInstanceRenderer::kMaxLods;
  GpuInstanceRenderer renderer(g.device, g.queue, *g.gen, 4, 1,
                               {thresholds.x, thresholds.y});
  REQUIRE(renderer.IsValid());
  renderer.UploadInstances(inputs);

  std::array<uint32_t, 6> idx = {0, 1, 2, 0, 2, 3};
  std::vector<float> verts = QuadVerticesSized(1.0f);
  wgpu::Buffer ibuf = UploadBuffer(g.device, idx.data(), sizeof(idx),
                                   wgpu::BufferUsage::Index);
  wgpu::Buffer vbuf = UploadBuffer(g.device, verts.data(),
                                   verts.size() * sizeof(float),
                                   wgpu::BufferUsage::Vertex);
  for (uint32_t b = 0; b < num_buckets; ++b) {
    renderer.SetBucketSubmesh(b, 0, vbuf, ibuf, wgpu::IndexFormat::Uint32, 6);
  }

  const std::vector<glm::vec4> tints(num_buckets,
                                     glm::vec4(1.0f, 1.0f, 1.0f, 1.0f));
  auto mats = MakeBucketMaterials(g, num_buckets, tints);

  // Verify the single-bucket routing via count readback first.
  {
    FrameContext frame;
    frame.Begin(g.device, g.queue, UniformData{});
    renderer.Cull(frame, cull_camera);
    wgpu::CommandBuffer cmd = frame.End();
    g.queue.Submit(1, &cmd);
    test::WaitForGpu(g.instance, g.device, g.queue);
    std::vector<uint32_t> counts = test::ReadBufferSync<uint32_t>(
        g.instance, g.device, g.queue, renderer.GetBucketCountBuffer(), 0,
        num_buckets);
    REQUIRE(counts.size() == num_buckets);
    CHECK(counts[0] == 4);
    CHECK(counts[1] == 0);
    CHECK(counts[2] == 0);
  }

  CpuImage albedo =
      CullAndRenderBuckets(g, renderer, cull_camera, mats->ptrs);
  for (glm::vec2 c : centers) {
    auto px = WorldXyToPixel(c);
    CpuImage::Color color = albedo.GetPixel(px.first, px.second);
    INFO("world (" << c.x << "," << c.y << ") rgb = " << (int)color.r << ","
                   << (int)color.g << "," << (int)color.b);
    CHECK(color.r > 180);  // white tint drawn
  }
}

// ===========================================================================
// Review round 2 RED/GREEN correctness fixes (#1, #2, #3).
// ===========================================================================

// ---------------------------------------------------------------------------
// Finding #1 (correctness): the classify pass must guard a garbage modelId
// BEFORE the `bucket = modelId*kMaxLods + lod` multiply. A modelId large enough
// that modelId*kMaxLods overflows u32 and WRAPS to a small in-range value
// defeats the post-multiply `bucket >= numBuckets` guard, corrupting a valid
// bucket with a garbage instance's transform.
//
// modelId = 0x55555556u, kMaxLods = 3: 0x55555556*3 = 0x100000002, truncated to
// u32 = 2 -> the garbage instance wraps into bucket 2. With num_models = 1
// (num_buckets = 3) bucket 2 is a legitimate bucket (model 0, lod 2); a real
// model-0 lod2 instance also lands there.
//
// RED (pre-fix, no pre-multiply guard): the wrapped bucket 2 is < numBuckets,
// so the garbage instance is appended -> counts[2] == 2 and bucket 2's compacted
// slice contains the garbage transform. GREEN (numModels = numBuckets/kMaxLods =
// 1; modelId 0x55555556 >= 1 -> SENTINEL): counts[2] == 1 and the slice holds
// ONLY the legitimate transform.
// ---------------------------------------------------------------------------
TEST_CASE("GPU classify: overflowing garbage modelId can't corrupt a valid bucket",
          "[gpu_instance][gpu]") {
  TestGpu& g = GetTestGpu();

  const glm::vec2 thresholds(10.0f, 20.0f);
  Camera camera = MakeCullCamera(1.0f);

  // Legit: model 0 at dist 25 (lod2) -> bucket 2. Garbage: modelId 0x55555556 at
  // dist 5 (lod0) -> bucket = 0x55555556*3 wraps to 2. Both on-axis, in-frustum.
  constexpr uint32_t kGarbageModel = 0x55555556u;
  GpuInstanceRenderer::InstanceInput legit =
      MakeInstance(glm::vec3(0.0f, 0.0f, -25.0f), 0u, 0.1f);
  GpuInstanceRenderer::InstanceInput garbage =
      MakeInstance(glm::vec3(0.0f, 0.0f, -5.0f), kGarbageModel, 0.1f);
  std::vector<GpuInstanceRenderer::InstanceInput> inputs = {legit, garbage};

  GpuInstanceRenderer renderer(g.device, g.queue, *g.gen, 2, /*num_models=*/1,
                               {thresholds.x, thresholds.y});
  REQUIRE(renderer.IsValid());
  REQUIRE(renderer.GetNumBuckets() == 3);
  renderer.UploadInstances(inputs);

  FrameContext frame;
  frame.Begin(g.device, g.queue, UniformData{});
  renderer.Cull(frame, camera);
  wgpu::CommandBuffer cmd = frame.End();
  g.queue.Submit(1, &cmd);
  test::WaitForGpu(g.instance, g.device, g.queue);

  std::vector<uint32_t> counts = test::ReadBufferSync<uint32_t>(
      g.instance, g.device, g.queue, renderer.GetBucketCountBuffer(), 0,
      renderer.GetNumBuckets());
  std::vector<uint32_t> bases = test::ReadBufferSync<uint32_t>(
      g.instance, g.device, g.queue, renderer.GetBucketBaseBuffer(), 0,
      renderer.GetNumBuckets());
  std::vector<glm::mat4> compacted = test::ReadBufferSync<glm::mat4>(
      g.instance, g.device, g.queue, renderer.GetCompactedBuffer(), 0,
      renderer.GetCompactedCapacity());
  REQUIRE(counts.size() == 3);
  REQUIRE(bases.size() == 3);

  INFO("counts = " << counts[0] << "," << counts[1] << "," << counts[2]);
  // Bucket 2 must contain ONLY the legitimate instance; the garbage instance
  // (whose modelId overflows into bucket 2) must be culled.
  CHECK(counts[2] == 1);
  REQUIRE(compacted.size() >= bases[2] + 1);
  CHECK(Mat4Equal(compacted[bases[2]], legit.transform));
  CHECK_FALSE(Mat4Equal(compacted[bases[2]], garbage.transform));
}

// ---------------------------------------------------------------------------
// Finding #2 (correctness): an instanced material drawn with NO parameters set
// must still bind its group-0 material-constants UBO. instanced_gbuffer declares
// mat_params @group(0) @binding(5); Dawn requires every layout binding present
// at draw. Before the fix, Bind() appended the params UBO only when
// GetOrCreateUniformBuffer() was non-null (>= 1 SetParameter), so a no-param
// instanced material omitted the binding.
//
// RED (pre-fix): the group-0 bind group is built without binding 5 -> Dawn
// raises a validation error (captured by the pushed scope). A null/IsValid check
// does NOT catch this. GREEN (a zero-filled UBO of the reflected size is always
// created + bound): NoError.
// ---------------------------------------------------------------------------
TEST_CASE("instanced material with no params still binds its group-0 UBO",
          "[gpu_instance][gpu]") {
  TestGpu& g = GetTestGpu();

  auto factory = MakeInstancedFactory(
      g, "instanced_gbuffer", MaterialPassType::kDeferred,
      {GBuffer::kNormalsFormat, GBuffer::kAlbedoFormat, GBuffer::kMaterialFormat},
      {});
  REQUIRE(factory != nullptr);

  MaterialInstanceCache cache;
  // No uniform_overrides at all: the material is resolved WITHOUT any
  // SetParameter, so MaterialInstance builds no constants buffer.
  InstanceParams params;
  entt::id_type key = ComposeMaterialCacheKey(
      entt::hashed_string{"instanced_gbuffer_noparams"}.value(),
      GeometryType::kInstancedMesh, RenderPassType::kGBuffer, 0);
  auto handle = cache.GetOrCreate(key, *factory, GeometryType::kInstancedMesh,
                                  MaterialPassType::kDeferred,
                                  RenderPassType::kGBuffer, params);
  REQUIRE(handle);
  REQUIRE(handle->IsValid());
  auto* inst = handle.operator->();
  REQUIRE(inst->GetGeometryType() == GeometryType::kInstancedMesh);

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
  std::array<uint32_t, 1> base0 = {0u};
  wgpu::Buffer basebuf = UploadBuffer(g.device, base0.data(), sizeof(base0),
                                      wgpu::BufferUsage::Storage);

  UniformData frame_uniforms = MakeOrthoFrame();
  ColorRenderTarget normals_t(g.device, kInstTarget, kInstTarget,
                              GBuffer::kNormalsFormat);
  ColorRenderTarget albedo_t(g.device, kInstTarget, kInstTarget,
                             GBuffer::kAlbedoFormat);
  ColorRenderTarget material_t(g.device, kInstTarget, kInstTarget,
                               GBuffer::kMaterialFormat);
  REQUIRE(albedo_t.IsValid());

  // Capture any validation error the bind/draw provokes (mirrors the
  // BindPerObject test + forward_pass_tests.cpp's validation-scope pattern).
  g.device.PushErrorScope(wgpu::ErrorFilter::Validation);

  FrameContext frame;
  frame.Begin(g.device, g.queue, frame_uniforms);
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
    REQUIRE(inst->BindInstanceData(pass, frame, instbuf, basebuf));
    pass.SetVertexBuffer(0, vbuf);
    pass.SetIndexBuffer(ibuf, wgpu::IndexFormat::Uint32);
    pass.DrawIndexed(6, kInstCount);
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
          INFO("captured error: "
               << (msg.length > 0 ? std::string(msg.data, msg.length)
                                  : std::string("(no message)")));
        }
        scope_done = true;
      });
  while (!scope_done) {
    g.instance.ProcessEvents();
    g.device.Tick();
  }

  CHECK_FALSE(validation_error);
}

// ---------------------------------------------------------------------------
// Finding #3 (correctness): SetBucketSubmesh must NOT clobber the
// GPU-published per-bucket instanceCount. The scan pass writes each bucket's
// survivor count into its indirect-args instanceCount@4 every Cull();
// SetBucketSubmesh only configures a slot's geometry, so calling it after
// Cull (e.g. to swap a slot's mesh) must leave instanceCount intact.
//
// RED (pre-fix, SetBucketSubmesh WriteBuffer'd the full 20-byte struct with
// instanceCount = 0): the post-Cull instanceCount is overwritten with 0. GREEN
// (SetBucketSubmesh writes only indexCount@0 +
// firstIndex/baseVertex/firstInstance @8, skipping instanceCount@4): the
// count survives.
// ---------------------------------------------------------------------------
TEST_CASE("SetBucketSubmesh preserves the GPU-published instanceCount",
          "[gpu_instance][gpu]") {
  TestGpu& g = GetTestGpu();

  const glm::vec2 thresholds(100.0f, 200.0f);  // everything is lod0 -> bucket 0
  Camera camera = MakeCullCamera(1.0f);

  // Four in-frustum instances, all model 0, all lod0 -> bucket 0 count = 4.
  std::vector<GpuInstanceRenderer::InstanceInput> inputs;
  for (int i = 0; i < 4; ++i) {
    inputs.push_back(
        MakeInstance(glm::vec3(0.0f, float(i) * 0.5f, -5.0f), 0u, 0.1f));
  }
  constexpr uint32_t kExpectedCount = 4;

  GpuInstanceRenderer renderer(g.device, g.queue, *g.gen, 4, /*num_models=*/1,
                               {thresholds.x, thresholds.y});
  REQUIRE(renderer.IsValid());
  renderer.UploadInstances(inputs);

  FrameContext frame;
  frame.Begin(g.device, g.queue, UniformData{});
  renderer.Cull(frame, camera);
  wgpu::CommandBuffer cmd = frame.End();
  g.queue.Submit(1, &cmd);
  test::WaitForGpu(g.instance, g.device, g.queue);

  // Sanity: the scan published instanceCount = 4 for bucket 0 (holds in both RED
  // and GREEN -- this read is BEFORE SetBucketSubmesh). instanceCount @ offset 4
  // bytes == index 1 of the 5-u32 indirect-args struct.
  auto read_bucket0_instance_count = [&]() -> uint32_t {
    std::vector<uint32_t> args = test::ReadBufferSync<uint32_t>(
        g.instance, g.device, g.queue, renderer.GetArgsBuffer(), 0, 5);
    REQUIRE(args.size() == 5);
    return args[1];
  };
  REQUIRE(read_bucket0_instance_count() == kExpectedCount);

  // Configure bucket 0's mesh AFTER Cull. This must not wipe instanceCount.
  std::array<uint32_t, 6> idx = {0, 1, 2, 0, 2, 3};
  std::vector<float> verts = QuadVerticesSized(1.0f);
  wgpu::Buffer ibuf = UploadBuffer(g.device, idx.data(), sizeof(idx),
                                   wgpu::BufferUsage::Index);
  wgpu::Buffer vbuf = UploadBuffer(g.device, verts.data(),
                                   verts.size() * sizeof(float),
                                   wgpu::BufferUsage::Vertex);
  renderer.SetBucketSubmesh(0, 0, vbuf, ibuf, wgpu::IndexFormat::Uint32, 6);

  // The published survivor count must be preserved...
  CHECK(read_bucket0_instance_count() == kExpectedCount);
  // ...and the geometry field SetBucketSubmesh DID write must have landed.
  std::vector<uint32_t> args = test::ReadBufferSync<uint32_t>(
      g.instance, g.device, g.queue, renderer.GetArgsBuffer(), 0, 5);
  CHECK(args[0] == 6);  // indexCount written by SetBucketSubmesh
}

// ===========================================================================
// Task 1 (submesh dimension): GpuInstanceRenderer::SetBucketSubmesh /
// GetNumSubmeshes / Draw's (bucket, submesh) callback. One (model,lod)
// bucket's ONE compacted transform slice can now drive several draws
// (different geometry/materials in different render passes), addressed by
// slot = bucket*numSubmeshes+submesh.
// ===========================================================================

// ---------------------------------------------------------------------------
// RED/GREEN: every submesh slot of a bucket must carry that bucket's OWN
// survivor instanceCount (the scan broadcasts one count per bucket to all its
// submesh slots), and each slot's own indexCount (set via SetBucketSubmesh)
// must land untouched at its own slot, independent of its sibling slots.
//
// Ground truth is bucket_count_buffer_ (unaffected by the submesh dimension —
// still one atomic<u32> per bucket), read back and compared against every
// submesh slot of that bucket in the (now numBuckets*numSubmeshes-sized)
// indirect-args buffer.
//
// RED (pre-fix instance_scan.wesl -- serial loop over buckets only, no inner
// submesh loop): the scan writes indirectArgs[b].instanceCount = cnt for b in
// [0, numBuckets) -- i.e. only the FIRST numBuckets physical slots of the now
// (numBuckets*numSubmeshes)-sized buffer. For numSubmeshes=2 those physical
// indices land on (bucket0,submesh0), (bucket0,submesh1) [== old "bucket
// 1"'s count], (bucket1,submesh0) [== old "bucket 2"'s count] -- everything
// from (bucket1,submesh1) onward is never written at all (stays the
// zero-init). So most submesh-1 (and every bucket-2) slot fails to match its
// OWN bucket's count. GREEN (scan loops submeshes inside the bucket loop,
// writing slot = b*numSubmeshes+s for every s): every submesh slot of every
// bucket gets that bucket's own count.
// ---------------------------------------------------------------------------
TEST_CASE("SetBucketSubmesh: every submesh slot of a bucket carries the "
          "bucket's own survivor instanceCount",
          "[gpu_instance][gpu]") {
  TestGpu& g = GetTestGpu();

  const glm::vec2 thresholds(10.0f, 20.0f);
  Camera camera = MakeCullCamera(1.0f);

  // Same distance spread as the LOD-selection test: bucket0=1, bucket1=2,
  // bucket2=2 survivors -- all three buckets non-empty and non-uniform, so a
  // slot reading the WRONG bucket's count can't coincidentally match.
  const std::array<float, 5> dists = {5.0f, 10.0f, 15.0f, 20.0f, 25.0f};
  std::vector<GpuInstanceRenderer::InstanceInput> inputs;
  for (float d : dists) {
    inputs.push_back(MakeInstance(glm::vec3(0.0f, 0.0f, -d), 0, 0.1f));
  }
  constexpr uint32_t kNumSubmeshes = 2;

  GpuInstanceRenderer renderer(g.device, g.queue, *g.gen,
                               static_cast<uint32_t>(inputs.size()),
                               /*num_models=*/1, {thresholds.x, thresholds.y},
                               kNumSubmeshes);
  REQUIRE(renderer.IsValid());
  REQUIRE(renderer.GetNumSubmeshes() == kNumSubmeshes);
  const uint32_t num_buckets = renderer.GetNumBuckets();
  REQUIRE(num_buckets == 3);
  renderer.UploadInstances(inputs);

  std::array<uint32_t, 6> idx = {0, 1, 2, 0, 2, 3};
  wgpu::Buffer ibuf = UploadBuffer(g.device, idx.data(), sizeof(idx),
                                   wgpu::BufferUsage::Index);
  std::vector<float> verts = QuadVerticesSized(1.0f);
  wgpu::Buffer vbuf = UploadBuffer(g.device, verts.data(),
                                   verts.size() * sizeof(float),
                                   wgpu::BufferUsage::Vertex);

  // Distinct indexCount per (bucket, submesh) slot -- proves SetBucketSubmesh
  // targets its own slot without disturbing a sibling slot's geometry.
  auto slot_index_count = [](uint32_t b, uint32_t s) -> uint32_t {
    return 6 + s * 100 + b;
  };
  for (uint32_t b = 0; b < num_buckets; ++b) {
    for (uint32_t s = 0; s < kNumSubmeshes; ++s) {
      renderer.SetBucketSubmesh(b, s, vbuf, ibuf, wgpu::IndexFormat::Uint32,
                                slot_index_count(b, s));
    }
  }

  FrameContext frame;
  frame.Begin(g.device, g.queue, UniformData{});
  renderer.Cull(frame, camera);
  wgpu::CommandBuffer cmd = frame.End();
  g.queue.Submit(1, &cmd);
  test::WaitForGpu(g.instance, g.device, g.queue);

  std::vector<uint32_t> bucket_counts = test::ReadBufferSync<uint32_t>(
      g.instance, g.device, g.queue, renderer.GetBucketCountBuffer(), 0,
      num_buckets);
  REQUIRE(bucket_counts.size() == num_buckets);
  INFO("bucket counts: " << bucket_counts[0] << "," << bucket_counts[1] << ","
                        << bucket_counts[2]);
  REQUIRE(bucket_counts[0] == 1);
  REQUIRE(bucket_counts[1] == 2);
  REQUIRE(bucket_counts[2] == 2);

  // args buffer laid out [bucket*numSubmeshes+submesh], 5 u32s (20 bytes) per
  // slot: indexCount, instanceCount, firstIndex, baseVertex, firstInstance.
  const size_t num_slots = size_t{num_buckets} * kNumSubmeshes;
  std::vector<uint32_t> args = test::ReadBufferSync<uint32_t>(
      g.instance, g.device, g.queue, renderer.GetArgsBuffer(), 0,
      num_slots * 5);
  REQUIRE(args.size() == num_slots * 5);

  for (uint32_t b = 0; b < num_buckets; ++b) {
    for (uint32_t s = 0; s < kNumSubmeshes; ++s) {
      const uint32_t slot = b * kNumSubmeshes + s;
      const uint32_t index_count = args[slot * 5 + 0];
      const uint32_t instance_count = args[slot * 5 + 1];
      INFO("bucket " << b << " submesh " << s << " slot " << slot
                     << ": indexCount=" << index_count
                     << " instanceCount=" << instance_count);
      CHECK(index_count == slot_index_count(b, s));
      // Every submesh slot of a bucket must carry THAT bucket's own survivor
      // count -- one compacted slice drives all its submesh draws.
      CHECK(instance_count == bucket_counts[b]);
    }
  }
}

// ---------------------------------------------------------------------------
// Shared-slice render: one bucket, two submeshes with DISTINCT geometry
// (mesh-local-offset quads) and DISTINCT materials (distinct tints), both
// baked with the SAME bucketId (0) -- proves both submeshes' draws read the
// SAME compacted transform slice: the vertex shader's
// compacted[bucketBase[bucketId] + instance_index] depends only on
// bucketId, which both materials share, so per instance BOTH submeshes must
// appear at that ONE instance's world transform (offset only by their own
// mesh-local geometry), never at some other/independently-culled position.
// ---------------------------------------------------------------------------
TEST_CASE("GPU shared-slice render: a bucket's submeshes draw the SAME "
          "instance transforms",
          "[gpu_instance][gpu]") {
  TestGpu& g = GetTestGpu();

  const glm::vec2 thresholds(100.0f, 200.0f);  // everything is lod0 -> bucket 0
  Camera cull_camera = MakeCullCamera(1.0f);

  // Two instances (translate-only transforms -- MakeInstance's default).
  const std::array<glm::vec2, 2> centers = {glm::vec2{-1.3f, 0.0f},
                                            glm::vec2{1.3f, 0.0f}};
  std::vector<GpuInstanceRenderer::InstanceInput> inputs;
  for (glm::vec2 c : centers) {
    inputs.push_back(MakeInstance(glm::vec3(c.x, c.y, -5.0f), 0, 0.3f));
  }

  constexpr uint32_t kNumSubmeshes = 2;
  GpuInstanceRenderer renderer(g.device, g.queue, *g.gen,
                               static_cast<uint32_t>(inputs.size()),
                               /*num_models=*/1, {thresholds.x, thresholds.y},
                               kNumSubmeshes);
  REQUIRE(renderer.IsValid());
  renderer.UploadInstances(inputs);

  // Submesh 0: a small quad offset -0.3 in mesh-local X. Submesh 1: +0.3.
  // Baked into the mesh, NOT the per-instance transform.
  std::array<uint32_t, 6> idx = {0, 1, 2, 0, 2, 3};
  wgpu::Buffer ibuf = UploadBuffer(g.device, idx.data(), sizeof(idx),
                                   wgpu::BufferUsage::Index);
  std::vector<float> verts0 = QuadVerticesOffset(0.15f, {-0.3f, 0.0f});
  std::vector<float> verts1 = QuadVerticesOffset(0.15f, {0.3f, 0.0f});
  wgpu::Buffer vbuf0 = UploadBuffer(g.device, verts0.data(),
                                    verts0.size() * sizeof(float),
                                    wgpu::BufferUsage::Vertex);
  wgpu::Buffer vbuf1 = UploadBuffer(g.device, verts1.data(),
                                    verts1.size() * sizeof(float),
                                    wgpu::BufferUsage::Vertex);
  renderer.SetBucketSubmesh(0, 0, vbuf0, ibuf, wgpu::IndexFormat::Uint32, 6);
  renderer.SetBucketSubmesh(0, 1, vbuf1, ibuf, wgpu::IndexFormat::Uint32, 6);

  // Two materials, BOTH bucketId=0 (the shared bucket), distinct tints.
  auto factory = MakeInstancedFactory(
      g, "instanced_gbuffer", MaterialPassType::kDeferred,
      {GBuffer::kNormalsFormat, GBuffer::kAlbedoFormat, GBuffer::kMaterialFormat},
      {});
  REQUIRE(factory != nullptr);
  MaterialInstanceCache cache;
  auto make_mat = [&](const char* name, glm::vec4 tint) {
    InstanceParams params;
    params.uniform_overrides["tint"] = MaterialParameterValue(tint);
    params.uniform_overrides["bucketId"] = MaterialParameterValue(uint32_t(0));
    entt::id_type key = ComposeMaterialCacheKey(
        entt::hashed_string{name}.value(), GeometryType::kInstancedMesh,
        RenderPassType::kGBuffer, 0);
    auto handle = cache.GetOrCreate(key, *factory, GeometryType::kInstancedMesh,
                                    MaterialPassType::kDeferred,
                                    RenderPassType::kGBuffer, params);
    REQUIRE(handle);
    REQUIRE(handle->IsValid());
    return handle;
  };
  auto mat0 = make_mat("shared_slice_submesh0", glm::vec4(1.0f, 0.0f, 0.0f, 1.0f));
  auto mat1 = make_mat("shared_slice_submesh1", glm::vec4(0.0f, 1.0f, 0.0f, 1.0f));
  RenderingMaterialInstance* mat0_ptr = mat0.operator->();
  RenderingMaterialInstance* mat1_ptr = mat1.operator->();

  UniformData frame_uniforms = MakeOrthoFrame();
  FrameContext frame;
  frame.Begin(g.device, g.queue, frame_uniforms);
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

    renderer.Draw(pass, frame,
                  [&](uint32_t bucket, uint32_t submesh)
                      -> RenderingMaterialInstance* {
                    if (bucket != 0) return nullptr;  // only bucket 0 has meshes
                    RenderingMaterialInstance* m =
                        submesh == 0 ? mat0_ptr : mat1_ptr;
                    if (!m->Bind(pass, frame)) return nullptr;
                    return m;
                  });
    pass.End();
  }

  wgpu::CommandBuffer cmd = frame.End();
  g.queue.Submit(1, &cmd);
  test::WaitForGpu(g.instance, g.device, g.queue);

  TextureReadback rb(g.instance, g.device, g.queue);
  CpuImage albedo = rb.ReadTextureSync(albedo_t.GetTexture(), kInstTarget,
                                       kInstTarget, GBuffer::kAlbedoFormat);

  for (glm::vec2 c : centers) {
    auto px0 = WorldXyToPixel({c.x - 0.3f, c.y});
    auto px1 = WorldXyToPixel({c.x + 0.3f, c.y});
    CpuImage::Color c0 = albedo.GetPixel(px0.first, px0.second);
    CpuImage::Color c1 = albedo.GetPixel(px1.first, px1.second);
    INFO("instance (" << c.x << "," << c.y << ") submesh0 @ (" << px0.first
                      << "," << px0.second << ") rgb=" << (int)c0.r << ","
                      << (int)c0.g << "," << (int)c0.b);
    CHECK(c0.r > 180);
    CHECK(c0.g < 60);
    CHECK(c0.b < 60);
    INFO("instance (" << c.x << "," << c.y << ") submesh1 @ (" << px1.first
                      << "," << px1.second << ") rgb=" << (int)c1.r << ","
                      << (int)c1.g << "," << (int)c1.b);
    CHECK(c1.r < 60);
    CHECK(c1.g > 180);
    CHECK(c1.b < 60);
  }
  // The gap between the two submeshes' quads (and between the two instances)
  // stays the clear color.
  auto gap_px = WorldXyToPixel({0.0f, 0.0f});
  CpuImage::Color gap = albedo.GetPixel(gap_px.first, gap_px.second);
  CHECK(gap.r < 40);
  CHECK(gap.g < 40);
  CHECK(gap.b < 40);
}
