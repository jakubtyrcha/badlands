// Task T4: Catch2 suite for the directional-shadow system (T2's ShadowMap +
// T3's deferred shadow sampling). See task-4-brief.md and
// task-4-report.md for the harness/oracle design.
//
// This suite is the rigorous check that T2/T3's shadow map + bias math are
// correct: if Test 1 fails because a rendered edge lies outside the derived
// E_leak band, that is a REAL T2/T3 defect (report it precisely) -- NOT a
// reason to widen E_leak. If instead the oracle's own math is wrong, fix the
// oracle (see task-4-report.md's diagnosis, if any).

#include <catch_amalgamated.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

#include <spdlog/spdlog.h>

#include "badlands_assets.h"
#include "core/util/cpu_image.hpp"
#include "engine/rendering/shadow_map.hpp"
#include "shadow_test_geometry.hpp"
#include "shadow_test_harness.hpp"

using namespace badlands;
using namespace badlands::shadowtest;

namespace {

// Real Camera for the T4 macro scene: fov/aspect/near/far chosen so the
// analytic floor (kFloorHalfSize = 500m) always fully covers whatever the
// camera can actually see -- worst-case ray length is
// far_plane*sqrt(1+2*(aspect*tan(fov/2))^2) ~= 1.16*far_plane for this
// fov/aspect, well under kFloorHalfSize even after adding the camera's local
// offset from ground_point (~10m). See task-4-report.md for the derivation.
Camera BuildCamera(const TestCamera& tc) {
  Camera cam;
  cam.position = tc.position;
  cam.up = glm::vec3(0.0f, 1.0f, 0.0f);
  cam.LookAt(tc.target);
  cam.fov = 45.0f;
  cam.aspect = 1.0f;
  cam.near_plane = 0.05f;
  cam.far_plane = 250.0f;
  return cam;
}

// Builds the T4 macro scene already posed at the fixed off-axis world
// transform (see MakeOffAxisPose's doc comment) -- the shared setup for
// every test below except Test 2 (which needs no Scene/Camera at all).
struct PosedMacroScene {
  Scene scene;
  Camera camera;
  glm::vec3 basis_u, basis_v;
  std::vector<glm::vec2> shadow_polygon;
};

PosedMacroScene BuildPosedMacroScene() {
  PosedMacroScene result;
  TestCamera cam = MakeMacroCamera();
  result.scene = MakeMacroScene();
  ApplyPose(result.scene, cam, MakeOffAxisPose());
  result.camera = BuildCamera(cam);
  PlaneBasis(result.scene.ground_normal, result.basis_u, result.basis_v);
  result.shadow_polygon = ComputeShadowPolygon(result.scene, result.basis_u, result.basis_v);
  return result;
}

// Task T3-fix Test 5's scene: MakeSlopeScene() (no caster), posed at the
// SAME fixed off-axis world transform as the macro scene (see
// MakeOffAxisPose's doc comment).
//
// Camera: straight down the slope's own (pre-pose) normal from a modest
// height, looking at ground_point -- NOT MakeMacroCamera's oblique framing.
// kFloorHalfSize's doc comment notes Test 1's camera/floor combo keeps the
// worst-case ray length well under the floor's 500m bound (so the CPU
// oracle's ClassifyPixel, which has no far_plane clip, never disagrees with
// the GPU's far-plane discard) -- that "well under" margin is a property of
// a near-HORIZONTAL floor with an elevated, moderately-downward-looking
// camera. Reusing that same camera against a 45-degree-tilted plane breaks
// it: corner rays can run nearly parallel to the tilted plane and travel
// hundreds of world units (past the camera's far_plane=250, see
// BuildCamera) before landing inside the 500m bound -- the oracle then
// calls a pixel "receiver" that the GPU actually far-plane-discarded to the
// harness's clear color (0.1), which read as spurious near-zero-visibility
// "acne". A straight-down view bounds every visible ray to a short,
// predictable distance (no grazing geometry), restoring the same margin
// Test 1 relies on.
struct PosedSlopeScene {
  Scene scene;
  Camera camera;
  glm::vec3 basis_u, basis_v;
};

PosedSlopeScene BuildPosedSlopeScene() {
  PosedSlopeScene result;
  result.scene = MakeSlopeScene();

  TestCamera cam;
  cam.position = result.scene.ground_normal * 15.0f;
  cam.target = result.scene.ground_point;

  ApplyPose(result.scene, cam, MakeOffAxisPose());
  result.camera = BuildCamera(cam);
  PlaneBasis(result.scene.ground_normal, result.basis_u, result.basis_v);
  return result;
}

void EnsureDiagnosticsDir(const std::filesystem::path& dir) {
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  if (ec) {
    spdlog::error("shadow_tests: failed to create diagnostics dir {}: {}", dir.string(),
                  ec.message());
  }
}

std::vector<uint8_t> VisibilityToRgba8(const CpuImage& img) {
  const uint32_t width = img.GetWidth();
  const uint32_t height = img.GetHeight();
  std::vector<uint8_t> out(static_cast<size_t>(width) * height * 4);
  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      const size_t i = static_cast<size_t>(y) * width + x;
      const uint8_t v =
          static_cast<uint8_t>(std::clamp(img.GetFloat(x, y), 0.0f, 1.0f) * 255.0f + 0.5f);
      out[i * 4 + 0] = v;
      out[i * 4 + 1] = v;
      out[i * 4 + 2] = v;
      out[i * 4 + 3] = 255;
    }
  }
  return out;
}

// diag codes: 0 = non-receiver (black), 1 = ignored E_leak band (gray),
// 2 = pass, expected-lit side (green), 3 = pass, expected-shadow side
// (blue), 4 = FAILED assertion (red).
std::vector<uint8_t> DiagnosisToRgba8(const std::vector<uint8_t>& diag) {
  std::vector<uint8_t> out(diag.size() * 4);
  for (size_t i = 0; i < diag.size(); ++i) {
    uint8_t r = 0, g = 0, b = 0;
    switch (diag[i]) {
      case 1: r = g = b = 90; break;
      case 2: g = 200; break;
      case 3: b = 220; break;
      case 4: r = 255; break;
      default: break;  // 0: stays black
    }
    out[i * 4 + 0] = r;
    out[i * 4 + 1] = g;
    out[i * 4 + 2] = b;
    out[i * 4 + 3] = 255;
  }
  return out;
}

struct FailureExample {
  uint32_t px, py;
  float d;
  float value;
  bool expected_lit;
};

// Dumps the visibility + diagnosis PNGs and logs r_sm/d_max/T_size/E_leak +
// light_view_proj (recomputed here purely for this log line -- a throwaway
// CPU-only ShadowMap, mirroring exactly what SceneRenderer::Render computes
// each frame, see scene_renderer.cpp's shadow_center_world/UpdateLightMatrices
// call) for a failed Test 1 config.
void DumpTest1Failure(const std::string& tag, const CpuImage& img,
                     const std::vector<uint8_t>& diag, const Scene& scene,
                     const Camera& camera, uint32_t r_sm, float d_max, float t_size,
                     float e_leak, size_t checked, size_t failed, float max_excess_d,
                     const std::vector<FailureExample>& examples) {
  const std::filesystem::path dir = std::filesystem::path("build") / "shadow_test_failures";
  EnsureDiagnosticsDir(dir);

  const std::vector<uint8_t> visibility_rgba = VisibilityToRgba8(img);
  const std::string visibility_path = (dir / (tag + "_visibility.png")).string();
  badlands_write_png(visibility_path.c_str(), visibility_rgba.data(), img.GetWidth(),
                     img.GetHeight());

  const std::vector<uint8_t> diagnosis_rgba = DiagnosisToRgba8(diag);
  const std::string diagnosis_path = (dir / (tag + "_diagnosis.png")).string();
  badlands_write_png(diagnosis_path.c_str(), diagnosis_rgba.data(), img.GetWidth(),
                     img.GetHeight());

  spdlog::error(
      "Shadow Test 1 [{}] FAILED: r_sm={} d_max={} t_size={:.6f} e_leak={:.6f} "
      "checked={} failed={} max_excess_D_beyond_e_leak={:.4f} ({:.2f}x T_size)",
      tag, r_sm, d_max, t_size, e_leak, checked, failed, max_excess_d, max_excess_d / t_size);
  spdlog::error("Shadow Test 1 [{}]: dumped {} and {}", tag, visibility_path, diagnosis_path);
  for (const FailureExample& ex : examples) {
    spdlog::error("  pixel ({},{}) D={:.6f} value={:.6f} expected={}", ex.px, ex.py, ex.d,
                  ex.value, ex.expected_lit ? "lit(>0.99)" : "shadowed(<0.01)");
  }

  ShadowMap diag_map;
  const glm::vec3 shadow_center_world =
      camera.GetPosition() + camera.direction * (0.5f * d_max);
  diag_map.UpdateLightMatrices(scene.sun_toward, shadow_center_world, d_max, r_sm, 100.0f);
  const glm::mat4& lvp = diag_map.GetLightViewProj();
  spdlog::error("Shadow Test 1 [{}]: light_view_proj (column-major rows below) =", tag);
  for (int row = 0; row < 4; ++row) {
    spdlog::error("  [{:>10.4f} {:>10.4f} {:>10.4f} {:>10.4f}]", lvp[0][row], lvp[1][row],
                 lvp[2][row], lvp[3][row]);
  }
}

// Dumps the visibility + diagnosis PNGs and logs r_sm/d_max/min_value +
// light_view_proj for a failed Test 5 (slope-acne) config -- same shape as
// DumpTest1Failure but simpler (no E_leak band / shadow-side; every checked
// receiver pixel is expected lit).
void DumpTest5Failure(const std::string& tag, const CpuImage& img,
                     const std::vector<uint8_t>& diag, const Scene& scene,
                     const Camera& camera, uint32_t r_sm, float d_max, size_t checked,
                     size_t failed, float min_value,
                     const std::vector<FailureExample>& examples) {
  const std::filesystem::path dir = std::filesystem::path("build") / "shadow_test_failures";
  EnsureDiagnosticsDir(dir);

  const std::vector<uint8_t> visibility_rgba = VisibilityToRgba8(img);
  const std::string visibility_path = (dir / (tag + "_visibility.png")).string();
  badlands_write_png(visibility_path.c_str(), visibility_rgba.data(), img.GetWidth(),
                     img.GetHeight());

  const std::vector<uint8_t> diagnosis_rgba = DiagnosisToRgba8(diag);
  const std::string diagnosis_path = (dir / (tag + "_diagnosis.png")).string();
  badlands_write_png(diagnosis_path.c_str(), diagnosis_rgba.data(), img.GetWidth(),
                     img.GetHeight());

  spdlog::error(
      "Shadow Test 5 [{}] FAILED: r_sm={} d_max={} checked={} failed={} min_value={:.6f}", tag,
      r_sm, d_max, checked, failed, min_value);
  spdlog::error("Shadow Test 5 [{}]: dumped {} and {}", tag, visibility_path, diagnosis_path);
  for (const FailureExample& ex : examples) {
    spdlog::error("  pixel ({},{}) value={:.6f} expected=lit(>0.99)", ex.px, ex.py, ex.value);
  }

  ShadowMap diag_map;
  const glm::vec3 shadow_center_world =
      camera.GetPosition() + camera.direction * (0.5f * d_max);
  diag_map.UpdateLightMatrices(scene.sun_toward, shadow_center_world, d_max, r_sm, 100.0f);
  const glm::mat4& lvp = diag_map.GetLightViewProj();
  spdlog::error("Shadow Test 5 [{}]: light_view_proj (column-major rows below) =", tag);
  for (int row = 0; row < 4; ++row) {
    spdlog::error("  [{:>10.4f} {:>10.4f} {:>10.4f} {:>10.4f}]", lvp[0][row], lvp[1][row],
                 lvp[2][row], lvp[3][row]);
  }
}

}  // namespace

// ============================================================================
// Milestone 1: harness smoke test -- prove the offscreen render + R32Float
// readback round-trip reads back LINEAR values, using a scene where the
// expected answer is known independent of the oracle (shadow map disabled ->
// the map stays cleared to far/1.0 everywhere -> every receiver/caster
// pixel's shadow term is ~1.0).
// ============================================================================
TEST_CASE("shadowtest harness: linear readback reads back ~1.0 when the shadow map is disabled") {
  PosedMacroScene posed = BuildPosedMacroScene();

  ShadowTestConfig config;
  config.r_sm = 1024;
  config.d_max = 128.0f;
  config.mode = ShadowDebugMode::ShadowMapOnly;
  config.enable_shadow_map = false;  // no casters drawn -> depth map stays cleared to far (1.0)
  config.enable_contact_shadows = false;

  CpuImage img = RenderShadowFrame(config, posed.scene, posed.camera);
  REQUIRE(img.GetWidth() == kFrameWidth);
  REQUIRE(img.GetHeight() == kFrameHeight);

  // Aggregate check: with no casters, the floor (which covers the whole
  // frame -- see kFloorHalfSize) and the box should both read back
  // (linear) ~1.0. Tolerate a small fraction of stray/edge pixels rather
  // than demanding literally every one of 262144 pixels.
  size_t near_one = 0;
  float min_value = 1.0f;
  const size_t pixel_count = static_cast<size_t>(img.GetWidth()) * img.GetHeight();
  for (uint32_t py = 0; py < img.GetHeight(); ++py) {
    for (uint32_t px = 0; px < img.GetWidth(); ++px) {
      const float v = img.GetFloat(px, py);
      if (v > 0.99f) ++near_one;
      min_value = std::min(min_value, v);
    }
  }
  INFO("min pixel value = " << min_value);
  CHECK(static_cast<double>(near_one) / static_cast<double>(pixel_count) > 0.99);

  // Precise check at a KNOWN ground/receiver point (the shadow polygon's
  // centroid -- guaranteed to be a receiver pixel by the sanity-check test
  // below): must read very close to exactly 1.0, not a suspicious
  // near-but-not-quite value that would indicate an accidental sRGB
  // roundtrip in the readback path.
  REQUIRE(posed.shadow_polygon.size() >= 3);
  glm::vec2 centroid(0.0f);
  for (const glm::vec2& p : posed.shadow_polygon) centroid += p;
  centroid /= static_cast<float>(posed.shadow_polygon.size());
  const glm::vec3 world_centroid =
      posed.scene.ground_point + centroid.x * posed.basis_u + centroid.y * posed.basis_v;

  glm::vec2 uv{};
  REQUIRE(ProjectToScreenUV(posed.camera, world_centroid, uv));
  REQUIRE(uv.x >= 0.0f);
  REQUIRE(uv.x <= 1.0f);
  REQUIRE(uv.y >= 0.0f);
  REQUIRE(uv.y <= 1.0f);
  const uint32_t px = std::min(kFrameWidth - 1, static_cast<uint32_t>(uv.x * kFrameWidth));
  const uint32_t py = std::min(kFrameHeight - 1, static_cast<uint32_t>(uv.y * kFrameHeight));
  CHECK_THAT(img.GetFloat(px, py), Catch::Matchers::WithinAbs(1.0f, 1e-3f));
}

// ============================================================================
// Milestone 2: pure-CPU sanity check -- the off-axis rigid pose is wired
// correctly end-to-end. Cross-validates TWO independent CPU paths (forward
// projection via the camera's view/proj matrices vs. the ray-caster used by
// the oracle) against each other, with no GPU rendering involved.
// ============================================================================
TEST_CASE("shadowtest scene: off-axis pose sanity (screen projection matches ray-cast)") {
  PosedMacroScene posed = BuildPosedMacroScene();
  REQUIRE(posed.scene.casters.size() == 1);
  const CasterMesh& box = posed.scene.casters[0];

  glm::vec2 min_uv(std::numeric_limits<float>::infinity());
  glm::vec2 max_uv(-std::numeric_limits<float>::infinity());
  int in_front = 0;
  for (int i = 0; i < 8; ++i) {
    const glm::vec3 local((i & 1) ? box.half_extents.x : -box.half_extents.x,
                          (i & 2) ? box.half_extents.y : -box.half_extents.y,
                          (i & 4) ? box.half_extents.z : -box.half_extents.z);
    const glm::vec3 world = glm::vec3(box.model_matrix * glm::vec4(local, 1.0f));
    glm::vec2 uv{};
    if (!ProjectToScreenUV(posed.camera, world, uv)) continue;
    ++in_front;
    min_uv = glm::min(min_uv, uv);
    max_uv = glm::max(max_uv, uv);
  }
  REQUIRE(in_front == 8);  // every corner is in front of the camera
  // The box should be comfortably framed (not clipped hard at the edges)
  // and at least overlap the visible [0,1]^2 region.
  CHECK(min_uv.x > -0.2f);
  CHECK(max_uv.x < 1.2f);
  CHECK(min_uv.y > -0.2f);
  CHECK(max_uv.y < 1.2f);
  CHECK(min_uv.x < 1.0f);
  CHECK(max_uv.x > 0.0f);
  CHECK(min_uv.y < 1.0f);
  CHECK(max_uv.y > 0.0f);

  // Cross-check #1: the screen-space center of the box's projected bounds
  // should ray-cast to the BOX itself (not the ground) -- the independent
  // ClassifyPixel ray-caster should agree it's occluded there.
  const glm::vec2 center_uv = 0.5f * (min_uv + max_uv);
  const uint32_t cpx =
      std::min(kFrameWidth - 1, static_cast<uint32_t>(std::clamp(center_uv.x, 0.0f, 0.999f) *
                                                       kFrameWidth));
  const uint32_t cpy =
      std::min(kFrameHeight - 1, static_cast<uint32_t>(std::clamp(center_uv.y, 0.0f, 0.999f) *
                                                        kFrameHeight));
  PixelHit box_hit = ClassifyPixel(posed.scene, posed.camera, posed.basis_u, posed.basis_v, cpx,
                                   cpy, kFrameWidth, kFrameHeight);
  CHECK_FALSE(box_hit.is_receiver);

  // Cross-check #2: the shadow polygon's centroid (a ground-plane point,
  // computed independently from the box corners' light-direction
  // projection) should both screen-project into the frame AND ray-cast to a
  // receiver (ground) hit.
  REQUIRE(posed.shadow_polygon.size() >= 3);
  glm::vec2 centroid(0.0f);
  for (const glm::vec2& p : posed.shadow_polygon) centroid += p;
  centroid /= static_cast<float>(posed.shadow_polygon.size());
  const glm::vec3 world_centroid =
      posed.scene.ground_point + centroid.x * posed.basis_u + centroid.y * posed.basis_v;

  glm::vec2 uv_c{};
  REQUIRE(ProjectToScreenUV(posed.camera, world_centroid, uv_c));
  REQUIRE(uv_c.x >= 0.0f);
  REQUIRE(uv_c.x <= 1.0f);
  REQUIRE(uv_c.y >= 0.0f);
  REQUIRE(uv_c.y <= 1.0f);
  const uint32_t gpx = std::min(kFrameWidth - 1,
                                static_cast<uint32_t>(std::clamp(uv_c.x, 0.0f, 0.999f) * kFrameWidth));
  const uint32_t gpy = std::min(
      kFrameHeight - 1, static_cast<uint32_t>(std::clamp(uv_c.y, 0.0f, 0.999f) * kFrameHeight));
  PixelHit ground_hit = ClassifyPixel(posed.scene, posed.camera, posed.basis_u, posed.basis_v,
                                      gpx, gpy, kFrameWidth, kFrameHeight);
  CHECK(ground_hit.is_receiver);
}

TEST_CASE("shadowtest oracle: reconstructLinearZ boundary mirror") {
  const float near_plane = 0.1f;
  const float far_plane = 250.0f;
  // Reversed-Z: depth=1 -> near, depth=0 -> far (see shaders/common/
  // frame.wesl's reconstructLinearZ and the T3 fix this pins down).
  CHECK_THAT(ReconstructLinearZ(1.0f, near_plane, far_plane),
            Catch::Matchers::WithinAbs(near_plane, 1e-4f));
  CHECK_THAT(ReconstructLinearZ(0.0f, near_plane, far_plane),
            Catch::Matchers::WithinAbs(far_plane, 1e-4f));
}

// ============================================================================
// Test 1: macro core & edge leak. For each (r_sm, d_max) in the matrix,
// every RECEIVER pixel whose signed edge-distance D lies outside the
// derived E_leak band must be unambiguously lit (D > E_leak) or shadowed
// (D < -E_leak); pixels inside the band are ignored (bias-affected).
// ============================================================================
TEST_CASE("Shadow Test 1: macro core & edge leak") {
  PosedMacroScene posed = BuildPosedMacroScene();
  REQUIRE(posed.shadow_polygon.size() >= 3);

  // T3-fix-2: this test now renders with sampleShadowMapPCF's HARD (no-PCF,
  // single unfiltered tap) debug path, so the PCF kernel's inherent soft
  // silhouette edge (task-3fix-report.md's diagnosed, proven-independent-of-
  // biasUV residual) no longer contaminates the signal being checked. With
  // PCF gone, E_leak's kernel term is K_pcf=0 -- only two real, geometric
  // effects remain, both derived (not fudged) below:
  //
  // 1. Incidence-corrected sub-texel snapping, worst-case over edge
  // orientation. A hard tap classifies a query point p by the STORED
  // classification of whichever shadow-map texel its uv lands in (texel
  // index floor(uv*dim)); that texel's own classification was decided, at
  // shadow-map render time, by whether the TEXEL'S OWN CENTER c lies on the
  // caster or receiver side of the true (continuous) boundary line L. p can
  // only be misclassified if L passes between p and c -- and then
  // dist(p, L) <= |p - c|, since the segment p-c crosses L. The worst case
  // is |p - c| = a texel's half-diagonal = T_size/sqrt(2) (p at a texel
  // corner, c its center) -- NOT the T_size/2 you'd get by assuming L
  // crosses the grid axis-aligned (that's only the tight bound for an edge
  // parallel to a texel row/column). This scene's shadow polygon sits at an
  // arbitrary angle to the shadow map's own (light-space) u/v grid --
  // MakeOffAxisPose deliberately poses it off-axis -- so the general,
  // any-orientation bound applies, not the axis-aligned special case.
  // Re-measured ON the (tilted) receiver surface (one perpendicular-to-
  // light texel spans ~T_size/NdotL there, same incidence argument as
  // before), the snapping-only band is e_leak = (T_size/sqrt(2)) / NdotL.
  // (Verified: at the axis-aligned-only coefficient 0.5 this test leaves a
  // small, resolution-scaling residual (<=6 pixels/config, <=0.12x T_size
  // beyond the band, out of ~240k checked pixels/config) -- consistent with
  // exactly this missing worst-case-orientation factor, not a rendering
  // defect; see task-3fix2-report.md.)
  //
  // 2. Receiver-side normal-offset (Peter-Panning). sampleShadowMapPCF's
  // step 1 displaces the sampled point by `ground_normal * offsetLen`
  // (offsetLen = ShadowMath::NormalOffsetLength(NdotL, T_size)) BEFORE
  // projecting into light space. Because that displacement is along the
  // receiver's normal (not along the light ray), it shifts the shadow-map
  // lookup sideways as well as in depth -- retracting the observed shadow
  // boundary from the TRUE geometric polygon (ComputeShadowPolygon) by a
  // fixed world-space vector. Baking that shift into the oracle's expected
  // polygon (rather than folding it into E_leak as a fudge term) keeps the
  // check symmetric and exact at the derived snapping value on BOTH sides.
  //
  // Derivation of the shift vector (independently verified via a numeric
  // Python cross-check before writing this):
  //   delta          = ground_normal * offsetLen              (the bias)
  //   light_ray_dir   = -normalize(sun_toward)                 (light travel dir)
  //   denom           = dot(light_ray_dir, ground_normal)      (== -NdotL)
  //   shift_vector    = delta - (dot(delta, ground_normal) / denom) * light_ray_dir
  // is the vector by which a light ray through a biased sample point
  // (worldPos + delta), re-projected onto the ground plane along
  // light_ray_dir, lands relative to worldPos itself. Setting that equal to
  // a TRUE boundary point B and solving for the actual (unbiased) receiver
  // position p gives p = B - shift_vector -- i.e. the polygon a hard tap
  // with normal-offset bias actually renders is `shadow_polygon -
  // shift_vector` (every vertex translated by the same fixed vector, since
  // ground_normal/sun_toward/NdotL are constant over this flat floor).
  // shift_vector is guaranteed in-plane (dot(shift_vector, ground_normal) ==
  // 0 algebraically: delta's normal component exactly cancels), so
  // subtracting it from the (basis_u, basis_v) polygon vertices is exact,
  // not an approximation.
  const glm::vec3& ground_normal = posed.scene.ground_normal;
  const glm::vec3& sun_toward = posed.scene.sun_toward;
  const float ndotl = glm::dot(ground_normal, sun_toward);
  const glm::vec3 light_ray_dir = -glm::normalize(sun_toward);
  const float denom = glm::dot(light_ray_dir, ground_normal);  // == -ndotl

  struct ConfigCase {
    uint32_t r_sm;
    float d_max;
  };
  const std::vector<ConfigCase> configs = {
      {512, 100.0f}, {512, 200.0f}, {1024, 100.0f}, {1024, 200.0f}, {2048, 100.0f}, {2048, 200.0f},
  };

  for (const ConfigCase& cfg : configs) {
    DYNAMIC_SECTION("r_sm=" << cfg.r_sm << " d_max=" << cfg.d_max) {
      const float t_size = cfg.d_max / static_cast<float>(cfg.r_sm);
      const float e_leak = (t_size / std::sqrt(2.0f)) / ndotl;

      // Bake the normal-offset (Peter-Panning) shift into the expected
      // polygon -- see the derivation above. ShadowMath::NormalOffsetLength
      // is the SAME CPU source-of-truth expression shadow_sampling.wesl's
      // step 1 mirrors (shadow_map.hpp).
      const float offset_len = ShadowMath::NormalOffsetLength(ndotl, t_size);
      const glm::vec3 delta = ground_normal * offset_len;
      const glm::vec3 shift_vector =
          delta - (glm::dot(delta, ground_normal) / denom) * light_ray_dir;
      const glm::vec2 shift_uv(glm::dot(shift_vector, posed.basis_u),
                               glm::dot(shift_vector, posed.basis_v));
      std::vector<glm::vec2> observed_polygon = posed.shadow_polygon;
      for (glm::vec2& v : observed_polygon) v -= shift_uv;

      ShadowTestConfig render_cfg;
      render_cfg.r_sm = cfg.r_sm;
      render_cfg.d_max = cfg.d_max;
      render_cfg.mode = ShadowDebugMode::ShadowMapOnly;
      render_cfg.enable_shadow_map = true;
      render_cfg.enable_contact_shadows = true;
      render_cfg.hard_shadow_debug = true;  // T3-fix-2: raw signal, no PCF

      CpuImage img = RenderShadowFrame(render_cfg, posed.scene, posed.camera);
      REQUIRE(img.GetWidth() == kFrameWidth);
      REQUIRE(img.GetHeight() == kFrameHeight);

      size_t checked = 0;
      size_t failed = 0;
      // Worst-case excess beyond the E_leak band among failures -- (d -
      // e_leak) on the lit side, (-e_leak - d) on the shadow side -- reported
      // on failure to distinguish "modest overshoot near the edge" from "a
      // large, resolution-independent leak" (see task-4-report.md). T3-fix
      // review minor: originally only tracked the lit-side term, silently
      // leaving shadow-side failures out of the diagnostic.
      float max_excess_d = 0.0f;
      std::vector<FailureExample> examples;
      std::vector<uint8_t> diag(static_cast<size_t>(img.GetWidth()) * img.GetHeight(), 0);

      for (uint32_t py = 0; py < img.GetHeight(); ++py) {
        for (uint32_t px = 0; px < img.GetWidth(); ++px) {
          const PixelHit hit = ClassifyPixel(posed.scene, posed.camera, posed.basis_u,
                                             posed.basis_v, px, py, img.GetWidth(),
                                             img.GetHeight());
          if (!hit.is_receiver) continue;

          const float d = SignedDistanceToPolygon(observed_polygon, hit.ground_uv);
          const float value = img.GetFloat(px, py);
          const size_t idx = static_cast<size_t>(py) * img.GetWidth() + px;

          if (d > e_leak) {
            ++checked;
            if (value > 0.99f) {
              diag[idx] = 2;
            } else {
              diag[idx] = 4;
              ++failed;
              max_excess_d = std::max(max_excess_d, d - e_leak);
              if (examples.size() < 8) examples.push_back({px, py, d, value, true});
            }
          } else if (d < -e_leak) {
            ++checked;
            if (value < 0.01f) {
              diag[idx] = 3;
            } else {
              diag[idx] = 4;
              ++failed;
              max_excess_d = std::max(max_excess_d, -e_leak - d);
              if (examples.size() < 8) examples.push_back({px, py, d, value, false});
            }
          } else {
            diag[idx] = 1;  // inside the E_leak band -- ignored
          }
        }
      }

      INFO("r_sm=" << cfg.r_sm << " d_max=" << cfg.d_max << " t_size=" << t_size
                   << " e_leak=" << e_leak << " checked=" << checked << " failed=" << failed);

      if (failed > 0) {
        const std::string tag = "test1_r" + std::to_string(cfg.r_sm) + "_d" +
                                std::to_string(static_cast<int>(cfg.d_max));
        DumpTest1Failure(tag, img, diag, posed.scene, posed.camera, cfg.r_sm, cfg.d_max, t_size,
                         e_leak, checked, failed, max_excess_d, examples);
      }

      // Sanity: the scene/camera framing should exercise a meaningful
      // number of receiver pixels outside the E_leak band -- a near-zero
      // count here would mean this config isn't actually testing anything
      // (a framing/oracle bug hiding as a false pass), not a real result.
      CHECK(checked > 1000);
      CHECK(failed == 0);
    }
  }
}

// ============================================================================
// Test 5 (T3-fix): RPDB slope-acne. MakeSlopeScene() (a single 45-degree-
// tilted receiver plane, NO caster) across R_sm in {512, 1024, 2048} at a
// fixed D_max -- every LIT receiver pixel must read visibility ~1.0
// (>0.99): with no caster anywhere in the scene, nothing can legitimately
// occlude any of these pixels, so any pixel below 1.0 is self-shadow acne
// from an incorrect receiver-plane depth-bias gradient. Isolates biasUV
// correctness from Test 1's silhouette/cast-shadow concerns.
// ============================================================================
TEST_CASE("Shadow Test 5: RPDB slope-acne") {
  PosedSlopeScene posed = BuildPosedSlopeScene();

  const std::vector<uint32_t> resolutions = {512, 1024, 2048};
  const float d_max = 100.0f;

  for (uint32_t r_sm : resolutions) {
    DYNAMIC_SECTION("r_sm=" << r_sm) {
      ShadowTestConfig render_cfg;
      render_cfg.r_sm = r_sm;
      render_cfg.d_max = d_max;
      render_cfg.mode = ShadowDebugMode::ShadowMapOnly;
      render_cfg.enable_shadow_map = true;
      render_cfg.enable_contact_shadows = true;

      CpuImage img = RenderShadowFrame(render_cfg, posed.scene, posed.camera);
      REQUIRE(img.GetWidth() == kFrameWidth);
      REQUIRE(img.GetHeight() == kFrameHeight);

      size_t checked = 0;
      size_t failed = 0;
      float min_value = 1.0f;
      std::vector<FailureExample> examples;
      std::vector<uint8_t> diag(static_cast<size_t>(img.GetWidth()) * img.GetHeight(), 0);

      for (uint32_t py = 0; py < img.GetHeight(); ++py) {
        for (uint32_t px = 0; px < img.GetWidth(); ++px) {
          const PixelHit hit = ClassifyPixel(posed.scene, posed.camera, posed.basis_u,
                                             posed.basis_v, px, py, img.GetWidth(),
                                             img.GetHeight());
          if (!hit.is_receiver) continue;

          ++checked;
          const float value = img.GetFloat(px, py);
          min_value = std::min(min_value, value);
          const size_t idx = static_cast<size_t>(py) * img.GetWidth() + px;
          if (value > 0.99f) {
            diag[idx] = 2;
          } else {
            diag[idx] = 4;
            ++failed;
            if (examples.size() < 8) examples.push_back({px, py, 0.0f, value, true});
          }
        }
      }

      INFO("r_sm=" << r_sm << " d_max=" << d_max << " checked=" << checked
                   << " failed=" << failed << " min_value=" << min_value);

      if (failed > 0) {
        const std::string tag = "test5_r" + std::to_string(r_sm);
        DumpTest5Failure(tag, img, diag, posed.scene, posed.camera, r_sm, d_max, checked, failed,
                         min_value, examples);
      }

      // Sanity: the scene/camera framing should exercise a meaningful
      // receiver area -- a near-zero count would mean this config isn't
      // actually testing anything.
      CHECK(checked > 1000);
      CHECK(failed == 0);
    }
  }
}

// ============================================================================
// Test 2: sub-texel snapping stability. Pure CPU -- directly exercises
// ShadowMap::UpdateLightMatrices, no rendering. Verifies the "same T_size
// grid cell -> byte-identical matrix" invariant (not a naive fixed-magnitude
// move, which could straddle a bin boundary from an uncontrolled start).
// ============================================================================
namespace {

// Mirrors ShadowMap::UpdateLightMatrices's basis derivation exactly
// (shadow_map.cpp) so this test can construct a center point with a
// controlled fractional grid-cell position on all three light-space axes.
void ComputeLightBasis(const glm::vec3& sun_dir, glm::vec3& right, glm::vec3& up,
                       glm::vec3& look_dir) {
  look_dir = -glm::normalize(sun_dir);
  up = glm::vec3(0.0f, 1.0f, 0.0f);
  if (std::abs(glm::dot(look_dir, up)) > 0.99f) up = glm::vec3(0.0f, 0.0f, 1.0f);
  right = glm::normalize(glm::cross(up, look_dir));
  up = glm::normalize(glm::cross(look_dir, right));
}

// Shifts `p` along unit `axis` so dot(axis, p) / t_size has fractional part
// `frac_target`. Because right/up/look_dir are mutually orthonormal,
// shifting along one axis leaves the other two axes' projections
// unperturbed -- so calling this once per axis lands all three at the
// target fraction simultaneously.
void SnapFraction(glm::vec3& p, const glm::vec3& axis, float t_size, float frac_target) {
  const float c = glm::dot(axis, p);
  const float cur_frac = c / t_size - std::floor(c / t_size);
  p += axis * ((frac_target - cur_frac) * t_size);
}

}  // namespace

TEST_CASE("Shadow Test 2: sub-texel snapping stability") {
  struct ConfigCase {
    uint32_t r_sm;
    float d_max;
  };
  const std::vector<ConfigCase> configs = {{512, 100.0f}, {2048, 200.0f}};
  const glm::vec3 sun_dir = glm::normalize(glm::vec3(0.3f, 1.0f, -0.6f));
  const float backward_extension = 100.0f;

  for (const ConfigCase& cfg : configs) {
    DYNAMIC_SECTION("r_sm=" << cfg.r_sm << " d_max=" << cfg.d_max) {
      const float t_size = cfg.d_max / static_cast<float>(cfg.r_sm);
      glm::vec3 right, up, look_dir;
      ComputeLightBasis(sun_dir, right, up, look_dir);

      // Arbitrary starting point, snapped so all three projected
      // coordinates sit at fractional 0.25 -- well inside a bin.
      glm::vec3 center0(123.456f, -789.012f, 345.678f);
      SnapFraction(center0, right, t_size, 0.25f);
      SnapFraction(center0, up, t_size, 0.25f);
      SnapFraction(center0, look_dir, t_size, 0.25f);

      ShadowMap map0;
      map0.UpdateLightMatrices(sun_dir, center0, cfg.d_max, cfg.r_sm, backward_extension);
      const glm::mat4 m0 = map0.GetLightViewProj();

      struct AxisCase {
        const char* name;
        glm::vec3 axis;
      };
      const std::vector<AxisCase> axis_cases = {{"right", right}, {"up", up}};

      for (const AxisCase& ac : axis_cases) {
        DYNAMIC_SECTION("axis=" << ac.name) {
          // Within-bin move: frac 0.25 -> 0.75, same T_size cell ->
          // byte-identical matrix.
          const glm::vec3 center1 = center0 + ac.axis * (0.5f * t_size);
          ShadowMap map1;
          map1.UpdateLightMatrices(sun_dir, center1, cfg.d_max, cfg.r_sm, backward_extension);
          const glm::mat4 m1 = map1.GetLightViewProj();
          CHECK(std::memcmp(&m0, &m1, sizeof(glm::mat4)) == 0);

          // Boundary-crossing move: frac 0.25 -> 1.35, crosses exactly one
          // bin boundary.
          const glm::vec3 center2 = center0 + ac.axis * (1.1f * t_size);
          ShadowMap map2;
          map2.UpdateLightMatrices(sun_dir, center2, cfg.d_max, cfg.r_sm, backward_extension);
          const glm::mat4 m2 = map2.GetLightViewProj();
          CHECK(std::memcmp(&m0, &m2, sizeof(glm::mat4)) != 0);

          // The snapped light-space translation must have shifted by
          // EXACTLY one T_size (not 0, not 2*T_size): transform the fixed
          // world origin through each light_view and measure the delta.
          const glm::vec3 origin0 = glm::vec3(map0.GetLightView() * glm::vec4(0, 0, 0, 1));
          const glm::vec3 origin2 = glm::vec3(map2.GetLightView() * glm::vec4(0, 0, 0, 1));
          const float shift = glm::length(origin2 - origin0);
          CHECK_THAT(shift, Catch::Matchers::WithinAbs(t_size, t_size * 0.01f));
        }
      }
    }
  }
}
