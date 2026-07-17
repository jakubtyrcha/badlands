// Volumetric-fog cascade addressing math (height-band clipmap). Pure CPU — no
// GPU/Dawn. These free functions live in engine/rendering/fog_cascade.hpp and
// are shared conceptually with shaders/common/fog_cascade.wesl (the shader
// mirrors the same world<->voxel + toroidal-wrap math so a filled voxel reads
// back what was written).
#include <climits>

#include <catch_amalgamated.hpp>
#include <glm/glm.hpp>

#include "engine/rendering/fog_cascade.hpp"

using namespace badlands;

TEST_CASE("cascade voxel sizes and extents double per level", "[fog]") {
  fog::CascadeLayout L;  // defaults: 3 cascades, 128x128x32, base half 64, height 64
  REQUIRE(fog::CascadeHalfExtent(L, 0) == Catch::Approx(64.0f));
  REQUIRE(fog::CascadeHalfExtent(L, 1) == Catch::Approx(128.0f));
  REQUIRE(fog::CascadeHalfExtent(L, 2) == Catch::Approx(256.0f));
  REQUIRE(fog::CascadeFullExtent(L, 0) == Catch::Approx(128.0f));
  REQUIRE(fog::CascadeVoxelSizeXZ(L, 0) == Catch::Approx(1.0f));
  REQUIRE(fog::CascadeVoxelSizeXZ(L, 1) == Catch::Approx(2.0f));
  REQUIRE(fog::CascadeVoxelSizeXZ(L, 2) == Catch::Approx(4.0f));
  REQUIRE(fog::CascadeVoxelSizeY(L) == Catch::Approx(2.0f));
}

TEST_CASE("PosMod wraps negatives into [0,n)", "[fog]") {
  REQUIRE(fog::PosMod(5, 128) == 5);
  REQUIRE(fog::PosMod(128, 128) == 0);
  REQUIRE(fog::PosMod(-1, 128) == 127);
  REQUIRE(fog::PosMod(-130, 128) == 126);
}

TEST_CASE("cascade min voxel centers the window on the camera", "[fog]") {
  fog::CascadeLayout L;
  // cascade 0: voxel size 1, res 128 -> min = floor(cam) - 64.
  REQUIRE(fog::CascadeMinVoxel(L, 0, 0.0f) == -64);
  REQUIRE(fog::CascadeMinVoxel(L, 0, 10.0f) == -54);
  REQUIRE(fog::CascadeMinVoxel(L, 0, -10.0f) == -74);
  // cascade 1: voxel size 2 -> min = floor(cam/2) - 64.
  REQUIRE(fog::CascadeMinVoxel(L, 1, 100.0f) == -14);
}

TEST_CASE("select finest cascade whose half-extent covers the distance", "[fog]") {
  fog::CascadeLayout L;  // half-extents 64, 128, 256
  REQUIRE(fog::SelectCascade(L, 50.0f) == 0);
  REQUIRE(fog::SelectCascade(L, 64.0f) == 0);
  REQUIRE(fog::SelectCascade(L, 65.0f) == 1);
  REQUIRE(fog::SelectCascade(L, 200.0f) == 2);
  REQUIRE(fog::SelectCascade(L, 300.0f) == -1);  // beyond coarsest
}

TEST_CASE("dirty boxes: no camera move produces nothing", "[fog]") {
  auto b = fog::ComputeDirtyBoxes(128, glm::ivec2(0, 0), glm::ivec2(0, 0), false);
  REQUIRE(b.empty());
}

TEST_CASE("dirty boxes: force refills the whole cascade", "[fog]") {
  auto b = fog::ComputeDirtyBoxes(128, glm::ivec2(0, 0), glm::ivec2(0, 0), true);
  REQUIRE(b.size() == 1);
  REQUIRE(b[0] == fog::DirtyBox{0, 0, 128, 128});
}

TEST_CASE("dirty boxes: a teleport (|delta| >= res) refills the whole cascade", "[fog]") {
  auto b = fog::ComputeDirtyBoxes(128, glm::ivec2(0, 0), glm::ivec2(200, 0), false);
  REQUIRE(b.size() == 1);
  REQUIRE(b[0] == fog::DirtyBox{0, 0, 128, 128});
}

TEST_CASE("dirty boxes: small +X move is one X slab, full Z", "[fog]") {
  auto b = fog::ComputeDirtyBoxes(128, glm::ivec2(0, 0), glm::ivec2(5, 0), false);
  REQUIRE(b.size() == 1);
  REQUIRE(b[0] == fog::DirtyBox{0, 0, 5, 128});
}

TEST_CASE("dirty boxes: small -X move is one X slab at the trailing edge", "[fog]") {
  auto b = fog::ComputeDirtyBoxes(128, glm::ivec2(10, 0), glm::ivec2(5, 0), false);
  REQUIRE(b.size() == 1);
  REQUIRE(b[0] == fog::DirtyBox{5, 0, 5, 128});
}

TEST_CASE("dirty boxes: +X move across the toroidal seam splits into two", "[fog]") {
  auto b = fog::ComputeDirtyBoxes(128, glm::ivec2(125, 0), glm::ivec2(130, 0), false);
  REQUIRE(b.size() == 2);
  REQUIRE(b[0] == fog::DirtyBox{125, 0, 3, 128});
  REQUIRE(b[1] == fog::DirtyBox{0, 0, 2, 128});
}

TEST_CASE("dirty boxes: diagonal move yields an X slab and a Z slab", "[fog]") {
  auto b = fog::ComputeDirtyBoxes(128, glm::ivec2(0, 0), glm::ivec2(3, 4), false);
  REQUIRE(b.size() == 2);
  REQUIRE(b[0] == fog::DirtyBox{0, 0, 3, 128});  // X slab (entered columns, full Z)
  REQUIRE(b[1] == fog::DirtyBox{0, 0, 128, 4});  // Z slab (entered rows, full X)
}

TEST_CASE("dirty boxes: INT_MIN first-fill sentinel refills fully (no overflow)", "[fog]") {
  // VolumetricFog seeds last_min_voxel with INT_MIN on the first frame / after a
  // resize; the delta vs a large +world camera must not overflow int (UB).
  auto b = fog::ComputeDirtyBoxes(128, glm::ivec2(INT_MIN, INT_MIN),
                                  glm::ivec2(1000000, -500000), true);
  REQUIRE(b.size() == 1);
  REQUIRE(b[0] == fog::DirtyBox{0, 0, 128, 128});
}

TEST_CASE("dirty boxes: huge non-sentinel delta refills fully without overflow", "[fog]") {
  auto b = fog::ComputeDirtyBoxes(128, glm::ivec2(-2000000000, 0),
                                  glm::ivec2(2000000000, 0), false);
  REQUIRE(b.size() == 1);
  REQUIRE(b[0] == fog::DirtyBox{0, 0, 128, 128});
}
