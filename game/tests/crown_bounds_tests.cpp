// The bounding box -> spacing circle conversion.
//
// Small enough to look correct by inspection, which is exactly why it is worth
// pinning: it sets the exclusion radius for every tree in the world, so being
// wrong by a constant factor here is invisible in review and shows up only as a
// forest that is mysteriously too thin or whose crowns grow through each other.

#include <catch_amalgamated.hpp>

#include "game/visual/crown_bounds.hpp"

using namespace badlands;

namespace {

// A box spanning [-x, x] by [-z, z] horizontally. Y is irrelevant to a radius
// about the vertical axis, so it is fixed and arbitrary.
Aabb Horizontal(float min_x, float max_x, float min_z, float max_z) {
  return Aabb{glm::vec3(min_x, 0.0f, min_z), glm::vec3(max_x, 10.0f, max_z)};
}

}  // namespace

TEST_CASE("A symmetric crown reports its half-extent", "[foliage]") {
  CHECK(CrownRadiusM(Horizontal(-3.0f, 3.0f, -3.0f, 3.0f), 1.0f) ==
        Catch::Approx(3.0f));
}

TEST_CASE("The radius is the largest half-extent, not the box diagonal",
          "[foliage]") {
  // THE decision this file exists for. A crown is roughly a disc INSCRIBED in
  // its bounding box, not one circumscribing it: taking the corner distance
  // (here sqrt(3^2 + 4^2) = 5) would inflate every radius by up to sqrt(2) and,
  // under a sum-of-radii rule, space the whole forest out by ~40% for geometry
  // that is not there.
  CHECK(CrownRadiusM(Horizontal(-3.0f, 3.0f, -4.0f, 4.0f), 1.0f) ==
        Catch::Approx(4.0f));
}

TEST_CASE("A leaning crown is measured from the trunk axis", "[foliage]") {
  // An off-centre box belongs to a tree that leans. Instances are randomly
  // yawed about the trunk axis (local x = z = 0), so the footprint they sweep
  // is a circle about THAT, not about the box's own centre -- measuring from
  // the centre would report 3 m for a crown that reaches 5 m out on one side
  // and let a neighbour stand inside it.
  CHECK(CrownRadiusM(Horizontal(-1.0f, 5.0f, -1.0f, 1.0f), 1.0f) ==
        Catch::Approx(5.0f));

  // Symmetric under reflection: the lean's direction cannot matter, because
  // yaw is uniform.
  CHECK(CrownRadiusM(Horizontal(-5.0f, 1.0f, -1.0f, 1.0f), 1.0f) ==
        Catch::Approx(5.0f));
}

TEST_CASE("Bounds are scaled from native units to metres", "[foliage]") {
  // Bounds arrive in the tree generator's native units; the radius has to come
  // out in the same metres the sampler spaces in.
  CHECK(CrownRadiusM(Horizontal(-3.0f, 3.0f, -3.0f, 3.0f), 2.5f) ==
        Catch::Approx(7.5f));
  CHECK(CrownRadiusM(Horizontal(-8.0f, 8.0f, -8.0f, 8.0f), 0.25f) ==
        Catch::Approx(2.0f));
}

TEST_CASE("An empty bounding box reports no radius", "[foliage]") {
  // Aabb::Empty() is INVERTED (min = +FLT_MAX, max = -FLT_MAX), so a naive
  // half-extent would read FLT_MAX and exclude the entire map. A model that
  // produced no geometry must report 0 and simply take no room.
  CHECK(CrownRadiusM(Aabb::Empty(), 1.0f) == 0.0f);
}
