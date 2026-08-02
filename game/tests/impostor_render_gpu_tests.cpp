// The impostor's RENDER path, through a full SceneRenderer frame.
//
// Every impostor bug that reached review lived here and was invisible to the
// bake tests, which read the atlas back and never draw with it: a G-buffer pack
// with swapped arguments, a normal decoded in the wrong basis, a fractional mip
// against a nearest-mip sampler, a params UBO in the wrong bind group, a quad
// projected from the wrong space, and an alpha cutoff that disagreed with the
// one the mips were fitted to. Four review rounds, all runtime-only.
//
// THE SHAPE IS ARTIFICIAL, AND SPEED IS THE LESSER REASON. A real tree's
// impostor has no expected value, so an assertion about it degrades to "looks
// plausible" -- which is exactly what let those through. A SPHERE's impostor is
// analytic from every direction: its silhouette is a circle of known radius,
// its thickness is 2*sqrt(R^2 - r^2), and the world normal at any surface point
// is the normalized offset from its centre. That turns "plausible" into
// "exactly this". The model is also assembled by hand -- no skeleton, no
// voxelization, no leaf texture -- so it costs nothing to build.
//
// The crown sphere is OFFSET from the origin on purpose: a centred sphere is
// invariant under yaw, so an instance-rotation mistake would be invisible.
//
// WHAT IS PROVEN, by reintroducing each bug and watching the suite go red:
//   * wrong-basis normal decode      -> the normal test fails
//   * frag_depth writing the plane   -> the depth test fails (by exactly one
//                                       crown radius, which is the whole
//                                       difference between a card and a volume)
//   * swapped AO/translucency args   -> the material-channel test fails
//   * a quad that never rasterizes   -> the placement test fails, by
//                                       construction (zero covered pixels)
//
// WHAT IS NOT, measured the same way. The silhouette-drift test does NOT catch
// the alpha-cutoff mismatch or the fractional-mip bug; both were reintroduced
// and it passed. The reason is inherent, not a loose band: a coverage fit only
// moves texels whose alpha sits near the threshold, so cutting lower admits a
// ring roughly ONE TEXEL wide. On a shape simple enough to have an analytic
// silhouette that is a few percent of the area -- inside any band that is not
// flaky at 110 m, where the impostor is barely a hundred pixels. A real crown
// is stippled with partial coverage throughout, which is why it showed there.
//
// So the cutoff is defended STRUCTURALLY instead: both call sites read one
// exported kImpostorAlphaCutoff and cannot disagree without editing it. The
// fractional mip has neither a test nor a structural guard, and that is a real
// remaining gap rather than a solved problem.

#include <catch_amalgamated.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <vector>

#include <dawn/webgpu_cpp.h>
#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "core/util/cpu_image.hpp"
#include "engine/core/camera.hpp"
#include "engine/rendering/color_render_target.hpp"
#include "engine/rendering/context/scene_context.hpp"
#include "engine/rendering/geometry/textured_mesh_builders.hpp"
#include "engine/rendering/scene_renderer.hpp"
#include "engine/rendering/shader/gpu_pipeline_generator.hpp"
#include "engine/rendering/texture_readback.hpp"
#include "engine/rendering/util/find_shader_directory.hpp"
#include "game/visual/foliage_voxel_config.hpp"
#include "game/visual/impostor_atlas.hpp"
#include "game/visual/impostor_baker.hpp"
#include "game/visual/octahedral.hpp"
#include "game/visual/tree_field.hpp"
#include "gpu_test_helpers.hpp"

using namespace badlands;

namespace {

constexpr uint32_t kSize = 384;

// The artificial model's geometry, in native units.
constexpr float kTrunkRadius = 0.25f;
constexpr float kCrownRadius = 1.0f;
constexpr glm::vec3 kCrownCenter{0.7f, 0.0f, 0.0f};  // offset: see file header

// The fixture keeps ONE voxel level (the bake takes its crown from
// leaf_lod_meshes[0]) plus the impostor, so every camera distance below the
// impostor's own cutoff silently renders the VOXEL crown instead -- whose AO is
// a constant 1.0 and whose translucency is a constant, which is exactly what a
// "the impostor's channels are constants" failure looks like. This bit once.
//
// The cutoff scales with the model's height: kFoliageImpostorThresholdPreviewM
// (130) * height / kFoliagePreviewHeight (8). A deliberately tiny height puts it
// at ~8 m, below every distance these tests use, while leaving the fixture's
// GEOMETRY -- and so its on-screen size and mip selection -- untouched.
constexpr float kFixtureHeight = 0.5f;
constexpr float kImpostorFromM =
    kFoliageImpostorThresholdPreviewM * kFixtureHeight / kFoliagePreviewHeight;

struct RenderGpu {
  wgpu::Instance instance;
  wgpu::Device device;
  wgpu::Queue queue;
  std::unique_ptr<GpuPipelineGenerator> gen;
};

RenderGpu& GetRenderGpu() {
  static RenderGpu* g = [] {
    auto* r = new RenderGpu();
    wgpu::InstanceDescriptor idesc = {};
    r->instance = wgpu::CreateInstance(&idesc);
    REQUIRE(r->instance);
    wgpu::Adapter adapter = test::RequestAdapter(r->instance);
    REQUIRE(adapter);
    r->device = test::RequestDevice(adapter);
    REQUIRE(r->device);
    r->queue = r->device.GetQueue();
    r->gen = std::make_unique<GpuPipelineGenerator>(r->device,
                                                    FindShaderDirectory());
    return r;
  }();
  return *g;
}

// Forces every vertex's uv.x to 1 so the crown's albedo is a FLAT known colour.
// The bake takes a voxel crown's albedo from `uv.x * tint` (mirroring
// voxel_foliage.wesl), and a generated sphere's spherical uv would otherwise
// make it vary with longitude -- fine for a tree, useless for an assertion.
void FlattenBrightness(TexturedMeshResult& mesh) {
  constexpr size_t kStride = 11;  // pos3 + uv2 + normal3 + tangent3
  for (size_t v = 0; v + kStride <= mesh.mesh.vertices.size(); v += kStride) {
    mesh.mesh.vertices[v + 3] = 1.0f;  // uv.x
    mesh.mesh.vertices[v + 4] = 0.0f;  // uv.y
  }
  mesh.mesh.dirty = true;
}

// The engine's sphere builder expands indices into a triangle SOUP and leaves
// `indices` empty (a legal non-indexed mesh, see mesh_components.hpp). The bake
// draws indexed, so the artificial model owes an identity index buffer.
void AddIdentityIndices(TexturedMeshResult& mesh) {
  mesh.mesh.indices.resize(mesh.mesh.vertex_count);
  for (uint32_t i = 0; i < mesh.mesh.vertex_count; ++i) mesh.mesh.indices[i] = i;
  mesh.mesh.dirty = true;
}

void Translate(TexturedMeshResult& mesh, glm::vec3 by) {
  constexpr size_t kStride = 11;
  for (size_t v = 0; v + kStride <= mesh.mesh.vertices.size(); v += kStride) {
    mesh.mesh.vertices[v + 0] += by.x;
    mesh.mesh.vertices[v + 1] += by.y;
    mesh.mesh.vertices[v + 2] += by.z;
  }
  mesh.local_bounds = Aabb{mesh.local_bounds.min + by, mesh.local_bounds.max + by};
  mesh.mesh.dirty = true;
}

// A trunk sphere at the origin plus an offset crown sphere. Both CLOSED, which
// the additive thickness pass requires; neither generated by the tree pipeline,
// so this costs no skeleton and no voxelization.
TreeFieldModel MakeSphereModel() {
  TreeFieldModel model;
  model.options.leaves.tint = glm::vec3(0.2f, 0.9f, 0.3f);
  model.options.leaves.transmission_strength = 0.6f;
  model.options.leaves.alpha_cutoff = 0.5f;

  model.bark_lod0 = GenerateSphereTexturedMesh(kTrunkRadius, 12);
  FlattenBrightness(model.bark_lod0);
  AddIdentityIndices(model.bark_lod0);

  TexturedMeshResult crown = GenerateSphereTexturedMesh(kCrownRadius, 20);
  FlattenBrightness(crown);
  Translate(crown, kCrownCenter);
  AddIdentityIndices(crown);
  model.leaf_lod_meshes.push_back(std::move(crown));

  model.native_to_world_scale = 1.0f;
  model.target_height_m = kFixtureHeight;
  return model;
}

// A deliberately RAGGED crown: a shell of small spheres instead of one smooth
// one.
//
// The smooth sphere cannot exercise the mip/cutoff interaction at all, and this
// was measured rather than assumed -- reintroducing the cutoff mismatch (the
// material cutting at 0.35 while the bake fitted coverage at 0.5) leaves the
// smooth-sphere drift test PASSING. A crisp circle has a one-texel partial
// rim, so moving the threshold changes almost nothing. A real crown is stippled
// with partial coverage throughout, which is why the bug showed there.
//
// Fixed offsets, not a random scatter: the bake and every assertion downstream
// have to be reproducible run to run.
TreeFieldModel MakeStippleModel() {
  constexpr glm::vec3 kBlobs[] = {
      {0.00f, 0.00f, 0.00f},  {0.55f, 0.20f, 0.10f},  {-0.50f, 0.15f, 0.25f},
      {0.20f, 0.55f, -0.30f}, {-0.25f, -0.45f, 0.35f}, {0.35f, -0.35f, -0.45f},
      {-0.40f, 0.40f, -0.20f}, {0.10f, -0.10f, 0.60f}, {-0.15f, -0.55f, -0.30f},
      {0.60f, -0.05f, 0.35f}, {-0.60f, -0.10f, -0.35f}, {0.05f, 0.60f, 0.35f},
  };

  TreeFieldModel model;
  model.options.leaves.tint = glm::vec3(0.2f, 0.9f, 0.3f);
  model.options.leaves.transmission_strength = 0.6f;
  model.options.leaves.alpha_cutoff = 0.5f;

  model.bark_lod0 = GenerateSphereTexturedMesh(kTrunkRadius, 12);
  FlattenBrightness(model.bark_lod0);
  AddIdentityIndices(model.bark_lod0);

  TexturedMeshResult crown;
  crown.mesh.geometry_type = GeometryType::kTexturedMesh;
  crown.local_bounds = Aabb::Empty();
  for (const glm::vec3& b : kBlobs) {
    TexturedMeshResult blob = GenerateSphereTexturedMesh(0.30f, 8);
    FlattenBrightness(blob);
    Translate(blob, kCrownCenter + b);
    crown.mesh.vertices.insert(crown.mesh.vertices.end(),
                               blob.mesh.vertices.begin(),
                               blob.mesh.vertices.end());
    crown.mesh.vertex_count += blob.mesh.vertex_count;
    crown.local_bounds = crown.local_bounds.Union(blob.local_bounds);
  }
  AddIdentityIndices(crown);
  model.leaf_lod_meshes.push_back(std::move(crown));

  model.native_to_world_scale = 1.0f;
  model.target_height_m = kFixtureHeight;
  return model;
}

struct SphereImpostor {
  std::unique_ptr<TreeField> field;
  ImpostorBakeResult baked;
};

SphereImpostor BuildImpostor(RenderGpu& g, TreeFieldModel model) {
  SphereImpostor out;
  const std::array<TreeFieldModel, 1> models{std::move(model)};
  out.baked = BakeImpostorAtlas(g.device, g.queue, *g.gen, models);
  REQUIRE(out.baked.ok);

  TreeFieldImpostor slot;
  slot.atlas = &out.baked.atlas;
  slot.placement = out.baked.placement;
  REQUIRE(slot.active(models.size()));

  out.field = BuildTreeField(g.device, g.queue, *g.gen, models, /*capacity=*/4,
                             slot);
  REQUIRE(out.field != nullptr);
  return out;
}

SphereImpostor BuildSphereImpostor(RenderGpu& g) {
  return BuildImpostor(g, MakeSphereModel());
}

// Places one instance with the given yaw, far enough away that the LOD chain
// selects the IMPOSTOR rather than the voxel level.
void UploadInstance(const SphereImpostor& s, float yaw) {
  const glm::mat4 xf =
      glm::rotate(glm::mat4(1.0f), yaw, glm::vec3(0.0f, 1.0f, 0.0f));
  GpuInstanceRenderer::InstanceInput inst{};
  inst.transform = xf;
  // Generous: only used for GPU culling, and over-including cannot fail a test.
  inst.bounds_sphere = glm::vec4(0.0f, 0.0f, 0.0f, 4.0f);
  inst.model_info = glm::uvec4(0u);
  s.field->field->UploadInstances(
      std::span<const GpuInstanceRenderer::InstanceInput>(&inst, 1));
}

// The crown sphere's world centre for a given instance yaw.
glm::vec3 CrownCenterWorld(float yaw) {
  return glm::vec3(glm::rotate(glm::mat4(1.0f), yaw,
                               glm::vec3(0.0f, 1.0f, 0.0f)) *
                   glm::vec4(kCrownCenter, 1.0f));
}

// A camera looking at the world origin from `azimuth` at `distance`, tilted
// down by `pitch_deg` -- deliberately not axis-aligned, so a basis mistake
// cannot hide behind a symmetry.
Camera OrbitCamera(float azimuth, float distance, float pitch_deg = 20.0f) {
  // Below this the GPU picks the voxel level and the test measures the wrong
  // material entirely, with no error anywhere.
  REQUIRE(distance > kImpostorFromM);
  const float p = glm::radians(pitch_deg);
  Camera camera;
  camera.position = glm::vec3(std::cos(azimuth) * std::cos(p) * distance,
                              std::sin(p) * distance,
                              std::sin(azimuth) * std::cos(p) * distance);
  camera.direction = glm::normalize(-camera.position);
  camera.up = glm::vec3(0.0f, 1.0f, 0.0f);
  camera.fov = 50.0f;
  camera.aspect = 1.0f;
  camera.near_plane = 0.05f;
  camera.far_plane = distance * 4.0f + 50.0f;
  return camera;
}

// `format` is per-test, not incidental. RGBA16Float is not a readback format
// (CpuImage decodes 8-bit and 32-bit-float only), so:
//   * RGBA8Unorm for Normals/Albedo -- gbuffer_debug.wesl returns those RAW,
//     with the sRGB encode confined to its Hdr case, so 8 bits needs no decode;
//   * R32Float for Depth, whose 8-bit grayscale would quantize to ~0.7 m over
//     this camera's range and could not resolve the radius this test turns on;
//   * R32Float for the lit frame, matching the rest of this target's harnesses.
CpuImage RenderFrame(RenderGpu& g, const SphereImpostor& s,
                     const Camera& camera, GBufferDebugMode debug,
                     glm::vec3 sun_direction,
                     wgpu::TextureFormat format = wgpu::TextureFormat::RGBA8Unorm) {
  std::array<InstancedMeshField*, 1> fields = {s.field->field.get()};
  entt::registry registry;
  SceneContext ctx;
  ctx.registry = &registry;
  ctx.instanced_fields = fields.data();
  ctx.instanced_field_count = 1;
  ctx.sun_direction = glm::normalize(sun_direction);
  ctx.sun_color = glm::vec3(2.0f);
  ctx.ambient_sh[0] = glm::vec3(0.4f);
  ctx.clear_color = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);

  ColorRenderTarget rt(g.device, kSize, kSize, format);
  REQUIRE(rt.IsValid());

  SceneRenderer renderer;
  renderer.Initialize(g.device, g.queue, g.gen.get(), format, kSize, kSize,
                      g.device.HasFeature(wgpu::FeatureName::TextureFormatsTier1));
  renderer.MutableFogConfig().enabled = false;  // would haze every readback
  renderer.SetDebugMode(debug);

  test::CapturedError err =
      test::RunCapturingValidationErrors(g.instance, g.device, [&] {
        renderer.Render(camera, registry, ctx, rt.GetView());
      });
  INFO("Dawn validation error: " << err.message);
  CHECK(err.type == wgpu::ErrorType::NoError);
  test::WaitForGpu(g.instance, g.device, g.queue);

  TextureReadback readback(g.instance, g.device, g.queue);
  return readback.ReadTextureSync(rt.GetTexture(), kSize, kSize, format);
}

glm::ivec2 WorldToPixel(const Camera& camera, glm::vec3 world) {
  const glm::vec4 clip =
      camera.GetProj() * camera.GetView() * glm::vec4(world, 1.0f);
  REQUIRE(clip.w > 0.0f);
  const glm::vec3 ndc = glm::vec3(clip) / clip.w;
  return glm::ivec2(
      static_cast<int>((ndc.x * 0.5f + 0.5f) * static_cast<float>(kSize)),
      static_cast<int>((1.0f - (ndc.y * 0.5f + 0.5f)) * static_cast<float>(kSize)));
}

// Pixels whose albedo is not the background clear.
struct Coverage {
  int count = 0;
  glm::vec2 centroid{0.0f};
};

Coverage MeasureCoverage(const CpuImage& albedo) {
  Coverage c;
  glm::vec2 sum(0.0f);
  for (uint32_t y = 0; y < kSize; ++y) {
    for (uint32_t x = 0; x < kSize; ++x) {
      const CpuImage::ColorF32 p = albedo.GetPixelF32(x, y);
      if (p.r + p.g + p.b < 0.02f) continue;
      ++c.count;
      sum += glm::vec2(static_cast<float>(x), static_cast<float>(y));
    }
  }
  if (c.count > 0) c.centroid = sum / static_cast<float>(c.count);
  return c;
}

}  // namespace

TEST_CASE("The impostor renders where the sphere actually is",
          "[impostor][gpu]") {
  // The cheapest and broadest assertion, and the one that would have caught the
  // two "renders as nothing" bugs at once: a params UBO declared in the wrong
  // bind group, and a quad built in world space then projected as if it were
  // camera-offset space (which puts it off-screen entirely).
  RenderGpu& g = GetRenderGpu();
  const SphereImpostor s = BuildSphereImpostor(g);
  UploadInstance(s, 0.0f);

  const Camera camera = OrbitCamera(0.9f, 40.0f);
  const CpuImage albedo =
      RenderFrame(g, s, camera, GBufferDebugMode::Albedo, {0.3f, 0.7f, 0.4f});

  const Coverage cov = MeasureCoverage(albedo);
  INFO("covered=" << cov.count << " centroid=(" << cov.centroid.x << ","
                  << cov.centroid.y << ")");
  REQUIRE(cov.count > 30);

  // The centroid must sit on the CROWN, which is offset from the pivot -- not
  // merely somewhere on screen.
  const glm::ivec2 expect = WorldToPixel(camera, CrownCenterWorld(0.0f));
  CHECK(std::abs(cov.centroid.x - static_cast<float>(expect.x)) < 25.0f);
  CHECK(std::abs(cov.centroid.y - static_cast<float>(expect.y)) < 25.0f);
}

TEST_CASE("The decoded normal points at the camera from every azimuth",
          "[impostor][gpu]") {
  // THE test for the wrong-basis bug, and the reason the shape is a sphere.
  //
  // At a sphere's silhouette centre the visible surface point is the one facing
  // the viewer, so its world normal IS the direction toward the camera --
  // exactly, at every azimuth. Decoding the baked normal in the quad's
  // right/up/toEye frame instead of rotating it by the INSTANCE's rotation
  // gives a normal that swings with the camera, which this catches immediately.
  RenderGpu& g = GetRenderGpu();
  const SphereImpostor s = BuildSphereImpostor(g);
  UploadInstance(s, 0.0f);

  for (int i = 0; i < 6; ++i) {
    const float azimuth = static_cast<float>(i) * 1.047f;  // ~60 deg apart
    const Camera camera = OrbitCamera(azimuth, 40.0f);
    const CpuImage normals =
        RenderFrame(g, s, camera, GBufferDebugMode::Normals, {0.3f, 0.7f, 0.4f});

    const glm::vec3 center = CrownCenterWorld(0.0f);
    const glm::ivec2 px = WorldToPixel(camera, center);
    REQUIRE(px.x >= 0);
    REQUIRE(px.x < static_cast<int>(kSize));

    // The debug view writes n * 0.5 + 0.5.
    const CpuImage::ColorF32 p = normals.GetPixelF32(
        static_cast<uint32_t>(px.x), static_cast<uint32_t>(px.y));
    const glm::vec3 n = glm::normalize(
        glm::vec3(p.r, p.g, p.b) * 2.0f - 1.0f);
    const glm::vec3 to_camera = glm::normalize(camera.position - center);

    INFO("azimuth " << azimuth << " n=(" << n.x << "," << n.y << "," << n.z
                    << ") toCamera=(" << to_camera.x << "," << to_camera.y
                    << "," << to_camera.z << ")");
    // Generous against the 16-view blend and RG8 quantization, but nowhere near
    // loose enough to admit a normal built on the wrong basis.
    CHECK(glm::dot(n, to_camera) > 0.8f);
  }
}

TEST_CASE("The impostor writes real per-pixel depth, not the quad's plane",
          "[impostor][gpu]") {
  // The frag_depth write, pinned against the one number that distinguishes it:
  // at the silhouette centre the visible surface is a full RADIUS nearer than
  // the sphere's centre, which is where the quad's plane sits. Without the
  // write, the depth reads as the centre distance.
  RenderGpu& g = GetRenderGpu();
  const SphereImpostor s = BuildSphereImpostor(g);
  UploadInstance(s, 0.0f);

  const float distance = 40.0f;
  const Camera camera = OrbitCamera(0.9f, distance);
  const CpuImage depth =
      RenderFrame(g, s, camera, GBufferDebugMode::Depth, {0.3f, 0.7f, 0.4f},
                  wgpu::TextureFormat::R32Float);

  const glm::vec3 center = CrownCenterWorld(0.0f);
  const glm::ivec2 px = WorldToPixel(camera, center);
  // The debug view is LOG-scaled and inverted, not linear:
  // v = 1 - log(z/near) / log(far/near)  (gbuffer_debug.wesl, case 1).
  // Inverting it is the whole reason this reads R32Float -- the value has to
  // survive an exponential.
  const float v =
      depth.GetFloat(static_cast<uint32_t>(px.x), static_cast<uint32_t>(px.y));
  const float linear =
      camera.near_plane *
      std::pow(camera.far_plane / camera.near_plane, 1.0f - v);
  const float to_center = glm::length(camera.position - center);
  INFO("linear=" << linear << " to_center=" << to_center
                 << " expected~" << (to_center - kCrownRadius));

  // Nearer than the centre by roughly the radius. The band is wide because the
  // debug view's normalization is coarse, but a plane-depth write would land at
  // to_center, a full radius away from this.
  CHECK(linear < to_center - kCrownRadius * 0.4f);
  CHECK(linear > to_center - kCrownRadius * 1.8f);
}

TEST_CASE("The silhouette does not drift as the impostor recedes",
          "[impostor][gpu]") {
  // Pins two failures that only appear far away, both of which shipped: an
  // alpha cutoff that disagreed with the one the mips were fitted to (the
  // silhouette GROWS with distance -- the coverage fit inverted), and a
  // fractional mip against a nearest-mip sampler.
  //
  // Coverage scales as 1/d^2, so coverage * d^2 is the invariant. The distances
  // span enough range to cross several mip levels.
  RenderGpu& g = GetRenderGpu();
  // The RAGGED fixture, not the smooth one -- see MakeStippleModel. A crisp
  // circle's silhouette barely moves when the cutoff does, so the smooth sphere
  // cannot fail this test even with the bug reintroduced.
  const SphereImpostor s = BuildImpostor(g, MakeStippleModel());
  UploadInstance(s, 0.0f);

  std::vector<float> normalized;
  // The near sample is load-bearing: at mip 0 the atlas alpha is still a binary
  // mask, so the cutoff cannot move the silhouette there. Every coarser mip is
  // filtered and coverage-fitted, so a cutoff that disagrees with the fit
  // shifts them ALL together -- and a sweep that never touches mip 0 compares
  // shifted values against each other and sees nothing.
  for (const float d : {10.0f, 25.0f, 45.0f, 70.0f, 110.0f}) {
    const Camera camera = OrbitCamera(0.9f, d);
    const CpuImage albedo =
        RenderFrame(g, s, camera, GBufferDebugMode::Albedo, {0.3f, 0.7f, 0.4f});
    const Coverage cov = MeasureCoverage(albedo);
    REQUIRE(cov.count > 10);
    normalized.push_back(static_cast<float>(cov.count) * d * d);
    INFO("d=" << d << " covered=" << cov.count);
  }

  const float lo = *std::min_element(normalized.begin(), normalized.end());
  const float hi = *std::max_element(normalized.begin(), normalized.end());
  INFO("normalized coverage lo=" << lo << " hi=" << hi);
  // Wide, because a few dozen pixels quantize hard -- but a cutoff mismatch
  // grows the silhouette monotonically and blows well past this.
  CHECK(hi < lo * 1.9f);
}

TEST_CASE("Rotating the instance moves the impostor, as an offset crown must",
          "[impostor][gpu]") {
  // The instance rotation is applied in two places -- the quad's world position
  // and the normal's basis -- and a dropped or transposed one is invisible on a
  // symmetric shape. The crown is offset from the pivot precisely so that a
  // 180 degree yaw has to move it to the opposite side.
  RenderGpu& g = GetRenderGpu();
  const SphereImpostor s = BuildSphereImpostor(g);
  // Azimuth pi/2 puts the camera on +Z, so the crown's +X offset is
  // SCREEN-HORIZONTAL. At azimuth 0 the camera looks straight down the offset
  // axis and a 180 degree yaw moves the crown toward and away from the camera
  // instead of across it -- the motion is real but invisible in screen x, which
  // is exactly how this test first passed a broken assertion.
  const Camera camera = OrbitCamera(glm::half_pi<float>(), 40.0f);

  UploadInstance(s, 0.0f);
  const Coverage a = MeasureCoverage(
      RenderFrame(g, s, camera, GBufferDebugMode::Albedo, {0.3f, 0.7f, 0.4f}));
  UploadInstance(s, glm::pi<float>());
  const Coverage b = MeasureCoverage(
      RenderFrame(g, s, camera, GBufferDebugMode::Albedo, {0.3f, 0.7f, 0.4f}));

  REQUIRE(a.count > 30);
  REQUIRE(b.count > 30);

  const glm::ivec2 expect_a = WorldToPixel(camera, CrownCenterWorld(0.0f));
  const glm::ivec2 expect_b =
      WorldToPixel(camera, CrownCenterWorld(glm::pi<float>()));
  INFO("a=(" << a.centroid.x << "," << a.centroid.y << ") b=(" << b.centroid.x
             << "," << b.centroid.y << ")");

  // Each lands on its own predicted centre...
  CHECK(std::abs(a.centroid.x - static_cast<float>(expect_a.x)) < 25.0f);
  CHECK(std::abs(b.centroid.x - static_cast<float>(expect_b.x)) < 25.0f);

  // ...and the separation matches the GEOMETRY rather than a magic number. The
  // crown sits 0.7 units off the pivot, so a 180 degree yaw moves it 1.4 units,
  // which at 40 m through this camera's ~412 px focal length is ~14 px. A
  // hand-picked threshold set just above that measured value failed on a
  // correct render; deriving it removes the guess.
  const float predicted = std::abs(static_cast<float>(expect_a.x - expect_b.x));
  const float measured = std::abs(a.centroid.x - b.centroid.x);
  INFO("predicted separation " << predicted << " measured " << measured);
  // Guards a vacuous pass: a fixture whose crown was not actually offset would
  // predict zero and then "agree" with a render that never moved.
  REQUIRE(predicted > 8.0f);
  CHECK(std::abs(measured - predicted) < 6.0f);
}

// Finds the last covered pixel walking right from `from`, then steps back in a
// little -- the crown's RIM, where the sphere is thin.
glm::ivec2 RimPixelRight(const CpuImage& albedo, glm::ivec2 from) {
  int x = from.x;
  while (x + 1 < static_cast<int>(kSize)) {
    const CpuImage::ColorF32 p =
        albedo.GetPixelF32(static_cast<uint32_t>(x + 1),
                           static_cast<uint32_t>(from.y));
    if (p.r + p.g + p.b < 0.02f) break;
    ++x;
  }
  return glm::ivec2(std::max(from.x, x - 1), from.y);
}

TEST_CASE("The material channels carry translucency and AO, in that order",
          "[impostor][gpu]") {
  // THE assertion the swapped-argument bug could not have survived, and it only
  // became possible by adding GBufferDebugMode::Translucency / ::BakedAo --
  // before that, material.g and material.b had no view and the pack order was
  // reachable only through its consequence in the lit image.
  //
  // A sphere makes the expected values arithmetic. Thickness is 2*sqrt(R^2-r^2),
  // normalized per model to its own p99, so at the silhouette CENTRE it is ~1
  // and at the RIM it is ~0. With transmit = strength * exp(-6 t) and
  // ao = clamp(1 - 1.6 t, 0.25, 1):
  //
  //            correct                 swapped
  //   centre   g~0.00  b~0.25          g~0.25  b~0.00
  //   rim      g~0.60  b~1.00          g~1.00  b~0.60
  //
  // So `g < b` at BOTH samples is the discriminator: the swap inverts it at
  // both, while every other error I can think of preserves the ordering.
  RenderGpu& g = GetRenderGpu();
  const SphereImpostor s = BuildSphereImpostor(g);
  UploadInstance(s, 0.0f);

  // Close enough that the impostor covers ~140 px and selects mip 0: the
  // centre-to-rim thickness gradient has to be resolvable in SCREEN pixels, and
  // at 25 m the sphere is ~57 px across so a rim sample is still thick enough
  // to sit on the kMinAo floor -- which makes the gradient assertion vacuous
  // rather than false.
  const Camera camera = OrbitCamera(0.9f, 10.0f);
  const glm::vec3 sun{0.3f, 0.7f, 0.4f};
  const CpuImage albedo =
      RenderFrame(g, s, camera, GBufferDebugMode::Albedo, sun);
  const CpuImage translucency =
      RenderFrame(g, s, camera, GBufferDebugMode::Translucency, sun);
  const CpuImage baked_ao =
      RenderFrame(g, s, camera, GBufferDebugMode::BakedAo, sun);

  const glm::ivec2 mid = WorldToPixel(camera, CrownCenterWorld(0.0f));
  const glm::ivec2 rim = RimPixelRight(albedo, mid);
  INFO("mid=(" << mid.x << "," << mid.y << ") rim=(" << rim.x << "," << rim.y
               << ")");
  REQUIRE(rim.x > mid.x + 4);  // the walk found a real silhouette

  const auto chan = [](const CpuImage& img, glm::ivec2 px) {
    return img.GetPixelF32(static_cast<uint32_t>(px.x),
                           static_cast<uint32_t>(px.y)).r;
  };
  const float g_mid = chan(translucency, mid);
  const float b_mid = chan(baked_ao, mid);
  const float g_rim = chan(translucency, rim);
  const float b_rim = chan(baked_ao, rim);
  INFO("centre g=" << g_mid << " b=" << b_mid << "  rim g=" << g_rim
                   << " b=" << b_rim);

  // Ordering: translucency below baked AO at both samples. Inverted by a swap.
  CHECK(g_mid < b_mid);
  CHECK(g_rim < b_rim);

  // And both rise toward the thin rim, which is what makes them thickness-driven
  // rather than the constants they replaced. AO is checked against the kMinAo
  // floor rather than against b_mid: the interior saturates there, so a
  // rim-vs-centre comparison would read equal and pass vacuously.
  CHECK(g_rim > g_mid);
  CHECK(b_rim > b_mid);

  // ...and the interior never goes darker than the occlusion the VOXEL level
  // already receives from screen-space GTAO. The impostor's baked AO stands in
  // for that rather than adding to it -- a floor set too low makes the impostor
  // visibly darker than the level it replaces, which is a LOD-switch step
  // rather than a shading improvement.
  CHECK(b_mid > 0.4f);
  CHECK(b_mid < 0.85f);
}

TEST_CASE("Back-lighting brightens the impostor through its thin edge",
          "[impostor][gpu]") {
  // The transmission path end to end: the bake's thickness channel, the
  // per-pixel translucency derived from it, and the deferred pass's foliage
  // branch. A sphere is the right shape for it -- thickness is 2*sqrt(R^2-r^2),
  // so the rim is thin and the centre thick by construction.
  //
  // WEAKER THAN IT SHOULD BE, deliberately noted: GBufferDebugMode has no view
  // for material.g (translucency) or material.b (baked AO), so the argument
  // ORDER in packVoxelFoliageGBuffer cannot be asserted directly -- only its
  // consequence in the lit image. Adding a Material debug view would make this
  // exact, and is an engine change.
  RenderGpu& g = GetRenderGpu();
  const SphereImpostor s = BuildSphereImpostor(g);
  UploadInstance(s, 0.0f);

  const Camera camera = OrbitCamera(0.0f, 40.0f);
  const glm::vec3 to_camera = glm::normalize(camera.position);

  // sun_direction points TOWARD the sun: toward the camera = the impostor is
  // back-lit; away = front-lit.
  const CpuImage back =
      RenderFrame(g, s, camera, GBufferDebugMode::None, -to_camera,
                  wgpu::TextureFormat::R32Float);
  const CpuImage front =
      RenderFrame(g, s, camera, GBufferDebugMode::None, to_camera,
                  wgpu::TextureFormat::R32Float);

  const glm::vec3 center = CrownCenterWorld(0.0f);
  const glm::ivec2 mid = WorldToPixel(camera, center);

  const auto lum = [](const CpuImage& img, glm::ivec2 px) {
    return img.GetFloat(static_cast<uint32_t>(px.x),
                        static_cast<uint32_t>(px.y));
  };

  const float back_mid = lum(back, mid);
  const float front_mid = lum(front, mid);
  INFO("back_mid=" << back_mid << " front_mid=" << front_mid);
  REQUIRE(back_mid > 0.0f);
  REQUIRE(front_mid > 0.0f);

  // Both must be real light, and neither absurd: a swapped AO/translucency pair
  // drives transmission from a near-constant and blows the back-lit frame out.
  CHECK(back_mid < front_mid * 6.0f);
  CHECK(back_mid > front_mid * 0.05f);
}

