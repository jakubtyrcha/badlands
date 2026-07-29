#pragma once

// The game HUD: a top bar (gold + time of day) and a right-hand detail panel
// for the selected building/hero, with Recruit/Destroy actions.
//
// This is the GAME UI surface, distinct from the Dear ImGui DEBUG UI. The split
// is: HudModel is plain data describing what to show; BuildHud turns it into a
// UiElement batch and hands it to the `ui` Rust crate, which solves layout and
// text and returns draw quads + hit rects. Nothing here touches the GPU, and
// nothing in the Rust crate knows what a hero is.
//
// Draw and hit-test cannot disagree, because both come from the same ui_build
// call: the rects the layout solved ARE the hit targets.

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "badlands_sim.hpp"
#include "badlands_ui.h"

namespace badlands {

// Stable element ids. Non-zero ids come back in UiHitRect::id, so these are the
// vocabulary of "what did the user click". kPanelBackground exists so a click
// on the panel's chrome is CONSUMED rather than falling through to the world
// and deselecting the very thing the panel is describing.
enum HudId : uint32_t {
  kHudNone = 0,
  kHudPanelBackground,
  kHudTopBarBackground,
  kHudBtnRecruit,
  kHudBtnDestroy,
  // Sim-speed buttons in the top bar. Setting the speed is a presentation-clock
  // op (SimClock::speed), NOT a sim Action -- the caller handles these ids by
  // writing sim_clock_.speed rather than dispatching to the sim.
  kHudBtnPause,
  kHudBtnSpeed1,
  kHudBtnSpeed2,
  kHudBtnSpeed4,
  // The combat-log region (bottom half of the right panel). It carries an id so
  // (a) a click on it is consumed like the rest of the panel chrome and (b) the
  // view can route a mouse-wheel over it to log scrolling instead of camera zoom.
  kHudCombatLog,
};

// Clickable entity rows (guild residents, building visitors, the hero's home
// link) carry ids at or above this base -- well clear of the small fixed HudId
// enum above -- so a HUD hit resolves to a SELECTION change rather than an
// action. The caller maps (id - kHudSelectBase) into a per-frame target table.
inline constexpr uint32_t kHudSelectBase = 0x40000000u;

// One label/value line in the detail panel. A non-zero click_id makes the row
// interactive (it emits a hit rect and reads as a link) -- used for the hero's
// home-guild link and for entity list entries.
struct HudRow {
  std::string label;
  std::string value;
  uint32_t click_id = 0;
  HudRow(std::string l, std::string v, uint32_t id = 0)
      : label(std::move(l)), value(std::move(v)), click_id(id) {}
};

// A titled, clickable list (a guild's residents, a building's visitors). Each
// entry is a HudRow carrying its own click_id; `overflow` renders a trailing
// "+N more" line when the source was capped (the ui crate has no scrolling yet).
struct HudList {
  std::string heading;
  std::vector<HudRow> entries;
  uint32_t overflow = 0;
};

struct HudSelection {
  enum class Kind { Building, Hero };
  Kind kind = Kind::Building;
  uint32_t id = 0;
  std::string title;
  // Label/value stat rows, rendered one per line. A row with a click_id is a
  // navigable link (e.g. a hero's home guild).
  std::vector<HudRow> rows;
  // Clickable member lists (residents / visitors), rendered after the stat rows.
  std::vector<HudList> lists;
  bool can_recruit = false;   // guild with roster space
  bool can_destroy = false;   // user_destructible and alive
  // Shown greyed (still clickable, so the click is consumed) when the action
  // exists for this kind but is unavailable right now -- e.g. a full guild.
  bool show_recruit = false;
  bool show_destroy = false;
};

struct HudModel {
  uint32_t gold = 0;
  std::string clock_text;          // e.g. "Day 2  14:20"
  float speed = 1.0f;              // sim speed (0/1/2/4) -- highlights its button
  bool has_selection = false;
  HudSelection selection;
  // Combat log (bottom half of the always-on right panel): the already-windowed
  // lines to show, oldest-first (newest at the bottom). The view owns the ring
  // buffer + scroll offset and passes only the visible slice; BuildHud just lays
  // these out (the ui crate has no scrolling, so windowing happens caller-side).
  std::vector<std::string> combat_log;
};

// Result of one HUD build: the quads to draw and the rects to hit-test.
struct HudFrame {
  std::vector<UiQuad> quads;
  std::vector<UiHitRect> hits;
};

// Builds the HUD for one frame. `viewport_*` are PHYSICAL pixels and `scale`
// is logical->physical, so the sizes in the .cpp are authored once in logical
// px. Returns false if the Rust layout call failed (and logs); `out` is then
// left empty, which draws nothing rather than drawing something wrong.
bool BuildHud(UiContext* ctx, const HudModel& model, float viewport_w_px,
              float viewport_h_px, float scale, HudFrame& out);

// Appends a hero's progression detail to a selection: `level` and `xp` rows,
// then a "Skills" list with, per learned skill, a name row, a label-less
// summary row ("active, direct, instant, cd 20s"), and its effect text
// word-wrapped onto further label-less rows -- the panel is a fixed width and
// the ui crate cannot clip, so long text is pre-wrapped here rather than
// overflowing. Pure model-building (no layout, no GPU), so tests cover the
// composition; no-op for non-hero rows (level <= 0).
void AppendHeroProgressionRows(HudSelection& sel, const CharacterState& hero,
                               const SkillCatalog& skills);

// How many combat-log lines fit the fixed-height bottom panel. The view calls it
// to window its log ring buffer before filling HudModel.combat_log -- a single
// source of truth for the layout math, so the windowing and the actual panel can
// never disagree about capacity. Scale-invariant (the log panel is a fixed
// logical height), so it takes no arguments.
uint32_t HudCombatLogCapacity();

// The id of the topmost element containing `p` (PHYSICAL pixels), or kHudNone.
// Hit rects arrive innermost-first, so the first containing rect is the most
// specific one.
uint32_t HudHitTest(const HudFrame& frame, float x, float y);

// True when that id names an action button that is currently disabled -- the
// click should be swallowed, but no action dispatched.
bool HudHitIsDisabled(const HudFrame& frame, float x, float y);

}  // namespace badlands
