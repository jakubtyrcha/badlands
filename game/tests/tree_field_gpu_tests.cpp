// Headless full-frame GPU tests for the volumetric-foliage Phase 3 viewer
// integration: builds a REAL Oak crown (GenerateLeafMesh + VoxelizeLeafCards
// -- the exact CPU pipeline model_viewer_view.cpp's Voxel L0/L1/L2 modes
// drive) as a deferred VoxelFoliage entity in a real SceneGraph, renders it
// through the FULL SceneRenderer pipeline (G-buffer -> shadow map ->
// deferred lighting -> tonemap), and reads pixels back. Harness modeled on
// src/engine/tests/gpu_instance_tests.cpp's RenderSceneWithFields
// (:3060-3097), adapted to an ENTITY-based scene (a real SceneGraph +
// AddMeshEntity, see engine/rendering/scene_build.hpp) instead of an
// InstancedMeshField -- volumetric-foliage Phase 3 doesn't touch
// InstancedMeshField/TreeField at all (that's a later phase).
//
// Two behaviours locked in here:
//   1. A voxel crown actually renders: the brightest pixel in a front-lit
//      frame is clearly brighter than an empty-background corner (proves the
//      tets rasterize, write real G-buffer depth, and reach deferred
//      lighting -- not just a non-null factory).
//   2. The deferred VoxelFoliage material's back-lit transmission (Phase 2's
//      deferred_lighting.wesl `materialData.a > kShadingModelFoliage * 0.5`
//      branch) actually fires: with the sun positioned behind the crown
//      relative to the camera (so camera-facing tets are back-lit, see
//      evaluateTranslucency's `backLit = max(-dot(N,L), 0)` in
//      shaders/common/standard_lighting.wesl) and ambient off (isolating the
//      direct back-lit term), the brightest pixel of a translucency_strength
//      = 0.6 render is brighter than the SAME pixel at strength 0.0.
//
// Both tests pick their query pixel via an argmax scan of the rendered image
// rather than hand-projecting a specific tet's centroid: EmitTetMesh's
// per-tet normal follows each leaf card's own (jittered) growth axis, which
// is NOT reliably "radially outward from the crown" for every leaf
// arrangement (e.g. Oak's FanFromStem) -- a hand-picked "frontmost" tet can
// easily have a normal that isn't actually camera-facing/back-lit under a
// given rig. Scanning for the brightest pixel sidesteps needing to predict
// any single tet's orientation: across the hundreds of tets in a real crown,
// some are reliably well-lit (Test 1) or reliably back-lit (Test 2) under a
// rig aimed roughly at the whole crown from a fixed axis.
#include <catch_amalgamated.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>  // glm::translate/scale, Phase 5 field tests

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <span>

#include "core/util/cpu_image.hpp"
#include "engine/core/camera.hpp"
#include "engine/rendering/color_render_target.hpp"
#include "engine/rendering/context/scene_context.hpp"
#include "engine/rendering/geometry/aabb.hpp"
#include "engine/rendering/geometry/textured_mesh_builders.hpp"
#include "engine/rendering/gpu_instance_renderer.hpp"  // Phase 5 field tests
#include "engine/rendering/instanced_mesh_field.hpp"    // Phase 5 field tests
#include "engine/rendering/material_library.hpp"
#include "engine/rendering/scene_build.hpp"
#include "engine/rendering/scene_renderer.hpp"
#include "engine/rendering/shader/gpu_pipeline_generator.hpp"
#include "engine/rendering/texture_readback.hpp"
#include "engine/rendering/util/find_shader_directory.hpp"
#include "engine/scene/scene_graph.hpp"
#include "engine/tests/gpu_test_helpers.hpp"
#include "game/geometry/leaf_voxelizer.hpp"
#include "game/geometry/tree_generator.hpp"
#include "game/geometry/tree_options.hpp"
#include "game/visual/tree_field.hpp"  // Phase 5 field tests

using namespace badlands;

namespace {

constexpr uint32_t kSceneSize = 256;

struct TestGpu {
  wgpu::Instance instance;
  wgpu::Device device;
  wgpu::Queue queue;
  std::unique_ptr<GpuPipelineGenerator> gen;
  MaterialLibrary matlib;
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
    t->gen =
        std::make_unique<GpuPipelineGenerator>(t->device, FindShaderDirectory());
    REQUIRE(t->matlib.Initialize(t->device, t->queue, t->gen.get()));
    return t;
  }();
  return *g;
}

struct CapturedError {
  wgpu::ErrorType type = wgpu::ErrorType::NoError;
  std::string message;
};

// Runs `fn` (expected to trigger the full-frame render) inside a Dawn
// validation-error scope and returns what it observed. Mirrors
// gpu_instance_tests.cpp's RunCapturingValidationErrors (:871-894): every
// other full-frame GPU test in the codebase wraps SceneRenderer::Render in
// this pattern -- the default device error callback only logs to stderr and
// cannot fail a test, so a WGSL/bind-group validation error introduced by
// this crown's VoxelFoliage material would otherwise go unnoticed here.
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

// Voxelizes OakPreset's leaf cards with default LeafVoxelizeOptions -- the
// same SplatLeafCards/EmitTetMesh pipeline model_viewer_view.cpp's Voxel
// L0/L1/L2 modes call (just a different cell_size per level; Phase 1's own
// tests already cover byte-determinism/topology, so this file takes that as
// given and focuses on the GPU-visible result).
TexturedMeshResult MakeOakCrown() {
  const TreeOptions oak = OakPreset();
  const TexturedMeshResult leaves = GenerateLeafMesh(oak);
  REQUIRE(leaves.mesh.vertex_count > 0u);
  const TexturedMeshResult crown = VoxelizeLeafCards(
      leaves.mesh, oak.leaves.silhouette, LeafVoxelizeOptions{});
  REQUIRE(crown.mesh.vertex_count > 0u);
  REQUIRE(crown.mesh.vertex_count % 4u == 0u);
  return crown;
}

// A camera framing `bounds`'s bounding sphere from +Z, far enough back that
// the whole sphere sits inside the frustum with margin (so an image-corner
// pixel is reliably outside the crown's silhouette -- see kBackgroundPixel
// below).
Camera FramingCamera(const Aabb& bounds) {
  const glm::vec3 center = bounds.Center();
  const float radius = std::max(glm::length(bounds.max - center), 0.1f);
  Camera camera;
  camera.position = center + glm::vec3(0.0f, 0.0f, radius * 3.0f + 2.0f);
  camera.direction = glm::vec3(0.0f, 0.0f, -1.0f);
  camera.up = glm::vec3(0.0f, 1.0f, 0.0f);
  camera.fov = 55.0f;
  camera.aspect = 1.0f;
  camera.near_plane = 0.05f;
  camera.far_plane = radius * 8.0f + 20.0f;
  return camera;
}

// A corner well outside FramingCamera's framed bounding sphere (see its
// margin comment) -- background for the "differs from background" test.
constexpr glm::ivec2 kBackgroundPixel{4, 4};

// Renders one full SceneRenderer frame (G-buffer -> shadow map -> deferred
// lighting -> tonemap) of a scene holding exactly one VoxelFoliage crown
// entity at `translucency_strength`, under `sun_direction`/`sun_color`/
// `ambient`, and reads back the R32Float target -- same harness shape as
// gpu_instance_tests.cpp's RenderSceneWithFields, adapted to an entity-based
// scene (a real SceneGraph via AddMeshEntity) instead of an
// InstancedMeshField.
CpuImage RenderOakCrownFrame(TestGpu& g, const Camera& camera,
                             float translucency_strength,
                             glm::vec3 sun_direction, glm::vec3 sun_color,
                             glm::vec3 ambient) {
  TexturedMeshResult crown = MakeOakCrown();

  SceneGraph scene;
  scene.SetSunDirection(sun_direction);
  scene.SetSunColor(sun_color);
  scene.SetAmbient(ambient);  // flat L0 SH
  AddMeshEntity(scene, "crown", std::move(crown),
               g.matlib.VoxelFoliage(glm::vec3(0.32f, 0.52f, 0.18f), 0.9f,
                                     translucency_strength));

  entt::registry registry;
  SceneContext scene_context;
  scene.SyncToRegistry(registry, scene_context);

  ColorRenderTarget rt(g.device, kSceneSize, kSceneSize,
                       wgpu::TextureFormat::R32Float);
  REQUIRE(rt.IsValid());

  SceneRenderer renderer;
  renderer.Initialize(g.device, g.queue, g.gen.get(),
                      wgpu::TextureFormat::R32Float, kSceneSize, kSceneSize,
                      g.device.HasFeature(wgpu::FeatureName::TextureFormatsTier1));
  renderer.MutableFogConfig().enabled = false;  // would haze the readback

  CapturedError err = RunCapturingValidationErrors(g, [&] {
    renderer.Render(camera, registry, scene_context, rt.GetView());
  });
  INFO("Dawn validation error: " << err.message);
  CHECK(err.type == wgpu::ErrorType::NoError);
  test::WaitForGpu(g.instance, g.device, g.queue);

  TextureReadback readback(g.instance, g.device, g.queue);
  return readback.ReadTextureSync(rt.GetTexture(), kSceneSize, kSceneSize,
                                  wgpu::TextureFormat::R32Float);
}

// Pixel coordinate of the brightest (max red-channel) texel in `image`.
glm::ivec2 ArgmaxPixel(const CpuImage& image) {
  glm::ivec2 best(0, 0);
  float best_value = -1e30f;
  for (uint32_t y = 0; y < kSceneSize; ++y) {
    for (uint32_t x = 0; x < kSceneSize; ++x) {
      const float v = image.GetDepth(x, y);
      if (v > best_value) {
        best_value = v;
        best = glm::ivec2(static_cast<int>(x), static_cast<int>(y));
      }
    }
  }
  return best;
}

// Pixel coordinate where `a` and `b` differ the most (max |a-b|). Used to
// find the transmission-driven pixel for the strength ON-vs-OFF comparison:
// ArgmaxPixel on a single frame isn't reliable there, since the crown's
// single brightest pixel can be a REFLECTANCE rim highlight (grazing-angle
// Fresnel brightening at a silhouette tet, strength-independent) rather than
// a transmission glow -- confirmed empirically: with a pure-axial backlighting
// rig, the crown's single brightest pixel was pixel-identical between
// strength 0.6 and 0.0, while a real +0.18 transmission-only delta existed
// elsewhere in the same frame. Finding the biggest ON-vs-OFF delta directly
// is what the test actually wants to assert on.
glm::ivec2 ArgmaxAbsDiffPixel(const CpuImage& a, const CpuImage& b) {
  glm::ivec2 best(0, 0);
  float best_diff = -1.0f;
  for (uint32_t y = 0; y < kSceneSize; ++y) {
    for (uint32_t x = 0; x < kSceneSize; ++x) {
      const float d = std::abs(a.GetDepth(x, y) - b.GetDepth(x, y));
      if (d > best_diff) {
        best_diff = d;
        best = glm::ivec2(static_cast<int>(x), static_cast<int>(y));
      }
    }
  }
  return best;
}

}  // namespace

TEST_CASE("voxel Oak crown renders: brightest pixel differs from background",
          "[tree_field][gpu]") {
  TestGpu& g = GetTestGpu();
  const TexturedMeshResult probe = MakeOakCrown();
  const Camera camera = FramingCamera(probe.local_bounds);

  // Front-lit rig (sun roughly on the camera's side): maximizes the odds
  // some tet's normal is well-aligned with the sun, independent of the
  // back-lit transmission path Test 2 below exercises.
  const CpuImage image = RenderOakCrownFrame(
      g, camera, /*translucency_strength=*/0.6f,
      /*sun_direction=*/glm::normalize(glm::vec3(0.3f, 0.6f, 0.7f)),
      /*sun_color=*/glm::vec3(2.0f), /*ambient=*/glm::vec3(0.4f));

  const glm::ivec2 crown_px = ArgmaxPixel(image);
  const float crown_red = image.GetDepth(static_cast<uint32_t>(crown_px.x),
                                         static_cast<uint32_t>(crown_px.y));
  const float background_red = image.GetDepth(
      static_cast<uint32_t>(kBackgroundPixel.x),
      static_cast<uint32_t>(kBackgroundPixel.y));
  INFO("crown_px=(" << crown_px.x << "," << crown_px.y
                    << ") crown_red=" << crown_red
                    << " background_red=" << background_red);
  CHECK(crown_red > background_red + 0.05f);
}

TEST_CASE(
    "voxel Oak crown: back-lit transmission at strength 0.6 is brighter than "
    "0.0 at the same pixel",
    "[tree_field][gpu]") {
  TestGpu& g = GetTestGpu();
  const TexturedMeshResult probe = MakeOakCrown();
  const Camera camera = FramingCamera(probe.local_bounds);

  // Sun directly behind the crown along the camera's own axis (pure -Z,
  // opposite the camera's +Z position -- NOT tilted toward +Y) so EVERY
  // camera-visible tet is back-lit, not just some: FramingCamera looks down
  // -Z, so Cull::Back visibility requires a tet's normal to have a positive
  // Z component, and dot(N, (0,0,-1)) = -N.z is then guaranteed negative,
  // i.e. backLit = max(-dot(N,L), 0) = N.z > 0 for every visible tet. A sun
  // with any +Y component (tried first) let upward-tilted-but-still-visible
  // tets pick up real front reflectance from that component and out-shine
  // the transmission signal entirely (ArgmaxPixel would then land on a
  // reflectance highlight where the strength=0.6/0.0 renders were pixel-
  // identical). Ambient off (isolates the direct back-lit term, mirroring
  // forward_pass_tests.cpp's translucency Test C) so the only per-pixel
  // difference between the two renders below is the transmission term's
  // `strength` factor.
  const glm::vec3 sun_direction = glm::vec3(0.0f, 0.0f, -1.0f);
  const glm::vec3 sun_color(3.0f);
  const glm::vec3 ambient(0.0f);

  const CpuImage on = RenderOakCrownFrame(g, camera, /*translucency_strength=*/0.6f,
                                          sun_direction, sun_color, ambient);
  const CpuImage off = RenderOakCrownFrame(g, camera, /*translucency_strength=*/0.0f,
                                           sun_direction, sun_color, ambient);

  const glm::ivec2 crown_px = ArgmaxAbsDiffPixel(on, off);
  const float on_red = on.GetDepth(static_cast<uint32_t>(crown_px.x),
                                   static_cast<uint32_t>(crown_px.y));
  const float off_red = off.GetDepth(static_cast<uint32_t>(crown_px.x),
                                     static_cast<uint32_t>(crown_px.y));

  INFO("crown_px=(" << crown_px.x << "," << crown_px.y << ") on_red=" << on_red
                    << " off_red=" << off_red);
  CHECK(on_red > 0.05f);  // sanity: a real transmission signal, not near-zero noise
  CHECK(on_red > off_red + 0.05f);
}

// ===========================================================================
// Phase 5 of the volumetric-foliage feature: TreeField/BuildTreeField --
// the field's leaf submesh is now a per-LOD voxelized crown (not a leaf-card
// quad), BOTH the bark and leaf submeshes are shadow-casting
// (InstancedMeshField::SetSubmeshShadow), and the leaf submesh renders
// kDeferred end-to-end (no kForwardOpaque leaf-card path anymore). These
// tests build a REAL OakPreset TreeField -- mirroring
// ModelViewerView::RebuildScene's Multi-mode pattern exactly (voxelize 3
// LODs once, at kFieldVoxelWorldSizes converted to native units via the same
// kFieldTreeHeight rescale `s`) -- and render it through the FULL
// SceneRenderer pipeline via SceneContext::instanced_fields (Cull/
// CullShadow/Draw dispatched automatically by SceneRenderer::Render -- see
// scene_renderer.cpp's ForEachInstancedField call sites), exactly as a real
// app would.
// ===========================================================================
namespace {

// Mirrors model_viewer_view.cpp's kTreePreviewHeight -- the height one field
// instance is rescaled to, so the camera distances / LOD thresholds below
// (picked in these display-space units) are meaningful.
constexpr float kFieldTreeHeight = 8.0f;
// Mirrors kFoliageVoxelWorldSizes (preview-space cell sizes per LOD; Phase 6
// retuned L1 from 0.30 to 0.20 -- see that constant's comment in
// model_viewer_view.cpp for why).
constexpr std::array<float, GpuInstanceRenderer::kMaxLods> kFieldVoxelWorldSizes = {
    0.15f, 0.20f, 0.60f};
// Straddled by the ~10/25/45m camera distances the LOD-selection test below
// uses: 10 < 15 -> LOD0, 15 <= 25 < 35 -> LOD1, 45 >= 35 -> LOD2.
constexpr std::array<float, 2> kFieldLodThresholds = {15.0f, 35.0f};

// A TreeField for OakPreset built the same way ModelViewerView's Multi mode
// builds one (RebuildScene's `multi` branch): voxelize 3 LODs once (Oak has
// no known-empty-LOD gap -- unlike some pine presets, see
// leaf_voxelizer.hpp -- so every LOD is asserted non-empty here), pass the
// span to BuildTreeField. `s` (native -> kFieldTreeHeight) is kept around for
// callers to place an instance / compute its world-space bounds the same way
// RebuildScene does.
struct OakField {
  std::unique_ptr<TreeField> tf;
  float s = 1.0f;
};

OakField BuildOakField(TestGpu& g, uint32_t capacity,
                       std::array<float, 2> lod_thresholds) {
  const TreeOptions oak = OakPreset();
  const std::vector<SkeletonBranch> skeleton = BuildTreeSkeleton(oak);
  const TexturedMeshResult bark = GenerateTreeMesh(oak, skeleton);
  const float h = bark.local_bounds.max.y - bark.local_bounds.min.y;
  const float s = kFieldTreeHeight / std::max(h, 0.001f);

  const TexturedMeshResult leaves = GenerateLeafMesh(oak, skeleton);
  std::array<TexturedMeshResult, GpuInstanceRenderer::kMaxLods> leaf_lod_meshes;
  for (uint32_t lod = 0; lod < GpuInstanceRenderer::kMaxLods; ++lod) {
    LeafVoxelizeOptions opts;
    opts.cell_size = kFieldVoxelWorldSizes[lod] / s;
    leaf_lod_meshes[lod] =
        VoxelizeLeafCards(leaves.mesh, oak.leaves.silhouette, opts);
    REQUIRE(leaf_lod_meshes[lod].mesh.vertex_count > 0u);
  }

  OakField result;
  result.s = s;
  result.tf = BuildTreeField(g.device, g.queue, *g.gen, oak, leaf_lod_meshes,
                             capacity, lod_thresholds);
  REQUIRE(result.tf != nullptr);
  return result;
}

// The rest-on-floor + kFieldTreeHeight-rescale transform for one instance at
// the world origin -- same derivation as ModelViewerView::RebuildScene's
// Multi-mode `xf`.
glm::mat4 OakInstanceTransform(const OakField& of) {
  return glm::translate(glm::mat4(1.0f),
                        glm::vec3(0.0f, -of.tf->bark_local_bounds.min.y * of.s,
                                  0.0f)) *
        glm::scale(glm::mat4(1.0f), glm::vec3(of.s));
}

// This instance's world-space bounds (bark UNION leaf when the tree has any,
// transformed by `xf`) -- for framing a camera / picking an off-canopy pixel.
Aabb OakInstanceWorldBounds(const OakField& of, const glm::mat4& xf) {
  Aabb local = of.tf->bark_local_bounds;
  if (of.tf->has_leaves) {
    local = local.Union(of.tf->leaf_local_bounds);
  }
  return local.TransformedBy(xf);
}

// Uploads ONE instance (at the world origin, OakInstanceTransform(of)) into
// `of.tf->field`, with a bounding sphere derived from `world_bounds`.
void UploadSingleOakInstance(const OakField& of, const glm::mat4& xf,
                             const Aabb& world_bounds) {
  const glm::vec3 center = world_bounds.Center();
  const float radius = std::max(glm::length(world_bounds.max - center), 0.1f);
  GpuInstanceRenderer::InstanceInput instance{};
  instance.transform = xf;
  instance.bounds_sphere = glm::vec4(center, radius);
  instance.model_info = glm::uvec4(0u);
  of.tf->field->UploadInstances(
      std::span<const GpuInstanceRenderer::InstanceInput>(&instance, 1));
}

// Renders one SceneRenderer frame with `field` registered via
// SceneContext::instanced_fields (Cull/CullShadow/Draw dispatched
// automatically -- see this section's file comment) plus whatever
// `registry`/`scene_context` already hold (an empty registry with manually
// set lighting, or a real SceneGraph's SyncToRegistry output -- see the
// call sites below), under a Dawn validation-error scope. Mirrors this
// file's own RenderOakCrownFrame / gpu_instance_tests.cpp's
// RenderSceneWithFields.
CpuImage RenderFieldFrame(TestGpu& g, InstancedMeshField* field,
                          entt::registry& registry, SceneContext& scene_context,
                          const Camera& camera,
                          ShadowDebugMode shadow_debug_mode) {
  std::array<InstancedMeshField*, 1> fields = {field};
  scene_context.registry = &registry;
  scene_context.instanced_fields = fields.data();
  scene_context.instanced_field_count = 1;

  ColorRenderTarget rt(g.device, kSceneSize, kSceneSize,
                       wgpu::TextureFormat::R32Float);
  REQUIRE(rt.IsValid());

  SceneRenderer renderer;
  renderer.Initialize(g.device, g.queue, g.gen.get(),
                      wgpu::TextureFormat::R32Float, kSceneSize, kSceneSize,
                      g.device.HasFeature(wgpu::FeatureName::TextureFormatsTier1));
  renderer.MutableFogConfig().enabled = false;  // would haze the readback
  renderer.SetShadowDebugMode(shadow_debug_mode);

  CapturedError err = RunCapturingValidationErrors(g, [&] {
    renderer.Render(camera, registry, scene_context, rt.GetView());
  });
  INFO("Dawn validation error: " << err.message);
  CHECK(err.type == wgpu::ErrorType::NoError);
  test::WaitForGpu(g.instance, g.device, g.queue);

  TextureReadback readback(g.instance, g.device, g.queue);
  return readback.ReadTextureSync(rt.GetTexture(), kSceneSize, kSceneSize,
                                  wgpu::TextureFormat::R32Float);
}

// World -> pixel through camera's own matrices (mirrors this file's
// ArgmaxPixel-adjacent helpers / gpu_instance_tests.cpp's WorldToPixel),
// used by the field-shadow test to pick "under the tree" vs "outside the
// canopy" pixels.
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

TEST_CASE("TreeField (voxel leaves): HasPass(kDeferred) && HasPass(kShadow), "
          "a deferred field frame renders without validation errors",
          "[tree_field][gpu]") {
  TestGpu& g = GetTestGpu();
  OakField of = BuildOakField(g, /*capacity=*/1, kFieldLodThresholds);
  REQUIRE(of.tf->field->IsValid());
  CHECK(of.tf->field->HasPass(InstancedMeshField::PassKind::kDeferred));
  CHECK(of.tf->field->HasPass(InstancedMeshField::PassKind::kShadow));

  const glm::mat4 xf = OakInstanceTransform(of);
  const Aabb world_bounds = OakInstanceWorldBounds(of, xf);
  UploadSingleOakInstance(of, xf, world_bounds);

  const glm::vec3 center = world_bounds.Center();
  const float radius = std::max(glm::length(world_bounds.max - center), 0.1f);
  Camera camera;
  camera.position = center + glm::vec3(0.0f, 0.0f, radius * 3.0f + 2.0f);
  camera.direction = glm::vec3(0.0f, 0.0f, -1.0f);
  camera.up = glm::vec3(0.0f, 1.0f, 0.0f);
  camera.fov = 55.0f;
  camera.aspect = 1.0f;
  camera.near_plane = 0.05f;
  camera.far_plane = radius * 8.0f + 20.0f;

  entt::registry registry;  // no entities -- the field is the only geometry
  SceneContext scene_context;
  scene_context.sun_direction = glm::normalize(glm::vec3(0.3f, 0.6f, 0.7f));
  scene_context.sun_color = glm::vec3(2.0f);
  scene_context.ambient_sh[0] = glm::vec3(0.4f);
  scene_context.clear_color = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);

  const CpuImage image = RenderFieldFrame(g, of.tf->field.get(), registry,
                                          scene_context, camera,
                                          ShadowDebugMode::Off);
  const glm::ivec2 crown_px = ArgmaxPixel(image);
  const float crown_red = image.GetDepth(static_cast<uint32_t>(crown_px.x),
                                         static_cast<uint32_t>(crown_px.y));
  INFO("crown_px=(" << crown_px.x << "," << crown_px.y
                    << ") crown_red=" << crown_red);
  CHECK(crown_red > 0.05f);  // sanity: real geometry rendered, not a blank frame
}

TEST_CASE("TreeField field shadow: floor under the tree is shadowed, a "
          "distant floor pixel is not",
          "[tree_field][gpu]") {
  TestGpu& g = GetTestGpu();
  OakField of = BuildOakField(g, /*capacity=*/1, kFieldLodThresholds);
  REQUIRE(of.tf->field->HasPass(InstancedMeshField::PassKind::kShadow));

  const glm::mat4 xf = OakInstanceTransform(of);
  const Aabb world_bounds = OakInstanceWorldBounds(of, xf);
  UploadSingleOakInstance(of, xf, world_bounds);

  SceneGraph graph;
  // Straight overhead sun: the shadow lands with zero horizontal offset
  // directly beneath the tree (same trick as gpu_instance_tests.cpp's
  // analogous field-shadow scene test), so "under the tree" and "outside its
  // canopy" are trivial to pick without re-deriving an oblique light
  // projection.
  graph.SetSunDirection(glm::vec3(0.0f, 1.0f, 0.0f));
  graph.SetSunColor(glm::vec3(1.0f));
  graph.SetClearColor(glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
  AddFloor(graph, 60.0f, g.matlib.SolidColor(glm::vec3(0.6f, 0.6f, 0.6f), 0.6f),
           1.0f);

  entt::registry registry;
  SceneContext scene_context;
  graph.SyncToRegistry(registry, scene_context);

  Camera camera;
  camera.position = glm::vec3(0.0f, 16.0f, 16.0f);
  camera.LookAt(glm::vec3(0.0f));
  camera.up = glm::vec3(0.0f, 1.0f, 0.0f);
  camera.fov = 50.0f;
  camera.aspect = 1.0f;
  camera.near_plane = 0.1f;
  camera.far_plane = 1000.0f;

  const CpuImage image = RenderFieldFrame(g, of.tf->field.get(), registry,
                                          scene_context, camera,
                                          ShadowDebugMode::ShadowMapOnly);

  // "Under the tree": a real voxel-tet crown is porous (gaps between
  // discrete tets -- see leaf_voxelizer.hpp), so a single fixed point (e.g.
  // the trunk's own base) is not guaranteed to land under enough canopy mass
  // to read fully shadowed (empirically ~0.4, not <0.3, straight under the
  // trunk). Instead, project the crown's own world-space XZ footprint (all 4
  // corners of world_bounds at y=0) to screen pixels and scan that rectangle
  // for its DARKEST pixel -- the meaningful "is this tree casting a real
  // shadow" signal. "Outside the canopy" = a point well past the footprint's
  // half-extent, offset along +X -- PERPENDICULAR to the camera's view axis
  // (camera sits at (0,16,16) looking at the origin, i.e. its view axis has
  // no X component). Offsetting along -Z instead (tried first) put the point
  // roughly BEHIND the tree from the camera's viewpoint (camera, tree, and a
  // -Z offset point are near-collinear from this angle), so the occluding
  // camera ray grazed the crown itself instead of reaching the open floor --
  // the X axis has no such occlusion risk.
  glm::ivec2 rect_lo(std::numeric_limits<int>::max(),
                     std::numeric_limits<int>::max());
  glm::ivec2 rect_hi(std::numeric_limits<int>::min(),
                     std::numeric_limits<int>::min());
  for (const float cx : {world_bounds.min.x, world_bounds.max.x}) {
    for (const float cz : {world_bounds.min.z, world_bounds.max.z}) {
      const glm::ivec2 px =
          WorldToPixel(camera, glm::vec3(cx, 0.0f, cz), kSceneSize, kSceneSize);
      rect_lo = glm::min(rect_lo, px);
      rect_hi = glm::max(rect_hi, px);
    }
  }
  rect_lo = glm::max(rect_lo, glm::ivec2(0));
  rect_hi = glm::min(rect_hi, glm::ivec2(static_cast<int>(kSceneSize) - 1));
  REQUIRE(rect_lo.x <= rect_hi.x);
  REQUIRE(rect_lo.y <= rect_hi.y);

  const float footprint_half_extent =
      std::max(world_bounds.Extents().x, world_bounds.Extents().z) * 0.5f;
  const glm::vec3 offset_world(footprint_half_extent + 3.0f, 0.0f, 0.0f);
  const glm::ivec2 offset =
      WorldToPixel(camera, offset_world, kSceneSize, kSceneSize);
  REQUIRE(offset.x >= 0);
  REQUIRE(offset.x < static_cast<int>(kSceneSize));
  REQUIRE(offset.y >= 0);
  REQUIRE(offset.y < static_cast<int>(kSceneSize));

  float shadowed = 1e30f;
  for (int y = rect_lo.y; y <= rect_hi.y; ++y) {
    for (int x = rect_lo.x; x <= rect_hi.x; ++x) {
      shadowed = std::min(
          shadowed, image.GetDepth(static_cast<uint32_t>(x), static_cast<uint32_t>(y)));
    }
  }
  const float lit = image.GetDepth(static_cast<uint32_t>(offset.x),
                                   static_cast<uint32_t>(offset.y));
  INFO("darkest-under-canopy shadow value = " << shadowed
                                              << ", offset (lit) = " << lit);
  CHECK(shadowed < 0.3f);
  CHECK(lit > 0.7f);
}

// Renders at 3 camera distances straddling kFieldLodThresholds (10 < 15 ->
// LOD0, 15 <= 25 < 35 -> LOD1, 45 >= 35 -> LOD2 -- see that constant's
// comment), so each render exercises a DIFFERENT GPU-selected LOD bucket's
// (bark, leaf) submesh pair end-to-end. This does not itself assert WHICH
// bucket the GPU cull/classify pass actually picked (that mechanism -- the
// engine's distance -> bucket routing -- is already covered by
// src/engine/tests/gpu_instance_tests.cpp's own dedicated LOD-selection
// test, which reads back GetBucketCountBuffer); it only asserts that
// whichever LOD ends up selected at each distance renders real crown
// geometry, not a blank/broken frame -- the risk Phase 5's per-LOD leaf
// mesh + shadow-material plumbing (including the possibility of an
// unconfigured/empty leaf slot at some LOD) actually introduces.
TEST_CASE("TreeField deferred frames at near/mid/far camera distances "
          "(straddling GPU LOD thresholds): crown pixels present at each",
          "[tree_field][gpu]") {
  TestGpu& g = GetTestGpu();
  OakField of = BuildOakField(g, /*capacity=*/1, kFieldLodThresholds);

  const glm::mat4 xf = OakInstanceTransform(of);
  const Aabb world_bounds = OakInstanceWorldBounds(of, xf);
  UploadSingleOakInstance(of, xf, world_bounds);
  const glm::vec3 center = world_bounds.Center();

  entt::registry registry;  // no entities -- the field is the only geometry
  for (const float distance : {10.0f, 25.0f, 45.0f}) {
    SceneContext scene_context;
    scene_context.sun_direction = glm::normalize(glm::vec3(0.3f, 0.6f, 0.7f));
    scene_context.sun_color = glm::vec3(2.0f);
    scene_context.ambient_sh[0] = glm::vec3(0.4f);
    scene_context.clear_color = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);

    Camera camera;
    camera.position = center + glm::vec3(0.0f, 0.0f, distance);
    camera.direction = glm::vec3(0.0f, 0.0f, -1.0f);
    camera.up = glm::vec3(0.0f, 1.0f, 0.0f);
    camera.fov = 55.0f;
    camera.aspect = 1.0f;
    camera.near_plane = 0.05f;
    camera.far_plane = distance + 50.0f;

    const CpuImage image = RenderFieldFrame(g, of.tf->field.get(), registry,
                                            scene_context, camera,
                                            ShadowDebugMode::Off);
    const glm::ivec2 crown_px = ArgmaxPixel(image);
    const float crown_red = image.GetDepth(static_cast<uint32_t>(crown_px.x),
                                           static_cast<uint32_t>(crown_px.y));
    INFO("distance=" << distance << " crown_px=(" << crown_px.x << ","
                     << crown_px.y << ") crown_red=" << crown_red);
    CHECK(crown_red > 0.05f);
  }
}
