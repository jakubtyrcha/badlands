#include "game/geometry/branch_junction.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_set>

#include <glm/gtc/constants.hpp>

namespace badlands {
namespace {

constexpr float kEps = 1e-6f;

// Fraction a colliding footprint loses per shrink pass. 0.85 converges from a
// full overlap to the floor in ~20 passes, which is well inside the cap below
// and gentle enough that a pair separates as soon as it can rather than
// collapsing to the floor in one step.
constexpr float kShrinkStep = 0.85f;
constexpr int   kMaxShrinkPasses = 32;

// Shortest signed angular difference, wrapped to [-pi, pi].
float WrapAngle(float a) {
  const float two_pi = glm::two_pi<float>();
  a = std::fmod(a + glm::pi<float>(), two_pi);
  if (a < 0.0f) a += two_pi;
  return a - glm::pi<float>();
}

// Normalized cumulative arc length around a closed loop: params[k] for
// k in [0, n], with params[0] = 0 and params[n] = 1.
std::vector<float> LoopParams(const std::vector<glm::vec3>& pos, size_t start) {
  const size_t n = pos.size();
  std::vector<float> params(n + 1, 0.0f);
  float total = 0.0f;
  for (size_t k = 0; k < n; ++k) {
    total += glm::length(pos[(start + k + 1) % n] - pos[(start + k) % n]);
    params[k + 1] = total;
  }
  if (total <= kEps) {  // degenerate loop: fall back to uniform spacing
    for (size_t k = 0; k <= n; ++k)
      params[k] = static_cast<float>(k) / static_cast<float>(n);
    return params;
  }
  for (float& p : params) p /= total;
  return params;
}

}  // namespace

TubeHit ClosestOnTube(const std::vector<BranchSection>& sections,
                      const glm::vec3& p) {
  TubeHit best;
  best.distance = std::numeric_limits<float>::max();
  if (sections.empty()) return best;
  if (sections.size() == 1) {
    best.section = 0;
    best.alpha = 0.0f;
    best.radius = sections[0].radius;
    best.distance = glm::length(p - sections[0].origin);
    return best;
  }

  for (size_t i = 0; i + 1 < sections.size(); ++i) {
    const glm::vec3& a = sections[i].origin;
    const glm::vec3& b = sections[i + 1].origin;
    const glm::vec3 ab = b - a;
    const float len_sq = glm::dot(ab, ab);
    const float t = (len_sq > kEps) ? std::clamp(glm::dot(p - a, ab) / len_sq, 0.0f, 1.0f)
                                    : 0.0f;
    const glm::vec3 on_axis = a + ab * t;
    const float d = glm::length(p - on_axis);
    if (d < best.distance) {
      best.section = static_cast<int>(i);
      best.alpha = t;
      best.radius = glm::mix(sections[i].radius, sections[i + 1].radius, t);
      best.distance = d;
      // Only the polyline's two ENDS are open; a clamp at an interior section
      // boundary is just the polyline bending, not the tube running out.
      const bool at_first = (i == 0 && t <= 0.0f);
      const bool at_last = (i + 2 == sections.size() && t >= 1.0f);
      best.beyond_end = (at_first && glm::dot(p - a, ab) < 0.0f) ||
                        (at_last && glm::dot(p - b, ab) > 0.0f);
    }
  }
  return best;
}

bool IsOutsideTube(const std::vector<BranchSection>& sections,
                   const glm::vec3& p, float slack) {
  const TubeHit hit = ClosestOnTube(sections, p);
  // Past an open end there is no surface to be inside of, whatever the radius.
  if (hit.beyond_end) return true;
  return hit.distance > hit.radius * (1.0f + slack);
}

SocketFootprint ComputeSocketFootprint(const std::vector<BranchSection>& parent,
                                       int attach_section, float attach_alpha,
                                       const glm::quat& child_orientation,
                                       float child_radius) {
  SocketFootprint out;
  const int last = static_cast<int>(parent.size()) - 1;
  if (last < 1 || child_radius <= kEps) return out;

  const int si = std::clamp(attach_section, 0, last);
  const BranchSection& a = parent[static_cast<size_t>(si)];
  const BranchSection& b = (si == last) ? a : parent[static_cast<size_t>(si + 1)];
  const float alpha = std::clamp(attach_alpha, 0.0f, 1.0f);

  const float parent_radius = (1.0f - alpha) * a.radius + alpha * b.radius;
  if (parent_radius <= kEps) return out;  // degenerate: nothing to cut into

  // Same slerp argument order GenerateChildBranches used to build the child's
  // frame, so this recovers exactly the frame the child was grown in.
  const glm::quat parent_q = glm::slerp(b.orientation, a.orientation, alpha);
  const glm::vec3 axis =
      glm::inverse(parent_q) * (child_orientation * glm::vec3(0.0f, 1.0f, 0.0f));

  // The parent's rings place vertex j at (cos, 0, sin), so this is the angle
  // the child leaves from.
  out.centre_angle = std::atan2(axis.z, axis.x);

  // Half-angle of a cylinder of radius r meeting one of radius R: asin(r / R).
  // Ratios >= 1 are real here (Oak's twigs are 1.19x their parent), so the
  // clamp below is what keeps the ring from being severed rather than a tidy-up.
  const float ratio = std::min(child_radius / parent_radius, 1.0f);
  const float max_half = glm::radians(kMaxSocketHalfAngleDeg);
  out.half_angle = std::min(std::asin(ratio), max_half);

  // Axial extent: the child's diameter, lengthened by how obliquely it leaves.
  // sin(theta) is the length of the child axis' component perpendicular to the
  // parent's own axis (+Y in the parent's frame).
  const float sin_theta = glm::length(glm::vec2(axis.x, axis.z));
  if (sin_theta < kAxialJoinSinThreshold) {
    // Too close to parallel to cut a socket: 2r/sin(theta) would run away. The
    // child leaves through the parent's end ring instead.
    out.axial = true;
    out.valid = true;
    return out;
  }

  const float section_len = glm::length(b.origin - a.origin);
  if (section_len <= kEps) return out;  // degenerate: e.g. Bush 1/2's stub trunk

  const float axial_world = 2.0f * child_radius / sin_theta;
  const float half_span = 0.5f * axial_world / section_len;
  const float centre = static_cast<float>(si) + alpha;
  out.axial_min = std::max(centre - half_span, 0.0f);
  out.axial_max = std::min(centre + half_span, static_cast<float>(last));
  if (out.axial_max <= out.axial_min) return out;

  out.valid = true;
  return out;
}

bool FootprintsOverlap(const SocketFootprint& a, const SocketFootprint& b) {
  if (!a.valid || !b.valid) return false;
  // Axial joins cut nothing out of the parent's side, so they never compete for
  // its surface -- they all meet at the end ring and share it happily.
  if (a.axial || b.axial) return false;
  // Both axes must intersect. Two branches at the same angle but well apart up
  // the trunk do not collide, and neither do two at the same height on
  // opposite sides -- which is exactly what the generator's stratified
  // placement produces most of the time.
  if (a.axial_max <= b.axial_min || b.axial_max <= a.axial_min) return false;
  const float d = std::abs(WrapAngle(a.centre_angle - b.centre_angle));
  return d < a.half_angle + b.half_angle;
}

bool FootprintContains(const SocketFootprint& f, float axial, float angle) {
  if (!f.valid || f.axial) return false;
  if (axial < f.axial_min || axial > f.axial_max) return false;
  return std::abs(WrapAngle(angle - f.centre_angle)) < f.half_angle;
}

int ShrinkToDisjoint(std::vector<SocketFootprint>& footprints,
                     float min_half_angle, float min_axial) {
  std::vector<bool> shrunk(footprints.size(), false);

  for (int pass = 0; pass < kMaxShrinkPasses; ++pass) {
    bool any_overlap = false;
    for (size_t i = 0; i < footprints.size(); ++i) {
      for (size_t j = i + 1; j < footprints.size(); ++j) {
        if (!FootprintsOverlap(footprints[i], footprints[j])) continue;
        any_overlap = true;
        bool moved = false;
        for (size_t which : {i, j}) {
          SocketFootprint& f = footprints[which];
          const float axial_half = 0.5f * (f.axial_max - f.axial_min);
          if (f.half_angle <= min_half_angle && axial_half <= min_axial) continue;
          const float centre = 0.5f * (f.axial_min + f.axial_max);
          const float next_axial_half =
              std::max(axial_half * kShrinkStep, min_axial);
          f.half_angle = std::max(f.half_angle * kShrinkStep, min_half_angle);
          f.axial_min = centre - next_axial_half;
          f.axial_max = centre + next_axial_half;
          shrunk[which] = true;
          moved = true;
        }
        // Both are already as small as they may go and they still collide --
        // the later one loses and its caller falls back to a plain tube.
        if (!moved) footprints[j].valid = false;
      }
    }
    if (!any_overlap) break;
  }

  return static_cast<int>(std::count(shrunk.begin(), shrunk.end(), true));
}

std::vector<float> MergeRingParams(const std::vector<float>& authored,
                                   const std::vector<std::pair<float, float>>& spans,
                                   int min_rings_per_span, float tol) {
  if (authored.size() < 2) return authored;
  const float lo = authored.front();
  const float hi = authored.back();

  std::vector<float> merged = authored;
  for (const auto& span : spans) {
    // Rings already inside the span, strictly between its ends.
    int inside = 0;
    for (float t : authored)
      if (t > span.first && t < span.second) ++inside;
    if (inside >= min_rings_per_span) continue;  // already resolved enough

    for (float t : {span.first, span.second}) {
      const float clamped = std::clamp(t, lo, hi);
      if (clamped > lo + tol && clamped < hi - tol) merged.push_back(clamped);
    }
  }

  // A TOTAL order first; deduplication is a separate pass. A tolerance-based
  // comparator is not transitive, and feeding one to std::sort is UB.
  std::sort(merged.begin(), merged.end());
  std::vector<float> out;
  out.reserve(merged.size());
  for (float t : merged)
    if (out.empty() || t - out.back() > tol) out.push_back(t);
  return out;
}

std::vector<uint32_t> StitchLoops(const std::vector<uint32_t>& a_ids,
                                  const std::vector<glm::vec3>& a_pos,
                                  const std::vector<uint32_t>& b_ids,
                                  const std::vector<glm::vec3>& b_pos) {
  const size_t n = a_ids.size();
  const size_t m = b_ids.size();
  if (n < 3 || m < 3 || a_pos.size() != n || b_pos.size() != m) return {};

  // Start B at whichever vertex is nearest A's first, so the strip does not
  // open with a twist. Everything after that is arc-length driven.
  size_t start_b = 0;
  float best = std::numeric_limits<float>::max();
  for (size_t j = 0; j < m; ++j) {
    const float d = glm::distance(a_pos[0], b_pos[j]);
    if (d < best) { best = d; start_b = j; }
  }

  const std::vector<float> pa = LoopParams(a_pos, 0);
  const std::vector<float> pb = LoopParams(b_pos, start_b);

  std::vector<uint32_t> tris;
  tris.reserve((n + m) * 3);
  size_t i = 0, j = 0;
  while (i < n || j < m) {
    // Advance whichever loop is behind in normalized arc length, so a 4-vertex
    // loop bridged to a 32-vertex one spreads evenly instead of fanning off one
    // corner.
    const bool advance_a = (j >= m) || (i < n && pa[i + 1] <= pb[j + 1]);
    const uint32_t ai = a_ids[i % n];
    const uint32_t bj = b_ids[(start_b + j) % m];
    if (advance_a) {
      tris.insert(tris.end(), {ai, bj, a_ids[(i + 1) % n]});
      ++i;
    } else {
      tris.insert(tris.end(), {ai, bj, b_ids[(start_b + j + 1) % m]});
      ++j;
    }
  }

  // Orient outward. The collar is a truncated cone about the line joining the
  // two centroids, so "outward" is away from that line's midpoint.
  glm::vec3 centre(0.0f);
  for (const glm::vec3& p : a_pos) centre += p;
  for (const glm::vec3& p : b_pos) centre += p;
  centre /= static_cast<float>(n + m);

  auto position_of = [&](uint32_t id) {
    for (size_t k = 0; k < n; ++k)
      if (a_ids[k] == id) return a_pos[k];
    for (size_t k = 0; k < m; ++k)
      if (b_ids[k] == id) return b_pos[k];
    return centre;
  };

  float outward = 0.0f;
  for (size_t t = 0; t + 2 < tris.size(); t += 3) {
    const glm::vec3 p0 = position_of(tris[t]);
    const glm::vec3 p1 = position_of(tris[t + 1]);
    const glm::vec3 p2 = position_of(tris[t + 2]);
    const glm::vec3 nrm = glm::cross(p1 - p0, p2 - p0);
    outward += glm::dot(nrm, (p0 + p1 + p2) / 3.0f - centre);
  }
  if (outward < 0.0f)
    for (size_t t = 0; t + 2 < tris.size(); t += 3) std::swap(tris[t + 1], tris[t + 2]);

  return tris;
}

void SmoothVertexNormals(std::vector<float>& vertices, size_t floats_per_vertex,
                         const std::vector<uint32_t>& indices,
                         const std::vector<uint32_t>& vertex_ids) {
  // 11 is this function's own BOUNDS requirement, not the kTexturedMesh stride:
  // it touches offsets 0..2 (position), 5..7 (normal) and 8..10 (tangent xyz),
  // so 11 floats is the least it can safely index. Deliberately not
  // kTexturedMeshFloatsPerVertex -- the parameter exists so this stays layout-
  // agnostic, and tying the guard to one layout would reject a caller whose
  // own stride is fine.
  if (vertex_ids.empty() || floats_per_vertex < 11) return;
  const std::unordered_set<uint32_t> targets(vertex_ids.begin(), vertex_ids.end());
  const size_t vertex_count = vertices.size() / floats_per_vertex;

  auto position = [&](uint32_t v) {
    const size_t base = v * floats_per_vertex;
    return glm::vec3(vertices[base], vertices[base + 1], vertices[base + 2]);
  };

  std::vector<glm::vec3> accum(vertex_count, glm::vec3(0.0f));
  for (size_t t = 0; t + 2 < indices.size(); t += 3) {
    const uint32_t i0 = indices[t], i1 = indices[t + 1], i2 = indices[t + 2];
    if (i0 >= vertex_count || i1 >= vertex_count || i2 >= vertex_count) continue;
    if (!targets.count(i0) && !targets.count(i1) && !targets.count(i2)) continue;
    // The un-normalized cross product's length is twice the triangle's area,
    // so accumulating it raw IS the area weighting.
    const glm::vec3 face = glm::cross(position(i1) - position(i0),
                                      position(i2) - position(i0));
    accum[i0] += face;
    accum[i1] += face;
    accum[i2] += face;
  }

  for (uint32_t v : targets) {
    if (v >= vertex_count) continue;
    if (glm::length(accum[v]) <= kEps) continue;  // keep the swept normal
    const size_t base = v * floats_per_vertex;
    const glm::vec3 nrm = glm::normalize(accum[v]);
    glm::vec3 tng(vertices[base + 8], vertices[base + 9], vertices[base + 10]);
    // Re-orthogonalize rather than recompute: the swept tangent already runs
    // around the branch, which is the direction bark UVs want.
    tng -= nrm * glm::dot(nrm, tng);
    if (glm::length(tng) <= kEps) {
      const glm::vec3 up = (std::abs(nrm.y) < 0.9f) ? glm::vec3(0, 1, 0)
                                                    : glm::vec3(1, 0, 0);
      tng = glm::cross(up, nrm);
    }
    tng = glm::normalize(tng);
    vertices[base + 5] = nrm.x; vertices[base + 6] = nrm.y; vertices[base + 7] = nrm.z;
    vertices[base + 8] = tng.x; vertices[base + 9] = tng.y; vertices[base + 10] = tng.z;
  }
}

}  // namespace badlands
