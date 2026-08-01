#include "mapgen/river_arcs.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace badlands::mapgen {

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kTwoPi = 2.0f * kPi;

// Below this the arc is emitted as exactly straight. Chosen against the scale
// of the problem: a river reach is metres to hundreds of metres long, so a
// radius past 1e6 m is straight to any precision that matters, and pinning
// curvature to exactly 0 keeps the straight path free of 1/k.
constexpr float kMinCurvature = 1e-6f;

// Shared geometric tolerance for "these two points coincide" / "this vector has
// no direction", in metres. World coordinates here are metres over a map of
// order 1e3 m, so 1e-5 is far below any real feature and far above float noise.
constexpr float kEps = 1e-5f;

float cross2(glm::vec2 a, glm::vec2 b) { return a.x * b.y - a.y * b.x; }

glm::vec2 left_normal(glm::vec2 t) { return glm::vec2(-t.y, t.x); }

glm::vec2 safe_normalize(glm::vec2 v, glm::vec2 fallback) {
  const float l = glm::length(v);
  return (l > kEps) ? v / l : fallback;
}

// The arc leaving `p` with unit tangent `t` and ending at `e`. This is the
// primitive both halves of a biarc are built from: a point, a heading and a
// destination determine exactly one circle.
//
//   k   = 2*cross(t, chord) / |chord|^2      (signed curvature)
//   phi = atan2(cross(t, chord), dot(t, chord))   (HALF the turn angle)
//   s   = |chord| * phi / sin(phi)           (arc length; -> |chord| as phi->0)
//
// phi is half the turn because the chord of a circular arc bisects the angle
// between the tangents at its ends. Returns false when the construction is
// degenerate -- coincident endpoints, or a turn of ~180 degrees where the
// chord carries no information about how far around the circle the arc goes.
bool make_arc(glm::vec2 p, glm::vec2 t, glm::vec2 e, RiverArc& out) {
  const glm::vec2 chord = e - p;
  const float len = glm::length(chord);
  if (len <= kEps) return false;

  const float cr = cross2(t, chord);
  const float dt = glm::dot(t, chord);
  const float phi = std::atan2(cr, dt);
  const float sphi = std::sin(phi);

  out.p0 = p;
  out.p1 = e;
  out.t0 = t;
  if (std::abs(sphi) <= kEps) {
    // Either straight (phi ~ 0) or a reversal (phi ~ +/-pi). A reversal has no
    // finite arc through it in this parameterisation, and is not a shape a
    // river reach takes; both fall back to the chord.
    out.curvature_1_m = 0.0f;
    out.length_m = len;
    if (std::abs(phi) > 0.5f * kPi) return false;
    out.t0 = chord / len;
    return true;
  }

  const float k = 2.0f * cr / (len * len);
  out.curvature_1_m = (std::abs(k) < kMinCurvature) ? 0.0f : k;
  out.length_m = (out.curvature_1_m == 0.0f) ? len : len * phi / sphi;
  return out.length_m > 0.0f && std::isfinite(out.length_m);
}

// Signed angular position of `p` around the arc's circle, measured from the
// start, in the direction of travel: 0 at p0, growing to k*length at p1.
// Returns the value wrapped into the half-turn the arc travels through, so
// containment is a single comparison against the sweep.
float arc_sweep_of(const RiverArc& a, glm::vec2 p) {
  const glm::vec2 c = arc_centre(a);
  const float phi0 = std::atan2(a.p0.y - c.y, a.p0.x - c.x);
  const float phip = std::atan2(p.y - c.y, p.x - c.x);
  float d = phip - phi0;
  if (a.curvature_1_m > 0.0f) {
    d = std::fmod(d, kTwoPi);
    if (d < 0.0f) d += kTwoPi;
  } else {
    d = std::fmod(d, kTwoPi);
    if (d > 0.0f) d -= kTwoPi;
  }
  return d;
}

float point_segment_distance(glm::vec2 p, glm::vec2 a, glm::vec2 b) {
  const glm::vec2 ab = b - a;
  const float len2 = glm::dot(ab, ab);
  if (len2 <= kEps * kEps) return glm::length(p - a);
  const float t = std::clamp(glm::dot(p - a, ab) / len2, 0.0f, 1.0f);
  return glm::length(p - (a + t * ab));
}

// Worst distance from any of pts[first+1 .. last-1] to the fitted biarc. The
// endpoints are interpolated exactly by construction and so are not measured.
float span_deviation_m(const std::vector<RiverArc>& arcs,
                       const std::vector<glm::vec2>& pts, size_t first,
                       size_t last) {
  float worst = 0.0f;
  for (size_t i = first + 1; i < last; ++i) {
    float best = std::numeric_limits<float>::max();
    for (const RiverArc& a : arcs) best = std::min(best, arc_distance_m(a, pts[i]));
    worst = std::max(worst, best);
  }
  return worst;
}

}  // namespace

// --- evaluation -------------------------------------------------------------

float arc_radius_m(const RiverArc& a) {
  if (a.curvature_1_m == 0.0f) return std::numeric_limits<float>::infinity();
  return 1.0f / std::abs(a.curvature_1_m);
}

glm::vec2 arc_centre(const RiverArc& a) {
  if (a.curvature_1_m == 0.0f) return a.p0;
  return a.p0 + left_normal(a.t0) / a.curvature_1_m;
}

glm::vec2 arc_point(const RiverArc& a, float s) {
  if (a.curvature_1_m == 0.0f) return a.p0 + a.t0 * s;
  // Heading rotates at a constant rate k along the arc; integrating the unit
  // tangent gives the circle directly.
  const float k = a.curvature_1_m;
  const float a0 = std::atan2(a.t0.y, a.t0.x);
  const glm::vec2 c = arc_centre(a);
  const float th = a0 + k * s;
  return c + glm::vec2(std::sin(th), -std::cos(th)) / k;
}

glm::vec2 arc_tangent(const RiverArc& a, float s) {
  if (a.curvature_1_m == 0.0f) return a.t0;
  const float a0 = std::atan2(a.t0.y, a.t0.x);
  const float th = a0 + a.curvature_1_m * s;
  return glm::vec2(std::cos(th), std::sin(th));
}

float arc_distance_m(const RiverArc& a, glm::vec2 p) {
  if (a.curvature_1_m == 0.0f) return point_segment_distance(p, a.p0, a.p1);
  const glm::vec2 c = arc_centre(a);
  const float r = arc_radius_m(a);
  const float sweep = a.curvature_1_m * a.length_m;
  const float d = arc_sweep_of(a, p);
  const bool inside = (a.curvature_1_m > 0.0f) ? (d <= sweep) : (d >= sweep);
  if (inside) return std::abs(glm::length(p - c) - r);
  return std::min(glm::length(p - a.p0), glm::length(p - a.p1));
}

// --- fitting ----------------------------------------------------------------

std::vector<RiverArc> fit_biarc(glm::vec2 p0, glm::vec2 t0, glm::vec2 p1,
                                glm::vec2 t1) {
  std::vector<RiverArc> out;
  const glm::vec2 v = p1 - p0;
  const float vlen = glm::length(v);
  if (vlen <= kEps) return out;

  t0 = safe_normalize(t0, v / vlen);
  t1 = safe_normalize(t1, v / vlen);

  // Single arc already? If the arc leaving p0 with tangent t0 and ending at p1
  // ALSO arrives with tangent t1, one arc is the answer and a biarc would only
  // introduce a spurious joint.
  {
    RiverArc single;
    if (make_arc(p0, t0, p1, single) &&
        glm::dot(arc_tangent(single, single.length_m), t1) > 1.0f - 1e-6f) {
      out.push_back(single);
      return out;
    }
  }

  // Solve |J - (p0 + d*t0)| = d for the shared tangent length d, where
  // J = ((p0 + d*t0) + (p1 - d*t1)) / 2. Expanding gives
  //     (|t0+t1|^2 - 4) d^2 - 2 (v . (t0+t1)) d + |v|^2 = 0
  // and |t0+t1|^2 - 4 = 2(t0.t1 - 1) <= 0, so the quadratic opens downward with
  // a positive constant term: exactly one positive root, no branch to choose.
  const glm::vec2 t = t0 + t1;
  const float qa = 2.0f * (glm::dot(t0, t1) - 1.0f);
  const float qb = -2.0f * glm::dot(v, t);
  const float qc = vlen * vlen;

  float d = 0.0f;
  if (std::abs(qa) <= kEps) {
    // Parallel tangents: the quadratic collapses to linear.
    const float vt = glm::dot(v, t);
    if (std::abs(vt) <= kEps) return out;
    d = qc / (2.0f * vt);
  } else {
    const float disc = qb * qb - 4.0f * qa * qc;
    if (disc < 0.0f) return out;
    d = (-qb - std::sqrt(disc)) / (2.0f * qa);
  }
  if (!(d > kEps) || !std::isfinite(d)) return out;

  const glm::vec2 joint = 0.5f * ((p0 + d * t0) + (p1 - d * t1));
  // Both arcs' tangent at the joint; equal by construction, which is what makes
  // the pair G1 without solving for it.
  const glm::vec2 tj = safe_normalize(v - d * t, t0);

  RiverArc a1, a2;
  const bool ok1 = make_arc(p0, t0, joint, a1);
  const bool ok2 = make_arc(joint, tj, p1, a2);
  if (!ok1 || !ok2) {
    // A degenerate half means the joint landed on an endpoint. One arc through
    // the whole span is still better than nothing.
    RiverArc single;
    if (make_arc(p0, t0, p1, single)) out.push_back(single);
    return out;
  }
  out.push_back(a1);
  out.push_back(a2);
  return out;
}

std::vector<glm::vec2> polyline_tangents(const std::vector<glm::vec2>& pts) {
  const size_t n = pts.size();
  std::vector<glm::vec2> t(n, glm::vec2(1.0f, 0.0f));
  if (n < 2) return t;

  for (size_t i = 1; i + 1 < n; ++i)
    t[i] = safe_normalize(pts[i + 1] - pts[i - 1], glm::vec2(1.0f, 0.0f));

  // Endpoints: extrapolate the heading in ANGLE off the two chords that reach
  // in from the end, which is exact on a circle. Falls back to the end segment
  // when there is no third point or the chords are degenerate.
  auto end_tangent = [&](size_t e, size_t m, size_t f) {
    const glm::vec2 c1 = pts[m] - pts[e];
    const glm::vec2 seg = safe_normalize(c1, glm::vec2(1.0f, 0.0f));
    if (n < 3) return seg;
    const glm::vec2 c2 = pts[f] - pts[e];
    if (glm::length(c1) <= kEps || glm::length(c2) <= kEps) return seg;
    const float a = 2.0f * std::atan2(c1.y, c1.x) - std::atan2(c2.y, c2.x);
    return glm::vec2(std::cos(a), std::sin(a));
  };
  t[0] = end_tangent(0, 1, std::min<size_t>(2, n - 1));
  // The last point's tangent points downstream, so extrapolate backwards and
  // flip: the chords run upstream from it.
  t[n - 1] = -end_tangent(n - 1, n - 2, (n >= 3) ? n - 3 : 0);
  return t;
}

std::vector<float> polyline_params(const std::vector<glm::vec2>& pts) {
  const size_t n = pts.size();
  std::vector<float> u(n, 0.0f);
  if (n < 2) return u;
  float total = 0.0f;
  for (size_t i = 1; i < n; ++i) {
    total += glm::length(pts[i] - pts[i - 1]);
    u[i] = total;
  }
  if (total <= kEps) return std::vector<float>(n, 0.0f);
  for (float& v : u) v /= total;
  u[n - 1] = 1.0f;
  return u;
}

float sample_at_param(const std::vector<float>& params,
                      const std::vector<float>& values, float u) {
  if (values.empty()) return 0.0f;
  if (params.size() != values.size()) return values.front();
  if (u <= params.front()) return values.front();
  if (u >= params.back()) return values.back();
  const auto it = std::upper_bound(params.begin(), params.end(), u);
  const size_t hi = static_cast<size_t>(it - params.begin());
  const size_t lo = hi - 1;
  const float span = params[hi] - params[lo];
  const float f = (span > 0.0f) ? (u - params[lo]) / span : 0.0f;
  return values[lo] + f * (values[hi] - values[lo]);
}

RiverArcChain fit_arc_chain(const std::vector<glm::vec2>& pts,
                            const std::vector<glm::vec2>& tangents,
                            float tolerance_m) {
  RiverArcChain chain;
  const size_t n = pts.size();
  if (n < 2 || tangents.size() != n) return chain;
  const std::vector<float> u = polyline_params(pts);

  size_t i = 0;
  while (i + 1 < n) {
    // Greedy: keep the longest span whose skipped points all stay within
    // tolerance. Stop extending at the first failure rather than searching on
    // -- deviation grows with span in practice, and the search is what would
    // make this quadratic in the reach length.
    size_t best_end = 0;
    std::vector<RiverArc> best;
    for (size_t j = i + 1; j < n; ++j) {
      std::vector<RiverArc> cand =
          fit_biarc(pts[i], tangents[i], pts[j], tangents[j]);
      if (cand.empty()) break;
      if (j > i + 1 && span_deviation_m(cand, pts, i, j) > tolerance_m) break;
      best_end = j;
      best = std::move(cand);
    }
    if (best.empty()) {
      // Even the single-segment span refused to fit -- coincident points, or a
      // near-reversal where no biarc through those tangents exists. Emit the
      // segment as a straight arc rather than dropping it: a gap in the chain
      // would break every consumer that walks it as one curve, and a chord is
      // never worse than the polyline it replaces.
      RiverArc seg;
      if (make_arc(pts[i], safe_normalize(pts[i + 1] - pts[i], tangents[i]),
                   pts[i + 1], seg)) {
        seg.param0 = u[i];
        seg.param1 = u[i + 1];
        chain.length_m += seg.length_m;
        chain.arcs.push_back(seg);
      }
      ++i;
      continue;
    }

    // Split the span's parameter range between the two arcs by arc length, so
    // an attribute lookup at a joint lands where the joint actually is.
    float total = 0.0f;
    for (const RiverArc& a : best) total += a.length_m;
    float acc = 0.0f;
    for (RiverArc& a : best) {
      const float f0 = (total > 0.0f) ? acc / total : 0.0f;
      acc += a.length_m;
      const float f1 = (total > 0.0f) ? acc / total : 1.0f;
      a.param0 = u[i] + (u[best_end] - u[i]) * f0;
      a.param1 = u[i] + (u[best_end] - u[i]) * f1;
      chain.length_m += a.length_m;
      chain.arcs.push_back(a);
    }
    i = best_end;
  }
  return chain;
}

RiverArcChain fit_arc_chain(const std::vector<glm::vec2>& pts,
                            float tolerance_m) {
  return fit_arc_chain(pts, polyline_tangents(pts), tolerance_m);
}

std::vector<RiverArcChain> build_river_arcs(const RiverGraph& g,
                                            float tolerance_m) {
  std::vector<RiverArcChain> out;
  out.reserve(g.edges.size());
  for (size_t e = 0; e < g.edges.size(); ++e) {
    if (g.edges[e].points_m.size() < 2) continue;
    RiverArcChain c = fit_arc_chain(g.edges[e].points_m, tolerance_m);
    if (c.arcs.empty()) continue;
    c.edge = static_cast<int32_t>(e);
    out.push_back(std::move(c));
  }
  return out;
}

}  // namespace badlands::mapgen
