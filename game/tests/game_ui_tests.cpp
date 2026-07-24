// Game-UI tests: world picking + HUD hit-testing.
//
// These cover the two places where a silent mistake would be invisible in a
// screenshot: the oriented-box rotation convention (which only misbehaves for
// diagonal buildings) and the click-priority contract between the HUD and the
// world (which only misbehaves on the click AFTER the one you were looking at).

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include <catch_amalgamated.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "badlands_sim.hpp"
#include "game/ui/hud.hpp"
#include "game/ui/picking.hpp"

using namespace badlands;

namespace {

BuildingState MakeBuilding(uint32_t id, BuildingKind kind, float x,
                           float z, int32_t rotation_index) {
  BuildingState b{};
  b.id = id;
  b.kind = kind;
  b.center_x = x;
  b.center_z = z;
  b.rotation_index = rotation_index;
  const BuildingDef def = BuildingDefOf(b.kind);
  b.width_tiles = def.width_tiles;
  b.depth_tiles = def.depth_tiles;
  return b;
}

CharacterState MakeHero(uint32_t id, float x, float z,
                        int32_t inside_building_id) {
  CharacterState c{};
  c.id = id;
  c.pos_x = x;
  c.pos_z = z;
  c.size_x = 1.0f;
  c.size_y = 2.0f;
  c.size_z = 1.0f;
  c.home_building_id = -1;
  c.inside_building_id = inside_building_id;
  return c;
}

// Bakes the shipping font into a UiContext (the real atlas path, same as the
// running app). Returns nullptr if the font is missing/unreadable; callers
// REQUIRE non-null so a missing asset fails loudly rather than skipping silently.
UiContext* LoadHudFont() {
  FILE* f = fopen("assets/fonts/IM_Fell_DW_Pica/IMFellDWPica-Regular.ttf", "rb");
  if (!f) return nullptr;
  fseek(f, 0, SEEK_END);
  const long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  std::vector<uint8_t> bytes(static_cast<size_t>(size));
  const size_t got = fread(bytes.data(), 1, bytes.size(), f);
  fclose(f);
  if (got != bytes.size()) return nullptr;
  return ui_create(bytes.data(), static_cast<uint32_t>(bytes.size()), 18.0f);
}

const UiHitRect* FindHitRect(const HudFrame& frame, uint32_t id) {
  for (const UiHitRect& r : frame.hits) {
    if (r.id == id) return &r;
  }
  return nullptr;
}

}  // namespace

// ---------------------------------------------------------------------------
// Oriented-box picking
// ---------------------------------------------------------------------------

TEST_CASE("glm rotation about +Y has the handedness picking assumes",
          "[game_ui][picking]") {
  // PointInOrientedBox inverts this rotation. If GLM's handedness ever
  // flipped, every diagonal pick would silently mis-rotate while axis-aligned
  // ones kept working -- so pin it. (1,0,0) rotated +90deg about Y -> (0,0,-1).
  const glm::vec4 r = glm::rotate(glm::mat4(1.0f), glm::radians(90.0f),
                                  glm::vec3(0.0f, 1.0f, 0.0f)) *
                      glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
  REQUIRE(std::fabs(r.x) < 1e-5f);
  REQUIRE(std::fabs(r.z + 1.0f) < 1e-5f);
}

TEST_CASE("PointInOrientedBox is exact on an unrotated box",
          "[game_ui][picking]") {
  const glm::vec2 center(10.0f, -4.0f);
  const glm::vec2 half(2.0f, 1.0f);

  REQUIRE(PointInOrientedBox(center, half, 0.0f, center));
  REQUIRE(PointInOrientedBox(center, half, 0.0f, {11.9f, -4.9f}));
  REQUIRE(PointInOrientedBox(center, half, 0.0f, {12.0f, -3.0f}));  // corner
  REQUIRE_FALSE(PointInOrientedBox(center, half, 0.0f, {12.1f, -4.0f}));
  REQUIRE_FALSE(PointInOrientedBox(center, half, 0.0f, {10.0f, -2.9f}));
}

TEST_CASE("PointInOrientedBox rotates the test frame, not just the extents",
          "[game_ui][picking]") {
  // A 4x1 box yawed 90deg spans 1 along X and 4 along Z. A point 1.8 away
  // along Z is INSIDE; the same distance along X is outside. Swap the sine
  // sign in the inverse rotation and these two swap, which is exactly the bug
  // this pins.
  const glm::vec2 center(0.0f, 0.0f);
  const glm::vec2 half(2.0f, 0.5f);
  const float yaw = glm::radians(90.0f);

  REQUIRE(PointInOrientedBox(center, half, yaw, {0.0f, 1.8f}));
  REQUIRE(PointInOrientedBox(center, half, yaw, {0.0f, -1.8f}));
  REQUIRE_FALSE(PointInOrientedBox(center, half, yaw, {1.8f, 0.0f}));
  REQUIRE_FALSE(PointInOrientedBox(center, half, yaw, {-1.8f, 0.0f}));
}

TEST_CASE("PointInOrientedBox agrees with a 45-degree diamond",
          "[game_ui][picking]") {
  // At 45deg a square's corners point along the axes: a point just past the
  // half-extent along X is outside, but the same distance along the box's own
  // diagonal is inside.
  const glm::vec2 center(0.0f, 0.0f);
  const glm::vec2 half(1.0f, 1.0f);
  const float yaw = glm::radians(45.0f);

  REQUIRE(PointInOrientedBox(center, half, yaw, {1.3f, 0.0f}));   // along a corner
  REQUIRE_FALSE(PointInOrientedBox(center, half, yaw, {1.1f, 1.1f}));  // past an edge
}

TEST_CASE("BuildingAtWorld round-trips a building's own centre",
          "[game_ui][picking]") {
  const BuildingState buildings[] = {
      MakeBuilding(7, BuildingKind::FreeCompanyQuarters, 12.0f, 0.0f, 0),
      MakeBuilding(9, BuildingKind::Tavern, 12.0f, 10.0f, 2),
      MakeBuilding(11, BuildingKind::Watchtower, -12.0f, 0.0f, 0),
  };
  REQUIRE(BuildingAtWorld(buildings, 3, {12.0f, 0.0f}) == 7u);
  REQUIRE(BuildingAtWorld(buildings, 3, {12.0f, 10.0f}) == 9u);
  REQUIRE(BuildingAtWorld(buildings, 3, {-12.0f, 0.0f}) == 11u);
}

TEST_CASE("BuildingAtWorld misses empty ground", "[game_ui][picking]") {
  const BuildingState buildings[] = {
      MakeBuilding(7, BuildingKind::FreeCompanyQuarters, 12.0f, 0.0f, 0)};
  REQUIRE(BuildingAtWorld(buildings, 1, {60.0f, 60.0f}) == kNoPick);
  REQUIRE(BuildingAtWorld(nullptr, 0, {0.0f, 0.0f}) == kNoPick);
}

TEST_CASE("BuildingAtWorld uses the rendered footprint, not the tile counts",
          "[game_ui][picking]") {
  // RenderBoxOf is authoritative: for a diagonal placement the spans are
  // NOT (width_tiles, depth_tiles). Pick just outside the tile half-extent but
  // inside the rendered box, and confirm the rendered box wins.
  const BuildingState b =
      MakeBuilding(3, BuildingKind::FreeCompanyQuarters, 0.0f, 0.0f, 1);
  const RenderBox box = RenderBoxOf(b.kind, b.rotation_index);

  // A point just inside the rendered box along its local X axis.
  const float s = std::sin(box.yaw_radians);
  const float c = std::cos(box.yaw_radians);
  const float t = box.size_x * 0.5f * 0.9f;
  // Forward-rotate a local-axis offset into world space (inverse of the test).
  const glm::vec2 p(c * t, -s * t);
  REQUIRE(BuildingAtWorld(&b, 1, p) == 3u);
}

TEST_CASE("HeroAtWorld ignores heroes inside a building", "[game_ui][picking]") {
  // Heroes indoors are not drawn (CharacterState::inside_building_id >= 0), so
  // they must not be clickable -- otherwise the player picks something invisible.
  const CharacterState heroes[] = {
      MakeHero(1, 5.0f, 5.0f, /*inside=*/-1),
      MakeHero(2, 8.0f, 8.0f, /*inside=*/4),
  };
  REQUIRE(HeroAtWorld(heroes, 2, {5.0f, 5.0f}) == 1u);
  REQUIRE(HeroAtWorld(heroes, 2, {8.0f, 8.0f}) == kNoPick);
}

TEST_CASE("GroundPickXZ hits a unit seated on elevated terrain", "[game_ui][picking]") {
  // A unit at (0,20) rendered on a 3 m plateau -> world (0,3,20). Camera looks
  // down-forward through that rendered position.
  const glm::vec3 cam(0.0f, 40.0f, -10.0f);
  const glm::vec3 look(0.0f, 3.0f, 20.0f);
  const glm::vec3 dir = glm::normalize(look - cam);
  const CharacterState units[] = {MakeHero(1, 0.0f, 20.0f, /*inside=*/-1)};

  // RED: intersecting the y=0 plane lands past the unit's footprint (the bug the
  // old picker had -- the click misses where the unit is drawn).
  const float t0 = -cam.y / dir.y;
  const glm::vec2 flat(cam.x + t0 * dir.x, cam.z + t0 * dir.z);
  CHECK(HeroAtWorld(units, 1, flat) == kNoPick);

  // GREEN: the terrain-aware pick follows the plateau height onto the unit.
  glm::vec2 xz;
  REQUIRE(GroundPickXZ(cam, dir, [](glm::vec2) { return 3.0f; }, xz));
  CHECK(HeroAtWorld(units, 1, xz) == 1u);
}

TEST_CASE("GroundPickXZ converges onto a unit standing on a slope",
          "[game_ui][picking]") {
  // Terrain ramps up with world z: h(p) = 0.1 * z. A unit at (0,20) is drawn at
  // height 2; the camera looks through that surface point.
  const auto ramp = [](glm::vec2 p) { return 0.1f * p.y; };  // p.y == world z
  const glm::vec3 cam(0.0f, 40.0f, -10.0f);
  const glm::vec3 look(0.0f, ramp({0.0f, 20.0f}), 20.0f);
  const glm::vec3 dir = glm::normalize(look - cam);
  const CharacterState units[] = {MakeHero(2, 0.0f, 20.0f, /*inside=*/-1)};

  glm::vec2 xz;
  REQUIRE(GroundPickXZ(cam, dir, ramp, xz, /*iterations=*/6));
  CHECK(HeroAtWorld(units, 1, xz) == 2u);  // iteration converged onto the surface
}

TEST_CASE("GroundPickXZ reports no hit when the ray does not descend",
          "[game_ui][picking]") {
  glm::vec2 xz{-42.0f, -42.0f};
  // Looking up: never crosses the ground.
  CHECK_FALSE(GroundPickXZ({0.0f, 5.0f, 0.0f}, glm::normalize(glm::vec3(0, 1, 1)),
                           [](glm::vec2) { return 0.0f; }, xz));
  CHECK(xz == glm::vec2(-42.0f, -42.0f));  // out param untouched on a miss
}

TEST_CASE("SelectedUnit drops a unit that walked into a building",
          "[game_ui][picking]") {
  CharacterState rows[] = {MakeHero(7, 5.0f, 5.0f, /*inside=*/-1)};
  // Selected and still outside -> kept.
  REQUIRE(SelectedUnit(rows, 1, 7) == &rows[0]);

  // It walks into a building: no longer drawn, so the selection must clear.
  rows[0].inside_building_id = 3;
  // RED baseline -- the naive by-id scan RefreshHud used still finds it:
  const CharacterState* naive = nullptr;
  for (const auto& c : rows) {
    if (c.id == 7) naive = &c;
  }
  CHECK(naive == &rows[0]);
  // GREEN -- SelectedUnit honours the same indoors rule as the pick:
  CHECK(SelectedUnit(rows, 1, 7) == nullptr);

  // Unknown id, empty set, and the no-selection sentinel all resolve to null.
  CHECK(SelectedUnit(rows, 1, 999) == nullptr);
  CHECK(SelectedUnit(nullptr, 0, 7) == nullptr);
  CHECK(SelectedUnit(rows, 1, kNoPick) == nullptr);
}

TEST_CASE("BuildingDef::recruits marks exactly the guild kinds and classes",
          "[game_ui][picking]") {
  // The HUD gates its Recruit button on recruit_count; it must match the sim's
  // own guild set (the 4 recruiting kinds) so the button is never shown enabled
  // for a building Dispatch(RecruitHero) would reject.
  auto recruits = [](BuildingKind k) {
    return BuildingDefOf(k).recruit_count > 0;
  };
  REQUIRE(recruits(BuildingKind::FreeCompanyQuarters));
  REQUIRE(recruits(BuildingKind::HuntersCamp));
  REQUIRE(recruits(BuildingKind::ThievesDen));
  REQUIRE(recruits(BuildingKind::Scriptorium));
  // Everything else -- Castle, the utility buildings, poppables -- is not.
  REQUIRE_FALSE(recruits(BuildingKind::Castle));
  REQUIRE_FALSE(recruits(BuildingKind::Tavern));
  REQUIRE_FALSE(recruits(BuildingKind::Apothecary));
  REQUIRE_FALSE(recruits(BuildingKind::Watchtower));
  REQUIRE_FALSE(recruits(BuildingKind::House));
  REQUIRE_FALSE(recruits(BuildingKind::Sewer));

  // Each guild recruits exactly its one class today (the table's source of
  // truth); guild_hero_class() derives from recruits[0], so keep them in step.
  REQUIRE(BuildingDefOf(BuildingKind::FreeCompanyQuarters).recruits[0] == HERO_MERCENARY);
  REQUIRE(BuildingDefOf(BuildingKind::HuntersCamp).recruits[0] == HERO_HUNTER);
  REQUIRE(BuildingDefOf(BuildingKind::ThievesDen).recruits[0] == HERO_GRAVE_ROBBER);
  REQUIRE(BuildingDefOf(BuildingKind::Scriptorium).recruits[0] == HERO_APPRENTICE);
}

// ---------------------------------------------------------------------------
// HUD hit-testing
// ---------------------------------------------------------------------------

namespace {

// A HudFrame with hand-placed hit rects, innermost-first (the order ui_build
// guarantees), so these tests exercise HudHitTest without needing a font.
HudFrame FrameWithRects() {
  HudFrame f;
  f.hits.push_back({kHudBtnRecruit, 1370.0f, 200.0f, 216.0f, 30.0f, 0});
  f.hits.push_back({kHudBtnDestroy, 1370.0f, 236.0f, 216.0f, 30.0f,
                    UI_FLAG_DISABLED});
  f.hits.push_back({kHudPanelBackground, 1360.0f, 34.0f, 240.0f, 866.0f, 0});
  f.hits.push_back({kHudTopBarBackground, 0.0f, 0.0f, 1600.0f, 34.0f, 0});
  return f;
}

}  // namespace

TEST_CASE("HudHitTest returns the innermost element", "[game_ui][hud]") {
  const HudFrame f = FrameWithRects();
  // A point inside the Recruit button is ALSO inside the panel background.
  // The button must win, or the action never fires.
  REQUIRE(HudHitTest(f, 1400.0f, 210.0f) == kHudBtnRecruit);
  REQUIRE(HudHitTest(f, 1400.0f, 246.0f) == kHudBtnDestroy);
}

TEST_CASE("HudHitTest consumes clicks on panel chrome", "[game_ui][hud]") {
  // The whole point of the panel background having an id: a click on empty
  // panel space must NOT fall through and deselect the entity being described.
  const HudFrame f = FrameWithRects();
  REQUIRE(HudHitTest(f, 1400.0f, 600.0f) == kHudPanelBackground);
  REQUIRE(HudHitTest(f, 800.0f, 10.0f) == kHudTopBarBackground);
}

TEST_CASE("HudHitTest misses the viewport", "[game_ui][hud]") {
  const HudFrame f = FrameWithRects();
  REQUIRE(HudHitTest(f, 600.0f, 500.0f) == kHudNone);
}

TEST_CASE("A disabled button is reported but still consumes the click",
          "[game_ui][hud]") {
  const HudFrame f = FrameWithRects();
  // Destroy is disabled here: it is still hit (so the click is swallowed)
  // but the caller must skip the dispatch.
  REQUIRE(HudHitTest(f, 1400.0f, 246.0f) == kHudBtnDestroy);
  REQUIRE(HudHitIsDisabled(f, 1400.0f, 246.0f));
  REQUIRE_FALSE(HudHitIsDisabled(f, 1400.0f, 210.0f));
  REQUIRE_FALSE(HudHitIsDisabled(f, 600.0f, 500.0f));
}

TEST_CASE("An empty HUD hits nothing", "[game_ui][hud]") {
  const HudFrame empty;
  REQUIRE(HudHitTest(empty, 100.0f, 100.0f) == kHudNone);
  REQUIRE_FALSE(HudHitIsDisabled(empty, 100.0f, 100.0f));
}

// ---------------------------------------------------------------------------
// HUD build (exercises the Rust ABI end to end)
// ---------------------------------------------------------------------------

TEST_CASE("BuildHud lays out a selection panel with working buttons",
          "[game_ui][hud]") {
    UiContext* ctx = nullptr;
    {
        // Real shipping font, so this covers the actual atlas bake path.
        FILE* f = fopen("assets/fonts/IM_Fell_DW_Pica/IMFellDWPica-Regular.ttf", "rb");
        REQUIRE(f != nullptr);
        fseek(f, 0, SEEK_END);
        const long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        std::vector<uint8_t> bytes(static_cast<size_t>(size));
        REQUIRE(fread(bytes.data(), 1, bytes.size(), f) == bytes.size());
        fclose(f);
        ctx = ui_create(bytes.data(), static_cast<uint32_t>(bytes.size()), 18.0f);
    }
    REQUIRE(ctx != nullptr);

    HudModel model;
    model.gold = 1000;
    model.clock_text = "Day 1   12:00";
    model.has_selection = true;
    model.selection.title = "Free Company Quarters";
    model.selection.rows.emplace_back("roster", "0 / 4");
    model.selection.show_recruit = true;
    model.selection.can_recruit = true;
    model.selection.show_destroy = true;
    model.selection.can_destroy = false;  // greyed, but still clickable

    HudFrame frame;
    REQUIRE(BuildHud(ctx, model, 1600.0f, 900.0f, 1.0f, frame));
    REQUIRE_FALSE(frame.quads.empty());

    // Both buttons produced hit rects, and the disabled one is flagged rather
    // than dropped -- that is what keeps its click from falling through.
    auto find = [&](uint32_t id) -> const UiHitRect* {
        for (const UiHitRect& r : frame.hits) {
            if (r.id == id) return &r;
        }
        return nullptr;
    };
    const UiHitRect* recruit = find(kHudBtnRecruit);
    const UiHitRect* destroy = find(kHudBtnDestroy);
    REQUIRE(recruit != nullptr);
    REQUIRE(destroy != nullptr);
    REQUIRE((recruit->flags & UI_FLAG_DISABLED) == 0);
    REQUIRE((destroy->flags & UI_FLAG_DISABLED) != 0);

    // The panel is on the right-hand side and inside the viewport.
    REQUIRE(recruit->x > 900.0f);
    REQUIRE(recruit->x + recruit->w <= 1600.0f);

    // Hit-testing the solved rect returns that button: draw and hit-test come
    // from the same solve, so they cannot drift apart.
    REQUIRE(HudHitTest(frame, recruit->x + 4.0f, recruit->y + 4.0f) ==
            kHudBtnRecruit);

    // With nothing selected the right panel is STILL present (it now always
    // hosts the combat log), so a click on it is consumed rather than falling
    // through to the world -- but the viewport to its left still hits nothing.
    HudModel bare;
    bare.gold = 5;
    bare.clock_text = "Day 1   00:00";
    HudFrame bare_frame;
    REQUIRE(BuildHud(ctx, bare, 1600.0f, 900.0f, 1.0f, bare_frame));
    REQUIRE_FALSE(bare_frame.quads.empty());
    REQUIRE(HudHitTest(bare_frame, 1500.0f, 400.0f) != kHudNone);  // panel present
    REQUIRE(HudHitTest(bare_frame, 400.0f, 400.0f) == kHudNone);   // viewport clear

    ui_destroy(ctx);
}

TEST_CASE("BuildHud hosts an always-on combat log in the bottom panel",
          "[game_ui][hud]") {
    UiContext* ctx = LoadHudFont();
    REQUIRE(ctx != nullptr);

    HudModel model;
    model.gold = 42;
    model.clock_text = "Day 1   12:00";
    // No selection, but combat has happened: the log fills the bottom panel.
    model.combat_log = {"Merc -> Rat  4", "Rat -> Merc  2", "Rat downed"};

    HudFrame frame;
    REQUIRE(BuildHud(ctx, model, 1600.0f, 900.0f, 1.0f, frame));

    // The log region is hit-testable (so wheel-scroll can target it), on the
    // right-hand panel and in its lower half.
    const UiHitRect* log = FindHitRect(frame, kHudCombatLog);
    REQUIRE(log != nullptr);
    REQUIRE(log->x > 900.0f);             // right-hand side
    REQUIRE(log->y > 900.0f * 0.5f);      // lower half of the 900px viewport
    REQUIRE(HudHitTest(frame, log->x + 4.0f, log->y + 4.0f) == kHudCombatLog);

    // Capacity is positive and consistent with the panel it just laid out.
    REQUIRE(HudCombatLogCapacity() > 0u);

    ui_destroy(ctx);
}

TEST_CASE("BuildHud renders the four speed buttons, each hit-testable",
          "[game_ui][hud]") {
    UiContext* ctx = LoadHudFont();
    REQUIRE(ctx != nullptr);

    HudModel model;
    model.gold = 100;
    model.clock_text = "Day 1   09:00";
    model.speed = 2.0f;  // 2x is the active speed

    HudFrame frame;
    REQUIRE(BuildHud(ctx, model, 1600.0f, 900.0f, 1.0f, frame));

    // All four buttons emit hit rects, and each solved rect hits its own id --
    // draw and hit-test come from the same solve, so they cannot drift apart.
    for (uint32_t id : {static_cast<uint32_t>(kHudBtnPause),
                        static_cast<uint32_t>(kHudBtnSpeed1),
                        static_cast<uint32_t>(kHudBtnSpeed2),
                        static_cast<uint32_t>(kHudBtnSpeed4)}) {
        const UiHitRect* r = FindHitRect(frame, id);
        REQUIRE(r != nullptr);
        REQUIRE(HudHitTest(frame, r->x + 2.0f, r->y + 2.0f) == id);
    }
    ui_destroy(ctx);
}

TEST_CASE("BuildHud makes list entries clickable selection targets",
          "[game_ui][hud]") {
    UiContext* ctx = LoadHudFont();
    REQUIRE(ctx != nullptr);

    // A building panel with a Residents list of two clickable entries. Their ids
    // are >= kHudSelectBase, which is how the view routes a HUD hit to a
    // selection change instead of an action.
    HudModel model;
    model.gold = 100;
    model.clock_text = "Day 1   09:00";
    model.has_selection = true;
    model.selection.kind = HudSelection::Kind::Building;
    model.selection.title = "Free Company Quarters";
    model.selection.rows.emplace_back("id", "#3");
    HudList residents;
    residents.heading = "Residents (2/4)";
    residents.entries.emplace_back("Aldric", "30/30", kHudSelectBase + 0u);
    residents.entries.emplace_back("Bex", "24/30", kHudSelectBase + 1u);
    model.selection.lists.push_back(std::move(residents));

    HudFrame frame;
    REQUIRE(BuildHud(ctx, model, 1600.0f, 900.0f, 1.0f, frame));

    const UiHitRect* a = FindHitRect(frame, kHudSelectBase + 0u);
    const UiHitRect* b = FindHitRect(frame, kHudSelectBase + 1u);
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    // The clickable row wins the innermost-first hit test over the panel chrome,
    // so a click on it selects that hero rather than consuming as panel chrome.
    REQUIRE(HudHitTest(frame, a->x + 2.0f, a->y + 2.0f) == kHudSelectBase + 0u);
    REQUIRE(HudHitTest(frame, b->x + 2.0f, b->y + 2.0f) == kHudSelectBase + 1u);

    ui_destroy(ctx);
}
