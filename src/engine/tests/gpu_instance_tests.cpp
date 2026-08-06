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
#include <cmath>
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
#include <spdlog/sinks/ringbuffer_sink.h>
#include <spdlog/spdlog.h>

#include "core/geometry_type.hpp"
#include "core/util/cpu_image.hpp"
#include "engine/core/camera.hpp"
#include "engine/rendering/color_render_target.hpp"
#include "engine/rendering/context/frame_context.hpp"
#include "engine/rendering/context/render_pass_context.hpp"
#include "engine/rendering/context/scene_context.hpp"
#include "engine/rendering/frustum.hpp"
#include "engine/rendering/gbuffer.hpp"
#include "engine/rendering/gpu_instance_renderer.hpp"
#include "engine/rendering/instanced_mesh_field.hpp"
#include "engine/rendering/material/material_instance_cache.hpp"
#include "engine/rendering/material/material_instance_factory.hpp"
#include "engine/rendering/material/rendering_material_instance.hpp"
#include "engine/rendering/material_library.hpp"
#include "engine/rendering/passes/render_forward.hpp"
#include "engine/rendering/scene_build.hpp"
#include "engine/rendering/scene_renderer.hpp"
#include "engine/rendering/shader/gpu_pipeline_generator.hpp"
#include "engine/rendering/texture_readback.hpp"
#include "engine/rendering/util/find_shader_directory.hpp"
#include "engine/scene/scene_graph.hpp"
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

// One kTexturedMesh quad (pos3+uv2+normal3+tangent4), z=0, +Z normal.
std::vector<float> QuadVertices() {
  return {
      -1, -1, 0, 0, 0, 0, 0, 1, 1, 0, 0, 1,  //
      1,  -1, 0, 1, 0, 0, 0, 1, 1, 0, 0, 1,  //
      1,  1,  0, 1, 1, 0, 0, 1, 1, 0, 0, 1,  //
      -1, 1,  0, 0, 1, 0, 0, 1, 1, 0, 0, 1,  //
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

// CapturedError/RunCapturingValidationErrors -- shared, see
// gpu_test_helpers.hpp for the full rationale (this file's own copy of both
// used to live here; hoisted so game/tests/tree_field_gpu_tests.cpp doesn't
// carry a second copy).
using badlands::test::CapturedError;
using badlands::test::RunCapturingValidationErrors;

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
  CapturedError err = RunCapturingValidationErrors(g.instance, g.device, [&] {
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
  CapturedError err = RunCapturingValidationErrors(g.instance, g.device, [&] {
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
  CapturedError err = RunCapturingValidationErrors(g.instance, g.device, [&] {
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

// Every renderer in this suite was written against the pre-per-model-LOD API,
// where ONE 3-level ladder (two cutoffs) applied to every model. This rebuilds
// that shape as `num_models` identical 3-level chains, so those tests keep
// exercising exactly what they did before. Tests that care about per-model
// chains (differing lod_count/thresholds) build their own vectors instead.
// (Callers wanting "everything lands in LOD0" pass two DISTINCT huge cutoffs,
// e.g. 1e30/2e30, not the same value twice: equal cutoffs would make the level
// between them unselectable, which the ctor rejects as a malformed chain.)
std::vector<GpuInstanceRenderer::ModelLod> Lod3Chains(uint32_t num_models,
                                                      float t0, float t1) {
  GpuInstanceRenderer::ModelLod chain;
  chain.lod_count = 3;
  chain.thresholds[0] = t0;
  chain.thresholds[1] = t1;
  return std::vector<GpuInstanceRenderer::ModelLod>(std::max(1u, num_models),
                                                     chain);
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
                               /*num_models=*/1, Lod3Chains(1, 1e30f, 2e30f));
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
                               Lod3Chains(1, thresholds.x, thresholds.y));
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
  // One bucket per (model, lod) SLOT -- kMaxLods of them for this 1-model
  // renderer, whatever the model's own 3-level chain uses. The buckets past
  // the model's lod_count stay empty (nothing can select them).
  REQUIRE(counts.size() == GpuInstanceRenderer::kMaxLods);
  INFO("lod counts: " << counts[0] << "," << counts[1] << "," << counts[2]);
  CHECK(counts[0] == 1);  // {5}
  CHECK(counts[1] == 2);  // {10 (boundary), 15}
  CHECK(counts[2] == 2);  // {20 (boundary), 25}
  for (size_t b = 3; b < counts.size(); ++b) {
    INFO("bucket " << b << " is past the model's 3-level chain");
    CHECK(counts[b] == 0);
  }
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
                               Lod3Chains(num_models, thresholds.x, thresholds.y));
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

// A quad (pos3+uv2+normal3+tangent4, +Z normal) of the given half-extent.
std::vector<float> QuadVerticesSized(float h) {
  return {
      -h, -h, 0, 0, 0, 0, 0, 1, 1, 0, 0, 1,  //
      h,  -h, 0, 1, 0, 0, 0, 1, 1, 0, 0, 1,  //
      h,  h,  0, 1, 1, 0, 0, 1, 1, 0, 0, 1,  //
      -h, h,  0, 0, 1, 0, 0, 1, 1, 0, 0, 1,  //
  };
}

// A quad of the given half-extent, centered at `offset` IN MESH-LOCAL space
// (baked into the vertex positions, not the per-instance transform). Two
// submeshes of the same bucket using different offsets render as distinct
// on-screen shapes per instance while still reading the exact same
// per-instance world transform.
std::vector<float> QuadVerticesOffset(float h, glm::vec2 offset) {
  return {
      offset.x - h, offset.y - h, 0, 0, 0, 0, 0, 1, 1, 0, 0, 1,  //
      offset.x + h, offset.y - h, 0, 1, 0, 0, 0, 1, 1, 0, 0, 1,  //
      offset.x + h, offset.y + h, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1,  //
      offset.x - h, offset.y + h, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1,  //
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
                               Lod3Chains(1, thresholds.x, thresholds.y));
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
  GpuInstanceRenderer renderer(g.device, g.queue, *g.gen, 3, 1, Lod3Chains(1, 10.0f, 20.0f));
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
                               Lod3Chains(1, thresholds.x, thresholds.y));
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
// A chain whose lod_count outruns the thresholds actually filled in is
// malformed: ModelLod::thresholds default to 0.0f, so selectLod runs straight
// past every level whose cutoff stayed 0 and those levels' meshes never draw.
// The ctor must say so rather than let it pass silently.
// ---------------------------------------------------------------------------
TEST_CASE("GpuInstanceRenderer: an under-filled LOD chain is reported",
          "[gpu_instance][gpu]") {
  TestGpu& g = GetTestGpu();

  auto ring_sink = std::make_shared<spdlog::sinks::ringbuffer_sink_mt>(32);
  std::shared_ptr<spdlog::logger> logger = spdlog::default_logger();
  const std::vector<spdlog::sink_ptr> saved_sinks = logger->sinks();

  // 4 levels declared, only the first two cutoffs set -- thresholds[2] stays
  // 0.0f, so LOD2 could never be selected.
  std::vector<GpuInstanceRenderer::ModelLod> chains(1);
  chains[0].lod_count = 4;
  chains[0].thresholds[0] = 10.0f;
  chains[0].thresholds[1] = 20.0f;

  logger->sinks() = {ring_sink};
  { GpuInstanceRenderer renderer(g.device, g.queue, *g.gen, 1, 1, chains); }
  logger->sinks() = saved_sinks;  // restore before any assertion below

  const std::vector<std::string> messages = ring_sink->last_formatted();
  const bool named = std::any_of(
      messages.begin(), messages.end(), [](const std::string& m) {
        return m.find("strictly ascending") != std::string::npos;
      });
  INFO("captured " << messages.size() << " message(s)");
  CHECK(named);
}

// ---------------------------------------------------------------------------
// Per-model LOD chains: kMaxLods is only a compile-time CAP (and the bucket
// stride). How many levels a model actually has, and the distances it switches
// at, are runtime and PER MODEL -- so two models with different chains can
// share one renderer, each selecting against its own.
//
// Model 0 declares 2 levels cutting at 10m; model 1 declares 5 levels cutting
// at 10/20/30/40m. Five instances of each sit at 5/15/25/35/45m. Model 0 must
// saturate at ITS coarsest level (lod1) for everything past 10m -- not run on
// to lod4 the way model 1 does, and not be capped at kMaxLods-1 either.
// ---------------------------------------------------------------------------
TEST_CASE("GPU classify: each model selects against its own LOD chain",
          "[gpu_instance][gpu]") {
  TestGpu& g = GetTestGpu();
  Camera camera = MakeCullCamera(1.0f);

  constexpr uint32_t kNumModels = 2;
  std::vector<GpuInstanceRenderer::ModelLod> chains(kNumModels);
  chains[0].lod_count = 2;
  chains[0].thresholds[0] = 10.0f;
  chains[1].lod_count = 5;
  chains[1].thresholds[0] = 10.0f;
  chains[1].thresholds[1] = 20.0f;
  chains[1].thresholds[2] = 30.0f;
  chains[1].thresholds[3] = 40.0f;

  const std::array<float, 5> distances = {5.0f, 15.0f, 25.0f, 35.0f, 45.0f};
  std::vector<GpuInstanceRenderer::InstanceInput> inputs;
  for (uint32_t model = 0; model < kNumModels; ++model) {
    for (float d : distances) {
      inputs.push_back(MakeInstance(glm::vec3(0.0f, 0.0f, -d), model, 0.1f));
    }
  }

  GpuInstanceRenderer renderer(g.device, g.queue, *g.gen,
                               static_cast<uint32_t>(inputs.size()), kNumModels,
                               chains);
  REQUIRE(renderer.IsValid());
  CHECK(renderer.GetModelLodCount(0) == 2);
  CHECK(renderer.GetModelLodCount(1) == 5);
  renderer.UploadInstances(inputs);

  FrameContext frame;
  frame.Begin(g.device, g.queue, UniformData{});
  renderer.Cull(frame, camera);
  wgpu::CommandBuffer cmd = frame.End();
  g.queue.Submit(1, &cmd);
  test::WaitForGpu(g.instance, g.device, g.queue);

  const std::vector<uint32_t> counts = test::ReadBufferSync<uint32_t>(
      g.instance, g.device, g.queue, renderer.GetBucketCountBuffer(), 0,
      renderer.GetNumBuckets());
  REQUIRE(counts.size() == kNumModels * GpuInstanceRenderer::kMaxLods);

  // Expected: model 0 -> one instance at lod0, the other four all at lod1
  // (its own coarsest). Model 1 -> one instance at each of its five levels.
  std::vector<uint32_t> expected(counts.size(), 0u);
  expected[GpuInstanceRenderer::BucketId(0, 0)] = 1;
  expected[GpuInstanceRenderer::BucketId(0, 1)] = 4;
  for (uint32_t lod = 0; lod < 5; ++lod) {
    expected[GpuInstanceRenderer::BucketId(1, lod)] = 1;
  }

  for (size_t b = 0; b < counts.size(); ++b) {
    INFO("bucket " << b << " (model " << b / GpuInstanceRenderer::kMaxLods
                   << ", lod " << b % GpuInstanceRenderer::kMaxLods << ")");
    CHECK(counts[b] == expected[b]);
  }
}

// ---------------------------------------------------------------------------
// Finding #1 (correctness): the classify pass must guard a garbage modelId
// BEFORE the `bucket = modelId*kMaxLods + lod` multiply. A modelId large enough
// that modelId*kMaxLods overflows u32 and WRAPS to a small in-range value
// defeats the post-multiply `bucket >= numBuckets` guard, corrupting a valid
// bucket with a garbage instance's transform.
//
// The wrapping modelId has to be re-derived whenever kMaxLods changes. At
// kMaxLods = 8: modelId*8 is always a multiple of 8, so after truncation to
// u32 the wrapped bucket is (multiple of 8) + lod -- to land inside a
// num_models = 1 renderer's buckets it must wrap to exactly 0, which
// modelId = 0x20000000 does (0x20000000*8 = 0x100000000, truncated to 0).
// The garbage instance is therefore placed at the SAME lod as the legitimate
// one (lod 2, i.e. beyond the 20m threshold) so both target bucket 2.
// (At the old kMaxLods = 3 this test used modelId 0x55555556, whose *3
// truncates to 2 directly, with the garbage instance at lod 0.)
//
// RED (pre-fix, no pre-multiply guard): the wrapped bucket 2 is < numBuckets,
// so the garbage instance is appended -> counts[2] == 2 and bucket 2's compacted
// slice contains the garbage transform. GREEN (numModels = numBuckets/kMaxLods =
// 1; modelId 0x20000000 >= 1 -> SENTINEL): counts[2] == 1 and the slice holds
// ONLY the legitimate transform.
// ---------------------------------------------------------------------------
TEST_CASE("GPU classify: overflowing garbage modelId can't corrupt a valid bucket",
          "[gpu_instance][gpu]") {
  TestGpu& g = GetTestGpu();

  const glm::vec2 thresholds(10.0f, 20.0f);
  Camera camera = MakeCullCamera(1.0f);

  // Legit: model 0 at dist 25 (lod2) -> bucket 2. Garbage: modelId 0x20000000
  // at dist 22 (also lod2) -> bucket = 0x20000000*kMaxLods wraps to 0, + lod 2
  // = 2. Both on-axis, in-frustum, at distinct positions so their transforms
  // are distinguishable.
  constexpr uint32_t kGarbageModel = 0x20000000u;
  GpuInstanceRenderer::InstanceInput legit =
      MakeInstance(glm::vec3(0.0f, 0.0f, -25.0f), 0u, 0.1f);
  GpuInstanceRenderer::InstanceInput garbage =
      MakeInstance(glm::vec3(0.0f, 0.0f, -22.0f), kGarbageModel, 0.1f);
  static_assert(kGarbageModel * GpuInstanceRenderer::kMaxLods == 0u,
                "kGarbageModel must wrap to bucket base 0 at this kMaxLods");
  std::vector<GpuInstanceRenderer::InstanceInput> inputs = {legit, garbage};

  GpuInstanceRenderer renderer(g.device, g.queue, *g.gen, 2, /*num_models=*/1,
                               Lod3Chains(1, thresholds.x, thresholds.y));
  REQUIRE(renderer.IsValid());
  REQUIRE(renderer.GetNumBuckets() == GpuInstanceRenderer::kMaxLods);
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
  REQUIRE(counts.size() == GpuInstanceRenderer::kMaxLods);
  REQUIRE(bases.size() == GpuInstanceRenderer::kMaxLods);

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
                               Lod3Chains(1, thresholds.x, thresholds.y));
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
                               /*num_models=*/1, Lod3Chains(1, thresholds.x, thresholds.y),
                               kNumSubmeshes);
  REQUIRE(renderer.IsValid());
  REQUIRE(renderer.GetNumSubmeshes() == kNumSubmeshes);
  const uint32_t num_buckets = renderer.GetNumBuckets();
  REQUIRE(num_buckets == GpuInstanceRenderer::kMaxLods);
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
                               /*num_models=*/1, Lod3Chains(1, thresholds.x, thresholds.y),
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

// ===========================================================================
// Review fix: the shadow cull buffer set (config/perInstanceBucket/
// bucketCount/bucketBase/writeCursor/compacted/args, 7 buffers + 3 bind
// groups) is now LAZY -- allocated on first need (CullShadow() or
// InstancedMeshField::SetSubmeshShadow's EnsureShadowCull() call) rather
// than eagerly by the constructor, since most GpuInstanceRenderers never
// cast a shadow. Proven directly via GpuInstanceRenderer: GetShadowArgsBuffer
// stays null until CullShadow() runs, and a slot configured via
// SetBucketSubmesh BEFORE that first CullShadow() still has its geometry
// (indexCount) correctly present in the shadow args buffer afterward -- the
// REPLAY EnsureShadowCullResources() performs on creation.
// ===========================================================================
TEST_CASE("GPU shadow cull resources are lazy: null until CullShadow(), then "
          "replay prefills a slot configured before the first CullShadow()",
          "[gpu_instance][gpu]") {
  TestGpu& g = GetTestGpu();

  // thresholds pushed to +inf -> lod0 -> bucket 0 regardless of distance.
  GpuInstanceRenderer renderer(g.device, g.queue, *g.gen, /*capacity=*/1,
                               /*num_models=*/1, Lod3Chains(1, 1e30f, 2e30f));
  REQUIRE(renderer.IsValid());

  // A dummy (never actually drawn in this test) mesh, just to give bucket
  // 0/submesh 0 real geometry -- SetBucketSubmesh's indexCount is what the
  // replay below must have carried into the shadow args buffer.
  constexpr uint32_t kIndexCount = 42;
  std::array<float, 3> dummy_verts = {0.0f, 0.0f, 0.0f};
  std::array<uint32_t, 3> dummy_indices = {0, 0, 0};
  wgpu::Buffer vbuf = UploadBuffer(g.device, dummy_verts.data(),
                                   dummy_verts.size() * sizeof(float),
                                   wgpu::BufferUsage::Vertex);
  wgpu::Buffer ibuf = UploadBuffer(g.device, dummy_indices.data(),
                                   dummy_indices.size() * sizeof(uint32_t),
                                   wgpu::BufferUsage::Index);
  renderer.SetBucketSubmesh(/*bucket=*/0, /*submesh=*/0, vbuf, ibuf,
                            wgpu::IndexFormat::Uint32, kIndexCount);

  // RED (pre-fix): the shadow set was allocated eagerly by the constructor,
  // so this was already non-null here -- no lazy-creation contract existed to
  // check. GREEN (post-fix): still null -- nothing has requested shadow
  // resources yet (SetBucketSubmesh alone doesn't).
  CHECK(renderer.GetShadowArgsBuffer() == nullptr);

  Camera camera = MakeCullCamera(1.0f);
  const glm::mat4 light_view_proj(1.0f);  // contents irrelevant -- not read back

  FrameContext frame;
  frame.Begin(g.device, g.queue, UniformData{});
  renderer.CullShadow(frame, camera, light_view_proj);
  wgpu::CommandBuffer cmd = frame.End();
  g.queue.Submit(1, &cmd);
  test::WaitForGpu(g.instance, g.device, g.queue);

  REQUIRE(renderer.GetShadowArgsBuffer() != nullptr);

  // Slot 0's indirect-args geometry fields (index_count@0, instance_count@4,
  // first_index@8, base_vertex@12, first_instance@16 -- kArgsStride=20 bytes,
  // 5 u32-sized fields) must show the REPLAYED indexCount, proving
  // EnsureShadowCullResources() picked up a slot configured before it ever
  // ran, not just future SetBucketSubmesh calls.
  std::vector<uint32_t> args = test::ReadBufferSync<uint32_t>(
      g.instance, g.device, g.queue, renderer.GetShadowArgsBuffer(), 0, 5);
  REQUIRE(args.size() == 5);
  CHECK(args[0] == kIndexCount);  // index_count
  CHECK(args[2] == 0u);           // first_index
  CHECK(args[3] == 0u);           // base_vertex
  CHECK(args[4] == 0u);           // first_instance
}

// ===========================================================================
// Phase 4 of the volumetric-foliage feature: a dedicated shadow cull set.
// CullShadow() dispatches the SAME classify/scan/scatter pipeline as Cull(),
// but against the light's frustum, into a SEPARATE buffer set -- Cull()'s
// camera-frustum result and CullShadow()'s light-frustum result must never
// leak into each other. Proven directly via GpuInstanceRenderer (not through
// InstancedMeshField): one instance placed INSIDE the light's ortho box but
// BEHIND the camera (so Cull() culls it, CullShadow() doesn't), and the
// mirrored placement (inside the camera frustum, outside the light box).
// ---------------------------------------------------------------------------
TEST_CASE("GPU cull: main and shadow cull sets are independent",
          "[gpu_instance][gpu]") {
  TestGpu& g = GetTestGpu();

  Camera camera = MakeCullCamera(1.0f);  // at the origin, looking down -Z

  // A top-down light: eye at (0,20,0) looking straight down at the origin,
  // "up" = -Z (so the light's local axes are: right=world X, its own up=
  // -world Z, forward=-world Y) -- deliberately unrelated to the camera's
  // -Z-facing frustum, so the two only agree by coincidence, never by
  // construction. The ortho box (verified numerically, not just by
  // construction, since a light-space basis is easy to mis-derive by hand)
  // covers world X in [-5,5], world Z in [-5,5], world Y in [-20,20]
  // (eye.y-far .. eye.y-near along the forward axis).
  const glm::mat4 light_proj = glm::ortho(-5.0f, 5.0f, -5.0f, 5.0f, 0.0f, 40.0f);
  const glm::mat4 light_view =
      glm::lookAt(glm::vec3(0.0f, 20.0f, 0.0f), glm::vec3(0.0f, 0.0f, 0.0f),
                  glm::vec3(0.0f, 0.0f, -1.0f));
  const glm::mat4 light_view_proj = light_proj * light_view;

  // Case 1: behind the camera (camera looks down -Z, so +Z is culled by
  // Cull()) but squarely inside the light's box (X=0, Z=3, Y=0 -- all within
  // the ranges above).
  const GpuInstanceRenderer::InstanceInput behind_camera =
      MakeInstance(glm::vec3(0.0f, 0.0f, 3.0f), /*model=*/0u, /*radius=*/0.5f);
  // Case 2: dead ahead of the camera (inside Cull()'s frustum -- the "straight
  // ahead" case other cull tests in this file use) but at Z=-10, outside the
  // light's Z in [-5,5] box (outside CullShadow()'s frustum).
  const GpuInstanceRenderer::InstanceInput outside_light =
      MakeInstance(glm::vec3(0.0f, 0.0f, -10.0f), /*model=*/0u,
                  /*radius=*/0.5f);

  struct Survivors {
    uint32_t main_count = 0;
    uint32_t shadow_count = 0;
  };
  auto run_case = [&](const GpuInstanceRenderer::InstanceInput& instance) {
    // thresholds pushed to +inf -> lod0 -> bucket 0 regardless of distance.
    GpuInstanceRenderer renderer(g.device, g.queue, *g.gen, /*capacity=*/1,
                                 /*num_models=*/1, Lod3Chains(1, 1e30f, 2e30f));
    REQUIRE(renderer.IsValid());
    renderer.UploadInstances(
        std::span<const GpuInstanceRenderer::InstanceInput>(&instance, 1));

    FrameContext frame;
    frame.Begin(g.device, g.queue, UniformData{});
    renderer.Cull(frame, camera);
    renderer.CullShadow(frame, camera, light_view_proj);
    wgpu::CommandBuffer cmd = frame.End();
    g.queue.Submit(1, &cmd);
    test::WaitForGpu(g.instance, g.device, g.queue);

    std::vector<uint32_t> main_args = test::ReadBufferSync<uint32_t>(
        g.instance, g.device, g.queue, renderer.GetArgsBuffer(), 0, 5);
    std::vector<uint32_t> shadow_args = test::ReadBufferSync<uint32_t>(
        g.instance, g.device, g.queue, renderer.GetShadowArgsBuffer(), 0, 5);
    REQUIRE(main_args.size() == 5);
    REQUIRE(shadow_args.size() == 5);
    Survivors s{main_args[1], shadow_args[1]};  // instanceCount @ index 1

    // Ground-truth cross-check against the raw per-bucket counts (bucket 0),
    // independent of the args-buffer broadcast.
    std::vector<uint32_t> main_counts = test::ReadBufferSync<uint32_t>(
        g.instance, g.device, g.queue, renderer.GetBucketCountBuffer(), 0, 1);
    std::vector<uint32_t> shadow_counts = test::ReadBufferSync<uint32_t>(
        g.instance, g.device, g.queue, renderer.GetShadowBucketCountBuffer(), 0,
        1);
    REQUIRE(main_counts.size() == 1);
    REQUIRE(shadow_counts.size() == 1);
    CHECK(main_counts[0] == s.main_count);
    CHECK(shadow_counts[0] == s.shadow_count);

    // The surviving set's compacted transform is the instance's own (proves
    // "shows 1 survivor" means the actual instance, not a stray write).
    if (s.main_count == 1) {
      std::vector<glm::mat4> compacted = test::ReadBufferSync<glm::mat4>(
          g.instance, g.device, g.queue, renderer.GetCompactedBuffer(), 0, 1);
      REQUIRE(compacted.size() == 1);
      CHECK(Mat4Equal(compacted[0], instance.transform));
    }
    if (s.shadow_count == 1) {
      std::vector<glm::mat4> shadow_compacted = test::ReadBufferSync<glm::mat4>(
          g.instance, g.device, g.queue, renderer.GetShadowCompactedBuffer(), 0,
          1);
      REQUIRE(shadow_compacted.size() == 1);
      CHECK(Mat4Equal(shadow_compacted[0], instance.transform));
    }
    return s;
  };

  {
    Survivors s = run_case(behind_camera);
    INFO("behind camera / inside light box: main=" << s.main_count
                                                    << " shadow=" << s.shadow_count);
    CHECK(s.main_count == 0);
    CHECK(s.shadow_count == 1);
  }
  {
    Survivors s = run_case(outside_light);
    INFO("inside camera / outside light box: main=" << s.main_count
                                                     << " shadow=" << s.shadow_count);
    CHECK(s.main_count == 1);
    CHECK(s.shadow_count == 0);
  }
}

// ===========================================================================
// Task 2: InstancedMeshField — the reusable engine component bundling a
// GpuInstanceRenderer with a per-(bucket,submesh) {PassKind, material}
// mapping. Fixture: one model, one lod (thresholds pushed to +inf so a single
// instance always lands in bucket 0), two submeshes on that ONE bucket — a
// deferred (instanced_gbuffer, no @group(2)) submesh and a forward-opaque
// (instanced_forward, @group(2)-declaring) submesh — each with its own
// mesh-local offset (distinct on-screen position) AND distinct color, so a
// pass-filter bug (Draw(kind) drawing the wrong submesh) is caught by a pixel
// assertion rather than relying on a pipeline/target-format mismatch to fail
// first.
// ===========================================================================
namespace {

constexpr glm::vec2 kFieldInstanceCenter{0.0f, 0.0f};
constexpr float kFieldSubmeshOffset = 0.35f;    // deferred at -offset, forward at +offset
constexpr float kFieldSubmeshHalfExtent = 0.15f;

// Owns everything the fixture's field references (factories/cache/handles
// keep the resolved RenderingMaterialInstances alive; the field only holds
// non-owning pointers to them, per SetSubmesh's contract).
struct DeferredForwardFieldFixture {
  std::unique_ptr<MaterialInstanceFactory> gbuffer_factory;
  std::unique_ptr<MaterialInstanceFactory> forward_factory;
  MaterialInstanceCache cache;
  entt::resource<RenderingMaterialInstance> gbuffer_handle;
  entt::resource<RenderingMaterialInstance> forward_handle;
  wgpu::Buffer ibuf;
  wgpu::Buffer vbuf_deferred;
  wgpu::Buffer vbuf_forward;
  std::unique_ptr<InstancedMeshField> field;
};

// `depth_format` defaults to Undefined (no depth-stencil state), matching the
// isolated Draw() tests below, which render into depth-less custom targets.
// The Task-3 SceneRenderer-integration test passes GBuffer::kDepthFormat
// instead: SceneRenderer's real G-buffer/forward-opaque passes DO carry a
// depth attachment, and deferred_lighting.wesl discards any pixel whose depth
// still reads as the far-plane clear value -- so the deferred submesh must
// actually write real depth for the lit result to show it at all.
std::unique_ptr<DeferredForwardFieldFixture> BuildDeferredForwardField(
    TestGpu& g,
    wgpu::TextureFormat depth_format = wgpu::TextureFormat::Undefined) {
  auto fx = std::make_unique<DeferredForwardFieldFixture>();

  fx->gbuffer_factory = MakeInstancedFactory(
      g, "instanced_gbuffer", MaterialPassType::kDeferred,
      {GBuffer::kNormalsFormat, GBuffer::kAlbedoFormat, GBuffer::kMaterialFormat},
      {}, /*casts_shadow=*/false, depth_format);
  REQUIRE(fx->gbuffer_factory != nullptr);
  // Forward factory built against the REAL accumulation format (not a
  // standalone BGRA8Unorm test target) -- this is the format
  // InstancedMeshField's forward-opaque draws actually target once
  // SceneRenderer drives this field (Task 3).
  fx->forward_factory = MakeInstancedFactory(
      g, "instanced_forward", MaterialPassType::kForwardOpaque,
      {SceneRenderer::kAccumulationFormat}, {"translucency"},
      /*casts_shadow=*/false, depth_format);
  REQUIRE(fx->forward_factory != nullptr);

  InstanceParams gbuffer_params;
  gbuffer_params.uniform_overrides["tint"] =
      MaterialParameterValue(glm::vec4(1.0f, 0.0f, 0.0f, 1.0f));  // red, unlit albedo
  gbuffer_params.uniform_overrides["bucketId"] = MaterialParameterValue(uint32_t(0));
  entt::id_type gbuffer_key = ComposeMaterialCacheKey(
      entt::hashed_string{"field_gbuffer"}.value(), GeometryType::kInstancedMesh,
      RenderPassType::kGBuffer, 0);
  fx->gbuffer_handle = fx->cache.GetOrCreate(
      gbuffer_key, *fx->gbuffer_factory, GeometryType::kInstancedMesh,
      MaterialPassType::kDeferred, RenderPassType::kGBuffer, gbuffer_params);
  REQUIRE(fx->gbuffer_handle);
  REQUIRE(fx->gbuffer_handle->IsValid());
  REQUIRE_FALSE(fx->gbuffer_handle->DeclaresBindGroup(2));

  InstanceParams forward_params;
  forward_params.uniform_overrides["tint"] =
      MaterialParameterValue(glm::vec4(1.0f, 1.0f, 1.0f, 1.0f));
  forward_params.uniform_overrides["params"] =
      MaterialParameterValue(glm::vec4(0.0f, 1.0f, 0.0f, 0.0f));  // cutoff/rough
  forward_params.uniform_overrides["transmission"] =
      MaterialParameterValue(glm::vec4(0.0f, 0.0f, 0.0f, 0.0f));
  forward_params.uniform_overrides["bucketId"] = MaterialParameterValue(uint32_t(0));
  entt::id_type forward_key = ComposeMaterialCacheKey(
      entt::hashed_string{"field_forward"}.value(), GeometryType::kInstancedMesh,
      RenderPassType::kForward, 0);
  fx->forward_handle = fx->cache.GetOrCreate(
      forward_key, *fx->forward_factory, GeometryType::kInstancedMesh,
      MaterialPassType::kForwardOpaque, RenderPassType::kForward, forward_params);
  REQUIRE(fx->forward_handle);
  REQUIRE(fx->forward_handle->IsValid());
  REQUIRE(fx->forward_handle->DeclaresBindGroup(2));

  std::array<uint32_t, 6> idx = {0, 1, 2, 0, 2, 3};
  fx->ibuf = UploadBuffer(g.device, idx.data(), sizeof(idx), wgpu::BufferUsage::Index);
  std::vector<float> deferred_verts =
      QuadVerticesOffset(kFieldSubmeshHalfExtent, {-kFieldSubmeshOffset, 0.0f});
  std::vector<float> forward_verts =
      QuadVerticesOffset(kFieldSubmeshHalfExtent, {kFieldSubmeshOffset, 0.0f});
  fx->vbuf_deferred = UploadBuffer(g.device, deferred_verts.data(),
                                   deferred_verts.size() * sizeof(float),
                                   wgpu::BufferUsage::Vertex);
  fx->vbuf_forward = UploadBuffer(g.device, forward_verts.data(),
                                  forward_verts.size() * sizeof(float),
                                  wgpu::BufferUsage::Vertex);

  // thresholds = {1e30, 1e30}: the one instance below always lands lod0 ->
  // bucket 0 (1 model). 2 submeshes on that bucket: 0 = deferred, 1 = forward.
  fx->field = std::make_unique<InstancedMeshField>(
      g.device, g.queue, *g.gen, /*capacity=*/1, /*num_models=*/1,
      /*num_submeshes=*/2, Lod3Chains(1, 1e30f, 2e30f));
  REQUIRE(fx->field->IsValid());

  GpuInstanceRenderer::InstanceInput instance = MakeInstance(
      glm::vec3(kFieldInstanceCenter, -5.0f), /*model=*/0u, /*radius=*/0.5f);
  fx->field->UploadInstances(
      std::span<const GpuInstanceRenderer::InstanceInput>(&instance, 1));

  fx->field->SetSubmesh(/*model=*/0, /*lod=*/0, /*submesh=*/0, fx->vbuf_deferred,
                        fx->ibuf, wgpu::IndexFormat::Uint32, 6,
                        InstancedMeshField::PassKind::kDeferred,
                        fx->gbuffer_handle.operator->());
  fx->field->SetSubmesh(/*model=*/0, /*lod=*/0, /*submesh=*/1, fx->vbuf_forward,
                        fx->ibuf, wgpu::IndexFormat::Uint32, 6,
                        InstancedMeshField::PassKind::kForwardOpaque,
                        fx->forward_handle.operator->());

  REQUIRE(fx->field->HasPass(InstancedMeshField::PassKind::kDeferred));
  REQUIRE(fx->field->HasPass(InstancedMeshField::PassKind::kForwardOpaque));

  return fx;
}

// Trivial group-2 engine resources (shadow map + IBL), identical setup to the
// "instanced forward material renders N instances" test above.
ForwardEngineResources MakeDummyForwardEngineResources(TestGpu& g) {
  ForwardEngineResources engine{};

  wgpu::TextureDescriptor sd{};
  sd.size = {1, 1, 1};
  sd.format = wgpu::TextureFormat::Depth32Float;
  sd.usage = wgpu::TextureUsage::TextureBinding;
  wgpu::Texture shadow_tex = g.device.CreateTexture(&sd);
  wgpu::TextureViewDescriptor shadow_vd{};
  shadow_vd.aspect = wgpu::TextureAspect::DepthOnly;
  engine.shadow_map = shadow_tex.CreateView(&shadow_vd);

  wgpu::SamplerDescriptor cmp{};
  cmp.compare = wgpu::CompareFunction::LessEqual;
  engine.shadow_sampler = g.device.CreateSampler(&cmp);

  engine.ibl_prefiltered = SolidCube1x1(g.device, g.queue, 255, 255, 255, 255);
  engine.ibl_sampler = g.device.CreateSampler(nullptr);
  engine.brdf_lut =
      test::CreateRgbaTexture(g.device, g.queue, 1, 1, {255, 255, 0, 255})
          .CreateView();
  engine.brdf_lut_sampler = g.device.CreateSampler(nullptr);
  return engine;
}

// IEEE-754 binary16 -> float32. TextureReadback/CpuImage don't support
// RGBA16Float (SceneRenderer::kAccumulationFormat) -- see
// resolve_composite_tests.cpp's file comment -- so reading the forward
// target below hand-rolls the decode, mirroring
// game/tests/water_gpu_tests.cpp's HalfToFloat.
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

// Reads one RGBA16Float pixel out of `texture` (must be CopySrc) via a manual
// CopyTextureToBuffer + MapRead (ColorRenderTarget's color texture already
// carries CopySrc -- see color_render_target.cpp).
glm::vec4 ReadRgba16FloatPixel(TestGpu& g, wgpu::Texture texture, uint32_t width,
                               uint32_t height, uint32_t x, uint32_t y) {
  constexpr uint32_t kBytesPerPixel = 8;  // RGBA16Float
  const uint32_t bytes_per_row =
      ((width * kBytesPerPixel + 255u) / 256u) * 256u;
  const uint64_t buffer_size = uint64_t{bytes_per_row} * height;

  wgpu::BufferDescriptor bd{};
  bd.size = buffer_size;
  bd.usage = wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst;
  wgpu::Buffer readback = g.device.CreateBuffer(&bd);

  {
    wgpu::CommandEncoder enc = g.device.CreateCommandEncoder();
    wgpu::TexelCopyTextureInfo src{};
    src.texture = texture;
    wgpu::TexelCopyBufferInfo dst{};
    dst.buffer = readback;
    dst.layout.bytesPerRow = bytes_per_row;
    dst.layout.rowsPerImage = height;
    wgpu::Extent3D extent = {width, height, 1};
    enc.CopyTextureToBuffer(&src, &dst, &extent);
    wgpu::CommandBuffer cmd = enc.Finish();
    g.queue.Submit(1, &cmd);
  }
  test::WaitForGpu(g.instance, g.device, g.queue);

  bool mapped = false;
  bool ok = false;
  readback.MapAsync(
      wgpu::MapMode::Read, 0, buffer_size, wgpu::CallbackMode::AllowProcessEvents,
      [&](wgpu::MapAsyncStatus status, wgpu::StringView) {
        ok = (status == wgpu::MapAsyncStatus::Success);
        mapped = true;
      });
  while (!mapped) {
    g.instance.ProcessEvents();
    g.device.Tick();
  }
  REQUIRE(ok);

  const uint8_t* base =
      static_cast<const uint8_t*>(readback.GetConstMappedRange(0, buffer_size));
  REQUIRE(base != nullptr);
  const uint8_t* pixel =
      base + uint64_t{y} * bytes_per_row + uint64_t{x} * kBytesPerPixel;
  std::array<uint16_t, 4> half{};
  std::memcpy(half.data(), pixel, sizeof(half));
  glm::vec4 result(HalfToFloat(half[0]), HalfToFloat(half[1]),
                   HalfToFloat(half[2]), HalfToFloat(half[3]));
  readback.Unmap();
  return result;
}

}  // namespace

// ---------------------------------------------------------------------------
// Draw(kDeferred) must render only the deferred submesh: the deferred slot's
// color lands at its own screen position, and the forward slot's position
// (which would show its color if the pass filter leaked) stays clear.
// ---------------------------------------------------------------------------
TEST_CASE("InstancedMeshField: Draw(kDeferred) renders only the deferred submesh",
          "[gpu_instance][gpu]") {
  TestGpu& g = GetTestGpu();
  auto fx = BuildDeferredForwardField(g);

  UniformData frame_uniforms = MakeOrthoFrame();
  FrameContext frame;
  frame.Begin(g.device, g.queue, frame_uniforms);

  Camera cull_camera = MakeCullCamera(1.0f);
  fx->field->Cull(frame, cull_camera);

  ColorRenderTarget normals_t(g.device, kInstTarget, kInstTarget, GBuffer::kNormalsFormat);
  ColorRenderTarget albedo_t(g.device, kInstTarget, kInstTarget, GBuffer::kAlbedoFormat);
  ColorRenderTarget material_t(g.device, kInstTarget, kInstTarget, GBuffer::kMaterialFormat);
  REQUIRE(albedo_t.IsValid());

  {
    std::array<wgpu::RenderPassColorAttachment, 3> ca = {
        ClearAttachment(normals_t.GetView()), ClearAttachment(albedo_t.GetView()),
        ClearAttachment(material_t.GetView())};
    wgpu::RenderPassDescriptor desc{};
    desc.colorAttachmentCount = ca.size();
    desc.colorAttachments = ca.data();
    RenderPassContext pass = frame.BeginRenderPass(desc);
    fx->field->Draw(pass, frame, InstancedMeshField::PassKind::kDeferred);
    pass.End();
  }

  wgpu::CommandBuffer cmd = frame.End();
  g.queue.Submit(1, &cmd);
  test::WaitForGpu(g.instance, g.device, g.queue);

  TextureReadback rb(g.instance, g.device, g.queue);
  CpuImage albedo = rb.ReadTextureSync(albedo_t.GetTexture(), kInstTarget,
                                       kInstTarget, GBuffer::kAlbedoFormat);

  auto deferred_px = WorldXyToPixel(
      {kFieldInstanceCenter.x - kFieldSubmeshOffset, kFieldInstanceCenter.y});
  auto forward_px = WorldXyToPixel(
      {kFieldInstanceCenter.x + kFieldSubmeshOffset, kFieldInstanceCenter.y});

  CpuImage::Color deferred_c = albedo.GetPixel(deferred_px.first, deferred_px.second);
  INFO("deferred-slot pixel rgb = " << (int)deferred_c.r << "," << (int)deferred_c.g
                                    << "," << (int)deferred_c.b);
  CHECK(deferred_c.r > 180);
  CHECK(deferred_c.g < 60);
  CHECK(deferred_c.b < 60);

  // The forward submesh must NOT have drawn into this pass.
  CpuImage::Color forward_c = albedo.GetPixel(forward_px.first, forward_px.second);
  INFO("forward-slot pixel (should stay clear) rgb = " << (int)forward_c.r << ","
                                                        << (int)forward_c.g << ","
                                                        << (int)forward_c.b);
  CHECK(forward_c.r < 40);
  CHECK(forward_c.g < 40);
  CHECK(forward_c.b < 40);
}

// ---------------------------------------------------------------------------
// Draw(kForwardOpaque, &engine) must render only the forward submesh: the
// forward slot's color lands at its own position (into a real
// kAccumulationFormat target, with the group-2 engine resources actually
// bound), and the deferred slot's position stays clear.
// ---------------------------------------------------------------------------
TEST_CASE("InstancedMeshField: Draw(kForwardOpaque, engine) renders only the "
          "forward submesh",
          "[gpu_instance][gpu]") {
  TestGpu& g = GetTestGpu();
  auto fx = BuildDeferredForwardField(g);
  ForwardEngineResources engine = MakeDummyForwardEngineResources(g);

  UniformData frame_uniforms = MakeOrthoFrame();
  FrameContext frame;
  frame.Begin(g.device, g.queue, frame_uniforms);

  Camera cull_camera = MakeCullCamera(1.0f);
  fx->field->Cull(frame, cull_camera);

  ColorRenderTarget accum_t(g.device, kInstTarget, kInstTarget,
                            SceneRenderer::kAccumulationFormat);
  REQUIRE(accum_t.IsValid());

  {
    wgpu::RenderPassColorAttachment ca = ClearAttachment(accum_t.GetView());
    wgpu::RenderPassDescriptor desc{};
    desc.colorAttachmentCount = 1;
    desc.colorAttachments = &ca;
    RenderPassContext pass = frame.BeginRenderPass(desc);
    fx->field->Draw(pass, frame, InstancedMeshField::PassKind::kForwardOpaque,
                    &engine);
    pass.End();
  }

  wgpu::CommandBuffer cmd = frame.End();
  g.queue.Submit(1, &cmd);
  test::WaitForGpu(g.instance, g.device, g.queue);

  auto deferred_px = WorldXyToPixel(
      {kFieldInstanceCenter.x - kFieldSubmeshOffset, kFieldInstanceCenter.y});
  auto forward_px = WorldXyToPixel(
      {kFieldInstanceCenter.x + kFieldSubmeshOffset, kFieldInstanceCenter.y});

  glm::vec4 forward_c = ReadRgba16FloatPixel(g, accum_t.GetTexture(), kInstTarget,
                                             kInstTarget, forward_px.first,
                                             forward_px.second);
  INFO("forward-slot pixel rgb = " << forward_c.r << "," << forward_c.g << ","
                                   << forward_c.b);
  CHECK(forward_c.r > 1.0f);
  CHECK(forward_c.g > 1.0f);
  CHECK(forward_c.b > 1.0f);

  // The deferred submesh must NOT have drawn into this pass.
  glm::vec4 deferred_c = ReadRgba16FloatPixel(g, accum_t.GetTexture(), kInstTarget,
                                              kInstTarget, deferred_px.first,
                                              deferred_px.second);
  INFO("deferred-slot pixel (should stay clear) rgb = "
       << deferred_c.r << "," << deferred_c.g << "," << deferred_c.b);
  CHECK(deferred_c.r < 0.05f);
  CHECK(deferred_c.g < 0.05f);
  CHECK(deferred_c.b < 0.05f);
}

// ---------------------------------------------------------------------------
// Draw(kForwardOpaque, nullptr): the group-2 gate must SKIP the forward
// submesh (engine unavailable) rather than draw with group 2 left unbound --
// NoError under a validation scope, and nothing drawn.
// ---------------------------------------------------------------------------
TEST_CASE("InstancedMeshField: Draw(kForwardOpaque, nullptr) skips the "
          "group-2 submesh without a validation error",
          "[gpu_instance][gpu]") {
  TestGpu& g = GetTestGpu();
  auto fx = BuildDeferredForwardField(g);

  UniformData frame_uniforms = MakeOrthoFrame();
  ColorRenderTarget accum_t(g.device, kInstTarget, kInstTarget,
                            SceneRenderer::kAccumulationFormat);
  REQUIRE(accum_t.IsValid());

  g.device.PushErrorScope(wgpu::ErrorFilter::Validation);

  FrameContext frame;
  frame.Begin(g.device, g.queue, frame_uniforms);
  Camera cull_camera = MakeCullCamera(1.0f);
  fx->field->Cull(frame, cull_camera);
  {
    wgpu::RenderPassColorAttachment ca = ClearAttachment(accum_t.GetView());
    wgpu::RenderPassDescriptor desc{};
    desc.colorAttachmentCount = 1;
    desc.colorAttachments = &ca;
    RenderPassContext pass = frame.BeginRenderPass(desc);
    fx->field->Draw(pass, frame, InstancedMeshField::PassKind::kForwardOpaque,
                    /*engine=*/nullptr);
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

  test::WaitForGpu(g.instance, g.device, g.queue);
  auto forward_px = WorldXyToPixel(
      {kFieldInstanceCenter.x + kFieldSubmeshOffset, kFieldInstanceCenter.y});
  glm::vec4 forward_c = ReadRgba16FloatPixel(g, accum_t.GetTexture(), kInstTarget,
                                             kInstTarget, forward_px.first,
                                             forward_px.second);
  INFO("forward-slot pixel (should stay clear -- gated/skipped) rgb = "
       << forward_c.r << "," << forward_c.g << "," << forward_c.b);
  CHECK(forward_c.r < 0.05f);
  CHECK(forward_c.g < 0.05f);
  CHECK(forward_c.b < 0.05f);
}

// ---------------------------------------------------------------------------
// F4A contract test: the shared @group(2) bind group InstancedMeshField::Draw
// builds (lazily, from the first group-2-declaring slot drawn this call) is
// reused for every other group-2 slot in the SAME call -- including slots
// resolved through a DIFFERENT MaterialInstanceFactory / pipeline than the
// first slot's, as long as that pipeline declares the same 6-entry engine
// group-2 layout (see the CONTRACT paragraph on Draw()'s declaration in
// instanced_mesh_field.hpp). Two `instanced_forward` factories built with
// different extra_features ({"translucency"} vs none) compile to genuinely
// DIFFERENT pipelines (a different WGSL variant -- see
// instanced_forward.wesl's @if(translucency) transmissionContribution
// overload), yet both declare the identical group-2 layout: the
// `translucency` feature only swaps that fragment-stage helper's body, never
// touching group 2. One field, one bucket, two forward-opaque submeshes --
// submesh 0 resolved through the translucency pipeline, submesh 1 through the
// plain pipeline -- each with a distinct tint and mesh-local offset (the
// shared-slice test's geometry pattern, QuadVerticesOffset). Cull + Draw
// under a Dawn validation-error scope: NoError (proves the shared bind group
// binds validly against BOTH pipelines, not just the one it was built from)
// AND both submeshes' colors land at their expected pixels (proves both
// actually drew, not merely that nothing errored).
// ---------------------------------------------------------------------------
TEST_CASE("InstancedMeshField: shared group-2 bind group is valid across two "
          "distinct pipelines with the same layout",
          "[gpu_instance][gpu]") {
  TestGpu& g = GetTestGpu();

  auto factory_translucent = MakeInstancedFactory(
      g, "instanced_forward", MaterialPassType::kForwardOpaque,
      {wgpu::TextureFormat::BGRA8Unorm}, {"translucency"});
  REQUIRE(factory_translucent != nullptr);
  auto factory_plain = MakeInstancedFactory(g, "instanced_forward",
                                            MaterialPassType::kForwardOpaque,
                                            {wgpu::TextureFormat::BGRA8Unorm}, {});
  REQUIRE(factory_plain != nullptr);

  MaterialInstanceCache cache;
  auto make_mat = [&](const char* name, MaterialInstanceFactory& factory,
                      glm::vec4 tint) {
    InstanceParams params;
    params.uniform_overrides["tint"] = MaterialParameterValue(tint);
    params.uniform_overrides["params"] =
        MaterialParameterValue(glm::vec4(0.0f, 1.0f, 0.0f, 0.0f));
    params.uniform_overrides["transmission"] =
        MaterialParameterValue(glm::vec4(0.0f, 0.0f, 0.0f, 0.0f));
    params.uniform_overrides["bucketId"] = MaterialParameterValue(uint32_t(0));
    entt::id_type key = ComposeMaterialCacheKey(
        entt::hashed_string{name}.value(), GeometryType::kInstancedMesh,
        RenderPassType::kForward, 0);
    auto handle = cache.GetOrCreate(key, factory, GeometryType::kInstancedMesh,
                                    MaterialPassType::kForwardOpaque,
                                    RenderPassType::kForward, params);
    REQUIRE(handle);
    REQUIRE(handle->IsValid());
    REQUIRE(handle->DeclaresBindGroup(2));
    return handle;
  };
  auto mat_translucent = make_mat("g2_contract_translucent", *factory_translucent,
                                  glm::vec4(1.0f, 0.0f, 0.0f, 1.0f));  // red
  auto mat_plain = make_mat("g2_contract_plain", *factory_plain,
                            glm::vec4(0.0f, 0.0f, 1.0f, 1.0f));  // blue
  RenderingMaterialInstance* mat_translucent_ptr = mat_translucent.operator->();
  RenderingMaterialInstance* mat_plain_ptr = mat_plain.operator->();
  // Sanity: these really are two distinct pipeline objects, not the cache
  // coincidentally handing back the same one -- otherwise this test would
  // not exercise the "different pipelines" half of the contract.
  REQUIRE(mat_translucent_ptr->GetPipeline().Get() !=
         mat_plain_ptr->GetPipeline().Get());

  constexpr float kOffset = 0.35f;
  constexpr float kHalfExtent = 0.15f;
  std::array<uint32_t, 6> idx = {0, 1, 2, 0, 2, 3};
  wgpu::Buffer ibuf =
      UploadBuffer(g.device, idx.data(), sizeof(idx), wgpu::BufferUsage::Index);
  std::vector<float> verts_translucent =
      QuadVerticesOffset(kHalfExtent, {-kOffset, 0.0f});
  std::vector<float> verts_plain = QuadVerticesOffset(kHalfExtent, {kOffset, 0.0f});
  wgpu::Buffer vbuf_translucent =
      UploadBuffer(g.device, verts_translucent.data(),
                  verts_translucent.size() * sizeof(float),
                  wgpu::BufferUsage::Vertex);
  wgpu::Buffer vbuf_plain =
      UploadBuffer(g.device, verts_plain.data(),
                  verts_plain.size() * sizeof(float), wgpu::BufferUsage::Vertex);

  // thresholds pushed to +inf: the one instance always lands lod0 -> bucket 0.
  InstancedMeshField field(g.device, g.queue, *g.gen, /*capacity=*/1,
                           /*num_models=*/1, /*num_submeshes=*/2,
                           Lod3Chains(1, 1e30f, 2e30f));
  REQUIRE(field.IsValid());

  GpuInstanceRenderer::InstanceInput instance =
      MakeInstance(glm::vec3(0.0f, 0.0f, -5.0f), /*model=*/0u, /*radius=*/0.5f);
  field.UploadInstances(
      std::span<const GpuInstanceRenderer::InstanceInput>(&instance, 1));

  field.SetSubmesh(/*model=*/0, /*lod=*/0, /*submesh=*/0, vbuf_translucent, ibuf,
                   wgpu::IndexFormat::Uint32, 6,
                   InstancedMeshField::PassKind::kForwardOpaque,
                   mat_translucent_ptr);
  field.SetSubmesh(/*model=*/0, /*lod=*/0, /*submesh=*/1, vbuf_plain, ibuf,
                   wgpu::IndexFormat::Uint32, 6,
                   InstancedMeshField::PassKind::kForwardOpaque, mat_plain_ptr);
  REQUIRE(field.HasPass(InstancedMeshField::PassKind::kForwardOpaque));

  ForwardEngineResources engine = MakeDummyForwardEngineResources(g);
  // MakeDummyForwardEngineResources' brdf_lut is (255,255,0,255) -- deliberately
  // pushing the OTHER forward-material test's ambient-specular term
  // (prefilteredColor * (F0*brdfR+brdfG)) past 1.0 so it shades non-clear
  // regardless of view/sun/albedo. That achromatic (albedo-independent)
  // specular term would swamp both submeshes to white here, washing out this
  // test's tint distinction, so this test zeroes it (brdf_lut = (0,0,0,1))
  // -- same 6-entry group-2 layout, just a dimmer value -- leaving ambient
  // DIFFUSE (which IS scaled by each submesh's own albedo/tint) as the only
  // light source.
  engine.brdf_lut =
      test::CreateRgbaTexture(g.device, g.queue, 1, 1, {0, 0, 0, 255}).CreateView();

  // MakeOrthoFrame's ambient is deliberately blinding (SH L0 = 30); boosted
  // further here (still moderate, not saturating) so each submesh's
  // albedo-scaled ambient diffuse term reads back clearly per-channel.
  UniformData frame_uniforms = MakeOrthoFrame();
  frame_uniforms.ambient_sh[0] = glm::vec4(3.0f, 3.0f, 3.0f, 0.0f);
  ColorRenderTarget target(g.device, kInstTarget, kInstTarget,
                           wgpu::TextureFormat::BGRA8Unorm);
  REQUIRE(target.IsValid());

  g.device.PushErrorScope(wgpu::ErrorFilter::Validation);

  FrameContext frame;
  frame.Begin(g.device, g.queue, frame_uniforms);
  Camera cull_camera = MakeCullCamera(1.0f);
  field.Cull(frame, cull_camera);
  {
    wgpu::RenderPassColorAttachment ca = ClearAttachment(target.GetView());
    wgpu::RenderPassDescriptor desc{};
    desc.colorAttachmentCount = 1;
    desc.colorAttachments = &ca;
    RenderPassContext pass = frame.BeginRenderPass(desc);
    field.Draw(pass, frame, InstancedMeshField::PassKind::kForwardOpaque, &engine);
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

  test::WaitForGpu(g.instance, g.device, g.queue);
  TextureReadback rb(g.instance, g.device, g.queue);
  CpuImage img = rb.ReadTextureSync(target.GetTexture(), kInstTarget, kInstTarget,
                                    wgpu::TextureFormat::BGRA8Unorm);

  auto px_translucent = WorldXyToPixel({-kOffset, 0.0f});
  auto px_plain = WorldXyToPixel({kOffset, 0.0f});
  CpuImage::Color c_translucent =
      img.GetPixel(px_translucent.first, px_translucent.second);
  CpuImage::Color c_plain = img.GetPixel(px_plain.first, px_plain.second);
  INFO("submesh0 (translucency pipeline, red) @ (" << px_translucent.first << ","
                    << px_translucent.second << ") rgb=" << (int)c_translucent.r
                    << "," << (int)c_translucent.g << "," << (int)c_translucent.b);
  CHECK(c_translucent.r > 180);
  CHECK(c_translucent.g < 60);
  CHECK(c_translucent.b < 60);
  INFO("submesh1 (plain pipeline, blue) @ (" << px_plain.first << ","
                    << px_plain.second << ") rgb=" << (int)c_plain.r << ","
                    << (int)c_plain.g << "," << (int)c_plain.b);
  CHECK(c_plain.r < 60);
  CHECK(c_plain.g < 60);
  CHECK(c_plain.b > 180);
}

// ---------------------------------------------------------------------------
// F9 guard test: SetSubmesh must reject an out-of-range (bucket, submesh)
// cleanly -- no crash, HasPass reflecting only the slots actually configured
// (a rejected call must not fabricate a phantom pass), and a subsequent
// Cull+Draw under a Dawn validation-error scope is NoError with nothing
// extra drawn (only the one valid slot's color appears).
// ---------------------------------------------------------------------------
TEST_CASE("InstancedMeshField::SetSubmesh rejects out-of-range lod/submesh cleanly",
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
      MaterialParameterValue(glm::vec4(1.0f, 0.0f, 0.0f, 1.0f));
  params.uniform_overrides["bucketId"] = MaterialParameterValue(uint32_t(0));
  entt::id_type key = ComposeMaterialCacheKey(
      entt::hashed_string{"setsubmesh_guard_valid"}.value(),
      GeometryType::kInstancedMesh, RenderPassType::kGBuffer, 0);
  auto handle = cache.GetOrCreate(key, *factory, GeometryType::kInstancedMesh,
                                  MaterialPassType::kDeferred,
                                  RenderPassType::kGBuffer, params);
  REQUIRE(handle);
  REQUIRE(handle->IsValid());

  std::array<uint32_t, 6> idx = {0, 1, 2, 0, 2, 3};
  wgpu::Buffer ibuf =
      UploadBuffer(g.device, idx.data(), sizeof(idx), wgpu::BufferUsage::Index);
  std::vector<float> verts = QuadVerticesSized(0.15f);
  wgpu::Buffer vbuf = UploadBuffer(g.device, verts.data(),
                                   verts.size() * sizeof(float),
                                   wgpu::BufferUsage::Vertex);

  // num_models=1 -> num_buckets = kMaxLods (3); num_submeshes=1 -> submesh 0
  // is the field's only valid submesh slot.
  InstancedMeshField field(g.device, g.queue, *g.gen, /*capacity=*/1,
                           /*num_models=*/1, /*num_submeshes=*/1,
                           Lod3Chains(1, 1e30f, 2e30f));
  REQUIRE(field.IsValid());

  GpuInstanceRenderer::InstanceInput instance =
      MakeInstance(glm::vec3(0.0f, 0.0f, -5.0f), /*model=*/0u, /*radius=*/0.5f);
  field.UploadInstances(
      std::span<const GpuInstanceRenderer::InstanceInput>(&instance, 1));

  // The one valid slot.
  field.SetSubmesh(/*model=*/0, /*lod=*/0, /*submesh=*/0, vbuf, ibuf,
                   wgpu::IndexFormat::Uint32, 6,
                   InstancedMeshField::PassKind::kDeferred, handle.operator->());
  REQUIRE(field.HasPass(InstancedMeshField::PassKind::kDeferred));
  REQUIRE_FALSE(field.HasPass(InstancedMeshField::PassKind::kForwardOpaque));

  // Out-of-range lod: BucketId(0, kMaxLods) == num_buckets (3) -- one past
  // the end. Tagged kForwardOpaque (a DIFFERENT pass than the valid slot's)
  // so a leaked write would flip HasPass(kForwardOpaque) below.
  field.SetSubmesh(/*model=*/0, /*lod=*/GpuInstanceRenderer::kMaxLods,
                   /*submesh=*/0, vbuf, ibuf, wgpu::IndexFormat::Uint32, 6,
                   InstancedMeshField::PassKind::kForwardOpaque,
                   handle.operator->());
  // Out-of-range submesh (num_submeshes == 1, so submesh 1 is out of range).
  field.SetSubmesh(/*model=*/0, /*lod=*/0, /*submesh=*/1, vbuf, ibuf,
                   wgpu::IndexFormat::Uint32, 6,
                   InstancedMeshField::PassKind::kForwardOpaque,
                   handle.operator->());

  // Neither rejected call disturbed the valid slot or fabricated a new pass.
  CHECK(field.HasPass(InstancedMeshField::PassKind::kDeferred));
  CHECK_FALSE(field.HasPass(InstancedMeshField::PassKind::kForwardOpaque));

  UniformData frame_uniforms = MakeOrthoFrame();
  ColorRenderTarget normals_t(g.device, kInstTarget, kInstTarget,
                              GBuffer::kNormalsFormat);
  ColorRenderTarget albedo_t(g.device, kInstTarget, kInstTarget,
                             GBuffer::kAlbedoFormat);
  ColorRenderTarget material_t(g.device, kInstTarget, kInstTarget,
                               GBuffer::kMaterialFormat);
  REQUIRE(albedo_t.IsValid());

  g.device.PushErrorScope(wgpu::ErrorFilter::Validation);

  FrameContext frame;
  frame.Begin(g.device, g.queue, frame_uniforms);
  Camera cull_camera = MakeCullCamera(1.0f);
  field.Cull(frame, cull_camera);
  {
    std::array<wgpu::RenderPassColorAttachment, 3> ca = {
        ClearAttachment(normals_t.GetView()), ClearAttachment(albedo_t.GetView()),
        ClearAttachment(material_t.GetView())};
    wgpu::RenderPassDescriptor desc{};
    desc.colorAttachmentCount = ca.size();
    desc.colorAttachments = ca.data();
    RenderPassContext pass = frame.BeginRenderPass(desc);
    field.Draw(pass, frame, InstancedMeshField::PassKind::kDeferred);
    // Nothing is configured for kForwardOpaque (both attempts above were
    // rejected) -- Draw() must be a safe no-op here, never crash.
    field.Draw(pass, frame, InstancedMeshField::PassKind::kForwardOpaque);
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

  test::WaitForGpu(g.instance, g.device, g.queue);
  TextureReadback rb(g.instance, g.device, g.queue);
  CpuImage albedo = rb.ReadTextureSync(albedo_t.GetTexture(), kInstTarget,
                                       kInstTarget, GBuffer::kAlbedoFormat);

  auto center_px = WorldXyToPixel({0.0f, 0.0f});
  CpuImage::Color c = albedo.GetPixel(center_px.first, center_px.second);
  INFO("valid slot pixel rgb = " << (int)c.r << "," << (int)c.g << "," << (int)c.b);
  CHECK(c.r > 180);
  CHECK(c.g < 60);
  CHECK(c.b < 60);
}

// ===========================================================================
// Phase 4 of the volumetric-foliage feature: InstancedMeshField's shadow
// slot. SetSubmeshShadow attaches a shadow-pass material to an ALREADY
// SetSubmesh-configured slot (geometry is shared, not re-specified);
// HasPass(kShadow) reflects only whether a shadow_material is attached
// (independent of the slot's main PassKind/material); Draw(kShadow) draws
// only the slots with one, routed through CullShadow()'s result set, into a
// depth-only Depth32Float pass (mirroring the real Pass 0 attachment).
// ---------------------------------------------------------------------------
namespace {

struct ShadowSlotFieldFixture {
  std::unique_ptr<MaterialInstanceFactory> factory;  // casts_shadow=true
  MaterialInstanceCache cache;
  wgpu::Buffer ibuf;
  wgpu::Buffer vbuf;
  std::unique_ptr<InstancedMeshField> field;
};

// One model/lod/submesh bucket: geometry configured via SetSubmesh with a
// NULL main-pass material (this fixture isolates the shadow path from the
// deferred/forward-opaque ones) and no shadow material attached yet --
// callers attach one via field->SetSubmeshShadow(...) to exercise the
// HasPass(kShadow) flip. The instance sits at world z=0.5 so, under
// MakeOrthoFrame's identity light_view_proj and zero camera offset, the
// shadow vertex path's worldCameraOffsetedSpaceToLightClipSpace(world) maps
// it straight to clip.xyz = (0,0,0.5) -- i.e. dead center of the depth
// target, at a depth comfortably inside the shadow pass's [0,1] (conventional
// Z, Less-compare) range.
std::unique_ptr<ShadowSlotFieldFixture> BuildShadowSlotField(TestGpu& g) {
  auto fx = std::make_unique<ShadowSlotFieldFixture>();

  fx->factory = MakeInstancedFactory(
      g, "instanced_gbuffer", MaterialPassType::kDeferred,
      {GBuffer::kNormalsFormat, GBuffer::kAlbedoFormat, GBuffer::kMaterialFormat},
      {}, /*casts_shadow=*/true, wgpu::TextureFormat::Depth32Float);
  REQUIRE(fx->factory != nullptr);

  std::array<uint32_t, 6> idx = {0, 1, 2, 0, 2, 3};
  fx->ibuf =
      UploadBuffer(g.device, idx.data(), sizeof(idx), wgpu::BufferUsage::Index);
  std::vector<float> verts = QuadVerticesSized(0.4f);
  fx->vbuf = UploadBuffer(g.device, verts.data(), verts.size() * sizeof(float),
                          wgpu::BufferUsage::Vertex);

  const std::vector<GpuInstanceRenderer::ModelLod> lods =
      Lod3Chains(1, 1e30f, 2e30f);
  fx->field = std::make_unique<InstancedMeshField>(
      g.device, g.queue, *g.gen, /*capacity=*/1, /*num_models=*/1,
      /*num_submeshes=*/1, lods);
  REQUIRE(fx->field->IsValid());

  GpuInstanceRenderer::InstanceInput instance =
      MakeInstance(glm::vec3(0.0f, 0.0f, 0.5f), /*model=*/0u, /*radius=*/0.5f);
  fx->field->UploadInstances(
      std::span<const GpuInstanceRenderer::InstanceInput>(&instance, 1));

  fx->field->SetSubmesh(/*model=*/0, /*lod=*/0, /*submesh=*/0, fx->vbuf, fx->ibuf,
                        wgpu::IndexFormat::Uint32, 6,
                        InstancedMeshField::PassKind::kDeferred,
                        /*material=*/nullptr);
  REQUIRE_FALSE(fx->field->HasPass(InstancedMeshField::PassKind::kShadow));

  return fx;
}

// Renders `field` into `depth_view` (Depth32Float, no color attachments,
// mirroring scene_renderer.cpp's Pass 0 descriptor) via Cull()+CullShadow()
// (light_view_proj matching MakeOrthoFrame's identity) then
// Draw(kShadow). Returns the Dawn validation-scope result.
CapturedError RenderShadowSlotPass(TestGpu& g, InstancedMeshField& field,
                                   wgpu::TextureView depth_view) {
  UniformData frame_uniforms = MakeOrthoFrame();
  FrameContext frame;
  frame.Begin(g.device, g.queue, frame_uniforms);
  Camera cull_camera = MakeCullCamera(1.0f);
  field.Cull(frame, cull_camera);
  field.CullShadow(frame, cull_camera, frame_uniforms.light_view_proj);

  CapturedError result;
  g.device.PushErrorScope(wgpu::ErrorFilter::Validation);
  {
    wgpu::RenderPassDepthStencilAttachment depth_attachment{};
    depth_attachment.view = depth_view;
    depth_attachment.depthClearValue = 1.0f;  // conventional-Z: far
    depth_attachment.depthLoadOp = wgpu::LoadOp::Clear;
    depth_attachment.depthStoreOp = wgpu::StoreOp::Store;
    depth_attachment.stencilLoadOp = wgpu::LoadOp::Undefined;
    depth_attachment.stencilStoreOp = wgpu::StoreOp::Undefined;
    wgpu::RenderPassDescriptor desc{};
    desc.colorAttachmentCount = 0;
    desc.depthStencilAttachment = &depth_attachment;
    RenderPassContext pass = frame.BeginRenderPass(desc);
    field.Draw(pass, frame, InstancedMeshField::PassKind::kShadow);
    pass.End();
  }
  wgpu::CommandBuffer cmd = frame.End();
  g.queue.Submit(1, &cmd);

  bool done = false;
  g.device.PopErrorScope(
      wgpu::CallbackMode::AllowProcessEvents,
      [&](wgpu::PopErrorScopeStatus, wgpu::ErrorType type, wgpu::StringView msg) {
        result.type = type;
        result.message =
            msg.length > 0 ? std::string(msg.data, msg.length) : std::string();
        done = true;
      });
  while (!done) {
    g.instance.ProcessEvents();
    g.device.Tick();
  }
  test::WaitForGpu(g.instance, g.device, g.queue);
  return result;
}

}  // namespace

TEST_CASE("InstancedMeshField: SetSubmeshShadow flips HasPass(kShadow), "
          "Draw(kShadow) draws only once attached",
          "[gpu_instance][gpu]") {
  TestGpu& g = GetTestGpu();
  auto fx = BuildShadowSlotField(g);

  constexpr uint32_t kShadowTarget = 32;
  wgpu::TextureDescriptor depth_desc{};
  depth_desc.size = {kShadowTarget, kShadowTarget, 1};
  depth_desc.format = wgpu::TextureFormat::Depth32Float;
  depth_desc.usage =
      wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::CopySrc;
  wgpu::Texture depth_tex = g.device.CreateTexture(&depth_desc);
  wgpu::TextureView depth_view = depth_tex.CreateView();

  // Before SetSubmeshShadow: no shadow material is attached, so Draw(kShadow)
  // must be a safe no-op -- NoError, and the depth target stays at its clear
  // value (nothing was rasterized).
  {
    CapturedError err = RenderShadowSlotPass(g, *fx->field, depth_view);
    INFO("Dawn validation error: " << err.message);
    CHECK(err.type == wgpu::ErrorType::NoError);
    TextureReadback rb(g.instance, g.device, g.queue);
    CpuImage img = rb.ReadTextureSync(depth_tex, kShadowTarget, kShadowTarget,
                                      wgpu::TextureFormat::Depth32Float);
    CHECK(img.GetDepth(kShadowTarget / 2, kShadowTarget / 2) == 1.0f);
  }

  // Resolve a real kShadow material instance (casts_shadow=true factory) and
  // attach it to the fixture's one slot.
  InstanceParams shadow_params;
  shadow_params.uniform_overrides["tint"] =
      MaterialParameterValue(glm::vec4(1.0f, 1.0f, 1.0f, 1.0f));
  shadow_params.uniform_overrides["bucketId"] = MaterialParameterValue(uint32_t(0));
  entt::id_type shadow_key = ComposeMaterialCacheKey(
      entt::hashed_string{"p4_shadow_slot"}.value(), GeometryType::kInstancedMesh,
      RenderPassType::kShadow, 0);
  auto shadow_handle = fx->cache.GetOrCreate(
      shadow_key, *fx->factory, GeometryType::kInstancedMesh,
      MaterialPassType::kDeferred, RenderPassType::kShadow, shadow_params);
  REQUIRE(shadow_handle);
  REQUIRE(shadow_handle->IsValid());

  fx->field->SetSubmeshShadow(/*model=*/0, /*lod=*/0, /*submesh=*/0,
                              shadow_handle.operator->());
  CHECK(fx->field->HasPass(InstancedMeshField::PassKind::kShadow));

  // After SetSubmeshShadow: Draw(kShadow) actually draws -- NoError, and the
  // depth target's center pixel (under the instance -- see BuildShadowSlotField's
  // comment) was written to something less than the 1.0 clear value.
  {
    CapturedError err = RenderShadowSlotPass(g, *fx->field, depth_view);
    INFO("Dawn validation error: " << err.message);
    CHECK(err.type == wgpu::ErrorType::NoError);
    TextureReadback rb(g.instance, g.device, g.queue);
    CpuImage img = rb.ReadTextureSync(depth_tex, kShadowTarget, kShadowTarget,
                                      wgpu::TextureFormat::Depth32Float);
    const float center_depth = img.GetDepth(kShadowTarget / 2, kShadowTarget / 2);
    INFO("center depth = " << center_depth);
    CHECK(center_depth < 1.0f);
  }
}

// ===========================================================================
// Review fix: SetSubmesh must overwrite the WHOLE SlotInfo (shadow_material
// reset to nullptr too), not just the main-pass fields -- a slot repurposed
// via a second SetSubmesh call (new mesh/material for that slot) otherwise
// keeps carrying whatever shadow_material an EARLIER, now-unrelated
// SetSubmeshShadow call attached, and would draw it into the shadow pass with
// (for a repurposed model/lod/submesh) mismatched geometry. SetSubmeshShadow
// still attaches AFTER SetSubmesh -- see instanced_mesh_field.hpp's updated
// order-contract comments on both methods.
// ===========================================================================
TEST_CASE("InstancedMeshField: SetSubmesh on an already-shadow-configured "
          "slot resets its stale shadow_material",
          "[gpu_instance][gpu]") {
  TestGpu& g = GetTestGpu();
  auto fx = BuildShadowSlotField(g);  // SetSubmesh(..., material=nullptr) already ran

  InstanceParams shadow_params;
  shadow_params.uniform_overrides["tint"] =
      MaterialParameterValue(glm::vec4(1.0f, 1.0f, 1.0f, 1.0f));
  shadow_params.uniform_overrides["bucketId"] = MaterialParameterValue(uint32_t(0));
  entt::id_type shadow_key = ComposeMaterialCacheKey(
      entt::hashed_string{"p_setsubmesh_reset_shadow"}.value(),
      GeometryType::kInstancedMesh, RenderPassType::kShadow, 0);
  auto shadow_handle = fx->cache.GetOrCreate(
      shadow_key, *fx->factory, GeometryType::kInstancedMesh,
      MaterialPassType::kDeferred, RenderPassType::kShadow, shadow_params);
  REQUIRE(shadow_handle);
  REQUIRE(shadow_handle->IsValid());

  fx->field->SetSubmeshShadow(/*model=*/0, /*lod=*/0, /*submesh=*/0,
                              shadow_handle.operator->());
  CHECK(fx->field->HasPass(InstancedMeshField::PassKind::kShadow));

  // Repurpose the SAME slot via SetSubmesh (its normal use -- e.g. swapping
  // the slot's mesh/main-pass material) WITHOUT re-attaching a shadow
  // material. The full-reset contract says this must clear the slot's stale
  // shadow_material along with everything else -- currently (pre-fix) it
  // survives, so HasPass(kShadow) wrongly stays true.
  fx->field->SetSubmesh(/*model=*/0, /*lod=*/0, /*submesh=*/0, fx->vbuf, fx->ibuf,
                        wgpu::IndexFormat::Uint32, 6,
                        InstancedMeshField::PassKind::kDeferred,
                        /*material=*/nullptr);
  CHECK_FALSE(fx->field->HasPass(InstancedMeshField::PassKind::kShadow));
}

// ===========================================================================
// Task 3: SceneRenderer drives SceneContext::instanced_fields. Task 3 wired
// SceneRenderer::Render to: Cull() every field BEFORE Pass 0 (frame.Begin,
// before any render pass opens on the encoder); Draw(kDeferred) each field
// inside Pass 1 (G-buffer), right after RenderTexturedMeshes; and extended
// Pass 3.7's gate to open on `registry.view<ForwardOpaqueRenderable>().size()
// > 0 || <any field HasPass(kForwardOpaque)>`, Draw(kForwardOpaque, &engine)
// each field right after RenderForwardMeshes.
//
// Renders a real SceneRenderer frame with an EMPTY registry (so
// ForwardOpaqueRenderable count is exactly ZERO -- the scenario the gate
// extension exists for) and one field (this file's Deferred+Forward fixture,
// with depth wired to GBuffer::kDepthFormat -- see BuildDeferredForwardField's
// comment: deferred_lighting.wesl discards any pixel whose depth still reads
// the far-plane clear value, so the deferred submesh must actually write real
// depth for the fully-lit result to show it). Framebuffer readback proves:
//   * Cull() ran pre-pass (an un-culled field draws nothing),
//   * the deferred submesh's G-buffer write reached deferred lighting (a
//     visible, LIT pixel, not just a raw unlit G-buffer value),
//   * the forward-opaque gate opened and drew with zero
//     ForwardOpaqueRenderable entities in the registry.
// A second render with instanced_field_count = 0 confirms the no-field path
// is unaffected (no instances visible; same as the baseline).
// ===========================================================================
namespace {

constexpr uint32_t kSceneWidth = 256;
constexpr uint32_t kSceneHeight = 256;

// Real perspective camera at the origin looking down -Z -- the SAME
// convention as MakeCullCamera above, so the frustum InstancedMeshField::Cull
// classifies against is exactly the one SceneRenderer::Render's deferred/
// forward passes then render with (a single Camera drives both).
Camera MakeSceneCamera() {
  Camera camera;
  camera.position = glm::vec3(0.0f);
  camera.direction = glm::vec3(0.0f, 0.0f, -1.0f);
  camera.up = glm::vec3(0.0f, 1.0f, 0.0f);
  camera.fov = 60.0f;
  camera.aspect =
      static_cast<float>(kSceneWidth) / static_cast<float>(kSceneHeight);
  camera.near_plane = 0.1f;
  camera.far_plane = 1000.0f;
  return camera;
}

// World -> pixel through the engine's own matrices -- mirrors
// decal_pass_tests.cpp's ProjectToPixel, so nothing about the projection is
// re-derived here.
glm::ivec2 SceneProjectToPixel(const Camera& camera, glm::vec3 world) {
  const glm::vec4 clip =
      camera.GetProj() * camera.GetView() * glm::vec4(world, 1.0f);
  REQUIRE(clip.w > 0.0f);  // in front of the camera
  const glm::vec3 ndc = glm::vec3(clip) / clip.w;
  return glm::ivec2(
      static_cast<int>((ndc.x * 0.5f + 0.5f) * static_cast<float>(kSceneWidth)),
      static_cast<int>((1.0f - (ndc.y * 0.5f + 0.5f)) *
                       static_cast<float>(kSceneHeight)));
}

// Red channel at the pixel a world point projects to (R32Float target --
// GetDepth, not GetPixelF32, is the raw-float accessor; see
// decal_pass_tests.cpp's RedAt for why).
float SceneRedAt(const CpuImage& image, const Camera& camera, glm::vec3 world) {
  const glm::ivec2 p = SceneProjectToPixel(camera, world);
  REQUIRE(p.x >= 0);
  REQUIRE(p.y >= 0);
  REQUIRE(p.x < static_cast<int>(kSceneWidth));
  REQUIRE(p.y < static_cast<int>(kSceneHeight));
  return image.GetDepth(static_cast<uint32_t>(p.x), static_cast<uint32_t>(p.y));
}

// Renders one SceneRenderer frame with `fields` (may be empty) over an EMPTY
// registry, under a Dawn validation-error scope -- asserts NoError (an
// IsValid()/null check alone does not catch validation errors on this Dawn
// build; see RunCapturingValidationErrors' file comment above) -- and returns
// the readback.
CpuImage RenderSceneWithFields(TestGpu& g, const Camera& camera,
                               std::span<InstancedMeshField* const> fields) {
  entt::registry registry;  // deliberately empty: zero ForwardOpaqueRenderable
  SceneContext scene_context;
  scene_context.registry = &registry;
  scene_context.sun_direction = glm::normalize(glm::vec3(0.3f, 0.8f, 0.5f));
  scene_context.sun_color = glm::vec3(1.0f);
  scene_context.ambient_sh[0] = glm::vec3(1.5f);  // flat ambient (SH L0/DC)
  scene_context.clear_color = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
  scene_context.instanced_fields = fields.data();
  scene_context.instanced_field_count = static_cast<uint32_t>(fields.size());

  ColorRenderTarget rt(g.device, kSceneWidth, kSceneHeight,
                       wgpu::TextureFormat::R32Float);
  REQUIRE(rt.IsValid());

  SceneRenderer renderer;
  renderer.Initialize(g.device, g.queue, g.gen.get(),
                      wgpu::TextureFormat::R32Float, kSceneWidth, kSceneHeight,
                      g.device.HasFeature(wgpu::FeatureName::TextureFormatsTier1));
  renderer.MutableFogConfig().enabled = false;  // would haze the readback

  CapturedError err = RunCapturingValidationErrors(g.instance, g.device, [&] {
    renderer.Render(camera, registry, scene_context, rt.GetView());
  });
  INFO("Dawn validation error: " << err.message);
  CHECK(err.type == wgpu::ErrorType::NoError);
  test::WaitForGpu(g.instance, g.device, g.queue);

  TextureReadback readback(g.instance, g.device, g.queue);
  return readback.ReadTextureSync(rt.GetTexture(), kSceneWidth, kSceneHeight,
                                  wgpu::TextureFormat::R32Float);
}

}  // namespace

TEST_CASE("SceneRenderer draws SceneContext::instanced_fields (empty registry, "
          "zero ForwardOpaqueRenderable)",
          "[gpu_instance][gpu]") {
  TestGpu& g = GetTestGpu();
  auto fx = BuildDeferredForwardField(g, GBuffer::kDepthFormat);
  const Camera camera = MakeSceneCamera();

  const glm::vec3 deferred_world(kFieldInstanceCenter.x - kFieldSubmeshOffset,
                                 kFieldInstanceCenter.y, -5.0f);
  const glm::vec3 forward_world(kFieldInstanceCenter.x + kFieldSubmeshOffset,
                                kFieldInstanceCenter.y, -5.0f);
  // Well clear of either submesh's quad but still comfortably inside the
  // frustum (fov=60/aspect=1 at z=-5 gives a +/-2.887 half-extent) -- must
  // stay background in every render below; the discriminator for "was
  // anything drawn here".
  const glm::vec3 background_world(2.0f, 2.0f, -5.0f);

  std::array<InstancedMeshField*, 1> fields = {fx->field.get()};

  SECTION("with the field: both submeshes are visible") {
    const CpuImage image = RenderSceneWithFields(g, camera, std::span(fields));

    const float deferred_red = SceneRedAt(image, camera, deferred_world);
    const float forward_red = SceneRedAt(image, camera, forward_world);
    const float background_red = SceneRedAt(image, camera, background_world);
    INFO("deferred = " << deferred_red << " forward = " << forward_red
                       << " background = " << background_red);

    // Presence at the expected screen regions, not exact equality with raw
    // G-buffer/material values (this is the fully-lit, tonemapped result of
    // the real deferred + forward-opaque pipeline) -- see the task brief.
    CHECK(deferred_red > background_red + 0.05f);
    CHECK(forward_red > background_red + 0.05f);
  }

  SECTION("instanced_field_count = 0: the no-field path is unaffected") {
    const CpuImage image = RenderSceneWithFields(
        g, camera, std::span<InstancedMeshField* const>());

    const float deferred_red = SceneRedAt(image, camera, deferred_world);
    const float forward_red = SceneRedAt(image, camera, forward_world);
    const float background_red = SceneRedAt(image, camera, background_world);
    INFO("deferred = " << deferred_red << " forward = " << forward_red
                       << " background = " << background_red);

    CHECK(std::abs(deferred_red - background_red) < 0.05f);
    CHECK(std::abs(forward_red - background_red) < 0.05f);
  }
}

// ===========================================================================
// Phase 4 of the volumetric-foliage feature, scene level: SceneRenderer's
// real Pass 0 (shadow depth) now culls + draws instanced-field shadow
// submeshes (scene_renderer.cpp's pre-pass cull block + Pass 0, gated on
// shadow_config_.enable_shadow_map && field->HasPass(kShadow)). A field
// carrying ONLY a shadow submesh (no deferred/forward-opaque material -- so
// the field itself never appears in the lit image) is placed as a horizontal
// card above a floor entity, under an overhead (straight-down) sun so the
// shadow lands directly beneath it with zero horizontal offset. Read back in
// ShadowDebugMode::ShadowMapOnly (shadowMapPCF alone, 1.0 = lit, 0.0 = fully
// shadowed -- see deferred_lighting.wesl): the floor pixel directly under the
// card must be shadowed; a floor pixel outside the card's footprint must not.
// ===========================================================================
namespace {

// World -> pixel through camera's own matrices (mirrors this file's
// SceneProjectToPixel/decal_pass_tests.cpp's ProjectToPixel), parameterized
// on the target size since this test uses a different size than that helper.
glm::ivec2 WorldToPixel(const Camera& camera, glm::vec3 world, uint32_t width,
                        uint32_t height) {
  const glm::vec4 clip = camera.GetProj() * camera.GetView() * glm::vec4(world, 1.0f);
  REQUIRE(clip.w > 0.0f);  // in front of the camera
  const glm::vec3 ndc = glm::vec3(clip) / clip.w;
  return glm::ivec2(
      static_cast<int>((ndc.x * 0.5f + 0.5f) * static_cast<float>(width)),
      static_cast<int>((1.0f - (ndc.y * 0.5f + 0.5f)) * static_cast<float>(height)));
}

}  // namespace

TEST_CASE("SceneRenderer Pass 0 draws instanced-field shadow submeshes onto "
          "real geometry",
          "[gpu_instance][gpu]") {
  TestGpu& g = GetTestGpu();

  // A shadow-only material: casts_shadow=true, depth wired to the real
  // shadow-map format (matches shadow_map.cpp's Depth32Float texture --
  // scene_renderer.cpp's Pass 0 attaches shadow_map_.GetDepthView() directly).
  auto shadow_factory = MakeInstancedFactory(
      g, "instanced_gbuffer", MaterialPassType::kDeferred,
      {GBuffer::kNormalsFormat, GBuffer::kAlbedoFormat, GBuffer::kMaterialFormat},
      {}, /*casts_shadow=*/true, wgpu::TextureFormat::Depth32Float);
  REQUIRE(shadow_factory != nullptr);

  MaterialInstanceCache field_cache;
  InstanceParams shadow_params;
  shadow_params.uniform_overrides["tint"] =
      MaterialParameterValue(glm::vec4(1.0f, 1.0f, 1.0f, 1.0f));
  shadow_params.uniform_overrides["bucketId"] = MaterialParameterValue(uint32_t(0));
  entt::id_type shadow_key = ComposeMaterialCacheKey(
      entt::hashed_string{"p4_scene_shadow_caster"}.value(),
      GeometryType::kInstancedMesh, RenderPassType::kShadow, 0);
  auto shadow_handle = field_cache.GetOrCreate(
      shadow_key, *shadow_factory, GeometryType::kInstancedMesh,
      MaterialPassType::kDeferred, RenderPassType::kShadow, shadow_params);
  REQUIRE(shadow_handle);
  REQUIRE(shadow_handle->IsValid());

  // A single 4x4 (half-extent 2) horizontal card, world-up normal, centered
  // 3 units above the origin -- the same -90deg-about-X trick AddFloorQuad
  // uses to turn a local +Z-normal quad into a horizontal one.
  std::array<uint32_t, 6> idx = {0, 1, 2, 0, 2, 3};
  wgpu::Buffer ibuf =
      UploadBuffer(g.device, idx.data(), sizeof(idx), wgpu::BufferUsage::Index);
  std::vector<float> verts = QuadVerticesSized(2.0f);
  wgpu::Buffer vbuf = UploadBuffer(g.device, verts.data(),
                                   verts.size() * sizeof(float),
                                   wgpu::BufferUsage::Vertex);

  InstancedMeshField field(g.device, g.queue, *g.gen, /*capacity=*/1,
                           /*num_models=*/1, /*num_submeshes=*/1,
                           Lod3Chains(1, 1e30f, 2e30f));
  REQUIRE(field.IsValid());

  GpuInstanceRenderer::InstanceInput caster{};
  caster.transform =
      glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 3.0f, 0.0f)) *
      glm::rotate(glm::mat4(1.0f), glm::radians(-90.0f), glm::vec3(1.0f, 0.0f, 0.0f));
  caster.bounds_sphere = glm::vec4(0.0f, 3.0f, 0.0f, 3.0f);
  caster.model_info = glm::uvec4(0u, 0u, 0u, 0u);
  field.UploadInstances(
      std::span<const GpuInstanceRenderer::InstanceInput>(&caster, 1));

  // Geometry + shadow material only -- no deferred/forward-opaque material,
  // so this field never appears in the lit image itself; only its shadow does.
  field.SetSubmesh(/*model=*/0, /*lod=*/0, /*submesh=*/0, vbuf, ibuf,
                   wgpu::IndexFormat::Uint32, 6,
                   InstancedMeshField::PassKind::kDeferred, /*material=*/nullptr);
  field.SetSubmeshShadow(/*model=*/0, /*lod=*/0, /*submesh=*/0,
                         shadow_handle.operator->());
  REQUIRE(field.HasPass(InstancedMeshField::PassKind::kShadow));

  MaterialLibrary matlib;
  REQUIRE(matlib.Initialize(g.device, g.queue, g.gen.get()));

  SceneGraph graph;
  // Straight overhead sun: the shadow lands with zero horizontal offset
  // directly beneath the card, so "under the card" and "outside it" are
  // trivial to pick without re-deriving the light's oblique projection.
  graph.SetSunDirection(glm::vec3(0.0f, 1.0f, 0.0f));
  graph.SetSunColor(glm::vec3(1.0f));
  graph.SetClearColor(glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
  AddFloor(graph, 20.0f, matlib.SolidColor(glm::vec3(0.6f, 0.6f, 0.6f), 0.6f), 1.0f);

  entt::registry registry;
  SceneContext scene_context;
  scene_context.registry = &registry;
  graph.SyncToRegistry(registry, scene_context);
  std::array<InstancedMeshField*, 1> fields = {&field};
  scene_context.instanced_fields = fields.data();
  scene_context.instanced_field_count = 1;

  Camera camera;
  camera.position = glm::vec3(0.0f, 8.0f, 8.0f);
  camera.LookAt(glm::vec3(0.0f));
  camera.up = glm::vec3(0.0f, 1.0f, 0.0f);
  camera.fov = 45.0f;
  camera.aspect = 1.0f;
  camera.near_plane = 0.1f;
  camera.far_plane = 1000.0f;

  constexpr uint32_t kShadowSceneSize = 128;
  ColorRenderTarget rt(g.device, kShadowSceneSize, kShadowSceneSize,
                       wgpu::TextureFormat::R32Float);
  REQUIRE(rt.IsValid());

  SceneRenderer renderer;
  renderer.Initialize(g.device, g.queue, g.gen.get(), wgpu::TextureFormat::R32Float,
                      kShadowSceneSize, kShadowSceneSize,
                      g.device.HasFeature(wgpu::FeatureName::TextureFormatsTier1));
  renderer.MutableFogConfig().enabled = false;  // would haze the readback
  renderer.SetShadowDebugMode(ShadowDebugMode::ShadowMapOnly);

  CapturedError err = RunCapturingValidationErrors(g.instance, g.device, [&] {
    renderer.Render(camera, registry, scene_context, rt.GetView());
  });
  INFO("Dawn validation error: " << err.message);
  CHECK(err.type == wgpu::ErrorType::NoError);
  test::WaitForGpu(g.instance, g.device, g.queue);

  TextureReadback readback(g.instance, g.device, g.queue);
  CpuImage image = readback.ReadTextureSync(
      rt.GetTexture(), kShadowSceneSize, kShadowSceneSize,
      wgpu::TextureFormat::R32Float);

  // World origin: directly under the card's center -> shadowed. (0,0,-5): 3
  // units outside the card's [-2,2] XZ footprint along -Z (further from the
  // camera, which keeps it a safely in-bounds pixel under this camera's
  // oblique projection -- +Z lands off-frame) -> lit.
  const glm::ivec2 under_card =
      WorldToPixel(camera, glm::vec3(0.0f, 0.0f, 0.0f), kShadowSceneSize,
                  kShadowSceneSize);
  const glm::ivec2 offset =
      WorldToPixel(camera, glm::vec3(0.0f, 0.0f, -5.0f), kShadowSceneSize,
                  kShadowSceneSize);
  REQUIRE(under_card.x >= 0);
  REQUIRE(under_card.x < static_cast<int>(kShadowSceneSize));
  REQUIRE(under_card.y >= 0);
  REQUIRE(under_card.y < static_cast<int>(kShadowSceneSize));
  REQUIRE(offset.x >= 0);
  REQUIRE(offset.x < static_cast<int>(kShadowSceneSize));
  REQUIRE(offset.y >= 0);
  REQUIRE(offset.y < static_cast<int>(kShadowSceneSize));

  const float shadowed = image.GetDepth(static_cast<uint32_t>(under_card.x),
                                        static_cast<uint32_t>(under_card.y));
  const float lit = image.GetDepth(static_cast<uint32_t>(offset.x),
                                   static_cast<uint32_t>(offset.y));
  INFO("under-card shadow value = " << shadowed << ", offset = " << lit);
  CHECK(shadowed < 0.3f);
  CHECK(lit > 0.7f);
}

// ===========================================================================
// Phase 2 of the volumetric-foliage feature: the "voxel_foliage" G-buffer
// material (shaders/material/voxel_foliage.wesl) + deferred_lighting.wesl's
// foliage-transmission branch. This phase is VISUALLY INERT -- nothing yet
// writes material.a = kShadingModelFoliage on any real scene, so there is no
// full-pipeline (lit) test here; that arrives with Phase 3+ (real geometry
// attached to this material). What's pinned:
//   1. All 4 (geometry x pass) shader variants compile.
//   2. A hand-built tet (Phase 1's leaf_voxelizer output shape, inlined --
//      this is an ENGINE test target and must not link game code) round-trips
//      through the non-instanced G-buffer pipeline with the expected
//      material.a/.g/.r encoding, and CullMode::Back respects triangle
//      winding (visible / gone).
//   3. deferred_lighting.wesl's widened imports + foliage branch don't
//      perturb the STANDARD (non-foliage) shading path -- material.a stays
//      0.0 for every existing material, so `transmission` is always zero.
// ===========================================================================
namespace {

// One hand-built Phase-1-style tet: 4 vertices / 12 indices, the 44-byte
// textured-mesh layout (pos3, uv2 with uv.x = brightness, normal3, tangent3
// -- see leaf_voxelizer.hpp's EmitTetMesh). v0,v1,v2 form the "near" face
// (facing the camera, dead-center on the view axis -- its centroid projects
// to the exact center pixel, see BuildVoxelFoliageFrame); v3 is placed FAR
// BEHIND the camera (z = +40, camera at the origin looking down -Z) so the
// other 3 faces (each sharing 2 vertices with the near face + v3) are almost
// entirely near-plane/behind-eye clipped -- only a razor-thin sliver hugging
// v0/v1/v2 themselves could remain, nowhere near the near face's own
// centroid. This is what makes "flip ALL 12 indices -> the center pixel goes
// background" a sound test: for a CLOSED, correctly-wound solid, flipping
// every face's winding does NOT generally empty its silhouette (the
// previously back-facing faces become front-facing-per-Dawn and typically
// cover much of the SAME screen area, by convexity) -- it only works here
// because the 3 "side" faces are frustum-clipped away regardless of winding.
constexpr glm::vec3 kTetNearP0(0.0f, 0.5f, -2.0f);
constexpr glm::vec3 kTetNearP1(-0.43f, -0.25f, -2.0f);
constexpr glm::vec3 kTetNearP2(0.43f, -0.25f, -2.0f);
constexpr glm::vec3 kTetFarApex(0.1f, -0.25f, 40.0f);

// All 4 faces outward-CCW (GenerateCube's "CCW viewed from outside"
// convention -- primitive_mesh_builders.cpp), matching the engine's
// CullMode::Back default: the near face (0,1,2) is front-facing (visible);
// the 3 apex faces are back-facing (their outward normal points away from
// the camera, toward the far apex) and thus culled.
constexpr std::array<uint32_t, 12> kTetIndicesVisible = {0, 1, 2, 0, 1, 3,
                                                          1, 2, 3, 2, 0, 3};
// Every triangle's last two indices swapped -- reverses all 4 faces' winding.
constexpr std::array<uint32_t, 12> kTetIndicesFlipped = {0, 2, 1, 0, 3, 1,
                                                          1, 3, 2, 2, 3, 0};

constexpr uint32_t kTetTarget = 96;

std::vector<float> BuildTetVertices(float brightness) {
  const std::array<glm::vec3, 4> pos = {kTetNearP0, kTetNearP1, kTetNearP2,
                                        kTetFarApex};
  std::vector<float> verts;
  verts.reserve(4 * kTexturedMeshFloatsPerVertex);
  for (const glm::vec3& p : pos) {
    // The normal/tangent values are never read by this test (the G-buffer
    // readback only asserts material.a/.g/.r, which packVoxelFoliageGBuffer
    // derives from params/tint, not the normal) -- any finite unit vectors
    // are fine.
    const float v[kTexturedMeshFloatsPerVertex] = {
        p.x,  p.y,  p.z,  brightness, 0.0f, 0.0f,
        0.0f, 1.0f, 1.0f, 0.0f,       0.0f, 1.0f};
    verts.insert(verts.end(), v, v + kTexturedMeshFloatsPerVertex);
  }
  return verts;
}

// A real (non-degenerate) perspective frame, camera at the world/offset
// origin looking down -Z -- unlike MakeOrthoFrame (which drops Z entirely,
// fine for the CullMode::None instanced tests above but WRONG here: this
// test needs genuine near-plane clipping to keep the 3 apex faces off the
// near face's centroid, see the comment above). object.modelMatrix is left
// identity (camera_world_pos = 0, so offset space == world space, and the
// tet's own coordinates are already the "already offset" positions the
// non-instanced vs_main expects).
UniformData BuildVoxelFoliageFrame() {
  UniformData u{};
  Camera cam;
  cam.position = glm::vec3(0.0f);
  cam.direction = glm::vec3(0.0f, 0.0f, -1.0f);
  cam.up = glm::vec3(0.0f, 1.0f, 0.0f);
  cam.fov = 50.0f;
  cam.aspect = 1.0f;
  cam.near_plane = 0.1f;
  cam.far_plane = 100.0f;
  u.view = glm::lookAt(glm::vec3(0.0f), cam.direction, cam.up);
  u.proj = cam.GetProj();
  u.view_prev = u.view;
  u.proj_prev = u.proj;
  u.light_view_proj = glm::mat4(1.0f);
  u.camera_world_pos = glm::vec4(0.0f);
  u.sunDir = glm::vec4(glm::normalize(glm::vec3(0.3f, 0.8f, 0.5f)), 0.0f);
  u.sunColor = glm::vec4(1.0f);
  u.ambient_sh[0] = glm::vec4(1.0f);
  u.near_plane = cam.near_plane;
  u.far_plane = cam.far_plane;
  u.screen_size = glm::vec2(float(kTetTarget), float(kTetTarget));
  u.output_is_linear = 1u;
  return u;
}

std::unique_ptr<MaterialInstanceFactory> MakeVoxelFoliageFactory(
    TestGpu& g, GeometryType geometry, bool casts_shadow,
    wgpu::TextureFormat depth_format = wgpu::TextureFormat::Undefined) {
  FactoryDescriptor desc;
  desc.shader_name = "voxel_foliage";
  desc.shader_path = "material/voxel_foliage.wesl";
  desc.supported_geometry_types = {geometry};
  desc.supported_pass_types = {MaterialPassType::kDeferred};
  desc.color_formats = {GBuffer::kNormalsFormat, GBuffer::kAlbedoFormat,
                        GBuffer::kMaterialFormat};
  desc.depth_format = depth_format;
  desc.cull_mode = wgpu::CullMode::Back;
  desc.casts_shadow = casts_shadow;
  return BuildMaterialInstanceFactory(desc, g.device, g.queue, g.gen.get());
}

// G-buffer MRT clear values matching scene_renderer.cpp's Pass 1 exactly
// (normals/albedo transparent-black, material {0.75, 1.0, 1.0, 0.0} -- so
// material.a == 0.0 is the "nothing drawn here" default, same threshold
// deferred_lighting.wesl's foliage gate relies on).
wgpu::RenderPassColorAttachment TetClearAttachment(wgpu::TextureView view,
                                                   wgpu::Color clear) {
  wgpu::RenderPassColorAttachment ca{};
  ca.view = view;
  ca.loadOp = wgpu::LoadOp::Clear;
  ca.storeOp = wgpu::StoreOp::Store;
  ca.clearValue = clear;
  ca.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
  return ca;
}

// Both G-buffer targets' exact center pixel for one RenderTetPixels() call --
// see that function's comment for why the center pixel is the meaningful one.
struct TetPixels {
  CpuImage::Color material;
  CpuImage::Color albedo;
};

// Renders `indices` (kTetIndicesVisible or kTetIndicesFlipped) through the
// non-instanced voxel_foliage material, with vertex uv.x = `brightness` and
// the material's `tint`, and returns BOTH the material and albedo targets'
// exact center pixel -- the near face's centroid projects there by
// construction (kTetNearP0/P1/P2 average to (0,0,-2), dead on the camera's
// view axis). CpuImage::GetPixel already converts BGRA8Unorm storage
// (GBuffer::kAlbedoFormat) to RGBA channel order, so the returned albedo's
// .r/.g/.b are plain RGB regardless of the target's memory layout.
TetPixels RenderTetPixels(TestGpu& g, MaterialInstanceFactory& factory,
                          float roughness, float strength, float brightness,
                          glm::vec3 tint,
                          const std::array<uint32_t, 12>& indices) {
  InstanceParams params;
  params.uniform_overrides = {
      {"tint", glm::vec4(tint, 1.0f)},
      {"params", glm::vec4(roughness, strength, 0.0f, 0.0f)},
  };
  auto instance =
      factory.CreateInstance(GeometryType::kTexturedMesh,
                             MaterialPassType::kDeferred,
                             RenderPassType::kGBuffer, params);
  REQUIRE(instance != nullptr);
  instance->SetParameterByName("modelMatrix",
                               MaterialParameterValue(glm::mat4(1.0f)));

  const std::vector<float> verts = BuildTetVertices(brightness);
  wgpu::Buffer vbuf = UploadBuffer(g.device, verts.data(),
                                   verts.size() * sizeof(float),
                                   wgpu::BufferUsage::Vertex);
  wgpu::Buffer ibuf = UploadBuffer(g.device, indices.data(),
                                   indices.size() * sizeof(uint32_t),
                                   wgpu::BufferUsage::Index);

  ColorRenderTarget normals_t(g.device, kTetTarget, kTetTarget,
                              GBuffer::kNormalsFormat);
  ColorRenderTarget albedo_t(g.device, kTetTarget, kTetTarget,
                             GBuffer::kAlbedoFormat);
  ColorRenderTarget material_t(g.device, kTetTarget, kTetTarget,
                               GBuffer::kMaterialFormat);
  REQUIRE(material_t.IsValid());

  UniformData frame_uniforms = BuildVoxelFoliageFrame();
  FrameContext frame;
  frame.Begin(g.device, g.queue, frame_uniforms);
  {
    std::array<wgpu::RenderPassColorAttachment, 3> ca = {
        TetClearAttachment(normals_t.GetView(), {0, 0, 0, 0}),
        TetClearAttachment(albedo_t.GetView(), {0, 0, 0, 0}),
        TetClearAttachment(material_t.GetView(), {0.75, 1.0, 1.0, 0.0})};
    wgpu::RenderPassDescriptor desc{};
    desc.colorAttachmentCount = ca.size();
    desc.colorAttachments = ca.data();
    RenderPassContext pass = frame.BeginRenderPass(desc);
    REQUIRE(instance->Bind(pass, frame));
    REQUIRE(instance->BindPerObject(pass, frame));
    pass.SetVertexBuffer(0, vbuf);
    pass.SetIndexBuffer(ibuf, wgpu::IndexFormat::Uint32);
    pass.DrawIndexed(static_cast<uint32_t>(indices.size()));
    pass.End();
  }
  wgpu::CommandBuffer cmd = frame.End();
  g.queue.Submit(1, &cmd);
  test::WaitForGpu(g.instance, g.device, g.queue);

  TextureReadback rb(g.instance, g.device, g.queue);
  CpuImage material_img = rb.ReadTextureSync(
      material_t.GetTexture(), kTetTarget, kTetTarget, GBuffer::kMaterialFormat);
  CpuImage albedo_img = rb.ReadTextureSync(albedo_t.GetTexture(), kTetTarget,
                                           kTetTarget, GBuffer::kAlbedoFormat);
  TetPixels result;
  result.material = material_img.GetPixel(kTetTarget / 2, kTetTarget / 2);
  result.albedo = albedo_img.GetPixel(kTetTarget / 2, kTetTarget / 2);
  return result;
}

}  // namespace

TEST_CASE("voxel_foliage material compiles all 4 (geometry x pass) variants",
          "[gpu_instance][gpu]") {
  TestGpu& g = GetTestGpu();

  auto textured_factory = MakeVoxelFoliageFactory(
      g, GeometryType::kTexturedMesh, /*casts_shadow=*/true,
      wgpu::TextureFormat::Depth32Float);
  auto instanced_factory = MakeVoxelFoliageFactory(
      g, GeometryType::kInstancedMesh, /*casts_shadow=*/true,
      wgpu::TextureFormat::Depth32Float);
  REQUIRE(textured_factory != nullptr);
  REQUIRE(instanced_factory != nullptr);

  struct Variant {
    const char* label;
    MaterialInstanceFactory* factory;
    GeometryType geometry;
    RenderPassType pass;
  };
  const std::array<Variant, 4> variants = {{
      {"voxel_foliage_textured_gbuffer", textured_factory.get(),
       GeometryType::kTexturedMesh, RenderPassType::kGBuffer},
      {"voxel_foliage_textured_shadow", textured_factory.get(),
       GeometryType::kTexturedMesh, RenderPassType::kShadow},
      {"voxel_foliage_instanced_gbuffer", instanced_factory.get(),
       GeometryType::kInstancedMesh, RenderPassType::kGBuffer},
      {"voxel_foliage_instanced_shadow", instanced_factory.get(),
       GeometryType::kInstancedMesh, RenderPassType::kShadow},
  }};

  MaterialInstanceCache cache;
  for (const Variant& v : variants) {
    entt::id_type key = ComposeMaterialCacheKey(
        entt::hashed_string{v.label}.value(), v.geometry, v.pass, 0);
    entt::resource<RenderingMaterialInstance> handle;
    CapturedError err = RunCapturingValidationErrors(g.instance, g.device, [&] {
      handle = cache.GetOrCreate(key, *v.factory, v.geometry,
                                 MaterialPassType::kDeferred, v.pass,
                                 InstanceParams{});
    });
    INFO(v.label << " Dawn validation error: " << err.message);
    CHECK(err.type == wgpu::ErrorType::NoError);
    REQUIRE(handle);
    REQUIRE(handle->IsValid());
  }
}

TEST_CASE("voxel_foliage G-buffer readback: material encoding + CullMode::Back "
          "winding",
          "[gpu_instance][gpu]") {
  TestGpu& g = GetTestGpu();
  auto factory = MakeVoxelFoliageFactory(g, GeometryType::kTexturedMesh,
                                         /*casts_shadow=*/false);
  REQUIRE(factory != nullptr);

  constexpr float kRoughness = 0.6f;
  constexpr float kStrength = 0.35f;
  // Brightness deliberately NOT 0.0/1.0 (both are fixed points of the sRGB
  // curve, srgb_to_linear(x) == x there -- a double-linearization bug would
  // be invisible at either endpoint) so a bug that runs uv.x through
  // srgb_to_linear at write time (this material) is unmistakable in the
  // readback below.
  constexpr float kBrightness = 0.5f;
  const glm::vec3 kTint(0.5f, 0.25f, 1.0f);

  SECTION("correct winding: material.a/.g/.r match, alpha tagged as foliage, "
          "albedo is brightness*tint RAW (sRGB-domain, not double-linearized)") {
    CapturedError err;
    TetPixels px;
    err = RunCapturingValidationErrors(g.instance, g.device, [&] {
      px = RenderTetPixels(g, *factory, kRoughness, kStrength, kBrightness,
                          kTint, kTetIndicesVisible);
    });
    INFO("Dawn validation error: " << err.message);
    CHECK(err.type == wgpu::ErrorType::NoError);
    INFO("material rgba = " << (int)px.material.r << "," << (int)px.material.g
                            << "," << (int)px.material.b << ","
                            << (int)px.material.a);
    // material.a = kShadingModelFoliage (1.0) -- comfortably above the 0.5
    // threshold deferred_lighting.wesl gates on.
    CHECK(px.material.a > 127);
    // material.g = translucency strength, material.r = roughness, both
    // RGBA8Unorm-quantized -- +/-2/255 tolerance.
    CHECK(std::abs(static_cast<int>(px.material.g) -
                   static_cast<int>(std::lround(kStrength * 255.0f))) <= 2);
    CHECK(std::abs(static_cast<int>(px.material.r) -
                   static_cast<int>(std::lround(kRoughness * 255.0f))) <= 2);

    // Albedo must be the RAW (sRGB-domain) product of the vertex-baked
    // brightness and the material tint -- deferred_lighting.wesl's
    // srgb_to_linear runs on READ (its albedoLinear line), so this material
    // writing srgb_to_linear(brightness)*tint too would double-linearize and
    // render crowns far too dark. +/-2/255 tolerance for RGBA8Unorm
    // quantization. Pre-fix, brightness=0.5 wrote srgb_to_linear(0.5) ~=
    // 0.214 (not 0.5) into the brightness term, so e.g. the r channel
    // (tint.r=0.5) landed at byte ~27 instead of the expected ~64.
    INFO("albedo rgb = " << (int)px.albedo.r << "," << (int)px.albedo.g << ","
                         << (int)px.albedo.b);
    const glm::vec3 expected_albedo = kBrightness * kTint;
    CHECK(std::abs(static_cast<int>(px.albedo.r) -
                   static_cast<int>(std::lround(expected_albedo.r * 255.0f))) <=
          2);
    CHECK(std::abs(static_cast<int>(px.albedo.g) -
                   static_cast<int>(std::lround(expected_albedo.g * 255.0f))) <=
          2);
    CHECK(std::abs(static_cast<int>(px.albedo.b) -
                   static_cast<int>(std::lround(expected_albedo.b * 255.0f))) <=
          2);
  }

  SECTION("flipped index winding: the near face is culled, pixel stays "
          "background") {
    CapturedError err;
    TetPixels px;
    err = RunCapturingValidationErrors(g.instance, g.device, [&] {
      px = RenderTetPixels(g, *factory, kRoughness, kStrength, kBrightness,
                          kTint, kTetIndicesFlipped);
    });
    INFO("Dawn validation error: " << err.message);
    CHECK(err.type == wgpu::ErrorType::NoError);
    INFO("material rgba = " << (int)px.material.r << "," << (int)px.material.g
                            << "," << (int)px.material.b << ","
                            << (int)px.material.a);
    // Nothing drawn at the center pixel -- stays at the material clear
    // value's alpha (0.0), well below the foliage-tag threshold.
    CHECK(px.material.a < 10);
  }
}

TEST_CASE("MaterialLibrary::VoxelFoliage builds a textureless deferred "
          "material",
          "[gpu_instance][gpu]") {
  TestGpu& g = GetTestGpu();
  MaterialLibrary matlib;
  REQUIRE(matlib.Initialize(g.device, g.queue, g.gen.get()));

  const glm::vec3 tint(0.3f, 0.7f, 0.2f);
  constexpr float kRoughness = 0.6f;
  constexpr float kStrength = 0.35f;

  DeferredMaterial result;
  CapturedError err = RunCapturingValidationErrors(g.instance, g.device, [&] {
    result = matlib.VoxelFoliage(tint, kRoughness, kStrength);
  });
  INFO("Dawn validation error: " << err.message);
  CHECK(err.type == wgpu::ErrorType::NoError);
  REQUIRE(result.factory != nullptr);
  REQUIRE(result.params.uniform_overrides.count("tint") == 1u);
  REQUIRE(result.params.uniform_overrides.count("params") == 1u);
  // No texture_overrides -- voxel_foliage.wesl declares no textures/samplers.
  CHECK(result.params.texture_overrides.empty());

  const auto& tint_value = result.params.uniform_overrides.at("tint");
  REQUIRE(std::holds_alternative<glm::vec4>(tint_value));
  CHECK(std::get<glm::vec4>(tint_value) == glm::vec4(tint, 1.0f));

  const auto& params_value = result.params.uniform_overrides.at("params");
  REQUIRE(std::holds_alternative<glm::vec4>(params_value));
  CHECK(std::get<glm::vec4>(params_value) ==
       glm::vec4(kRoughness, kStrength, 0.0f, 0.0f));

  // Same (tint, roughness, strength) -> same cached factory (shared, lazily
  // built once) and equal params, mirroring SolidColor's cache contract.
  DeferredMaterial again = matlib.VoxelFoliage(tint, kRoughness, kStrength);
  CHECK(again.factory == result.factory);
}

TEST_CASE("deferred_lighting foliage branch is inert for standard "
          "(non-foliage) materials",
          "[gpu_instance][gpu]") {
  // Renders a real SceneRenderer frame (fog off, matching RenderSceneWithFields
  // above) with a single SolidColor-material floor -- the "normalmapped"
  // shader's packNormalMappedGBuffer always writes material.a = 0.0, so
  // deferred_lighting.wesl's `materialData.a > kShadingModelFoliage * 0.5`
  // gate must stay false for every pixel this scene draws; `transmission`
  // stays vec3(0.0) and `finalColorLinear + transmission` is arithmetically
  // identical to the pre-Phase-2 `finalColorLinear` alone. This is a
  // compile/shade smoke test (the widened imports + new branch don't corrupt
  // the standard path), not a byte-exact pin against a prior build.
  TestGpu& g = GetTestGpu();

  MaterialLibrary matlib;
  REQUIRE(matlib.Initialize(g.device, g.queue, g.gen.get()));

  SceneGraph graph;
  graph.SetSunDirection(glm::normalize(glm::vec3(0.2f, 1.0f, 0.3f)));
  graph.SetSunColor(glm::vec3(1.0f));
  graph.SetAmbient(glm::vec3(0.6f));
  graph.SetClearColor(glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
  AddFloor(graph, 20.0f, matlib.SolidColor(glm::vec3(0.6f, 0.35f, 0.2f), 0.6f),
          1.0f);

  entt::registry registry;
  SceneContext scene_context;
  scene_context.registry = &registry;
  graph.SyncToRegistry(registry, scene_context);

  Camera camera;
  camera.position = glm::vec3(0.0f, 8.0f, 8.0f);
  camera.LookAt(glm::vec3(0.0f));
  camera.up = glm::vec3(0.0f, 1.0f, 0.0f);
  camera.fov = 45.0f;
  camera.aspect = 1.0f;
  camera.near_plane = 0.1f;
  camera.far_plane = 1000.0f;

  constexpr uint32_t kFloorTestSize = 128;
  ColorRenderTarget rt(g.device, kFloorTestSize, kFloorTestSize,
                       wgpu::TextureFormat::R32Float);
  REQUIRE(rt.IsValid());

  SceneRenderer renderer;
  renderer.Initialize(g.device, g.queue, g.gen.get(),
                      wgpu::TextureFormat::R32Float, kFloorTestSize,
                      kFloorTestSize,
                      g.device.HasFeature(wgpu::FeatureName::TextureFormatsTier1));
  renderer.MutableFogConfig().enabled = false;  // would haze the readback

  CapturedError err = RunCapturingValidationErrors(g.instance, g.device, [&] {
    renderer.Render(camera, registry, scene_context, rt.GetView());
  });
  INFO("Dawn validation error: " << err.message);
  CHECK(err.type == wgpu::ErrorType::NoError);
  test::WaitForGpu(g.instance, g.device, g.queue);

  TextureReadback readback(g.instance, g.device, g.queue);
  CpuImage image = readback.ReadTextureSync(
      rt.GetTexture(), kFloorTestSize, kFloorTestSize,
      wgpu::TextureFormat::R32Float);

  // Floor center (world origin) projects to the image center under this
  // top-down-tilted camera; a corner stays sky/background.
  const float floor_red = image.GetDepth(kFloorTestSize / 2, kFloorTestSize / 2);
  const float background_red = image.GetDepth(4, 4);
  INFO("floor = " << floor_red << " background = " << background_red);
  // The floor is lit (sun + ambient on a mid-gray albedo) -- comfortably
  // brighter than the black clear color; finite (no NaN/Inf from a
  // miscomposed transmission term).
  CHECK(floor_red > background_red + 0.05f);
  CHECK(std::isfinite(floor_red));
  CHECK(background_red < 0.01f);
}
