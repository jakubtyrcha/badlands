// The CPU coarse-cull decision for a foliage cell.
//
// Both behaviours pinned here were bugs found in review, and both are the kind
// that a screenshot shows only as an intermittent flicker at the frame edge:
//   1. Culling on the camera frustum alone deleted SHADOW CASTERS, because the
//      one uploaded instance buffer feeds the engine's separate light-frustum
//      cull too.
//   2. Testing the bare 32 m footprint culled trees whose crowns overhang the
//      cell they are filed under.

#include <catch_amalgamated.hpp>

#include <glm/gtc/matrix_transform.hpp>

#include "engine/core/camera.hpp"
#include "game/visual/foliage_cell_cull.hpp"

using namespace badlands;

namespace {

// A camera at the origin looking down -Z, so "in front" is negative Z and the
// frustum's extent is easy to reason about by hand.
Frustum LookingDownNegZ(float far_plane = 300.0f) {
  Camera camera;
  camera.position = glm::vec3(0.0f, 10.0f, 0.0f);
  camera.direction = glm::vec3(0.0f, 0.0f, -1.0f);
  camera.up = glm::vec3(0.0f, 1.0f, 0.0f);
  camera.fov = 60.0f;
  camera.aspect = 1.0f;
  camera.near_plane = 0.1f;
  camera.far_plane = far_plane;
  return Frustum::FromViewProj(camera.GetProj() * camera.GetView());
}

// Straight overhead sun.
constexpr glm::vec3 kNoon{0.0f, 1.0f, 0.0f};

}  // namespace

TEST_CASE("A visible cell is kept", "[foliage]") {
  const Frustum f = LookingDownNegZ();
  const Aabb in_view = FoliageCellBounds({-16.0f, -60.0f}, 32.0f, 0.0f, 20.0f, 0.0f);
  CHECK(FoliageCellVisibleOrCasts(f, in_view, kNoon, 0.0f));
}

TEST_CASE("A cell behind the camera with the sun overhead is dropped",
          "[foliage]") {
  // Overhead sun means the shadow falls straight down, reaching nothing that is
  // not already under the cell -- so a cell behind the camera has no reason to
  // be uploaded. This is the control for the shadow test below: without it,
  // "kept" would prove nothing.
  const Frustum f = LookingDownNegZ();
  const Aabb behind = FoliageCellBounds({-16.0f, 60.0f}, 32.0f, 0.0f, 20.0f, 0.0f);
  CHECK_FALSE(FoliageCellVisibleOrCasts(f, behind, kNoon, 0.0f));
}

TEST_CASE("An off-screen cell whose shadow falls into view is kept",
          "[foliage]") {
  // THE regression: with a low sun behind the camera, a stand just off the near
  // edge throws its shadow onto ground that is still on screen. Culling on the
  // camera alone dropped it, and the shadow vanished.
  const Frustum f = LookingDownNegZ();

  // A tall cell just behind the camera.
  const Aabb behind = FoliageCellBounds({-16.0f, 40.0f}, 32.0f, 0.0f, 25.0f, 0.0f);
  // Low sun behind the camera (+Z side, low in the sky), so shadows are thrown
  // forward into the view along -Z.
  const glm::vec3 low_sun = glm::normalize(glm::vec3(0.0f, 0.25f, 1.0f));

  CHECK_FALSE(f.Intersects(behind));  // genuinely not visible itself
  CHECK(FoliageCellVisibleOrCasts(f, behind, low_sun, 0.0f));
}

TEST_CASE("A sun at or below the horizon casts nothing", "[foliage]") {
  // Night: no sun, no shadow, no reason to keep an off-screen cell. Also guards
  // the divide by sun.y, which would otherwise run to infinity.
  const Frustum f = LookingDownNegZ();
  const Aabb behind = FoliageCellBounds({-16.0f, 40.0f}, 32.0f, 0.0f, 25.0f, 0.0f);

  CHECK_FALSE(FoliageCellVisibleOrCasts(
      f, behind, glm::normalize(glm::vec3(0.0f, 0.0f, 1.0f)), 0.0f));
  CHECK_FALSE(FoliageCellVisibleOrCasts(
      f, behind, glm::normalize(glm::vec3(0.0f, -0.5f, 1.0f)), 0.0f));
}

TEST_CASE("Shadow reach is capped near the horizon", "[foliage]") {
  // As the sun approaches the horizon the geometric reach runs to infinity. A
  // cell far enough away must still be dropped, or a near-sunset frame uploads
  // the whole map.
  const Frustum f = LookingDownNegZ(/*far_plane=*/300.0f);
  // Low, but still above the horizon cutoff -- a sun below it is handled by the
  // night case above, and would exercise that branch instead of the cap.
  const glm::vec3 grazing = glm::normalize(glm::vec3(0.0f, 0.05f, 1.0f));

  const Aabb very_far =
      FoliageCellBounds({-16.0f, 5000.0f}, 32.0f, 0.0f, 25.0f, 0.0f);
  CHECK_FALSE(FoliageCellVisibleOrCasts(f, very_far, grazing, 0.0f));

  // And the cap is what does it -- a cell inside the cap is still kept.
  const Aabb within_cap =
      FoliageCellBounds({-16.0f, 60.0f}, 32.0f, 0.0f, 25.0f, 0.0f);
  CHECK(FoliageCellVisibleOrCasts(f, within_cap, grazing, 0.0f));
}

TEST_CASE("Crown padding widens the cell box", "[foliage]") {
  const Aabb bare = FoliageCellBounds({0.0f, 0.0f}, 32.0f, 1.0f, 21.0f, 0.0f);
  CHECK(bare.min.x == 0.0f);
  CHECK(bare.max.x == 32.0f);
  CHECK(bare.min.z == 0.0f);
  CHECK(bare.max.z == 32.0f);
  // Y comes from the instances, never padded -- a crown's horizontal reach says
  // nothing about its vertical extent, which cell_y already measured.
  CHECK(bare.min.y == 1.0f);
  CHECK(bare.max.y == 21.0f);

  const Aabb padded = FoliageCellBounds({0.0f, 0.0f}, 32.0f, 1.0f, 21.0f, 6.0f);
  CHECK(padded.min.x == -6.0f);
  CHECK(padded.max.x == 38.0f);
  CHECK(padded.min.z == -6.0f);
  CHECK(padded.max.z == 38.0f);
  CHECK(padded.min.y == 1.0f);
  CHECK(padded.max.y == 21.0f);
}

TEST_CASE("A tree overhanging its cell edge is not culled with the cell",
          "[foliage]") {
  // THE second regression. The cell sits just outside the frustum, but a crown
  // planted near its edge still reaches into view; the bare footprint dropped
  // it and the tree popped in at the frame edge.
  const Frustum f = LookingDownNegZ();

  // Placed so the bare footprint misses the frustum's side plane but a padded
  // one does not. At 60 deg fov / aspect 1 the half-width is z * tan(30 deg),
  // so the cell's FAR edge is what has to clear it: spanning z [-52, -20], the
  // widest the frustum gets across the cell is 52 * tan(30 deg) = 30.0 m, and
  // the cell starts at x = 31.
  const glm::vec2 origin{31.0f, -52.0f};
  const Aabb bare = FoliageCellBounds(origin, 32.0f, 0.0f, 20.0f, 0.0f);
  const Aabb padded = FoliageCellBounds(origin, 32.0f, 0.0f, 20.0f, 8.0f);

  CHECK_FALSE(f.Intersects(bare));
  CHECK(f.Intersects(padded));
  // Sun overhead, so this is the padding doing the work and not the shadow
  // sweep standing in for it.
  CHECK(FoliageCellVisibleOrCasts(f, padded, kNoon, 0.0f));
}
