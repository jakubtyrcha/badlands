// Pure-CPU tests for the projected-decal math (engine/rendering/decal_math.hpp),
// the CPU mirror of shaders/passes/decals.wesl. These pin the SDFs, the
// perimeter parameterisation and the dash pattern with exact expected values,
// so the shader transcription has an oracle to be checked against (and so the
// hardest piece -- the rounded-rect arc-length walk -- is verified without a
// GPU in the loop).

#include <catch_amalgamated.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "engine/rendering/decal_math.hpp"
#include "engine/rendering/projected_decal.hpp"

using namespace badlands::decal_math;
using badlands::DecalShape;
using badlands::ProjectedDecal;

namespace {
constexpr float kEps = 1e-4f;
}  // namespace

// === Ring SDF ==============================================================

TEST_CASE("RingOutlineDistance: zero on the circle, grows either side") {
  const float r = 3.0f;
  // On the circle, in each quadrant.
  CHECK(RingOutlineDistance({3.0f, 0.0f}, r) == Catch::Approx(0.0f).margin(kEps));
  CHECK(RingOutlineDistance({0.0f, 3.0f}, r) == Catch::Approx(0.0f).margin(kEps));
  CHECK(RingOutlineDistance({-3.0f, 0.0f}, r) == Catch::Approx(0.0f).margin(kEps));
  const float diag = 3.0f / std::sqrt(2.0f);
  CHECK(RingOutlineDistance({diag, diag}, r) == Catch::Approx(0.0f).margin(kEps));

  // Inside and outside are both positive (unsigned outline distance).
  CHECK(RingOutlineDistance({1.0f, 0.0f}, r) == Catch::Approx(2.0f).margin(kEps));
  CHECK(RingOutlineDistance({5.0f, 0.0f}, r) == Catch::Approx(2.0f).margin(kEps));
  // The centre is `radius` away from the ring.
  CHECK(RingOutlineDistance({0.0f, 0.0f}, r) == Catch::Approx(3.0f).margin(kEps));
}

// === Rounded-rect SDF ======================================================

TEST_CASE("RoundedRectSignedDistance: sign and boundary zeros") {
  const glm::vec2 he{4.0f, 2.0f};
  const float r = 0.5f;

  // Edge midpoints lie exactly on the boundary.
  CHECK(RoundedRectSignedDistance({4.0f, 0.0f}, he, r) ==
        Catch::Approx(0.0f).margin(kEps));
  CHECK(RoundedRectSignedDistance({0.0f, 2.0f}, he, r) ==
        Catch::Approx(0.0f).margin(kEps));
  CHECK(RoundedRectSignedDistance({-4.0f, 0.0f}, he, r) ==
        Catch::Approx(0.0f).margin(kEps));
  CHECK(RoundedRectSignedDistance({0.0f, -2.0f}, he, r) ==
        Catch::Approx(0.0f).margin(kEps));

  // A point on the +X+Y corner arc: centre (he - r) offset by r at 45 degrees.
  const glm::vec2 corner = he - glm::vec2(r);
  const float k = r / std::sqrt(2.0f);
  CHECK(RoundedRectSignedDistance(corner + glm::vec2(k, k), he, r) ==
        Catch::Approx(0.0f).margin(kEps));

  // Interior is negative, exterior positive.
  CHECK(RoundedRectSignedDistance({0.0f, 0.0f}, he, r) < 0.0f);
  CHECK(RoundedRectSignedDistance({4.5f, 0.0f}, he, r) ==
        Catch::Approx(0.5f).margin(kEps));
  CHECK(RoundedRectSignedDistance({0.0f, 3.0f}, he, r) ==
        Catch::Approx(1.0f).margin(kEps));
}

TEST_CASE("RoundedRectSignedDistance: radius 0 is a sharp box") {
  const glm::vec2 he{2.0f, 1.0f};
  CHECK(RoundedRectSignedDistance({2.0f, 0.0f}, he, 0.0f) ==
        Catch::Approx(0.0f).margin(kEps));
  CHECK(RoundedRectSignedDistance({2.0f, 1.0f}, he, 0.0f) ==
        Catch::Approx(0.0f).margin(kEps));  // sharp corner
  // (3,1) is 1 unit outside in x and exactly ON the boundary in y, so the
  // nearest boundary point is the corner (2,1) and the distance is 1 -- not a
  // diagonal.
  CHECK(RoundedRectSignedDistance({3.0f, 1.0f}, he, 0.0f) ==
        Catch::Approx(1.0f).margin(kEps));
  // Genuinely diagonal from the sharp corner.
  CHECK(RoundedRectSignedDistance({3.0f, 2.0f}, he, 0.0f) ==
        Catch::Approx(std::sqrt(2.0f)).margin(kEps));
}

TEST_CASE("RoundedRectSignedDistance: corner radius clamps to the half-size") {
  // An over-large radius must degrade to a capsule/circle, not go negative.
  const glm::vec2 he{2.0f, 2.0f};
  CHECK(RoundedRectSignedDistance({2.0f, 0.0f}, he, 10.0f) ==
        Catch::Approx(0.0f).margin(kEps));
  CHECK(RoundedRectOutlineDistance({0.0f, 2.0f}, he, 10.0f) ==
        Catch::Approx(0.0f).margin(kEps));
}

// === Perimeters ============================================================

TEST_CASE("Perimeter formulas") {
  CHECK(RingPerimeter(2.0f) == Catch::Approx(kTwoPi * 2.0f).margin(kEps));

  const glm::vec2 he{4.0f, 2.0f};
  const float r = 0.5f;
  // 4*b.x + 4*b.y + 2*pi*r, b = he - r.
  const float expected = 4.0f * 3.5f + 4.0f * 1.5f + kTwoPi * 0.5f;
  CHECK(RoundedRectPerimeter(he, r) == Catch::Approx(expected).margin(kEps));

  // Sharp box: exactly the rectangle perimeter.
  CHECK(RoundedRectPerimeter(he, 0.0f) == Catch::Approx(24.0f).margin(kEps));
  // Fully rounded square: a circle.
  CHECK(RoundedRectPerimeter({2.0f, 2.0f}, 2.0f) ==
        Catch::Approx(kTwoPi * 2.0f).margin(kEps));
}

// === Ring parameterisation =================================================

TEST_CASE("RingPerimeterParam: CCW arc length from +X") {
  const float r = 2.0f;
  CHECK(RingPerimeterParam({2.0f, 0.0f}, r) == Catch::Approx(0.0f).margin(kEps));
  CHECK(RingPerimeterParam({0.0f, 2.0f}, r) ==
        Catch::Approx(kHalfPi * r).margin(kEps));
  CHECK(RingPerimeterParam({-2.0f, 0.0f}, r) ==
        Catch::Approx(kPi * r).margin(kEps));
  // Just below +X wraps to nearly the full circumference.
  CHECK(RingPerimeterParam({2.0f, -0.0001f}, r) ==
        Catch::Approx(kTwoPi * r).margin(1e-3f));
  // Independent of distance from the centre (it is an angle * radius).
  CHECK(RingPerimeterParam({0.0f, 10.0f}, r) ==
        Catch::Approx(kHalfPi * r).margin(kEps));
}

// === Rounded-rect parameterisation (the hard one) ==========================

TEST_CASE("RoundedRectPerimeterParam: segment boundaries are the cumulative lengths") {
  const glm::vec2 he{4.0f, 2.0f};
  const float r = 0.5f;
  const glm::vec2 b = he - glm::vec2(r);  // (3.5, 1.5)
  const float arc = r * kHalfPi;
  const float P = RoundedRectPerimeter(he, r);

  auto S = [&](glm::vec2 p) {
    return RoundedRectPerimeterParam(p, he, r);
  };

  // s = 0 at the middle of the +X edge.
  CHECK(S({he.x, 0.0f}) == Catch::Approx(0.0f).margin(kEps));
  // End of the +X edge upper half == b.y.
  CHECK(S({he.x, b.y}) == Catch::Approx(b.y).margin(kEps));
  // End of the +X+Y arc == b.y + arc (top of the arc, straight up from the
  // corner centre).
  CHECK(S({b.x, he.y}) == Catch::Approx(b.y + arc).margin(kEps));
  // End of the +Y edge == b.y + arc + 2*b.x.
  CHECK(S({-b.x, he.y}) == Catch::Approx(b.y + arc + 2.0f * b.x).margin(kEps));
  // End of the -X+Y arc.
  CHECK(S({-he.x, b.y}) ==
        Catch::Approx(b.y + 2.0f * arc + 2.0f * b.x).margin(kEps));
  // End of the -X edge.
  CHECK(S({-he.x, -b.y}) ==
        Catch::Approx(3.0f * b.y + 2.0f * arc + 2.0f * b.x).margin(kEps));
  // End of the -X-Y arc.
  CHECK(S({-b.x, -he.y}) ==
        Catch::Approx(3.0f * b.y + 3.0f * arc + 2.0f * b.x).margin(kEps));
  // End of the -Y edge.
  CHECK(S({b.x, -he.y}) ==
        Catch::Approx(3.0f * b.y + 3.0f * arc + 4.0f * b.x).margin(kEps));
  // End of the +X-Y arc == start of the +X edge lower half.
  CHECK(S({he.x, -b.y}) ==
        Catch::Approx(3.0f * b.y + 4.0f * arc + 4.0f * b.x).margin(kEps));
  // ...which is also P - b.y, closing the loop back to s = 0.
  CHECK(S({he.x, -b.y}) == Catch::Approx(P - b.y).margin(kEps));
}

TEST_CASE("RoundedRectPerimeterParam: monotonic CCW around the outline") {
  const glm::vec2 he{3.0f, 2.0f};
  const float r = 0.6f;
  const float P = RoundedRectPerimeter(he, r);

  // Walk the OUTSIDE of the shape by casting rays from the centre through
  // increasing angles: the parameter must increase monotonically, since both
  // the walk and the parameterisation are CCW from +X.
  float prev = -1.0f;
  const int kSteps = 720;
  for (int i = 0; i < kSteps; ++i) {
    const float angle = kTwoPi * (static_cast<float>(i) + 0.5f) /
                        static_cast<float>(kSteps);
    // A point comfortably outside the shape along this ray.
    const glm::vec2 p{std::cos(angle) * 20.0f, std::sin(angle) * 20.0f};
    const float s = RoundedRectPerimeterParam(p, he, r);
    CHECK(s >= 0.0f);
    CHECK(s <= P + kEps);
    CHECK(s > prev);
    prev = s;
  }
}

TEST_CASE("RoundedRectPerimeterParam: sharp box (r = 0) still walks the rectangle") {
  const glm::vec2 he{2.0f, 1.0f};
  const float P = RoundedRectPerimeter(he, 0.0f);
  REQUIRE(P == Catch::Approx(12.0f).margin(kEps));

  auto S = [&](glm::vec2 p) { return RoundedRectPerimeterParam(p, he, 0.0f); };
  CHECK(S({2.0f, 0.0f}) == Catch::Approx(0.0f).margin(kEps));
  CHECK(S({2.0f, 1.0f}) == Catch::Approx(1.0f).margin(kEps));   // +X+Y corner
  CHECK(S({0.0f, 1.0f}) == Catch::Approx(3.0f).margin(kEps));   // mid +Y edge
  CHECK(S({-2.0f, 1.0f}) == Catch::Approx(5.0f).margin(kEps));  // -X+Y corner
  CHECK(S({-2.0f, 0.0f}) == Catch::Approx(6.0f).margin(kEps));  // mid -X edge
  CHECK(S({0.0f, -1.0f}) == Catch::Approx(9.0f).margin(kEps));  // mid -Y edge
  CHECK(S({2.0f, -1.0f}) == Catch::Approx(11.0f).margin(kEps)); // +X-Y corner
}

TEST_CASE("RoundedRectPerimeterParam: interior returns 0") {
  CHECK(RoundedRectPerimeterParam({0.0f, 0.0f}, {4.0f, 2.0f}, 0.5f) ==
        Catch::Approx(0.0f).margin(kEps));
}

// === Dash pattern ==========================================================

TEST_CASE("FitDashPeriod: ties round half UP, matching WGSL floor(x + 0.5)") {
  // The mirror and the shader must agree on exact .5 repeat counts. WGSL's
  // round() breaks ties to EVEN and std::round breaks them AWAY FROM ZERO, so
  // both sides use floor(x + 0.5) -- which always rounds a tie up. Pin both
  // parities: an even-tie (6.5) and an odd-tie (7.5) must both go up, which is
  // exactly where round-half-to-even would have disagreed.
  //
  // perimeter 13, period 2 -> 6.5 repeats -> 7 (round-to-even would give 6).
  CHECK(13.0f / FitDashPeriod(2.0f, 13.0f) == Catch::Approx(7.0f).margin(kEps));
  // perimeter 15, period 2 -> 7.5 repeats -> 8 (round-to-even also gives 8).
  CHECK(15.0f / FitDashPeriod(2.0f, 15.0f) == Catch::Approx(8.0f).margin(kEps));
}

TEST_CASE("FitDashPeriod: divides the perimeter exactly") {
  // 10 / 3 -> nearest whole repeat count is 3, period 10/3.
  const float p = FitDashPeriod(3.0f, 10.0f);
  const float n = 10.0f / p;
  CHECK(n == Catch::Approx(std::round(n)).margin(kEps));
  CHECK(p == Catch::Approx(10.0f / 3.0f).margin(kEps));

  // Already an exact divisor: unchanged.
  CHECK(FitDashPeriod(2.5f, 10.0f) == Catch::Approx(2.5f).margin(kEps));

  // A period longer than the whole curve collapses to exactly one repeat.
  CHECK(FitDashPeriod(100.0f, 10.0f) == Catch::Approx(10.0f).margin(kEps));

  // Degenerate inputs pass through.
  CHECK(FitDashPeriod(0.0f, 10.0f) == Catch::Approx(0.0f).margin(kEps));
  CHECK(FitDashPeriod(3.0f, 0.0f) == Catch::Approx(3.0f).margin(kEps));
}

TEST_CASE("DashDuty") {
  CHECK(DashDuty(1.0f, 1.0f) == Catch::Approx(0.5f).margin(kEps));
  CHECK(DashDuty(3.0f, 1.0f) == Catch::Approx(0.75f).margin(kEps));
  CHECK(DashDuty(0.0f, 1.0f) == Catch::Approx(0.0f).margin(kEps));
  CHECK(DashDuty(1.0f, 0.0f) == Catch::Approx(1.0f).margin(kEps));
  CHECK(DashDuty(0.0f, 0.0f) == Catch::Approx(1.0f).margin(kEps));  // degenerate
}

TEST_CASE("IsDashA: alternates on the period, honours duty") {
  const float period = 2.0f;
  const float duty = 0.5f;  // 1 unit dash, 1 unit gap
  const float t = 0.0f;
  const float scroll = 0.0f;

  CHECK(IsDashA(0.0f, period, duty, t, scroll));
  CHECK(IsDashA(0.9f, period, duty, t, scroll));
  CHECK_FALSE(IsDashA(1.1f, period, duty, t, scroll));
  CHECK_FALSE(IsDashA(1.9f, period, duty, t, scroll));
  CHECK(IsDashA(2.1f, period, duty, t, scroll));  // next period

  // Negative arc length still lands correctly (fract handles the wrap).
  CHECK_FALSE(IsDashA(-0.5f, period, duty, t, scroll));

  // Solid-line degenerate cases.
  CHECK(IsDashA(1.5f, period, 1.0f, t, scroll));
  CHECK_FALSE(IsDashA(0.5f, period, 0.0f, t, scroll));
}

TEST_CASE("IsDashA: scrolling shifts the pattern by whole periods per second") {
  const float period = 2.0f;
  const float duty = 0.5f;
  const float scroll = 1.0f;  // one period per second

  // After exactly one second the pattern has advanced a full period, so it is
  // identical to t = 0.
  for (float s = 0.0f; s < 4.0f; s += 0.25f) {
    CHECK(IsDashA(s, period, duty, 0.0f, scroll) ==
          IsDashA(s, period, duty, 1.0f, scroll));
  }
  // Half a second inverts it (a half-period shift with a 50% duty).
  CHECK(IsDashA(0.25f, period, duty, 0.0f, scroll) !=
        IsDashA(0.25f, period, duty, 0.5f, scroll));
}

// === Coverage ==============================================================

TEST_CASE("OutlineCoverage: 1 on the line, 0 well outside it") {
  const float half_width = 0.5f;
  const float aa = 0.05f;
  CHECK(OutlineCoverage(0.0f, half_width, aa) ==
        Catch::Approx(1.0f).margin(kEps));
  CHECK(OutlineCoverage(0.4f, half_width, aa) ==
        Catch::Approx(1.0f).margin(kEps));
  CHECK(OutlineCoverage(half_width, half_width, aa) ==
        Catch::Approx(0.5f).margin(kEps));  // midpoint of the AA band
  CHECK(OutlineCoverage(0.6f, half_width, aa) ==
        Catch::Approx(0.0f).margin(kEps));
  CHECK(OutlineCoverage(10.0f, half_width, aa) ==
        Catch::Approx(0.0f).margin(kEps));

  // Monotonically decreasing across the band.
  float prev = 2.0f;
  for (float d = 0.0f; d <= 1.0f; d += 0.05f) {
    const float c = OutlineCoverage(d, half_width, aa);
    CHECK(c <= prev + kEps);
    prev = c;
  }
}

TEST_CASE("OutlineCoverage: zero AA does not divide by zero") {
  CHECK(OutlineCoverage(0.0f, 0.5f, 0.0f) == Catch::Approx(1.0f).margin(kEps));
  CHECK(OutlineCoverage(1.0f, 0.5f, 0.0f) == Catch::Approx(0.0f).margin(kEps));
}

// === Scissor bounds ========================================================
//
// This is the one piece of decal code whose failure mode is "the highlight
// silently vanishes", so the off-screen / edge-straddling / behind-the-camera
// cases are pinned here rather than left to the GPU test (which only ever
// renders decals near the centre of the view, the case a scissor cannot break).

namespace {

// A camera 20 units above the origin looking straight down, +Z on screen-up.
// Deliberately built from the same matrices the renderer feeds the scissor.
glm::mat4 TopDownViewProj(glm::vec3 eye = glm::vec3(0.0f, 20.0f, 0.0f)) {
  const glm::mat4 view =
      glm::lookAt(eye, eye + glm::vec3(0.0f, -1.0f, 0.0f), glm::vec3(0, 0, -1));
  glm::mat4 proj = glm::perspective(glm::radians(60.0f), 1.0f, 0.1f, 1000.0f);
  // Reversed-Z remap, matching Camera::GetProj.
  glm::mat4 remap(1.0f);
  remap[2][2] = -1.0f;
  remap[3][2] = 1.0f;
  return remap * proj * view;
}

ProjectedDecal RingAt(glm::vec3 center, float radius) {
  ProjectedDecal d;
  d.shape = DecalShape::Ring;
  d.center = center;
  d.half_extents = glm::vec2(radius);
  d.line_width = 0.2f;
  d.projector_half_height = 1.0f;
  return d;
}

}  // namespace

TEST_CASE("ComputeDecalScissor: rejects degenerate inputs") {
  ScissorRect r;
  const ProjectedDecal d = RingAt(glm::vec3(0.0f), 2.0f);
  const glm::mat4 vp = TopDownViewProj();

  CHECK_FALSE(ComputeDecalScissor(nullptr, 1, vp, 800, 600, r));
  CHECK_FALSE(ComputeDecalScissor(&d, 0, vp, 800, 600, r));
  CHECK_FALSE(ComputeDecalScissor(&d, 1, vp, 0, 600, r));
  CHECK_FALSE(ComputeDecalScissor(&d, 1, vp, 800, 0, r));
}

TEST_CASE("ComputeDecalScissor: a centred decal yields a small centred rect") {
  ScissorRect r;
  const ProjectedDecal d = RingAt(glm::vec3(0.0f), 2.0f);
  REQUIRE(ComputeDecalScissor(&d, 1, TopDownViewProj(), 800, 800, r));

  // Well inside the viewport, and much smaller than it -- the whole point.
  CHECK(r.width > 0);
  CHECK(r.height > 0);
  CHECK(r.width < 800);
  CHECK(r.height < 800);
  CHECK(r.x + r.width <= 800);
  CHECK(r.y + r.height <= 800);
  // Straddles the screen centre.
  CHECK(r.x < 400);
  CHECK(r.x + r.width > 400);
  CHECK(r.y < 400);
  CHECK(r.y + r.height > 400);
}

TEST_CASE("ComputeDecalScissor: never exceeds the attachment") {
  // A decal far larger than the view must clamp, not overflow (SetScissorRect
  // validation rejects x + width > attachment width).
  ScissorRect r;
  const ProjectedDecal d = RingAt(glm::vec3(0.0f), 500.0f);
  REQUIRE(ComputeDecalScissor(&d, 1, TopDownViewProj(), 800, 600, r));
  CHECK(r.x + r.width <= 800);
  CHECK(r.y + r.height <= 600);
  // It should cover essentially everything.
  CHECK(r.width == 800);
  CHECK(r.height == 600);
}

TEST_CASE("ComputeDecalScissor: a fully off-screen decal collapses to empty") {
  // Far off to one side but still in front of the camera: the caller reads a
  // zero-size rect as "nothing to draw". This must NOT be confused with the
  // false return, which means "shade everything".
  ScissorRect r;
  const ProjectedDecal d = RingAt(glm::vec3(4000.0f, 0.0f, 0.0f), 2.0f);
  REQUIRE(ComputeDecalScissor(&d, 1, TopDownViewProj(), 800, 800, r));
  CHECK((r.width == 0 || r.height == 0));
}

TEST_CASE("ComputeDecalScissor: a decal straddling an edge keeps the on-screen part") {
  ScissorRect r;
  const glm::mat4 vp = TopDownViewProj();
  // Ground half-extent visible at height 20 with a 60-degree vertical fov is
  // 20*tan(30) ~= 11.5 world units; put a big decal centred beyond the left
  // edge so only its right side is visible.
  const ProjectedDecal d = RingAt(glm::vec3(-11.0f, 0.0f, 0.0f), 4.0f);
  REQUIRE(ComputeDecalScissor(&d, 1, vp, 800, 800, r));
  CHECK(r.width > 0);
  CHECK(r.height > 0);
  CHECK(r.x == 0);  // clamped at the left edge
  CHECK(r.x + r.width < 800);  // but not spanning the whole screen
}

TEST_CASE("ComputeDecalScissor: bails out when the box spans the eye plane") {
  // Camera INSIDE the decal's projector box: some corners land at/behind the
  // eye, where the perspective divide flips and a screen AABB is meaningless.
  // Must return false (shade everything), never a wrong rect.
  ScissorRect r;
  ProjectedDecal d = RingAt(glm::vec3(0.0f, 20.0f, 0.0f), 5.0f);
  d.projector_half_height = 10.0f;  // box spans y 10..30, camera sits at y=20
  CHECK_FALSE(ComputeDecalScissor(&d, 1, TopDownViewProj(), 800, 800, r));
}

TEST_CASE("ComputeDecalScissor: the rect covers every decal in the set") {
  ScissorRect r;
  const ProjectedDecal decals[2] = {RingAt(glm::vec3(-6.0f, 0.0f, 0.0f), 1.0f),
                                    RingAt(glm::vec3(6.0f, 0.0f, 0.0f), 1.0f)};
  const glm::mat4 vp = TopDownViewProj();
  REQUIRE(ComputeDecalScissor(decals, 2, vp, 800, 800, r));

  // The union must contain each decal's own rect.
  for (const ProjectedDecal& d : decals) {
    ScissorRect one;
    REQUIRE(ComputeDecalScissor(&d, 1, vp, 800, 800, one));
    CHECK(one.x >= r.x);
    CHECK(one.y >= r.y);
    CHECK(one.x + one.width <= r.x + r.width);
    CHECK(one.y + one.height <= r.y + r.height);
  }
}
