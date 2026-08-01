#include <catch_amalgamated.hpp>

#include <cmath>
#include <vector>

#include "mapgen/river_arcs.hpp"

using namespace badlands::mapgen;

namespace {

constexpr float kPi = 3.14159265358979323846f;

// Samples of a circle of radius r centred at the origin, CCW from angle 0.
// CCW means LEFT-turning, so every arc fitted to these must come out with
// POSITIVE curvature -- the sign convention is part of what is under test.
std::vector<glm::vec2> circle_points(float r, float sweep_rad, int n) {
  std::vector<glm::vec2> pts;
  pts.reserve(n);
  for (int i = 0; i < n; ++i) {
    const float a = sweep_rad * static_cast<float>(i) / static_cast<float>(n - 1);
    pts.push_back(glm::vec2(r * std::cos(a), r * std::sin(a)));
  }
  return pts;
}

// The exact circle tangents at those samples (CCW => tangent is the left
// normal of the radius).
std::vector<glm::vec2> circle_tangents(float sweep_rad, int n) {
  std::vector<glm::vec2> t;
  t.reserve(n);
  for (int i = 0; i < n; ++i) {
    const float a = sweep_rad * static_cast<float>(i) / static_cast<float>(n - 1);
    t.push_back(glm::vec2(-std::sin(a), std::cos(a)));
  }
  return t;
}

// Worst distance from any input point to the fitted chain. This is the number
// the tolerance argument is a promise about.
float chain_deviation_m(const RiverArcChain& c,
                        const std::vector<glm::vec2>& pts) {
  float worst = 0.0f;
  for (const glm::vec2& p : pts) {
    float best = 1e30f;
    for (const RiverArc& a : c.arcs) best = std::min(best, arc_distance_m(a, p));
    worst = std::max(worst, best);
  }
  return worst;
}

}  // namespace

// --- arc evaluation ---------------------------------------------------------

TEST_CASE("arc evaluation is consistent with its own endpoints", "[river_arcs]") {
  // A quarter circle of radius 10 starting at (10,0) heading +Y.
  RiverArc a;
  a.p0 = glm::vec2(10.0f, 0.0f);
  a.t0 = glm::vec2(0.0f, 1.0f);
  a.curvature_1_m = 1.0f / 10.0f;
  a.length_m = 10.0f * kPi * 0.5f;
  a.p1 = glm::vec2(0.0f, 10.0f);

  CHECK(arc_point(a, 0.0f).x == Catch::Approx(10.0f).margin(1e-4));
  CHECK(arc_point(a, 0.0f).y == Catch::Approx(0.0f).margin(1e-4));
  CHECK(arc_point(a, a.length_m).x == Catch::Approx(0.0f).margin(1e-3));
  CHECK(arc_point(a, a.length_m).y == Catch::Approx(10.0f).margin(1e-3));

  CHECK(arc_tangent(a, 0.0f).y == Catch::Approx(1.0f).margin(1e-5));
  // A quarter turn left: +Y becomes -X.
  CHECK(arc_tangent(a, a.length_m).x == Catch::Approx(-1.0f).margin(1e-3));

  CHECK(arc_radius_m(a) == Catch::Approx(10.0f));
  CHECK(glm::length(arc_centre(a)) == Catch::Approx(0.0f).margin(1e-4));

  // Unit speed: the parameter IS arc length, which is the whole reason the
  // representation is worth having.
  const float step = glm::length(arc_point(a, 1.0f) - arc_point(a, 0.0f));
  CHECK(step == Catch::Approx(2.0f * 10.0f * std::sin(0.5f / 10.0f)).margin(1e-4));
}

TEST_CASE("a straight arc needs no special case at the call site",
          "[river_arcs]") {
  RiverArc a;
  a.p0 = glm::vec2(0.0f, 0.0f);
  a.p1 = glm::vec2(30.0f, 0.0f);
  a.t0 = glm::vec2(1.0f, 0.0f);
  a.curvature_1_m = 0.0f;
  a.length_m = 30.0f;

  CHECK(arc_point(a, 15.0f).x == Catch::Approx(15.0f));
  CHECK(arc_tangent(a, 15.0f).x == Catch::Approx(1.0f));
  CHECK(std::isinf(arc_radius_m(a)));
  CHECK(arc_distance_m(a, glm::vec2(15.0f, 4.0f)) == Catch::Approx(4.0f));
  // Past the end, the nearest point is the endpoint -- not the infinite line.
  CHECK(arc_distance_m(a, glm::vec2(40.0f, 0.0f)) == Catch::Approx(10.0f));
}

TEST_CASE("arc distance is bounded by the sweep, not the whole circle",
          "[river_arcs]") {
  // Quarter circle, radius 10, first quadrant.
  RiverArc a;
  a.p0 = glm::vec2(10.0f, 0.0f);
  a.t0 = glm::vec2(0.0f, 1.0f);
  a.curvature_1_m = 0.1f;
  a.length_m = 10.0f * kPi * 0.5f;
  a.p1 = glm::vec2(0.0f, 10.0f);

  // On the arc.
  const glm::vec2 mid(10.0f * std::cos(kPi * 0.25f), 10.0f * std::sin(kPi * 0.25f));
  CHECK(arc_distance_m(a, mid) == Catch::Approx(0.0f).margin(1e-3));
  // Radially outside it.
  CHECK(arc_distance_m(a, mid * 1.5f) == Catch::Approx(5.0f).margin(1e-3));
  // On the CIRCLE but outside the swept quadrant: the answer must be the
  // distance to the nearer endpoint, not zero.
  const glm::vec2 behind(-10.0f, 0.0f);
  CHECK(arc_distance_m(a, behind) == Catch::Approx(glm::length(behind - a.p1)));
}

// --- biarc fitting ----------------------------------------------------------

TEST_CASE("a biarc interpolates its endpoints and tangents", "[river_arcs]") {
  const glm::vec2 p0(0.0f, 0.0f), p1(100.0f, 40.0f);
  const glm::vec2 t0 = glm::normalize(glm::vec2(1.0f, 0.0f));
  const glm::vec2 t1 = glm::normalize(glm::vec2(1.0f, 1.0f));
  const std::vector<RiverArc> arcs = fit_biarc(p0, t0, p1, t1);
  REQUIRE(arcs.size() == 2);

  CHECK(glm::length(arcs.front().p0 - p0) == Catch::Approx(0.0f).margin(1e-4));
  CHECK(glm::length(arcs.back().p1 - p1) == Catch::Approx(1e-4).margin(1e-2));
  CHECK(glm::dot(arcs.front().t0, t0) == Catch::Approx(1.0f).margin(1e-5));
  const glm::vec2 end_t = arc_tangent(arcs.back(), arcs.back().length_m);
  CHECK(glm::dot(end_t, t1) == Catch::Approx(1.0f).margin(1e-4));
}

TEST_CASE("a biarc is G1 at its joint", "[river_arcs]") {
  // The property the whole representation rests on: the two halves meet with a
  // shared position AND heading, so a chain of them has no visible kinks.
  const glm::vec2 p0(0.0f, 0.0f), p1(80.0f, -25.0f);
  const glm::vec2 t0 = glm::normalize(glm::vec2(1.0f, 0.5f));
  const glm::vec2 t1 = glm::normalize(glm::vec2(1.0f, -0.9f));
  const std::vector<RiverArc> arcs = fit_biarc(p0, t0, p1, t1);
  REQUIRE(arcs.size() == 2);

  const glm::vec2 end0 = arc_point(arcs[0], arcs[0].length_m);
  CHECK(glm::length(end0 - arcs[1].p0) == Catch::Approx(0.0f).margin(1e-3));
  const glm::vec2 tan0 = arc_tangent(arcs[0], arcs[0].length_m);
  CHECK(glm::dot(tan0, arcs[1].t0) == Catch::Approx(1.0f).margin(1e-5));
}

TEST_CASE("a configuration that is already one arc stays one arc",
          "[river_arcs]") {
  // Endpoints and tangents taken off a real circle: emitting a biarc here would
  // insert a joint that carries no information and split the curvature in two.
  const float r = 25.0f;
  const glm::vec2 p0(r, 0.0f), t0(0.0f, 1.0f);
  const float th = 1.1f;
  const glm::vec2 p1(r * std::cos(th), r * std::sin(th));
  const glm::vec2 t1(-std::sin(th), std::cos(th));

  const std::vector<RiverArc> arcs = fit_biarc(p0, t0, p1, t1);
  REQUIRE(arcs.size() == 1);
  CHECK(arcs[0].curvature_1_m == Catch::Approx(1.0f / r).margin(1e-4));
  CHECK(arcs[0].length_m == Catch::Approx(r * th).margin(1e-3));
}

TEST_CASE("curvature sign follows the turn direction", "[river_arcs]") {
  // Left-positive. A renderer offsetting banks, and a meander-migration pass
  // deciding which bank erodes, both depend on this sign.
  const std::vector<RiverArc> left =
      fit_biarc(glm::vec2(0.0f), glm::vec2(1.0f, 0.0f), glm::vec2(50.0f, 50.0f),
                glm::normalize(glm::vec2(0.0f, 1.0f)));
  REQUIRE(!left.empty());
  CHECK(left.front().curvature_1_m > 0.0f);

  const std::vector<RiverArc> right =
      fit_biarc(glm::vec2(0.0f), glm::vec2(1.0f, 0.0f), glm::vec2(50.0f, -50.0f),
                glm::normalize(glm::vec2(0.0f, -1.0f)));
  REQUIRE(!right.empty());
  CHECK(right.front().curvature_1_m < 0.0f);
}

// --- tangent estimation -----------------------------------------------------

TEST_CASE("polyline tangents recover the circle tangent, ends included",
          "[river_arcs]") {
  const float r = 40.0f, sweep = 1.2f;
  const int n = 9;
  const std::vector<glm::vec2> pts = circle_points(r, sweep, n);
  const std::vector<glm::vec2> want = circle_tangents(sweep, n);
  const std::vector<glm::vec2> got = polyline_tangents(pts);
  REQUIRE(got.size() == want.size());
  for (int i = 0; i < n; ++i)
    CHECK(glm::dot(got[i], want[i]) == Catch::Approx(1.0f).margin(1e-4));
}

// --- chain fitting ----------------------------------------------------------

TEST_CASE("a straight run collapses to a single arc", "[river_arcs]") {
  // The compression that makes arcs worth the fit: 50 collinear points are one
  // straight arc, not 49 segments or 98 half-arcs.
  std::vector<glm::vec2> pts;
  for (int i = 0; i < 50; ++i) pts.push_back(glm::vec2(6.0f * i, 0.0f));

  const RiverArcChain c = fit_arc_chain(pts, 0.5f);
  REQUIRE(c.arcs.size() == 1);
  CHECK(c.arcs[0].curvature_1_m == 0.0f);
  CHECK(c.length_m == Catch::Approx(6.0f * 49.0f).margin(1e-2));
  CHECK(c.arcs[0].param0 == Catch::Approx(0.0f));
  CHECK(c.arcs[0].param1 == Catch::Approx(1.0f));
}

TEST_CASE("a circular arc is reproduced with one curvature throughout",
          "[river_arcs]") {
  const float r = 60.0f, sweep = 1.5f;
  const int n = 21;
  const std::vector<glm::vec2> pts = circle_points(r, sweep, n);
  const RiverArcChain c =
      fit_arc_chain(pts, circle_tangents(sweep, n), 0.05f);

  REQUIRE(!c.arcs.empty());
  for (const RiverArc& a : c.arcs)
    CHECK(a.curvature_1_m == Catch::Approx(1.0f / r).margin(2e-4));
  CHECK(c.length_m == Catch::Approx(r * sweep).margin(0.05));
  // 21 points, one radius: the greedy span should swallow the lot.
  CHECK(c.arcs.size() <= 2);
}

TEST_CASE("the chain honours its tolerance and its endpoints", "[river_arcs]") {
  // A meandering polyline -- a sine, which is NOT a circle anywhere, so the fit
  // has to work rather than land exactly.
  std::vector<glm::vec2> pts;
  for (int i = 0; i <= 60; ++i) {
    const float x = 5.0f * static_cast<float>(i);
    pts.push_back(glm::vec2(x, 40.0f * std::sin(x / 60.0f)));
  }

  for (const float tol : {2.0f, 0.5f, 0.1f}) {
    const RiverArcChain c = fit_arc_chain(pts, tol);
    REQUIRE(!c.arcs.empty());
    INFO("tolerance " << tol << " arcs " << c.arcs.size());
    CHECK(chain_deviation_m(c, pts) <= tol);
    // Endpoints are interpolated, never approximated.
    CHECK(glm::length(c.arcs.front().p0 - pts.front()) ==
          Catch::Approx(0.0f).margin(1e-3));
    CHECK(glm::length(c.arcs.back().p1 - pts.back()) ==
          Catch::Approx(0.0f).margin(1e-3));
  }

  // Tighter tolerance must not need FEWER arcs.
  CHECK(fit_arc_chain(pts, 0.1f).arcs.size() >=
        fit_arc_chain(pts, 2.0f).arcs.size());
}

TEST_CASE("the chain is G1 across span boundaries too", "[river_arcs]") {
  std::vector<glm::vec2> pts;
  for (int i = 0; i <= 40; ++i) {
    const float x = 8.0f * static_cast<float>(i);
    pts.push_back(glm::vec2(x, 30.0f * std::sin(x / 45.0f) + 0.4f * x));
  }
  const RiverArcChain c = fit_arc_chain(pts, 0.25f);
  REQUIRE(c.arcs.size() > 2);

  for (size_t i = 1; i < c.arcs.size(); ++i) {
    INFO("joint " << i);
    const glm::vec2 prev_end = arc_point(c.arcs[i - 1], c.arcs[i - 1].length_m);
    CHECK(glm::length(prev_end - c.arcs[i].p0) ==
          Catch::Approx(0.0f).margin(1e-2));
    const glm::vec2 prev_t = arc_tangent(c.arcs[i - 1], c.arcs[i - 1].length_m);
    CHECK(glm::dot(prev_t, c.arcs[i].t0) == Catch::Approx(1.0f).margin(1e-3));
  }
}

TEST_CASE("chain params are monotone and span the reach", "[river_arcs]") {
  std::vector<glm::vec2> pts;
  for (int i = 0; i <= 30; ++i) {
    const float x = 7.0f * static_cast<float>(i);
    pts.push_back(glm::vec2(x, 20.0f * std::sin(x / 30.0f)));
  }
  const RiverArcChain c = fit_arc_chain(pts, 0.3f);
  REQUIRE(!c.arcs.empty());

  CHECK(c.arcs.front().param0 == Catch::Approx(0.0f).margin(1e-5));
  CHECK(c.arcs.back().param1 == Catch::Approx(1.0f).margin(1e-5));
  float last = -1.0f;
  for (const RiverArc& a : c.arcs) {
    CHECK(a.param0 >= last - 1e-5f);
    CHECK(a.param1 >= a.param0 - 1e-5f);
    last = a.param1;
  }
}

// --- attribute lookup -------------------------------------------------------

TEST_CASE("param lookup is arc length, not vertex index", "[river_arcs]") {
  // Unevenly spaced: 1 m then 9 m. The midpoint by arc length sits at 0.5, well
  // past the middle VERTEX -- indexing by vertex would put it at 0.1.
  const std::vector<glm::vec2> pts = {glm::vec2(0.0f, 0.0f),
                                      glm::vec2(1.0f, 0.0f),
                                      glm::vec2(10.0f, 0.0f)};
  const std::vector<float> u = polyline_params(pts);
  REQUIRE(u.size() == 3);
  CHECK(u[0] == Catch::Approx(0.0f));
  CHECK(u[1] == Catch::Approx(0.1f));
  CHECK(u[2] == Catch::Approx(1.0f));

  const std::vector<float> width = {0.0f, 1.0f, 10.0f};
  CHECK(sample_at_param(u, width, 0.0f) == Catch::Approx(0.0f));
  CHECK(sample_at_param(u, width, 0.1f) == Catch::Approx(1.0f));
  CHECK(sample_at_param(u, width, 0.55f) == Catch::Approx(5.5f));
  // Clamped, never extrapolated -- a width must not go negative off the end.
  CHECK(sample_at_param(u, width, -1.0f) == Catch::Approx(0.0f));
  CHECK(sample_at_param(u, width, 2.0f) == Catch::Approx(10.0f));
}

TEST_CASE("a degenerate polyline yields no arcs rather than NaNs",
          "[river_arcs]") {
  CHECK(fit_arc_chain({}, 1.0f).arcs.empty());
  CHECK(fit_arc_chain({glm::vec2(3.0f, 4.0f)}, 1.0f).arcs.empty());
  const std::vector<glm::vec2> same(5, glm::vec2(2.0f, 2.0f));
  const RiverArcChain c = fit_arc_chain(same, 1.0f);
  for (const RiverArc& a : c.arcs) {
    CHECK(std::isfinite(a.length_m));
    CHECK(std::isfinite(a.curvature_1_m));
  }
}

// --- graph level ------------------------------------------------------------

TEST_CASE("build_river_arcs skips geometry-free reaches and keeps edge ids",
          "[river_arcs]") {
  RiverGraph g;
  g.nodes.resize(4);
  // Edge 0: a real reach.
  RiverEdge e0;
  for (int i = 0; i <= 10; ++i)
    e0.points_m.push_back(glm::vec2(4.0f * i, 0.0f));
  g.edges.push_back(e0);
  // Edge 1: a lake connection -- topology only, no drawable geometry.
  g.edges.push_back(RiverEdge{});
  // Edge 2: another real reach.
  RiverEdge e2;
  for (int i = 0; i <= 10; ++i)
    e2.points_m.push_back(glm::vec2(0.0f, 4.0f * i));
  g.edges.push_back(e2);

  const std::vector<RiverArcChain> chains = build_river_arcs(g, 0.5f);
  REQUIRE(chains.size() == 2);
  CHECK(chains[0].edge == 0);
  CHECK(chains[1].edge == 2);  // NOT 1 -- the index is the edge, not the slot
  for (const RiverArcChain& c : chains) CHECK(c.length_m > 0.0f);
}

TEST_CASE("an end tangent never fights the direction of travel", "[river_arcs]") {
  // The angle extrapolation is exact on a circle and WRONG on a corner: it
  // doubles the chord-to-chord angle, so an L-bend two points in swung the
  // estimate 45 degrees off the way the polyline actually leaves, bowing the
  // end arc ~0.15-0.21 m off a 1 m segment. Nothing else caught it -- the end
  // arc interpolates both knots exactly, so the knot-distance fit metric reads
  // zero, and fit_arc_chain skips the deviation check on a one-segment span.
  const std::vector<std::vector<glm::vec2>> corners = {
      {{0, 0}, {1, 0}, {1, 1}, {1, 2}, {1, 3}},          // L at the start
      {{0, 0}, {1, 0}, {1, 1}, {2, 1}, {2, 2}, {3, 2}},  // staircase, both ends
      {{0, 0}, {2, 0}, {2, 2}},                          // the minimum case
  };
  for (const auto& pts : corners) {
    const size_t n = pts.size();
    INFO("polyline of " << n << " points");
    const std::vector<glm::vec2> t = polyline_tangents(pts);
    // Never more than 45 degrees off the segment it belongs to.
    CHECK(glm::dot(t.front(), glm::normalize(pts[1] - pts[0])) >= 0.707f);
    CHECK(glm::dot(t.back(), glm::normalize(pts[n - 1] - pts[n - 2])) >= 0.707f);

    // And the fitted chain stays near the polyline BETWEEN knots, which is
    // where the bow lived.
    const RiverArcChain c = fit_arc_chain(pts, 0.5f);
    REQUIRE(!c.arcs.empty());
    for (size_t i = 1; i < n; ++i) {
      const glm::vec2 mid = 0.5f * (pts[i - 1] + pts[i]);
      float best = 1e30f;
      for (const RiverArc& a : c.arcs) best = std::min(best, arc_distance_m(a, mid));
      INFO("segment " << i << " midpoint");
      CHECK(best <= 0.5f);
    }
  }
}

TEST_CASE("the circle case still uses the extrapolation", "[river_arcs]") {
  // The guard must not disable the thing it guards: on a genuine circle the
  // extrapolated end tangent is exact and well inside the 45-degree window, so
  // it still wins over the (half-angle-off) end segment.
  const float r = 40.0f, sweep = 1.2f;
  const int n = 9;
  std::vector<glm::vec2> pts;
  for (int i = 0; i < n; ++i) {
    const float a = sweep * static_cast<float>(i) / static_cast<float>(n - 1);
    pts.push_back(glm::vec2(r * std::cos(a), r * std::sin(a)));
  }
  const std::vector<glm::vec2> t = polyline_tangents(pts);
  CHECK(glm::dot(t.front(), glm::vec2(0.0f, 1.0f)) == Catch::Approx(1.0f).margin(1e-4));
  const glm::vec2 want_end(-std::sin(sweep), std::cos(sweep));
  CHECK(glm::dot(t.back(), want_end) == Catch::Approx(1.0f).margin(1e-4));
}
