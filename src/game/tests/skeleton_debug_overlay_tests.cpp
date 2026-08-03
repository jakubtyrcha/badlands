// The skeleton overlay end to end, minus the GPU: a stepped Sim produces
// CharacterAnim, the overlay maps it to a clip, poses the skeleton, and appends
// bone lines at the character's world transform.
//
// This is the only automated check that M3's WIRING works. The pure functions
// are covered next door in character_animation_tests; what this adds is that the
// pieces are actually connected -- which a screenshot cannot verify, because
// SaveScreenshot calls Update(0.0f) so the sim never ticks and no character has
// been projected yet.
//
// The overlay needs no device, so all of this runs headless.

#include "game/visual/skeleton_debug_overlay.hpp"

#include "engine/tests/animation_fixture.hpp"

#include <catch_amalgamated.hpp>

#include <fstream>
#include <string>
#include <vector>

using namespace badlands;
using namespace badlands::test;

namespace {

// Writes a manifest naming every logical clip the overlay resolves, all backed
// by the same generated fixture clip. Built in-test so the suite never depends
// on assets/characters/ (nor on git-lfs having been pulled).
std::string WriteFixtureManifest() {
  const std::string skeleton = WriteOzz(*BuildRawSkeleton(), "overlay_skeleton.ozz");
  const std::string clip = WriteOzz(*BuildRawAnimation(), "overlay_clip.ozz");
  const std::string skeleton_file = std::filesystem::path(skeleton).filename().string();
  const std::string clip_file = std::filesystem::path(clip).filename().string();

  const std::filesystem::path path = TempDir() / "overlay_manifest.json";
  std::ofstream out(path);
  REQUIRE(out.good());
  out << "{\n  \"skeleton\": \"" << skeleton_file << "\",\n  \"clips\": {\n";
  const char* names[] = {"idle", "walk", "jog", "sprint", "cast_idle", "hit_body"};
  for (const char* name : names) {
    out << "    \"" << name << "\": \"" << clip_file << "\",\n";
  }
  // The attack clip carries a pivot, the one piece of metadata the overlay reads.
  out << "    \"attack\": { \"file\": \"" << clip_file << "\", \"pivot\": 0.45 }\n";
  out << "  }\n}\n";
  out.close();
  return path.string();
}

// Flat world, no colony, one character, STEPPED -- CharacterAnim is written by
// the projector at the end of a tick, so an unstepped world has none and the
// overlay would correctly draw nothing.
//
// Driven entirely through the public Sim API, because that is what the overlay
// itself consumes.
WorldConfig FlatConfig() {
  WorldConfig cfg;
  cfg.map = MapKind::FlatPlains;
  cfg.terrain_blocking = false;
  cfg.prebuild_colony = false;
  return cfg;
}

uint32_t SpawnAndStep(Sim& sim, float x, float z) {
  CharacterDesc d = MercenaryDesc(x, z);
  d.move_speed = 6.0f;
  const uint32_t slot = sim.Spawn(d);
  sim.Step();
  return slot;
}

}  // namespace

TEST_CASE("the overlay draws nothing until it is switched on", "[overlay]") {
  Sim sim{FlatConfig(), BrainDesc{}};
  SpawnAndStep(sim, 3.0f, -4.0f);

  SkeletonDebugOverlay overlay;
  REQUIRE(overlay.Initialize(WriteFixtureManifest()));
  CHECK_FALSE(overlay.enabled());

  const std::vector<CharacterState> rows = sim.Characters();
  DebugLineBuffer lines;
  overlay.Rebuild(sim, rows, lines, [](float, float) { return 0.0f; }, 0.016f);

  // Off by default: capsules remain the blockout mesh and this costs nothing.
  CHECK(lines.empty());
  CHECK(overlay.drawn() == 0);
}

TEST_CASE("an enabled overlay emits a live character's bones", "[overlay]") {
  Sim sim{FlatConfig(), BrainDesc{}};
  SpawnAndStep(sim, 3.0f, -4.0f);

  SkeletonDebugOverlay overlay;
  REQUIRE(overlay.Initialize(WriteFixtureManifest()));
  overlay.mutable_enabled() = true;

  const std::vector<CharacterState> rows = sim.Characters();
  REQUIRE(rows.size() == 1);

  DebugLineBuffer lines;
  overlay.Rebuild(sim, rows, lines, [](float, float) { return 0.0f; }, 0.016f);

  // One segment per non-root joint -- the whole chain resolved.
  CHECK(overlay.drawn() == 1);
  CHECK(lines.lines.size() == static_cast<size_t>(kFixtureJoints - kFixtureRoots));
}

TEST_CASE("bones land at the character's world position", "[overlay]") {
  constexpr float kX = 3.0f;
  constexpr float kZ = -4.0f;
  constexpr float kGround = 12.5f;

  Sim sim{FlatConfig(), BrainDesc{}};
  SpawnAndStep(sim, kX, kZ);

  SkeletonDebugOverlay overlay;
  REQUIRE(overlay.Initialize(WriteFixtureManifest()));
  overlay.mutable_enabled() = true;

  const std::vector<CharacterState> rows = sim.Characters();
  DebugLineBuffer lines;
  // anim_dt = 0 holds the loop at phase zero. The fixture clip deliberately
  // slides its ROOT along +X (that is what makes the runtime's sampling
  // assertions unambiguous next door), so advancing time here would move the
  // bones by the clip's own animation and this test would be measuring the
  // fixture rather than the placement. A non-zero ground height keeps the
  // y-check meaningful.
  overlay.Rebuild(sim, rows, lines, [](float, float) { return kGround; }, 0.0f);

  REQUIRE_FALSE(lines.empty());
  for (const DebugLine& line : lines.lines) {
    // The fixture rig is a vertical bar at its own origin, so at phase zero
    // every bone starts at the character's world XZ, riding the supplied ground.
    CHECK(line.start.x == Catch::Approx(kX).margin(1e-3));
    CHECK(line.start.z == Catch::Approx(kZ).margin(1e-3));
    CHECK(line.start.y == Catch::Approx(kGround).margin(1e-3));
  }
}

TEST_CASE("the overlay APPENDS, so other overlays survive it", "[overlay]") {
  Sim sim{FlatConfig(), BrainDesc{}};
  SpawnAndStep(sim, 3.0f, -4.0f);

  SkeletonDebugOverlay overlay;
  REQUIRE(overlay.Initialize(WriteFixtureManifest()));
  overlay.mutable_enabled() = true;

  // Stand in for whatever another overlay (the navmesh) already put there.
  DebugLineBuffer lines;
  lines.AddLine(glm::vec3(0.0f), glm::vec3(1.0f), glm::vec3(1.0f));
  const size_t pre_existing = lines.lines.size();

  const std::vector<CharacterState> rows = sim.Characters();
  overlay.Rebuild(sim, rows, lines, [](float, float) { return 0.0f; }, 0.016f);

  // SceneContext::debug_lines is a single pointer, so an overlay that cleared
  // the shared buffer would silently erase the navmesh. Regression-guard it.
  CHECK(lines.lines.size() > pre_existing);
  CHECK(lines.lines[0].start == glm::vec3(0.0f));
}

TEST_CASE("a hidden character is not drawn", "[overlay]") {
  Sim sim{FlatConfig(), BrainDesc{}};
  SpawnAndStep(sim, 3.0f, -4.0f);

  SkeletonDebugOverlay overlay;
  REQUIRE(overlay.Initialize(WriteFixtureManifest()));
  overlay.mutable_enabled() = true;

  std::vector<CharacterState> rows = sim.Characters();
  REQUIRE(rows.size() == 1);
  rows[0].inside_building_id = 0;  // indoors: "don't draw; list in the panel"

  DebugLineBuffer lines;
  overlay.Rebuild(sim, rows, lines, [](float, float) { return 0.0f; }, 0.016f);
  CHECK(lines.empty());
  CHECK(overlay.drawn() == 0);
}

TEST_CASE("a row naming a dead entity is skipped, not dereferenced", "[overlay]") {
  Sim sim{FlatConfig(), BrainDesc{}};
  SpawnAndStep(sim, 3.0f, -4.0f);

  SkeletonDebugOverlay overlay;
  REQUIRE(overlay.Initialize(WriteFixtureManifest()));
  overlay.mutable_enabled() = true;

  // A snapshot row is a COPY and can outlive its entity by a frame, which is
  // exactly why HandleForSlot returns an invalid handle instead of asserting.
  std::vector<CharacterState> rows = sim.Characters();
  REQUIRE(rows.size() == 1);
  rows[0].id = 9999;  // no such slot

  DebugLineBuffer lines;
  overlay.Rebuild(sim, rows, lines, [](float, float) { return 0.0f; }, 0.016f);
  CHECK(lines.empty());
}

TEST_CASE("the overlay stays disabled when its assets are missing", "[overlay]") {
  SkeletonDebugOverlay overlay;
  // A character-asset problem must never stop an app starting; the overlay just
  // reports that it cannot draw.
  CHECK_FALSE(overlay.Initialize("/nonexistent/characters/clips.json"));
  CHECK_FALSE(overlay.enabled());

  overlay.mutable_enabled() = true;
  CHECK_FALSE(overlay.enabled());  // the toggle cannot enable a broken overlay
}
