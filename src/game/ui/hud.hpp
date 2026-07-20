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
};

struct HudSelection {
  enum class Kind { Building, Hero };
  Kind kind = Kind::Building;
  uint32_t id = 0;
  std::string title;
  // Label/value rows, rendered one per line.
  std::vector<std::pair<std::string, std::string>> rows;
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
  bool has_selection = false;
  HudSelection selection;
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

// The id of the topmost element containing `p` (PHYSICAL pixels), or kHudNone.
// Hit rects arrive innermost-first, so the first containing rect is the most
// specific one.
uint32_t HudHitTest(const HudFrame& frame, float x, float y);

// True when that id names an action button that is currently disabled -- the
// click should be swallowed, but no action dispatched.
bool HudHitIsDisabled(const HudFrame& frame, float x, float y);

}  // namespace badlands
