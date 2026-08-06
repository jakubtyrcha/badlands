#include <catch_amalgamated.hpp>

#include <algorithm>
#include <cmath>
#include <set>
#include <vector>

#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>

#include "engine/rendering/geometry/textured_mesh_builders.hpp"  // kTexturedMeshFloatsPerVertex
#include "game/geometry/branch_junction.hpp"

using namespace badlands;

namespace {

// A straight tube up +Y: `rings` sections, `len` tall, tapering from r0 to r1.
std::vector<BranchSection> StraightTube(int rings, float len, float r0, float r1) {
  std::vector<BranchSection> out;
  for (int i = 0; i < rings; ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(rings - 1);
    out.push_back({glm::vec3(0.0f, t * len, 0.0f),
                   glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::mix(r0, r1, t)});
  }
  return out;
}

// A circle of `n` points in the XZ plane at height y.
std::vector<glm::vec3> Circle(int n, float radius, float y, float phase = 0.0f) {
  std::vector<glm::vec3> out;
  for (int i = 0; i < n; ++i) {
    const float a = phase + glm::two_pi<float>() * static_cast<float>(i) /
                                static_cast<float>(n);
    out.push_back({std::cos(a) * radius, y, std::sin(a) * radius});
  }
  return out;
}

std::vector<uint32_t> Ids(uint32_t base, size_t n) {
  std::vector<uint32_t> out;
  for (size_t i = 0; i < n; ++i) out.push_back(base + static_cast<uint32_t>(i));
  return out;
}

// Every stitch has to satisfy these, whatever the loops look like.
void CheckStrip(const std::vector<uint32_t>& tris,
                const std::vector<uint32_t>& a_ids, const std::vector<glm::vec3>& a_pos,
                const std::vector<uint32_t>& b_ids, const std::vector<glm::vec3>& b_pos) {
  REQUIRE(tris.size() % 3 == 0);
  // One triangle per edge of each loop -- no more, no less.
  REQUIRE(tris.size() == (a_ids.size() + b_ids.size()) * 3);

  // INDEX IDENTITY: the bridge allocates no vertices, so every index it emits
  // must come from a loop it was handed.
  std::set<uint32_t> allowed(a_ids.begin(), a_ids.end());
  allowed.insert(b_ids.begin(), b_ids.end());
  for (uint32_t idx : tris) REQUIRE(allowed.count(idx) == 1);

  // Both loops fully covered.
  const std::set<uint32_t> used(tris.begin(), tris.end());
  for (uint32_t id : a_ids) REQUIRE(used.count(id) == 1);
  for (uint32_t id : b_ids) REQUIRE(used.count(id) == 1);

  auto position_of = [&](uint32_t id) {
    for (size_t k = 0; k < a_ids.size(); ++k)
      if (a_ids[k] == id) return a_pos[k];
    for (size_t k = 0; k < b_ids.size(); ++k)
      if (b_ids[k] == id) return b_pos[k];
    FAIL("index not in either loop");
    return glm::vec3(0.0f);
  };

  glm::vec3 centre(0.0f);
  for (const glm::vec3& p : a_pos) centre += p;
  for (const glm::vec3& p : b_pos) centre += p;
  centre /= static_cast<float>(a_pos.size() + b_pos.size());

  for (size_t t = 0; t + 2 < tris.size(); t += 3) {
    CAPTURE(t);
    // No repeated corner -- that would be a degenerate triangle by index.
    REQUIRE(tris[t] != tris[t + 1]);
    REQUIRE(tris[t] != tris[t + 2]);
    REQUIRE(tris[t + 1] != tris[t + 2]);

    const glm::vec3 p0 = position_of(tris[t]);
    const glm::vec3 p1 = position_of(tris[t + 1]);
    const glm::vec3 p2 = position_of(tris[t + 2]);
    const glm::vec3 nrm = glm::cross(p1 - p0, p2 - p0);
    REQUIRE(glm::length(nrm) > 1e-6f);  // non-zero area
    // Consistent winding: no triangle folds back through the collar.
    REQUIRE(glm::dot(nrm, (p0 + p1 + p2) / 3.0f - centre) > 0.0f);
  }
}

}  // namespace

TEST_CASE("StitchLoops: bridges drastically unequal vertex counts", "[junction]") {
  // 4 -> 32 is the case the all-quad "pants" constraint (waist + 2 = legA +
  // legB) cannot express at all, and the reason this generator stitches
  // triangles instead: a branch's ring resolution never has to match its
  // parent's, so no reduction rings are needed anywhere.
  const auto a_pos = Circle(4, 2.0f, 0.0f);
  const auto b_pos = Circle(32, 0.6f, 1.0f);
  const auto a_ids = Ids(0, 4);
  const auto b_ids = Ids(100, 32);

  const auto tris = StitchLoops(a_ids, a_pos, b_ids, b_pos);
  CheckStrip(tris, a_ids, a_pos, b_ids, b_pos);
}

TEST_CASE("StitchLoops: holds up for orthogonal and near-coplanar loops", "[junction]") {
  const auto a_pos = Circle(8, 1.0f, 0.0f);
  const auto a_ids = Ids(0, 8);

  SECTION("child ring squarely above the hole") {
    const auto b_pos = Circle(6, 0.5f, 1.5f);
    const auto b_ids = Ids(50, 6);
    CheckStrip(StitchLoops(a_ids, a_pos, b_ids, b_pos), a_ids, a_pos, b_ids, b_pos);
  }
  SECTION("nearly coplanar -- a swept-back branch") {
    // Not synthetic: Pine's angle[1] is 110-129 degrees, so its branches leave
    // the trunk sharply swept and their base rings sit almost in the trunk's
    // own surface.
    const auto b_pos = Circle(6, 0.5f, 0.08f);
    const auto b_ids = Ids(50, 6);
    CheckStrip(StitchLoops(a_ids, a_pos, b_ids, b_pos), a_ids, a_pos, b_ids, b_pos);
  }
  SECTION("phase-rotated so vertex 0 of each loop does not line up") {
    const auto b_pos = Circle(6, 0.5f, 1.5f, glm::pi<float>() * 0.37f);
    const auto b_ids = Ids(50, 6);
    CheckStrip(StitchLoops(a_ids, a_pos, b_ids, b_pos), a_ids, a_pos, b_ids, b_pos);
  }
}

TEST_CASE("StitchLoops: refuses a loop too small to bridge", "[junction]") {
  const auto a_pos = Circle(2, 1.0f, 0.0f);
  const auto b_pos = Circle(6, 0.5f, 1.0f);
  REQUIRE(StitchLoops(Ids(0, 2), a_pos, Ids(50, 6), b_pos).empty());
}

TEST_CASE("ClosestOnTube: matches an analytic cylinder", "[junction]") {
  const auto tube = StraightTube(5, 4.0f, 1.0f, 1.0f);

  const TubeHit side = ClosestOnTube(tube, glm::vec3(2.0f, 2.0f, 0.0f));
  REQUIRE(side.distance == Catch::Approx(2.0f));
  REQUIRE(side.radius == Catch::Approx(1.0f));

  // Beyond the end cap the closest point is the end ring itself.
  const TubeHit past = ClosestOnTube(tube, glm::vec3(0.0f, 6.0f, 0.0f));
  REQUIRE(past.distance == Catch::Approx(2.0f));

  REQUIRE(IsOutsideTube(tube, glm::vec3(1.5f, 2.0f, 0.0f), 0.0f));
  REQUIRE_FALSE(IsOutsideTube(tube, glm::vec3(0.5f, 2.0f, 0.0f), 0.0f));

  // A taper's radius is interpolated, not taken from the nearest ring.
  const auto cone = StraightTube(5, 4.0f, 1.0f, 0.0f);
  REQUIRE(ClosestOnTube(cone, glm::vec3(0.0f, 2.0f, 0.0f)).radius == Catch::Approx(0.5f));
}

TEST_CASE("ComputeSocketFootprint: clamps a child thicker than its parent", "[junction]") {
  const auto parent = StraightTube(5, 4.0f, 1.0f, 1.0f);
  // Perpendicular child: angleAxis(90 deg, X) sends the growth axis +Y to +Z.
  const glm::quat child = glm::angleAxis(glm::half_pi<float>(), glm::vec3(1, 0, 0));

  SECTION("thin child gets its analytic half-angle") {
    const SocketFootprint f = ComputeSocketFootprint(parent, 2, 0.0f, child, 0.5f);
    REQUIRE(f.valid);
    REQUIRE(f.half_angle == Catch::Approx(std::asin(0.5f)));
    REQUIRE(f.centre_angle == Catch::Approx(glm::half_pi<float>()));
  }

  SECTION("r/R = 1.19 (Oak's level-3 twigs) never exceeds 120 degrees total") {
    // Unclamped this is undefined -- asin of 1.19 -- and a socket that wide
    // would sever the parent's ring outright.
    const SocketFootprint f = ComputeSocketFootprint(parent, 2, 0.0f, child, 1.19f);
    REQUIRE(f.valid);
    REQUIRE(f.half_angle <= glm::radians(kMaxSocketHalfAngleDeg) + 1e-5f);
    REQUIRE(2.0f * f.half_angle <= glm::two_pi<float>() / 3.0f + 1e-5f);
    REQUIRE(std::isfinite(f.axial_min));
    REQUIRE(std::isfinite(f.axial_max));
  }

  SECTION("axial extent grows as the branch leaves more obliquely") {
    const glm::quat oblique = glm::angleAxis(glm::radians(20.0f), glm::vec3(1, 0, 0));
    const SocketFootprint square = ComputeSocketFootprint(parent, 2, 0.0f, child, 0.5f);
    const SocketFootprint swept = ComputeSocketFootprint(parent, 2, 0.0f, oblique, 0.5f);
    REQUIRE(swept.valid);
    REQUIRE(swept.axial_max - swept.axial_min > square.axial_max - square.axial_min);
  }
}

TEST_CASE("ComputeSocketFootprint: rejects degenerate parents", "[junction]") {
  const glm::quat child = glm::angleAxis(glm::half_pi<float>(), glm::vec3(1, 0, 0));

  SECTION("a stub trunk -- Bush 1/2 are ~0.05 long in total") {
    const auto stub = StraightTube(4, 0.0f, 0.58f, 0.58f);  // zero-length sections
    const SocketFootprint f = ComputeSocketFootprint(stub, 1, 0.0f, child, 0.2f);
    REQUIRE_FALSE(f.valid);
  }
  SECTION("a zero-radius parent") {
    const auto pin = StraightTube(4, 4.0f, 0.0f, 0.0f);
    REQUIRE_FALSE(ComputeSocketFootprint(pin, 1, 0.0f, child, 0.2f).valid);
  }
}

TEST_CASE("ComputeSocketFootprint: a coaxial child joins axially, not by socket",
          "[junction]") {
  // Bush 3's `angle` is {0, 66.52, 52.83, 0}, so all 208 of its level-3
  // children grow straight along their parent's axis. There is no side to cut
  // into, and 2r/sin(theta) runs away as theta goes to zero -- so these leave
  // through the parent's end ring instead. Rejecting them left three quarters
  // of that preset as loose tubes.
  const auto parent = StraightTube(5, 4.0f, 1.0f, 1.0f);

  SECTION("exactly parallel") {
    const SocketFootprint f =
        ComputeSocketFootprint(parent, 2, 0.0f, glm::quat(1.0f, 0.0f, 0.0f, 0.0f), 0.5f);
    REQUIRE(f.valid);
    REQUIRE(f.axial);
  }

  SECTION("just inside the threshold still joins axially") {
    const float deg = glm::degrees(std::asin(kAxialJoinSinThreshold)) - 1.0f;
    const glm::quat q = glm::angleAxis(glm::radians(deg), glm::vec3(1, 0, 0));
    REQUIRE(ComputeSocketFootprint(parent, 2, 0.0f, q, 0.5f).axial);
  }

  SECTION("just outside it is a normal socket") {
    const float deg = glm::degrees(std::asin(kAxialJoinSinThreshold)) + 1.0f;
    const glm::quat q = glm::angleAxis(glm::radians(deg), glm::vec3(1, 0, 0));
    const SocketFootprint f = ComputeSocketFootprint(parent, 2, 0.0f, q, 0.5f);
    REQUIRE(f.valid);
    REQUIRE_FALSE(f.axial);
  }

  SECTION("axial joins never compete for the parent's surface") {
    // They all meet at the same end ring, so shrinking them against each other
    // would be meaningless.
    SocketFootprint a; a.valid = true; a.axial = true;
    SocketFootprint b; b.valid = true; b.axial = true;
    REQUIRE_FALSE(FootprintsOverlap(a, b));
  }
}

TEST_CASE("ShrinkToDisjoint: resolves the Oak level-3 configuration", "[junction]") {
  // Three children on a 3-segment parent, each clamped to a full 120-degree
  // segment and sitting closer together axially than their footprints are long.
  // They overlap by construction; falling back would discard most of the tree.
  const float full = glm::radians(kMaxSocketHalfAngleDeg);
  std::vector<SocketFootprint> f = {
      {0.0f, full, 0.0f, 2.7f, true},
      {0.3f, full, 1.9f, 4.6f, true},
      {0.6f, full, 3.8f, 6.5f, true},
  };

  const int shrunk = ShrinkToDisjoint(f, glm::radians(10.0f), 0.05f);
  REQUIRE(shrunk > 0);
  for (size_t i = 0; i < f.size(); ++i) {
    for (size_t j = i + 1; j < f.size(); ++j) {
      CAPTURE(i, j);
      REQUIRE_FALSE(FootprintsOverlap(f[i], f[j]));
    }
  }
  // Everything still stitches -- shrinking makes a collar steeper, it does not
  // discard the junction.
  for (const SocketFootprint& s : f) {
    REQUIRE(s.valid);
    REQUIRE(s.half_angle >= glm::radians(10.0f) - 1e-5f);
    REQUIRE(s.axial_max > s.axial_min);
  }
}

TEST_CASE("ShrinkToDisjoint: leaves well-spread footprints untouched", "[junction]") {
  // The generator's stratified placement puts most siblings far apart in angle,
  // height or both -- those must cost nothing.
  std::vector<SocketFootprint> f = {
      {0.0f, 0.2f, 0.0f, 0.5f, true},
      {glm::pi<float>(), 0.2f, 0.0f, 0.5f, true},   // opposite side
      {0.0f, 0.2f, 3.0f, 3.5f, true},               // same angle, far up
  };
  const std::vector<SocketFootprint> before = f;
  REQUIRE(ShrinkToDisjoint(f, glm::radians(10.0f), 0.05f) == 0);
  for (size_t i = 0; i < f.size(); ++i) {
    REQUIRE(f[i].half_angle == Catch::Approx(before[i].half_angle));
    REQUIRE(f[i].axial_min == Catch::Approx(before[i].axial_min));
  }
}

TEST_CASE("MergeRingParams: refines only where a footprint would fall between rings",
          "[junction]") {
  const std::vector<float> authored = {0.0f, 1.0f, 2.0f, 3.0f, 4.0f};

  SECTION("a narrow span gets exactly two bracketing rings") {
    // Pine's case: a ~0.4-tall footprint against ~4.2-tall quads.
    const auto out = MergeRingParams(authored, {{1.9f, 2.1f}}, 2, 1e-4f);
    REQUIRE(out.size() == authored.size() + 2);
    REQUIRE(std::is_sorted(out.begin(), out.end()));
    REQUIRE(std::find(out.begin(), out.end(), 1.9f) != out.end());
    REQUIRE(std::find(out.begin(), out.end(), 2.1f) != out.end());
  }

  SECTION("a span already covering enough rings costs nothing") {
    // Oak's trunk: the footprint is longer than the ring spacing already.
    REQUIRE(MergeRingParams(authored, {{0.5f, 3.5f}}, 2, 1e-4f) == authored);
  }

  SECTION("output is sorted and deduplicated regardless of span order") {
    const auto a = MergeRingParams(authored, {{1.9f, 2.1f}, {2.05f, 2.2f}}, 2, 1e-4f);
    const auto b = MergeRingParams(authored, {{2.05f, 2.2f}, {1.9f, 2.1f}}, 2, 1e-4f);
    REQUIRE(a == b);
    REQUIRE(std::is_sorted(a.begin(), a.end()));
    for (size_t i = 1; i < a.size(); ++i) REQUIRE(a[i] - a[i - 1] > 1e-4f);
  }

  SECTION("spans outside the branch are clamped away, never appended past the ends") {
    const auto out = MergeRingParams(authored, {{-2.0f, 0.0f}, {4.0f, 9.0f}}, 2, 1e-4f);
    REQUIRE(out.front() == Catch::Approx(0.0f));
    REQUIRE(out.back() == Catch::Approx(4.0f));
  }
}

TEST_CASE("SmoothVertexNormals: averages only its targets and never moves a vertex",
          "[junction]") {
  constexpr size_t kStride = kTexturedMeshFloatsPerVertex;
  // Two triangles meeting at a 90-degree crease along the x axis, with both
  // faces' normals stored as-is; vertex 1 and 2 are on the crease.
  std::vector<float> verts;
  auto push = [&](glm::vec3 p, glm::vec3 n) {
    // pos(3) uv(2) normal(3) tangent(4) -- the trailing 1.0f is the tangent's
    // handedness, without which this fixture writes a short vertex and every
    // subsequent one reads its attributes from its neighbour.
    verts.insert(verts.end(), {p.x, p.y, p.z, 0.0f, 0.0f,
                               n.x, n.y, n.z, 1.0f, 0.0f, 0.0f, 1.0f});
  };
  push({0, 0, 0}, {0, 0, 1});
  push({1, 0, 0}, {0, 0, 1});
  push({1, 1, 0}, {0, 0, 1});
  push({0, 1, 0}, {0, 0, 1});
  const std::vector<float> before = verts;
  const std::vector<uint32_t> indices = {0, 1, 2, 0, 2, 3};

  SmoothVertexNormals(verts, kStride, indices, {1u});

  // Positions and UVs are untouched -- this is what keeps the bark AABB, and
  // therefore CrownRadiusM and the forest's spacing, exactly where it was.
  for (uint32_t v = 0; v < 4; ++v) {
    for (size_t f = 0; f < 5; ++f) {
      CAPTURE(v, f);
      REQUIRE(verts[v * kStride + f] == before[v * kStride + f]);
    }
  }
  // Untargeted vertices keep their swept normal verbatim.
  for (uint32_t v : {0u, 2u, 3u})
    for (size_t f = 5; f < kStride; ++f)
      REQUIRE(verts[v * kStride + f] == before[v * kStride + f]);

  const glm::vec3 n(verts[kStride + 5], verts[kStride + 6], verts[kStride + 7]);
  REQUIRE(glm::length(n) == Catch::Approx(1.0f));
  const glm::vec3 t(verts[kStride + 8], verts[kStride + 9], verts[kStride + 10]);
  REQUIRE(glm::length(t) == Catch::Approx(1.0f));
  REQUIRE(glm::dot(n, t) == Catch::Approx(0.0f).margin(1e-5f));
}
