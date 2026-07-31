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

#include <algorithm>
#include <cstdint>
#include <memory>

#include "core/util/cpu_image.hpp"
#include "engine/core/camera.hpp"
#include "engine/rendering/color_render_target.hpp"
#include "engine/rendering/context/scene_context.hpp"
#include "engine/rendering/geometry/aabb.hpp"
#include "engine/rendering/geometry/textured_mesh_builders.hpp"
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

  renderer.Render(camera, registry, scene_context, rt.GetView());
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
