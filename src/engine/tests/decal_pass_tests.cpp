// GPU tests for the projected-decal pass (rendering/passes/render_projected_decals
// + shaders/passes/decals.wesl).
//
// Renders real frames through SceneRenderer into an offscreen R32Float target
// (the shadow-test harness's approach: TextureReadback does not support
// RGBA16Float, and R32Float is treated as linear by the tonemap pass, so the
// HDR red channel arrives at the CPU unchanged). Known world points are
// projected to pixels with the engine's own camera matrices, so each assertion
// names an exact surface point rather than a hand-tuned pixel.
//
// What this pins that the pure-CPU decal_math tests cannot:
//   * that shaders/passes/decals.wesl agrees with decal_math.hpp (the dash
//     cross-check below is the transcription oracle),
//   * outline-only rendering (no fill), the yaw convention, the vertical
//     projector band, and steep-surface rejection,
//   * that the pass compiles and composites at all.

#include <cmath>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include <catch_amalgamated.hpp>

#include <dawn/webgpu_cpp.h>
#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <spdlog/spdlog.h>

#include "core/util/cpu_image.hpp"
#include "engine/core/camera.hpp"
#include "engine/rendering/color_render_target.hpp"
#include "engine/rendering/context/scene_context.hpp"
#include "engine/rendering/decal_math.hpp"
#include "engine/rendering/geometry/primitive_mesh_builders.hpp"
#include "engine/rendering/material_library.hpp"
#include "engine/rendering/projected_decal.hpp"
#include "engine/rendering/scene_build.hpp"
#include "engine/rendering/scene_renderer.hpp"
#include "engine/rendering/shader/gpu_pipeline_generator.hpp"
#include "engine/rendering/texture_loader.hpp"
#include "engine/rendering/texture_readback.hpp"
#include "engine/rendering/util/find_shader_directory.hpp"
#include "engine/scene/scene_graph.hpp"
#include "gpu_test_helpers.hpp"

using namespace badlands;

namespace {

constexpr uint32_t kWidth = 640;
constexpr uint32_t kHeight = 640;

// Decal colours chosen to be unmistakable in the readback's red channel:
// dash A is HDR-bright (nothing else in the scene reaches it), dash B is a
// mid value, and the scene's floor is black albedo so it sits near zero.
constexpr float kDashARed = 8.0f;
constexpr float kDashBRed = 1.0f;

glm::vec4 DashAColor() { return glm::vec4(kDashARed, 0.0f, 0.0f, 1.0f); }
glm::vec4 DashBColor() { return glm::vec4(kDashBRed, 0.0f, 0.0f, 1.0f); }

// Process-lifetime headless GPU context (same rationale + leak as the shadow
// harness: a short-lived test binary, and Dawn's ref-counted handles do not
// enjoy static destruction order).
struct TestGpu {
  wgpu::Instance instance;
  wgpu::Device device;
  wgpu::Queue queue;
  std::unique_ptr<GpuPipelineGenerator> pipeline_gen;
  MaterialLibrary matlib;
};

TestGpu& GetTestGpu() {
  static TestGpu* instance = [] {
    auto* g = new TestGpu();
    wgpu::InstanceDescriptor instance_desc = {};
    g->instance = wgpu::CreateInstance(&instance_desc);
    if (!g->instance) {
      spdlog::error("decal_pass_tests: wgpu::CreateInstance failed");
      std::abort();
    }
    wgpu::Adapter adapter = badlands::test::RequestAdapter(g->instance);
    if (!adapter) {
      spdlog::error("decal_pass_tests: RequestAdapter failed");
      std::abort();
    }
    g->device = badlands::test::RequestDevice(adapter);
    if (!g->device) {
      spdlog::error("decal_pass_tests: RequestDevice failed");
      std::abort();
    }
    g->queue = g->device.GetQueue();
    g->pipeline_gen =
        std::make_unique<GpuPipelineGenerator>(g->device, FindShaderDirectory());
    if (!g->matlib.Initialize(g->device, g->queue, g->pipeline_gen.get())) {
      spdlog::error("decal_pass_tests: MaterialLibrary::Initialize failed");
      std::abort();
    }
    return g;
  }();
  return *instance;
}

// The camera every test renders with: high and tilted, so both the ground and
// vertical faces are visible.
Camera MakeCamera() {
  Camera camera;
  camera.position = glm::vec3(0.0f, 14.0f, 14.0f);
  camera.LookAt(glm::vec3(0.0f));
  camera.up = glm::vec3(0.0f, 1.0f, 0.0f);
  camera.fov = 45.0f;
  camera.aspect = static_cast<float>(kWidth) / static_cast<float>(kHeight);
  camera.near_plane = 0.1f;
  camera.far_plane = 1000.0f;
  return camera;
}

// World -> pixel, through the engine's own matrices (the same product the
// frustum culler uses), so nothing about the projection is re-derived here.
glm::ivec2 ProjectToPixel(const Camera& camera, glm::vec3 world) {
  const glm::vec4 clip =
      camera.GetProj() * camera.GetView() * glm::vec4(world, 1.0f);
  REQUIRE(clip.w > 0.0f);  // in front of the camera
  const glm::vec3 ndc = glm::vec3(clip) / clip.w;
  return glm::ivec2(
      static_cast<int>((ndc.x * 0.5f + 0.5f) * static_cast<float>(kWidth)),
      static_cast<int>((1.0f - (ndc.y * 0.5f + 0.5f)) * static_cast<float>(kHeight)));
}

// A wall/box entity: GenerateCube is centred on the origin, so `center` is the
// box centre in world space.
void AddBox(SceneGraph& graph, const char* name, MaterialLibrary& matlib,
            glm::vec3 center, glm::vec3 half_extents) {
  AddMeshEntity(graph, name, GenerateCube(half_extents),
                matlib.SolidColor(glm::vec3(0.0f), 1.0f),
                glm::translate(glm::mat4(1.0f), center));
}

// What the scene contains besides the floor.
enum class SceneKind { FloorOnly, FloorAndWall };

// Renders one frame with `decals` applied and returns the readback.
CpuImage RenderDecalFrame(const std::vector<ProjectedDecal>& decals,
                          const Camera& camera,
                          SceneKind kind = SceneKind::FloorOnly) {
  TestGpu& gpu = GetTestGpu();

  SceneGraph graph;
  // Straight-down sun and a black-albedo floor: the scene renders near zero, so
  // any bright red in the readback is unambiguously a decal.
  graph.SetSunDirection(glm::normalize(glm::vec3(0.2f, 1.0f, 0.3f)));
  graph.SetSunColor(glm::vec3(1.0f));
  graph.SetClearColor(glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));

  AddFloor(graph, 80.0f, gpu.matlib.SolidColor(glm::vec3(0.0f), 1.0f), 1.0f);

  if (kind == SceneKind::FloorAndWall) {
    // A wall straddling the r = 5 ring: its TOP (y = 3) is above the decal
    // plane (band test) and its CAMERA-FACING +Z face is vertical
    // (receiver-normal test). Centred at z = 4.0 so the visible face (z = 4.2)
    // is INSIDE the ring radius -- at z = 5.0 the front face would sit at
    // z = 5.2, entirely outside the r = 5 circle, and the ring would never
    // reach any surface a camera at +Z can actually see.
    AddBox(graph, "wall", gpu.matlib, glm::vec3(0.0f, 1.5f, 4.0f),
           glm::vec3(4.0f, 1.5f, 0.2f));
  }

  entt::registry registry;
  SceneContext scene_context;
  scene_context.registry = &registry;
  graph.SyncToRegistry(registry, scene_context);
  scene_context.decals = decals.data();
  scene_context.decal_count = static_cast<uint32_t>(decals.size());
  scene_context.time_seconds = 0.0f;
  // The decal pass scrolls off the wall-clock (UI) clock, not the sim-scaled
  // presentation one -- this is the field the dash phase actually reads.
  scene_context.real_time_seconds = 0.0f;

  ColorRenderTarget rt(gpu.device, kWidth, kHeight, wgpu::TextureFormat::R32Float);

  SceneRenderer renderer;
  renderer.Initialize(gpu.device, gpu.queue, gpu.pipeline_gen.get(),
                      wgpu::TextureFormat::R32Float, kWidth, kHeight,
                      gpu.device.HasFeature(wgpu::FeatureName::TextureFormatsTier1));
  renderer.MutableFogConfig().enabled = false;  // would haze the readback

  renderer.Render(camera, registry, scene_context, rt.GetView());
  badlands::test::WaitForGpu(gpu.instance, gpu.device, gpu.queue);

  TextureReadback readback(gpu.instance, gpu.device, gpu.queue);
  return readback.ReadTextureSync(rt.GetTexture(), kWidth, kHeight,
                                  wgpu::TextureFormat::R32Float);
}

// Red channel at the pixel a world point projects to.
//
// GetDepth -- NOT GetPixelF32 -- is the raw-float accessor for R32Float:
// GetPixelF32 routes through GetPixel, which renders float formats as an 8-bit
// grayscale VISUALISATION, saturating everything above 1.0 to 1.0. That would
// silently flatten the HDR dash colour to the same value as an SDR one.
float RedAt(const CpuImage& image, const Camera& camera, glm::vec3 world) {
  const glm::ivec2 p = ProjectToPixel(camera, world);
  REQUIRE(p.x >= 0);
  REQUIRE(p.y >= 0);
  REQUIRE(p.x < static_cast<int>(kWidth));
  REQUIRE(p.y < static_cast<int>(kHeight));
  return image.GetDepth(static_cast<uint32_t>(p.x), static_cast<uint32_t>(p.y));
}

bool IsDashA(float red) { return red > 0.5f * kDashARed; }
bool IsDashB(float red) {
  return std::abs(red - kDashBRed) < 0.25f * kDashBRed;
}

ProjectedDecal MakeRing(glm::vec3 center, float radius) {
  ProjectedDecal d;
  d.shape = DecalShape::Ring;
  d.center = center;
  d.half_extents = glm::vec2(radius);
  d.line_width = 0.5f;
  d.projector_half_height = 1.0f;
  d.color_a = DashAColor();
  d.color_b = DashBColor();
  // Solid by default; individual tests opt into dashes.
  d.dash_length = 1.0f;
  d.dash_gap = 0.0f;
  d.scroll_speed = 0.0f;
  return d;
}

// A point on the ring's outline at angle `theta` (atan2(z, x)).
glm::vec3 RingPoint(glm::vec3 center, float radius, float theta) {
  return glm::vec3(center.x + std::cos(theta) * radius, center.y,
                   center.z + std::sin(theta) * radius);
}

}  // namespace

TEST_CASE("Decal pass: the scene without decals is dark (baseline)") {
  const Camera camera = MakeCamera();
  const CpuImage image = RenderDecalFrame({}, camera);

  // The black-albedo floor must sit well below the dash colours, otherwise
  // every other assertion in this file is meaningless.
  const float floor_red = RedAt(image, camera, glm::vec3(-5.0f, 0.0f, 0.0f));
  INFO("floor baseline red = " << floor_red);
  REQUIRE(floor_red < 0.5f * kDashBRed);
}

TEST_CASE("Decal pass: a ring draws its outline and nothing else") {
  const Camera camera = MakeCamera();
  const float radius = 5.0f;
  const std::vector<ProjectedDecal> decals = {MakeRing(glm::vec3(0.0f), radius)};
  const CpuImage image = RenderDecalFrame(decals, camera);

  // On the outline, all the way around (skipping angles near +Z, which the
  // wall-free scene does not occlude but the next test's wall would).
  for (int i = 0; i < 8; ++i) {
    const float theta = decal_math::kTwoPi * static_cast<float>(i) / 8.0f;
    const glm::vec3 p = RingPoint(glm::vec3(0.0f), radius, theta);
    const float red = RedAt(image, camera, p);
    INFO("theta = " << theta << " world = (" << p.x << ", " << p.z
                    << ") red = " << red);
    CHECK(IsDashA(red));
  }

  // Outline ONLY: the interior is not filled, and just outside is clear.
  for (int i = 0; i < 8; ++i) {
    const float theta = decal_math::kTwoPi * static_cast<float>(i) / 8.0f;
    INFO("theta = " << theta);
    CHECK_FALSE(IsDashA(RedAt(image, camera,
                              RingPoint(glm::vec3(0.0f), radius * 0.5f, theta))));
    CHECK_FALSE(IsDashA(RedAt(image, camera,
                              RingPoint(glm::vec3(0.0f), radius * 1.5f, theta))));
  }
  // Dead centre, too.
  CHECK_FALSE(IsDashA(RedAt(image, camera, glm::vec3(0.0f))));
}

TEST_CASE("Decal pass: a rounded rect draws its outline, and yaw rotates it") {
  const Camera camera = MakeCamera();

  ProjectedDecal rect;
  rect.shape = DecalShape::RoundedRect;
  rect.center = glm::vec3(0.0f);
  rect.half_extents = glm::vec2(6.0f, 3.0f);
  rect.corner_radius = 0.8f;
  rect.line_width = 0.5f;
  rect.projector_half_height = 1.0f;
  rect.color_a = DashAColor();
  rect.color_b = DashBColor();
  rect.dash_length = 1.0f;
  rect.dash_gap = 0.0f;

  SECTION("unrotated") {
    const CpuImage image = RenderDecalFrame({rect}, camera);
    // Edge midpoints lie on the outline.
    CHECK(IsDashA(RedAt(image, camera, glm::vec3(6.0f, 0.0f, 0.0f))));
    CHECK(IsDashA(RedAt(image, camera, glm::vec3(-6.0f, 0.0f, 0.0f))));
    CHECK(IsDashA(RedAt(image, camera, glm::vec3(0.0f, 0.0f, 3.0f))));
    CHECK(IsDashA(RedAt(image, camera, glm::vec3(0.0f, 0.0f, -3.0f))));
    // Interior is empty.
    CHECK_FALSE(IsDashA(RedAt(image, camera, glm::vec3(0.0f))));
    CHECK_FALSE(IsDashA(RedAt(image, camera, glm::vec3(3.0f, 0.0f, 1.0f))));
    // And the long axis really is along X: (0,0,5) is outside the shape.
    CHECK_FALSE(IsDashA(RedAt(image, camera, glm::vec3(0.0f, 0.0f, 5.0f))));
  }

  SECTION("yawed 90 degrees swaps the axes") {
    // Rotation about +Y by yaw maps local (x,z) to world
    // (x*cos + z*sin, -x*sin + z*cos), so at yaw = pi/2 the local +X extent
    // (6) points along world -Z.
    rect.yaw = decal_math::kHalfPi;
    const CpuImage image = RenderDecalFrame({rect}, camera);
    CHECK(IsDashA(RedAt(image, camera, glm::vec3(0.0f, 0.0f, -6.0f))));
    CHECK(IsDashA(RedAt(image, camera, glm::vec3(3.0f, 0.0f, 0.0f))));
    CHECK(IsDashA(RedAt(image, camera, glm::vec3(-3.0f, 0.0f, 0.0f))));
    // The unrotated outline points are now interior/exterior instead.
    CHECK_FALSE(IsDashA(RedAt(image, camera, glm::vec3(6.0f, 0.0f, 0.0f))));
    CHECK_FALSE(IsDashA(RedAt(image, camera, glm::vec3(0.0f, 0.0f, 3.0f))));
  }
}

namespace {

// A point on the wall's CAMERA-FACING vertical face (z = 4.2) that is exactly
// on the r = 5 ring in XZ: sqrt(2.7129319^2 + 4.2^2) == 5.
//
// Two traps this avoids, both of which would let a rejection test pass for the
// wrong reason (the sample landing on some other surface entirely):
//   * the face must FACE THE CAMERA -- the box's far face is back-facing and
//     occluded by the box itself;
//   * the height must be mid-face -- near the base the face is a foreshortened
//     sliver under this camera and a one-pixel sample can hit the floor.
constexpr glm::vec3 kWallFacePoint{2.7129319f, 1.5f, 4.2f};
// A point on the wall's TOP face that is also exactly on the ring
// (sqrt(3^2 + 4^2) == 5), above the band below.
constexpr glm::vec3 kWallTopPoint{3.0f, 3.0f, 4.0f};
// The band must reach the face sample at y = 1.5 while still excluding the wall
// top at y = 3, so the two rejections stay independently testable.
constexpr float kTallBand = 2.0f;

}  // namespace

TEST_CASE("Decal pass: the projector band and steep surfaces reject the decal") {
  const Camera camera = MakeCamera();
  const float radius = 5.0f;
  ProjectedDecal ring = MakeRing(glm::vec3(0.0f), radius);
  ring.projector_half_height = kTallBand;
  const CpuImage image =
      RenderDecalFrame({ring}, camera, SceneKind::FloorAndWall);

  // Sanity: the ring still lands on open floor away from the wall.
  CHECK(IsDashA(RedAt(image, camera, glm::vec3(-radius, 0.0f, 0.0f))));
  CHECK(IsDashA(RedAt(image, camera, glm::vec3(0.0f, 0.0f, -radius))));

  // BAND: the wall's top face is at y = 3, outside +/- 2.0 of the decal plane,
  // even though it is exactly on the ring in XZ.
  REQUIRE(std::abs(std::sqrt(kWallTopPoint.x * kWallTopPoint.x +
                             kWallTopPoint.z * kWallTopPoint.z) -
                   radius) < 1e-4f);
  const float top_red = RedAt(image, camera, kWallTopPoint);
  INFO("wall top red = " << top_red);
  CHECK_FALSE(IsDashA(top_red));

  // STEEP: the wall's -Z face is vertical and sits INSIDE the band, and is
  // exactly on the ring -- so only the receiver-normal test can reject it.
  REQUIRE(std::abs(std::sqrt(kWallFacePoint.x * kWallFacePoint.x +
                             kWallFacePoint.z * kWallFacePoint.z) -
                   radius) < 1e-4f);
  REQUIRE(std::abs(kWallFacePoint.y) < kTallBand);
  const float face_red = RedAt(image, camera, kWallFacePoint);
  INFO("wall face red = " << face_red);
  CHECK_FALSE(IsDashA(face_red));
}

TEST_CASE("Decal pass: the receiver-normal band is per decal, not hardcoded") {
  // The same wall face the previous test proves is rejected by default must be
  // ACCEPTED by a decal that opts into any surface orientation -- otherwise the
  // ground-only rule is baked into the shader and no VFX decal could ever wrap
  // a wall.
  const Camera camera = MakeCamera();
  const float radius = 5.0f;

  ProjectedDecal any_surface = MakeRing(glm::vec3(0.0f), radius);
  any_surface.projector_half_height = kTallBand;
  any_surface.receiver_min_normal_y = -1.0f;  // accept every orientation
  any_surface.receiver_max_normal_y = -1.0f;  // degenerate band -> hard step
  const CpuImage image =
      RenderDecalFrame({any_surface}, camera, SceneKind::FloorAndWall);

  // Still lands on open floor...
  CHECK(IsDashA(RedAt(image, camera, glm::vec3(-radius, 0.0f, 0.0f))));
  // ...and now also on the vertical face.
  const float face_red = RedAt(image, camera, kWallFacePoint);
  INFO("wall face red (any-surface decal) = " << face_red);
  CHECK(IsDashA(face_red));

  // The vertical PROJECTOR BAND is independent of the receiver band, so the
  // wall top (y = 3, outside the band) is still rejected.
  CHECK_FALSE(IsDashA(RedAt(image, camera, kWallTopPoint)));
}

TEST_CASE("Decal pass: dashes match the CPU mirror (shader transcription)") {
  // The transcription oracle: for many points around the ring, the shader's
  // dash choice must agree with decal_math.hpp's, which decal_math_tests pins
  // independently.
  const Camera camera = MakeCamera();
  const float radius = 5.0f;

  ProjectedDecal ring = MakeRing(glm::vec3(0.0f), radius);
  ring.dash_length = 1.0f;
  ring.dash_gap = 1.0f;
  ring.scroll_speed = 0.0f;  // time is 0 anyway; keep the pattern static
  const CpuImage image = RenderDecalFrame({ring}, camera);

  const float perimeter = decal_math::RingPerimeter(radius);
  const float period =
      decal_math::FitDashPeriod(ring.dash_length + ring.dash_gap, perimeter);
  const float duty = decal_math::DashDuty(ring.dash_length, ring.dash_gap);
  INFO("perimeter = " << perimeter << " fitted period = " << period);

  int checked = 0;
  int saw_a = 0;
  int saw_b = 0;
  constexpr int kSamples = 96;
  for (int i = 0; i < kSamples; ++i) {
    const float theta =
        decal_math::kTwoPi * static_cast<float>(i) / static_cast<float>(kSamples);
    // Skip the wedge the (absent) wall would occupy in the other test, and,
    // more importantly, skip samples too close to a dash boundary, where a
    // sub-pixel difference legitimately flips the answer.
    const float s = theta * radius;
    const float phase = decal_math::Fract(s / period);
    const float edge_margin = 0.12f;
    if (phase < edge_margin || std::abs(phase - duty) < edge_margin ||
        phase > 1.0f - edge_margin) {
      continue;
    }

    const glm::vec3 p = RingPoint(glm::vec3(0.0f), radius, theta);
    const float red = RedAt(image, camera, p);
    const bool expect_a =
        decal_math::IsDashA(s, period, duty, /*time=*/0.0f, /*scroll=*/0.0f);

    INFO("theta = " << theta << " s = " << s << " phase = " << phase
                    << " expected " << (expect_a ? "A" : "B") << " red = " << red);
    if (expect_a) {
      CHECK(IsDashA(red));
      ++saw_a;
    } else {
      CHECK(IsDashB(red));
      ++saw_b;
    }
    ++checked;
  }

  // The pattern must actually alternate -- otherwise the checks above pass
  // trivially on a solid line.
  INFO("checked = " << checked << " A = " << saw_a << " B = " << saw_b);
  CHECK(checked > 30);
  CHECK(saw_a > 5);
  CHECK(saw_b > 5);
}

TEST_CASE(
    "MaterialLibrary::TranslucentFoliage builds a transmitting "
    "forward-opaque material") {
  // Reuses this file's TestGpu harness (its matlib is already initialized
  // via MaterialLibrary::Initialize) rather than standing up a second GPU
  // context just for this material builder.
  TestGpu& g = GetTestGpu();

  wgpu::TextureView albedo_view =
      CreateSolidColorTexture(g.device, g.queue, 255, 255, 255, 255);
  wgpu::SamplerDescriptor sampler_desc{};
  sampler_desc.minFilter = wgpu::FilterMode::Linear;
  sampler_desc.magFilter = wgpu::FilterMode::Linear;
  wgpu::Sampler sampler = g.device.CreateSampler(&sampler_desc);

  const glm::vec3 tint(0.2f, 0.6f, 0.1f);
  const glm::vec3 transmission_tint(0.3f, 0.8f, 0.15f);
  const float transmission_strength = 0.6f;

  DeferredMaterial result = g.matlib.TranslucentFoliage(
      albedo_view, sampler, /*cutoff=*/0.5f, tint, transmission_tint,
      transmission_strength);

  REQUIRE(result.factory != nullptr);

  REQUIRE(result.params.uniform_overrides.count("transmission") == 1u);
  REQUIRE(result.params.uniform_overrides.count("tint") == 1u);
  REQUIRE(result.params.uniform_overrides.count("params") == 1u);

  const auto& transmission_value =
      result.params.uniform_overrides.at("transmission");
  REQUIRE(std::holds_alternative<glm::vec4>(transmission_value));
  const glm::vec4 expected_transmission(transmission_tint,
                                        transmission_strength);
  CHECK(std::get<glm::vec4>(transmission_value) == expected_transmission);

  bool found_albedo = false;
  for (const auto& tex : result.params.texture_overrides) {
    if (tex.param_name == "albedo") {
      found_albedo = true;
      CHECK(tex.view.Get() == albedo_view.Get());
    }
  }
  CHECK(found_albedo);
}
