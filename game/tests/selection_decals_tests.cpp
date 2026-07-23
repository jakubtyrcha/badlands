// Pure-CPU tests for the selection -> ProjectedDecal mapping
// (src/game/visual/selection_decals.{hpp,cpp}).
//
// These pin the GAME side of the decal feature: which shape a selected unit /
// building becomes, where it sits, how big it is, and how a building's
// placement rotation reaches the decal. The rendering of those decals is
// covered separately by badlands_decal_tests (GPU) and their underlying math by
// badlands_decal_math_tests (CPU).

#include <catch_amalgamated.hpp>

#include <glm/glm.hpp>

#include "badlands_sim.hpp"
#include "game/visual/selection_decals.hpp"

using namespace badlands;

namespace {

constexpr float kEps = 1e-4f;

CharacterState MakeUnit(float x, float z, float size_x, float size_z) {
  CharacterState c{};
  c.id = 7;
  c.pos_x = x;
  c.pos_z = z;
  c.size_x = size_x;
  c.size_y = 1.0f;
  c.size_z = size_z;
  c.inside_building_id = -1;
  return c;
}

BuildingState MakeBuilding(BuildingKind kind, float x, float z,
                           int32_t rotation_index) {
  BuildingState b{};
  b.id = 3;
  b.kind = kind;
  b.center_x = x;
  b.center_z = z;
  b.rotation_index = rotation_index;
  return b;
}

}  // namespace

TEST_CASE("YawFromRotationIndex: 45-degree steps about +Y") {
  CHECK(YawFromRotationIndex(0) == Catch::Approx(0.0f).margin(kEps));
  CHECK(YawFromRotationIndex(1) ==
        Catch::Approx(glm::radians(45.0f)).margin(kEps));
  CHECK(YawFromRotationIndex(2) ==
        Catch::Approx(glm::radians(90.0f)).margin(kEps));
  CHECK(YawFromRotationIndex(3) ==
        Catch::Approx(glm::radians(135.0f)).margin(kEps));
}

TEST_CASE("MakeUnitRing: a circle on the ground under the unit") {
  const CharacterState unit = MakeUnit(4.0f, -9.0f, 0.8f, 0.6f);
  const ProjectedDecal decal = MakeUnitRing(unit, 2.5f);

  CHECK(decal.shape == DecalShape::Ring);
  // Centred on the unit, seated on the supplied ground height.
  CHECK(decal.center.x == Catch::Approx(4.0f).margin(kEps));
  CHECK(decal.center.y == Catch::Approx(2.5f).margin(kEps));
  CHECK(decal.center.z == Catch::Approx(-9.0f).margin(kEps));
  // Sized off the LARGER footprint extent (0.8), plus the outward margin, so
  // the ring encloses the whole unit.
  CHECK(decal.half_extents.x > 0.4f);
  // A circle carries no orientation.
  CHECK(decal.yaw == Catch::Approx(0.0f).margin(kEps));
}

TEST_CASE("MakeUnitRing: the ring always encloses the unit footprint") {
  // Whatever the fixed margin is, the ring must sit strictly outside the
  // capsule -- a highlight clipping through the unit reads as a bug.
  for (const float size : {0.4f, 1.0f, 2.5f}) {
    const ProjectedDecal d = MakeUnitRing(MakeUnit(0.0f, 0.0f, size, size), 0.0f);
    INFO("unit size = " << size);
    CHECK(d.half_extents.x > 0.5f * size);
  }
  // A non-square footprint is sized off the LARGER extent.
  const ProjectedDecal wide = MakeUnitRing(MakeUnit(0.0f, 0.0f, 2.0f, 0.5f), 0.0f);
  CHECK(wide.half_extents.x > 1.0f);
}

TEST_CASE("MakeBuildingRect: a rounded rect around the drawn footprint") {
  const BuildingState building =
      MakeBuilding(BuildingKind::Tavern, -12.0f, 30.0f, 0);
  const ProjectedDecal decal = MakeBuildingRect(building, 1.25f);

  CHECK(decal.shape == DecalShape::RoundedRect);
  CHECK(decal.center.x == Catch::Approx(-12.0f).margin(kEps));
  CHECK(decal.center.y == Catch::Approx(1.25f).margin(kEps));
  CHECK(decal.center.z == Catch::Approx(30.0f).margin(kEps));

  // Half-extents come from the UNROTATED render box + the margin -- the same
  // box the building mesh is built from.
  const RenderBox box = RenderBoxOf(BuildingKind::Tavern, 0);
  CHECK(decal.half_extents.x > 0.5f * box.size_x);
  CHECK(decal.half_extents.y > 0.5f * box.size_z);
  // Both axes take the SAME outward margin, so the outline stays concentric.
  CHECK(decal.half_extents.x - 0.5f * box.size_x ==
        Catch::Approx(decal.half_extents.y - 0.5f * box.size_z).margin(kEps));
  CHECK(decal.yaw == Catch::Approx(0.0f).margin(kEps));
}

TEST_CASE("MakeBuildingRect: rotation reaches the decal as yaw, not as new extents") {
  const ProjectedDecal unrotated = MakeBuildingRect(
      MakeBuilding(BuildingKind::FreeCompanyQuarters, 0.0f, 0.0f, 0), 0.0f);
  const ProjectedDecal rotated = MakeBuildingRect(
      MakeBuilding(BuildingKind::FreeCompanyQuarters, 0.0f, 0.0f, 2), 0.0f);

  // The decal is rotated by yaw; its local extents are the same box either way
  // (using the rotated render box AND the yaw would double-apply the rotation).
  CHECK(rotated.yaw == Catch::Approx(glm::radians(90.0f)).margin(kEps));
  CHECK(rotated.half_extents.x ==
        Catch::Approx(unrotated.half_extents.x).margin(kEps));
  CHECK(rotated.half_extents.y ==
        Catch::Approx(unrotated.half_extents.y).margin(kEps));
}

TEST_CASE("MakeBuildingRect: the corner radius never exceeds the half-size") {
  const ProjectedDecal decal =
      MakeBuildingRect(MakeBuilding(BuildingKind::House, 0.0f, 0.0f, 0), 0.0f);

  CHECK(decal.corner_radius <=
        std::min(decal.half_extents.x, decal.half_extents.y) + kEps);
  CHECK(decal.corner_radius > 0.0f);
}

TEST_CASE("Selection decals are marching-ants white/black dashed outlines") {
  // The look is fixed in selection_decals.cpp, so assert it on the produced
  // decals rather than on a config struct.
  const ProjectedDecal ring = MakeUnitRing(MakeUnit(0.0f, 0.0f, 1.0f, 1.0f), 0.0f);
  const ProjectedDecal rect =
      MakeBuildingRect(MakeBuilding(BuildingKind::Watchtower, 0.0f, 0.0f, 0), 0.0f);

  for (const ProjectedDecal& d : {ring, rect}) {
    CHECK(d.color_a == glm::vec4(1.0f, 1.0f, 1.0f, 1.0f));  // white
    CHECK(d.color_b == glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));  // black
    CHECK(d.dash_length > 0.0f);
    CHECK(d.dash_gap > 0.0f);  // actually dashed, not a solid line
    CHECK(d.line_width > 0.0f);
    CHECK(d.projector_half_height > 0.0f);
  }
}
