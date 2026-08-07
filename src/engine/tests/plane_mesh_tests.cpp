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
  //
  // Eight octahedron faces, each a triangular lattice of (n+1)(n+2)/2 vertices.
  // Faces do not share vertices -- the octahedron's edges are exactly where the
  // octahedral UV map is discontinuous.
  const size_t n = kSphereSubdivisions;
  CHECK(grid.vertices.size() == 8 * (n + 1) * (n + 2) / 2);
  CHECK(grid.TriangleCount() == 8 * n * n);
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
    // NO SKIP. The UV sphere this replaced had degenerate polar triangles that
    // had to be stepped over, and a `continue` there would now hide a genuine
    // degeneracy by making its winding assertion pass vacuously. The octahedron
    // has none -- which the area test above asserts rather than assumes.
    const glm::vec3 face = glm::cross(b - a, c - a);
    INFO("triangle " << t / 3);
    CHECK(glm::dot(glm::normalize(face), glm::normalize(centroid)) > 0.0f);
  }
}

TEST_CASE("spheres: the octahedral map has no pole and no degenerate triangle",
          "[plane]") {
  const SceneMesh grid = BuildSphereGrid();

  // THE REASON THIS REPLACED A UV SPHERE. A UV sphere's rings converge at the
  // poles into a fan of degenerate slivers whose UVs pinch to a point and whose
  // tangent frame is undefined -- which reads as a smeared, aliased artifact on
  // the tips, worst on exactly the low-roughness cells where the reflection is
  // sharpest. A subdivided octahedron has no such vertex anywhere.
  float smallest = 1e9f, largest = 0.0f, total = 0.0f;
  for (size_t t = 0; t < grid.indices.size(); t += 3) {
    const glm::vec3 a(grid.vertices[grid.indices[t + 0]].pos_nx);
    const glm::vec3 b(grid.vertices[grid.indices[t + 1]].pos_nx);
    const glm::vec3 c(grid.vertices[grid.indices[t + 2]].pos_nx);
    const float area = 0.5f * glm::length(glm::cross(b - a, c - a));
    smallest = std::min(smallest, area);
    largest = std::max(largest, area);
    total += area;
  }
  const float average = total / float(grid.TriangleCount());
  INFO("smallest " << smallest << " largest " << largest << " average "
                   << average);

  // THE CLAIM THAT MATTERS: the smallest triangle is a real fraction of the
  // average. This is what a UV sphere cannot satisfy at any ring count -- its
  // polar triangles shrink toward zero as the tessellation refines, so the
  // ratio is not merely large, it is unbounded.
  CHECK(smallest > 0.2f * average);

  // And the overall spread is bounded. 4.7 is MEASURED for this tessellation,
  // not a target: normalizing an octahedron compresses each face's corners
  // relative to its centre, and that is the price of having no pole at all.
  CHECK(largest / smallest < 6.0f);
}

TEST_CASE("spheres: every vertex is on the sphere with a matching normal",
          "[plane]") {
  const SceneMesh grid = BuildSphereGrid(1.0f);
  for (const auto& v : grid.vertices) {
    const glm::vec3 p(v.pos_nx);
    const glm::vec3 n(v.pos_nx.w, v.nyz_uv.x, v.nyz_uv.y);
    CHECK(glm::length(p) == Catch::Approx(1.0f).margin(1e-5));
    CHECK(glm::length(n) == Catch::Approx(1.0f).margin(1e-5));
    // On a sphere the normal IS the normalized position, which makes this a
    // closed form rather than a smoothing check.
    CHECK(glm::dot(n, glm::normalize(p)) == Catch::Approx(1.0f).margin(1e-5));
  }
}

TEST_CASE("spheres: no triangle straddles a UV seam", "[plane]") {
  // THE ASSERTION THE BOUNDS CHECK BELOW CANNOT MAKE, and the one that catches
  // a real seam. A vertex whose fold branch came out wrong still lands inside
  // [0,1]^2 and still leaves the min/max over all vertices at exactly 0 and 1 --
  // so coverage passes while the texture is wrapped across the whole map on a
  // strip two triangles wide.
  //
  // What a seam actually looks like is a UV edge far longer than the geometry
  // edge that carries it. On a lattice of n subdivisions no correct triangle
  // spans more than about 1/n of the map, so anything near half a map is a
  // fold that disagreed with its neighbour.
  const SceneMesh grid = BuildSphereGrid();

  // The WORST edge, found first and asserted once -- rather than an assertion
  // per edge, which aborts on the mildest violation and reports a number that
  // says nothing about how bad the seam is.
  float worst = 0.0f;
  size_t worst_tri = 0;
  for (size_t t = 0; t < grid.indices.size(); t += 3) {
    glm::vec2 uv[3];
    for (int k = 0; k < 3; ++k) {
      const auto& v = grid.vertices[grid.indices[t + size_t(k)]];
      uv[k] = glm::vec2(v.nyz_uv.z, v.nyz_uv.w);
    }
    for (int k = 0; k < 3; ++k) {
      const float len = glm::length(uv[(k + 1) % 3] - uv[k]);
      if (len > worst) {
        worst = len;
        worst_tri = t / 3;
      }
    }
  }

  // A correct lattice's longest UV edge is about 1/n of the map; the budget is
  // a little over that. A vertex that took the wrong fold branch lands most of
  // a map away from its neighbours, so the two are not close.
  const float budget = 1.6f / float(kSphereSubdivisions);
  INFO("worst uv edge " << worst << " on triangle " << worst_tri
                        << ", budget " << budget);
  CHECK(worst < budget);
  CHECK(worst > 0.0f);  // and it is a real mesh, not an empty loop
}

TEST_CASE("spheres: octahedral UVs cover the unit square and fold at the "
          "equator", "[plane]") {
  // The map must be onto [0,1]^2 -- a parameterization that used half the
  // square would waste half of every texture and show as a doubled tiling.
  const SceneMesh grid = BuildSphereGrid();
  glm::vec2 lo(2.0f), hi(-1.0f);
  for (const auto& v : grid.vertices) {
    const glm::vec2 uv(v.nyz_uv.z, v.nyz_uv.w);
    lo = glm::min(lo, uv);
    hi = glm::max(hi, uv);
  }
  CHECK(lo.x == Catch::Approx(0.0f).margin(1e-4));
  CHECK(lo.y == Catch::Approx(0.0f).margin(1e-4));
  CHECK(hi.x == Catch::Approx(1.0f).margin(1e-4));
  CHECK(hi.y == Catch::Approx(1.0f).margin(1e-4));

  // The fold: +Y is the centre of the square, -Y goes to the corners, and the
  // equator is the diamond between them. Getting the fold wrong is the classic
  // octahedral bug and it looks like a plausible but wrong texture layout.
  CHECK(glm::length(OctEncode({0, 1, 0}) - glm::vec2(0.5f, 0.5f)) < 1e-5f);
  const glm::vec2 down = OctEncode({0, -1, 0});
  CHECK((std::abs(down.x - 0.0f) < 1e-5f || std::abs(down.x - 1.0f) < 1e-5f));
  // On the equator the map is continuous with the upper hemisphere: +X sits at
  // the middle of an edge either way.
  CHECK(glm::length(OctEncode({1, 0, 0}) - glm::vec2(1.0f, 0.5f)) < 1e-5f);
  CHECK(glm::length(OctEncode({0, 0, 1}) - glm::vec2(0.5f, 1.0f)) < 1e-5f);
}
