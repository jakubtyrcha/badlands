#include <catch_amalgamated.hpp>
#include "game/geometry/tree_options.hpp"
#include "game/geometry/tree_generator.hpp"

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
