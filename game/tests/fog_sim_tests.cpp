// Fog-generator CPU core (Task: map fog generator). Pure CPU — no GPU/Dawn.
// Exercises the emitter footprint AABBs and the uniform-grid broadphase
// (cell -> emitter-index buckets) that the composer (compute/fog_fill.wesl)
// consumes. The math lives header-only in engine/rendering/fog_sim.hpp (glm
// only), mirroring the fog_cascade.hpp/fog_cascade_tests.cpp split.
#include <array>
#include <cmath>
#include <vector>

#include <catch_amalgamated.hpp>
#include <glm/glm.hpp>

#include "engine/rendering/fog_sim.hpp"

using namespace badlands;

// --- Emitter footprint AABB (conservative, for broadphase) -------------------

TEST_CASE("disc emitter AABB is the bounding square", "[fogsim]") {
  fog::Emitter e{};
  e.center = {10.0f, 20.0f};
  e.half_extent = {3.0f, 3.0f};
  e.shape = fog::EmitterShape::Disc;
  REQUIRE(fog::EmitterAabbMin(e).x == Catch::Approx(7.0f));
  REQUIRE(fog::EmitterAabbMin(e).y == Catch::Approx(17.0f));
  REQUIRE(fog::EmitterAabbMax(e).x == Catch::Approx(13.0f));
  REQUIRE(fog::EmitterAabbMax(e).y == Catch::Approx(23.0f));
}

TEST_CASE("disc AABB uses the radius (half_extent.x) on both axes", "[fogsim]") {
  // The GPU evaluator treats a disc's radius as half_extent.x only, so the
  // broadphase AABB must too — a stray/default .y must not shrink the Z extent.
  fog::Emitter e{};
  e.center = {0.0f, 0.0f};
  e.half_extent = {5.0f, 1.0f};  // .y left at a non-radius value
  e.shape = fog::EmitterShape::Disc;
  REQUIRE(fog::EmitterAabbMin(e) == glm::vec2(-5.0f, -5.0f));
  REQUIRE(fog::EmitterAabbMax(e) == glm::vec2(5.0f, 5.0f));
}

TEST_CASE("axis-aligned OBB AABB matches its half extents", "[fogsim]") {
  fog::Emitter e{};
  e.center = {0.0f, 0.0f};
  e.half_extent = {4.0f, 1.0f};
  e.rotation = 0.0f;
  e.shape = fog::EmitterShape::Obb;
  REQUIRE(fog::EmitterAabbMin(e).x == Catch::Approx(-4.0f));
  REQUIRE(fog::EmitterAabbMin(e).y == Catch::Approx(-1.0f));
  REQUIRE(fog::EmitterAabbMax(e).x == Catch::Approx(4.0f));
  REQUIRE(fog::EmitterAabbMax(e).y == Catch::Approx(1.0f));
}

TEST_CASE("rotated OBB AABB grows to the rotated extent", "[fogsim]") {
  fog::Emitter e{};
  e.center = {0.0f, 0.0f};
  e.half_extent = {4.0f, 1.0f};
  e.shape = fog::EmitterShape::Obb;

  // 90 deg swaps the extents.
  e.rotation = static_cast<float>(M_PI) * 0.5f;
  REQUIRE(fog::EmitterAabbMax(e).x == Catch::Approx(1.0f).margin(1e-4f));
  REQUIRE(fog::EmitterAabbMax(e).y == Catch::Approx(4.0f).margin(1e-4f));

  // 45 deg: half extent along each axis = |hx*cos| + |hz*sin|.
  e.rotation = static_cast<float>(M_PI) * 0.25f;
  const float expect = (4.0f + 1.0f) * std::cos(static_cast<float>(M_PI) * 0.25f);
  REQUIRE(fog::EmitterAabbMax(e).x == Catch::Approx(expect).margin(1e-4f));
  REQUIRE(fog::EmitterAabbMax(e).y == Catch::Approx(expect).margin(1e-4f));
}

TEST_CASE("ellipse AABB matches the OBB (inscribed, so bound is the box)",
          "[fogsim]") {
  fog::Emitter e{};
  e.center = {0.0f, 0.0f};
  e.half_extent = {4.0f, 1.0f};
  e.shape = fog::EmitterShape::Ellipse;

  // Axis-aligned: same box bound as an OBB (the ellipse is inscribed in it).
  REQUIRE(fog::EmitterAabbMax(e).x == Catch::Approx(4.0f));
  REQUIRE(fog::EmitterAabbMax(e).y == Catch::Approx(1.0f));

  // Rotated 90 deg swaps the extents, exactly like the OBB.
  e.rotation = static_cast<float>(M_PI) * 0.5f;
  REQUIRE(fog::EmitterAabbMax(e).x == Catch::Approx(1.0f).margin(1e-4f));
  REQUIRE(fog::EmitterAabbMax(e).y == Catch::Approx(4.0f).margin(1e-4f));
}

// --- Broadphase: cell -> emitter-index buckets -------------------------------

TEST_CASE("broadphase buckets a single emitter into one cell", "[fogsim]") {
  std::array<fog::Emitter, 1> ems{};
  ems[0].center = {-6.0f, -6.0f};
  ems[0].half_extent = {1.0f, 1.0f};
  ems[0].shape = fog::EmitterShape::Disc;

  fog::Broadphase bp = fog::BuildBroadphase(ems, {-8.0f, -8.0f}, {8.0f, 8.0f}, 4.0f);
  REQUIRE(bp.nx == 4);
  REQUIRE(bp.nz == 4);
  REQUIRE(bp.cells.size() == 16u);
  REQUIRE(bp.indices.size() == 1u);  // covers exactly one cell

  REQUIRE(bp.CellEmitters({-6.0f, -6.0f}) == std::vector<uint32_t>{0u});
  REQUIRE(bp.CellEmitters({6.0f, 6.0f}).empty());  // far cell is empty
}

TEST_CASE("broadphase assigns a straddling emitter to every overlapped cell",
          "[fogsim]") {
  // Disc at (-4,-4) r=1.5 -> AABB [-5.5,-5.5]..[-2.5,-2.5] straddles the
  // boundary at world -4 on both axes -> cells (0,0),(1,0),(0,1),(1,1).
  std::array<fog::Emitter, 1> ems{};
  ems[0].center = {-4.0f, -4.0f};
  ems[0].half_extent = {1.5f, 1.5f};
  ems[0].shape = fog::EmitterShape::Disc;

  fog::Broadphase bp = fog::BuildBroadphase(ems, {-8.0f, -8.0f}, {8.0f, 8.0f}, 4.0f);
  REQUIRE(bp.indices.size() == 4u);
  REQUIRE(bp.CellEmitters({-6.0f, -6.0f}) == std::vector<uint32_t>{0u});  // (0,0)
  REQUIRE(bp.CellEmitters({-2.0f, -6.0f}) == std::vector<uint32_t>{0u});  // (1,0)
  REQUIRE(bp.CellEmitters({-6.0f, -2.0f}) == std::vector<uint32_t>{0u});  // (0,1)
  REQUIRE(bp.CellEmitters({-2.0f, -2.0f}) == std::vector<uint32_t>{0u});  // (1,1)
}

TEST_CASE("broadphase groups multiple emitters sharing a cell", "[fogsim]") {
  std::array<fog::Emitter, 2> ems{};
  ems[0].center = {-6.0f, -6.0f};
  ems[0].half_extent = {0.5f, 0.5f};
  ems[0].shape = fog::EmitterShape::Disc;
  ems[1].center = {-5.0f, -7.0f};
  ems[1].half_extent = {0.5f, 0.5f};
  ems[1].shape = fog::EmitterShape::Disc;

  fog::Broadphase bp = fog::BuildBroadphase(ems, {-8.0f, -8.0f}, {8.0f, 8.0f}, 4.0f);
  std::vector<uint32_t> hit = bp.CellEmitters({-6.0f, -6.0f});
  std::sort(hit.begin(), hit.end());
  REQUIRE(hit == std::vector<uint32_t>{0u, 1u});
}
