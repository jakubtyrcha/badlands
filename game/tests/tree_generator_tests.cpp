#include <catch_amalgamated.hpp>
#include <cmath>
#include "game/geometry/tree_options.hpp"
#include "game/geometry/tree_generator.hpp"
#include "engine/rendering/geometry/textured_mesh_builders.hpp"

using namespace badlands;

TEST_CASE("OakPreset and PinePreset carry the expected ez-tree values") {
  const TreeOptions oak = OakPreset();
  REQUIRE(oak.seed == 35729u);
  REQUIRE(oak.type == TreeType::Deciduous);
  REQUIRE(oak.levels == 3);
  REQUIRE(oak.children[0] == 6);
  REQUIRE(oak.sections[0] == 8);

  const TreeOptions pine = PinePreset();
  REQUIRE(pine.seed == 13977u);
  REQUIRE(pine.type == TreeType::Evergreen);
  REQUIRE(pine.levels == 1);
  REQUIRE(pine.children[0] == 82);
}

TEST_CASE("BuildTreeSkeleton: deterministic branch structure") {
  const auto oak = BuildTreeSkeleton(OakPreset());
  // Structure is fixed by recursion (1 continuation + children[level] per branch):
  // L0=1, L1=7, L2=35, L3=140 -> 183.
  REQUIRE(oak.size() == 183u);
  // Evergreen: trunk + children[0] radial, no continuation.
  const auto pine = BuildTreeSkeleton(PinePreset());
  REQUIRE(pine.size() == 83u);
}

TEST_CASE("BuildTreeSkeleton: trunk rooted at origin, tapers, deterministic") {
  const auto a = BuildTreeSkeleton(OakPreset());
  const auto b = BuildTreeSkeleton(OakPreset());
  REQUIRE(a.size() == b.size());

  const SkeletonBranch& trunk = a[0];
  REQUIRE(trunk.sections.size() == static_cast<size_t>(OakPreset().sections[0] + 1));
  REQUIRE(glm::length(trunk.sections.front().origin) == Catch::Approx(0.0f));
  // Base wider than tip.
  REQUIRE(trunk.sections.front().radius > trunk.sections.back().radius);
  // Run-twice identical (determinism): compare a mid branch's first origin.
  REQUIRE(a[10].sections.front().origin.x == Catch::Approx(b[10].sections.front().origin.x));
  REQUIRE(a[10].sections.front().origin.y == Catch::Approx(b[10].sections.front().origin.y));
  REQUIRE(a[10].sections.front().origin.z == Catch::Approx(b[10].sections.front().origin.z));
}

TEST_CASE("GenerateTreeMesh: well-formed indexed mesh") {
  const TexturedMeshResult r = GenerateTreeMesh(OakPreset());
  const auto& m = r.mesh;
  REQUIRE(m.vertex_count > 0u);
  REQUIRE(m.vertices.size() == m.vertex_count * kTexturedMeshFloatsPerVertex);
  REQUIRE_FALSE(m.indices.empty());
  REQUIRE(m.indices.size() % 3 == 0);
  for (uint32_t idx : m.indices) REQUIRE(idx < m.vertex_count);
  for (float f : m.vertices) REQUIRE(std::isfinite(f));
  // Base on floor, grows up.
  REQUIRE(r.local_bounds.min.y == Catch::Approx(0.0f).margin(0.05f));
  REQUIRE(r.local_bounds.max.y > 1.0f);
}

TEST_CASE("GenerateTreeMesh: deterministic, seed-sensitive") {
  const TexturedMeshResult a = GenerateTreeMesh(OakPreset());
  const TexturedMeshResult b = GenerateTreeMesh(OakPreset());
  REQUIRE(a.mesh.vertices == b.mesh.vertices);
  REQUIRE(a.mesh.indices == b.mesh.indices);

  TreeOptions other = OakPreset();
  other.seed = 999u;
  const TexturedMeshResult c = GenerateTreeMesh(other);
  REQUIRE(c.mesh.vertices != a.mesh.vertices);  // different tree
}

TEST_CASE("GenerateTreeMesh: exact counts for the (continuation-free) Pine") {
  // Pine is evergreen -> no stem continuation -> clean per-level counts.
  // Trunk: 13 rings * (8+1) = 117 verts; 82 branches * (11 rings * (6+1)) = 6314.
  // Indices: 12*8*6 + 82*(10*6*6) = 576 + 29520 = 30096.
  const TexturedMeshResult p = GenerateTreeMesh(PinePreset());
  REQUIRE(p.mesh.vertex_count == 6431u);
  REQUIRE(p.mesh.indices.size() == 30096u);
}
