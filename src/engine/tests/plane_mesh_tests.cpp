// The debug plane's geometry, on the CPU, with no device at all.
//
// Every one of these properties is invisible in a rendered image until it is
// wrong in a way that empties the frame. A reversed winding renders NOTHING
// under backface culling and reads as a missing draw call; a tangent that is
// not orthogonal to the normal skews the normal map by an amount that looks
// like a lighting choice. Asserting them here means a failure in stage 4d is a
// failure in the resolve, not a mesh bug wearing a resolve's clothes.

#include <cmath>

#include <catch_amalgamated.hpp>
#include <glm/glm.hpp>

#include "executables/object_viewer/plane_mesh.hpp"

using namespace badlands::object_viewer;

namespace {

glm::vec3 PositionOf(const PlaneVertex& v) { return glm::vec3(v.pos_nx); }
glm::vec3 NormalOf(const PlaneVertex& v) {
  return {v.pos_nx.w, v.nyz_uv.x, v.nyz_uv.y};
}
glm::vec2 UvOf(const PlaneVertex& v) { return {v.nyz_uv.z, v.nyz_uv.w}; }

}  // namespace

TEST_CASE("plane: 8x8 quads make 128 triangles", "[plane]") {
  // 128, not 2. A two-triangle quad makes the triangle-ID debug view a
  // two-colour image, which cannot tell a correct resolve from a plausible one
  // -- and puts no internal edges in front of the gradient code.
  const PlaneMesh mesh = BuildPlaneMesh();
  CHECK(mesh.vertices.size() == 9 * 9);
  CHECK(mesh.indices.size() == 8 * 8 * 6);
  CHECK(mesh.TriangleCount() == 128);
  CHECK(ValidatePrimitiveCount(mesh));
}

TEST_CASE("plane: every index is in range", "[plane]") {
  const PlaneMesh mesh = BuildPlaneMesh();
  for (uint32_t i : mesh.indices) {
    REQUIRE(i < mesh.vertices.size());
  }
}

TEST_CASE("plane: spans the requested extent and lies on y = 0", "[plane]") {
  const PlaneMesh mesh = BuildPlaneMesh(5.0f);
  float min_x = 1e9f, max_x = -1e9f, min_z = 1e9f, max_z = -1e9f;
  for (const auto& v : mesh.vertices) {
    const glm::vec3 p = PositionOf(v);
    CHECK(p.y == Catch::Approx(0.0f));
    min_x = std::min(min_x, p.x);
    max_x = std::max(max_x, p.x);
    min_z = std::min(min_z, p.z);
    max_z = std::max(max_z, p.z);
  }
  CHECK(min_x == Catch::Approx(-5.0f));
  CHECK(max_x == Catch::Approx(5.0f));
  CHECK(min_z == Catch::Approx(-5.0f));
  CHECK(max_z == Catch::Approx(5.0f));
}

TEST_CASE("plane: UVs tile the requested number of times", "[plane]") {
  const PlaneMesh mesh = BuildPlaneMesh(5.0f, 4.0f);
  float max_u = 0, max_v = 0, min_u = 1e9f, min_v = 1e9f;
  for (const auto& v : mesh.vertices) {
    const glm::vec2 uv = UvOf(v);
    max_u = std::max(max_u, uv.x);
    max_v = std::max(max_v, uv.y);
    min_u = std::min(min_u, uv.x);
    min_v = std::min(min_v, uv.y);
  }
  CHECK(min_u == Catch::Approx(0.0f));
  CHECK(min_v == Catch::Approx(0.0f));
  CHECK(max_u == Catch::Approx(4.0f));
  CHECK(max_v == Catch::Approx(4.0f));
}

TEST_CASE("plane: every triangle winds counter-clockwise from above",
          "[plane]") {
  // THE ONE THAT EMPTIES THE FRAME. The pipeline culls back faces with
  // FrontFace::Ccw, so a reversed winding renders nothing at all and reads as a
  // missing draw call rather than as a geometry mistake. Checked per triangle
  // rather than on one sample: a single flipped quad is a hole, not a blank
  // screen, and is far harder to spot.
  const PlaneMesh mesh = BuildPlaneMesh();
  for (size_t t = 0; t < mesh.indices.size(); t += 3) {
    const glm::vec3 a = PositionOf(mesh.vertices[mesh.indices[t]]);
    const glm::vec3 b = PositionOf(mesh.vertices[mesh.indices[t + 1]]);
    const glm::vec3 c = PositionOf(mesh.vertices[mesh.indices[t + 2]]);
    // Right-handed cross product; +y means counter-clockwise seen from above.
    const glm::vec3 n = glm::cross(b - a, c - a);
    INFO("triangle " << t / 3 << " normal y = " << n.y);
    CHECK(n.y > 0.0f);
  }
}

TEST_CASE("plane: the tangent frame is orthonormal and right-handed",
          "[plane]") {
  const PlaneMesh mesh = BuildPlaneMesh();
  for (const auto& v : mesh.vertices) {
    const glm::vec3 n = NormalOf(v);
    const glm::vec3 t = glm::vec3(v.tangent);
    CHECK(glm::length(n) == Catch::Approx(1.0f));
    CHECK(glm::length(t) == Catch::Approx(1.0f));
    // A tangent not perpendicular to the normal skews the normal map by an
    // amount that looks like a lighting choice rather than a bug.
    CHECK(glm::dot(n, t) == Catch::Approx(0.0f).margin(1e-6));
    // The handedness sign must be one of the two values it can mean. Three
    // components cannot express it, which is why it rides in tangent.w.
    CHECK((v.tangent.w == 1.0f || v.tangent.w == -1.0f));
  }
}

TEST_CASE("plane: the packing's primitive limit is enforced, not assumed",
          "[plane]") {
  // The shader MASKS the primitive so an overflow cannot fetch an out-of-bounds
  // DrawInfo; this is what REPORTS it. A mask alone contains the damage
  // silently, and the CPU is the only side that knows the triangle count.
  PlaneMesh ok = BuildPlaneMesh();
  CHECK(ValidatePrimitiveCount(ok));

  PlaneMesh too_many;
  too_many.vertices.resize(3);
  too_many.indices.resize(size_t(kMaxPrimitivesPerDraw + 1) * 3, 0);
  CHECK_FALSE(ValidatePrimitiveCount(too_many));

  PlaneMesh ragged;
  ragged.indices.resize(4, 0);  // not a whole number of triangles
  CHECK_FALSE(ValidatePrimitiveCount(ragged));
}
