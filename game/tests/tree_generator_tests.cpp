#include <catch_amalgamated.hpp>
#include <cmath>
#include "game/geometry/tree_options.hpp"
#include "game/geometry/tree_generator.hpp"
#include "game/geometry/leaf_texture.hpp"
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

TEST_CASE("TreeCatalog: every predefined setup generates a well-formed mesh") {
  const std::vector<NamedTreeOptions> catalog = TreeCatalog();
  REQUIRE(catalog.size() == 15u);  // oak/pine/ash/aspen x3 sizes + 3 bushes

  for (const NamedTreeOptions& setup : catalog) {
    INFO("setup: " << setup.name);
    REQUIRE_FALSE(setup.name.empty());

    const TexturedMeshResult r = GenerateTreeMesh(setup.options);
    const auto& m = r.mesh;
    REQUIRE(m.vertex_count > 0u);
    REQUIRE(m.vertices.size() == m.vertex_count * kTexturedMeshFloatsPerVertex);
    REQUIRE_FALSE(m.indices.empty());
    REQUIRE(m.indices.size() % 3 == 0);

    bool indices_in_range = true;
    for (uint32_t idx : m.indices)
      if (idx >= m.vertex_count) indices_in_range = false;
    REQUIRE(indices_in_range);

    bool all_finite = true;
    for (float f : m.vertices)
      if (!std::isfinite(f)) all_finite = false;
    REQUIRE(all_finite);

    const float height = r.local_bounds.max.y - r.local_bounds.min.y;
    REQUIRE(std::isfinite(height));
    REQUIRE(height > 0.0f);
  }
}

TEST_CASE("GenerateLeafMesh: deterministic") {
  const TexturedMeshResult a = GenerateLeafMesh(OakPreset());
  const TexturedMeshResult b = GenerateLeafMesh(OakPreset());
  REQUIRE(a.mesh.vertices == b.mesh.vertices);
  REQUIRE(a.mesh.indices == b.mesh.indices);
}

TEST_CASE("GenerateLeafMesh: well-formed indexed mesh (Oak, Pine)") {
  for (const TreeOptions& o : {OakPreset(), PinePreset()}) {
    const TexturedMeshResult r = GenerateLeafMesh(o);
    const auto& m = r.mesh;
    REQUIRE(m.vertex_count > 0u);
    REQUIRE(m.vertices.size() == m.vertex_count * kTexturedMeshFloatsPerVertex);
    REQUIRE(m.indices.size() % 3 == 0);
    for (uint32_t idx : m.indices) REQUIRE(idx < m.vertex_count);
    for (float f : m.vertices) REQUIRE(std::isfinite(f));
  }
}

TEST_CASE("GenerateLeafMesh: billboard=2 is exactly 2x billboard=1") {
  TreeOptions single = OakPreset();
  single.leaves.billboard = 1;
  TreeOptions cross = OakPreset();
  cross.leaves.billboard = 2;

  const TexturedMeshResult r1 = GenerateLeafMesh(single);
  const TexturedMeshResult r2 = GenerateLeafMesh(cross);

  REQUIRE(r1.mesh.vertex_count > 0u);
  REQUIRE(r2.mesh.vertex_count == r1.mesh.vertex_count * 2u);
  REQUIRE(r2.mesh.indices.size() == r1.mesh.indices.size() * 2u);

  // Each quad = 4 verts + 6 indices.
  REQUIRE(r1.mesh.indices.size() == r1.mesh.vertex_count / 4u * 6u);
  REQUIRE(r2.mesh.indices.size() == r2.mesh.vertex_count / 4u * 6u);
}

TEST_CASE("GenerateLeafMesh: disabled produces an empty mesh") {
  TreeOptions o = OakPreset();
  o.leaves.enabled = false;
  const TexturedMeshResult r = GenerateLeafMesh(o);
  REQUIRE(r.mesh.vertex_count == 0u);
  REQUIRE(r.mesh.indices.empty());
}

TEST_CASE("BuildLeafRgba8: leaf-shaped alpha card") {
  const int n = 64;
  const std::vector<uint8_t> px = BuildLeafRgba8(n, glm::vec3(0.30f, 0.55f, 0.18f));
  REQUIRE(px.size() == static_cast<size_t>(n) * static_cast<size_t>(n) * 4);
  auto alpha = [&](int x, int y) {
    return px[(static_cast<size_t>(y) * n + static_cast<size_t>(x)) * 4 + 3];
  };
  REQUIRE(alpha(n / 2, n / 2) == 255);   // center is inside the leaf
  REQUIRE(alpha(0, 0) == 0);             // corners are outside
  REQUIRE(alpha(n - 1, 0) == 0);
  REQUIRE(alpha(0, n - 1) == 0);
  REQUIRE(alpha(n - 1, n - 1) == 0);
  // RGB carries the leaf color (green > red at the center texel).
  const size_t c = (static_cast<size_t>(n / 2) * n + static_cast<size_t>(n / 2)) * 4;
  REQUIRE(px[c + 1] > px[c + 0]);
}

TEST_CASE("GenerateLeafMesh: terminal-tip leaf adds one leaf per leaf-bearing branch") {
  auto count_terminal = [](const TreeOptions& o) {
    int n = 0;
    for (const SkeletonBranch& b : BuildTreeSkeleton(o))
      if (b.level == o.levels && static_cast<int>(b.sections.size()) - 1 >= 1) ++n;
    return n;
  };
  TreeOptions on = OakPreset();  on.leaves.tip_leaf = true;
  TreeOptions off = OakPreset(); off.leaves.tip_leaf = false;
  const uint32_t vn = GenerateLeafMesh(on).mesh.vertex_count;
  const uint32_t vf = GenerateLeafMesh(off).mesh.vertex_count;
  const int quads = (OakPreset().leaves.billboard >= 2) ? 2 : 1;
  REQUIRE(vn > vf);
  // Each tip leaf = quads * 4 verts; one per leaf-bearing terminal branch.
  REQUIRE(vn - vf == static_cast<uint32_t>(count_terminal(OakPreset()) * quads * 4));
}
