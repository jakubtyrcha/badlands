#include <catch_amalgamated.hpp>
#include "game/geometry/tree_options.hpp"

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
