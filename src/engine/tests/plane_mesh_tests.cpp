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

#include "executables/object_viewer/mesh_types.hpp"
#include "executables/object_viewer/plane_mesh.hpp"
#include "executables/object_viewer/sphere_grid.hpp"

using namespace badlands::object_viewer;

namespace {

glm::vec3 PositionOf(const MeshVertex& v) { return glm::vec3(v.pos_nx); }
glm::vec3 NormalOf(const MeshVertex& v) {
  return {v.pos_nx.w, v.nyz_uv.x, v.nyz_uv.y};
}
glm::vec2 UvOf(const MeshVertex& v) { return {v.nyz_uv.z, v.nyz_uv.w}; }

}  // namespace

TEST_CASE("plane: 8x8 quads make 128 triangles", "[plane]") {
  // 128, not 2. A two-triangle quad makes the triangle-ID debug view a
  // two-colour image, which cannot tell a correct resolve from a plausible one
  // -- and puts no internal edges in front of the gradient code.
  const SceneMesh mesh = BuildPlaneMesh();
  CHECK(mesh.vertices.size() == 9 * 9);
  CHECK(mesh.indices.size() == 8 * 8 * 6);
  CHECK(mesh.TriangleCount() == 128);
  CHECK(ValidateSceneMesh(mesh));
}

TEST_CASE("plane: every index is in range", "[plane]") {
  const SceneMesh mesh = BuildPlaneMesh();
  for (uint32_t i : mesh.indices) {
    REQUIRE(i < mesh.vertices.size());
  }
}

TEST_CASE("plane: spans the requested extent and lies on y = 0", "[plane]") {
  const SceneMesh mesh = BuildPlaneMesh(5.0f);
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
  const SceneMesh mesh = BuildPlaneMesh(5.0f, 4.0f);
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
  const SceneMesh mesh = BuildPlaneMesh();
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
  const SceneMesh mesh = BuildPlaneMesh();
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
  SceneMesh ok = BuildPlaneMesh();
  CHECK(ValidateSceneMesh(ok));

  SceneMesh too_many;
  too_many.vertices.resize(3);
  too_many.indices.resize(size_t(kMaxPrimitivesPerDraw + 1) * 3, 0);
  CHECK_FALSE(ValidateSceneMesh(too_many));

  SceneMesh ragged;
  ragged.indices.resize(4, 0);  // not a whole number of triangles
  CHECK_FALSE(ValidateSceneMesh(ragged));
}

// --- The sphere chart -------------------------------------------------------

TEST_CASE("spheres: one mesh, one instance per chart cell", "[plane]") {
  const SceneMesh grid = BuildSphereGrid();
  CHECK(ValidateSceneMesh(grid));
  CHECK(grid.InstanceCount() == kSphereCount);
  CHECK(grid.InstanceCount() == kRoughnessSteps * kMetallicSteps);
  // ONE mesh, not fourteen: the whole point of the instanced draw is that the
  // vertex data is shared. A build that concatenated fourteen spheres would
  // have fourteen times the vertices and still pass every other check here.
  CHECK(grid.vertices.size() ==
        size_t(kSphereSegments + 1) * (kSphereRings + 1));
}

TEST_CASE("spheres: the chart sweeps roughness and metallic", "[plane]") {
  const SceneMesh grid = BuildSphereGrid();
  for (uint32_t row = 0; row < kMetallicSteps; ++row) {
    float previous = -1.0f;
    for (uint32_t col = 0; col < kRoughnessSteps; ++col) {
      const DrawInfo& d = grid.draws[row * kRoughnessSteps + col];
      // BOTH overridden: a chart that read roughness from the pack would sweep
      // nothing, and the ARM map is constant in the tests.
      CHECK((d.override_mask & kOverrideRoughness) != 0u);
      CHECK((d.override_mask & kOverrideMetallic) != 0u);
      CHECK(d.material.x > previous);
      previous = d.material.x;
      CHECK(d.material.y == Catch::Approx(row == 0 ? 0.0f : 1.0f));
    }
    // The endpoints are the whole range: a mirror at one end, fully rough at
    // the other. A sweep over 0.2..0.8 would be monotonic and would exercise
    // neither the mirror path nor the energy deficit.
    CHECK(grid.draws[row * kRoughnessSteps].material.x == Catch::Approx(0.0f));
    CHECK(grid.draws[row * kRoughnessSteps + kRoughnessSteps - 1].material.x ==
          Catch::Approx(1.0f));
  }
}

TEST_CASE("spheres: instances are distinct and the layout is shared", "[plane]") {
  const SceneMesh grid = BuildSphereGrid();
  // No two spheres sit in the same place -- an offset that was never written
  // would stack all fourteen at the origin, which renders as ONE sphere and
  // still sweeps roughness in the DrawInfo array.
  for (size_t i = 0; i < grid.draws.size(); ++i) {
    for (size_t j = i + 1; j < grid.draws.size(); ++j) {
      CHECK(glm::length(glm::vec3(grid.draws[i].offset) -
                        glm::vec3(grid.draws[j].offset)) > 0.5f);
    }
  }
  // The oracle's layout function and the mesh's must agree, or every predicted
  // pixel lands beside the sphere it was meant to sample.
  for (uint32_t row = 0; row < kMetallicSteps; ++row) {
    for (uint32_t col = 0; col < kRoughnessSteps; ++col) {
      const glm::vec3 want = SphereGridCenter(col, row);
      const glm::vec3 got =
          glm::vec3(grid.draws[row * kRoughnessSteps + col].offset);
      CHECK(glm::length(want - got) < 1e-5f);
    }
  }
}

TEST_CASE("spheres: normals point outward and the winding is CCW", "[plane]") {
  const SceneMesh grid = BuildSphereGrid();
  // A unit sphere at the origin: the normal IS the normalized position, and the
  // face winding must be counter-clockwise seen from outside or CullMode::Back
  // erases the whole chart -- which reads as a missing draw call.
  for (size_t t = 0; t < grid.indices.size(); t += 3) {
    const glm::vec3 a = glm::vec3(grid.vertices[grid.indices[t + 0]].pos_nx);
    const glm::vec3 b = glm::vec3(grid.vertices[grid.indices[t + 1]].pos_nx);
    const glm::vec3 c = glm::vec3(grid.vertices[grid.indices[t + 2]].pos_nx);
    const glm::vec3 centroid = (a + b + c) / 3.0f;
    // Degenerate triangles exist at the poles, where the ring collapses; skip
    // those rather than assert a normal they do not have.
    const glm::vec3 face = glm::cross(b - a, c - a);
    if (glm::length(face) < 1e-6f) continue;
    INFO("triangle " << t / 3);
    CHECK(glm::dot(glm::normalize(face), glm::normalize(centroid)) > 0.0f);
  }
}
