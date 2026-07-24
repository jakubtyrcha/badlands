#include "game/ui/hud.hpp"

#include <cmath>
#include <cstdio>

#include <spdlog/spdlog.h>

namespace badlands {
namespace {

// Sizes are LOGICAL pixels; the Rust side multiplies by `scale`.
constexpr float kTopBarHeight = 34.0f;
constexpr float kPanelWidth = 240.0f;
constexpr float kPanelPad = 12.0f;
constexpr float kRowHeight = 22.0f;
constexpr float kTitleHeight = 28.0f;
constexpr float kButtonHeight = 30.0f;
constexpr float kGap = 6.0f;
// Combat log: a FIXED-height region at the bottom of the right panel (the
// selection detail above it takes the remaining space). Fixed rather than a
// grow=1 half so (a) a content-heavy selection can't overflow into it and
// (b) its line capacity is exact -- the `ui` crate can't clip, so the windowing
// and the panel must agree on how many lines fit. A heading sits over the lines.
constexpr float kLogPanelHeight = 300.0f;
constexpr float kLogHeadingHeight = 22.0f;
constexpr float kLogRowHeight = 18.0f;
// Top-bar speed buttons. Pause is wider to fit its word; 1x/2x/4x are compact.
constexpr float kPauseBtnWidth = 60.0f;
constexpr float kSpeedBtnWidth = 40.0f;

// 0xRRGGBBAA. Warm near-black chrome so the HUD reads against both the sunlit
// and night palettes without competing with the scene.
constexpr uint32_t kBarBg = 0x171512ecu;
constexpr uint32_t kPanelBg = 0x1a1816ee;
constexpr uint32_t kLogBg = 0x0f0d0bee;  // slightly darker well for the log
constexpr uint32_t kButtonBg = 0x3a352cff;
constexpr uint32_t kButtonActiveBg = 0x6a5a34ff;  // highlighted (active speed)
constexpr uint32_t kLinkBg = 0x2a2620ff;          // clickable list/link row
constexpr uint32_t kGoldFg = 0xfad45cff;
constexpr uint32_t kTextFg = 0xe8e2d4ff;
constexpr uint32_t kMutedFg = 0x9d9689ff;
constexpr uint32_t kTitleFg = 0xf2e9d2ff;

// model.speed is set to exactly 0/1/2/4 by the caller, but compare with slack.
bool SpeedIs(float speed, float target) { return std::fabs(speed - target) < 0.01f; }

// Appends `s` to the blob and returns its (offset, length) for a UiElement.
struct TextBlob {
  std::string data;
  std::pair<uint32_t, uint32_t> Add(const std::string& s) {
    const uint32_t off = static_cast<uint32_t>(data.size());
    data += s;
    return {off, static_cast<uint32_t>(s.size())};
  }
};

UiElement Make(UiElementKind kind, int32_t parent) {
  UiElement e{};
  e.kind = static_cast<uint8_t>(kind);
  e.parent = parent;
  return e;
}

// Adds a text leaf and returns its index.
int32_t AddLabel(std::vector<UiElement>& els, TextBlob& blob, int32_t parent,
                 const std::string& text, uint32_t color, float fixed,
                 uint32_t flags = UI_FLAG_NONE) {
  UiElement e = Make(UI_ELEM_LABEL, parent);
  const auto [off, len] = blob.Add(text);
  e.text_off = off;
  e.text_len = len;
  e.fg_rgba = color;
  e.fixed = fixed;
  e.flags = flags;
  els.push_back(e);
  return static_cast<int32_t>(els.size()) - 1;
}

// "active, direct, instant, cd 20s" -- duration shows as seconds when timed,
// "instant" otherwise; the cd fragment is omitted when the skill has none.
std::string SkillSummary(const SkillSpec& s) {
  std::string out =
      s.activation == SkillActivation::Passive ? "passive" : "active";
  out += s.targeting == SkillTargeting::Aoe ? ", aoe" : ", direct";
  // >= 0.5f so a sub-half-second duration rounds to "0s" under %.0f and reads
  // as "instant" instead of the misleading ", 0s".
  if (s.duration_seconds >= 0.5f) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), ", %.0fs", s.duration_seconds);
    out += buf;
  } else {
    out += ", instant";
  }
  if (s.cooldown_seconds > 0.0f) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), ", cd %.0fs", s.cooldown_seconds);
    out += buf;
  }
  return out;
}

// The right panel is kPanelWidth (240px) wide; the ui crate cannot clip, so
// text that would overflow it has to be pre-wrapped here. Greedily packs
// whitespace-separated words onto label-less rows of at most `budget` chars;
// a word longer than `budget` rides alone on its own row, untruncated.
void AppendWrapped(HudList& list, const std::string& text, size_t budget) {
  size_t pos = 0;
  std::string line;
  while (pos < text.size()) {
    const size_t space = text.find(' ', pos);
    const size_t word_end = space == std::string::npos ? text.size() : space;
    const std::string word = text.substr(pos, word_end - pos);
    if (line.empty()) {
      line = word;
    } else if (line.size() + 1 + word.size() <= budget) {
      line += ' ';
      line += word;
    } else {
      list.entries.emplace_back("", line);
      line = word;
    }
    pos = space == std::string::npos ? text.size() : space + 1;
  }
  if (!line.empty()) {
    list.entries.emplace_back("", line);
  }
}

// Row-wrap budget for the skills list (name/summary/effect rows all share the
// panel's content width): picked to fit the shipped font/panel (see the
// finding this answers -- 216px content width, ~6.7-7px/char).
constexpr size_t kSkillTextWrapChars = 30;

}  // namespace

bool BuildHud(UiContext* ctx, const HudModel& model, float viewport_w_px,
              float viewport_h_px, float scale, HudFrame& out) {
  if (!ctx) {
    out.quads.clear();
    out.hits.clear();
    return false;
  }
  // NOTE: out is NOT cleared here on purpose -- the grow-and-retry sizing below
  // relies on the vectors keeping last frame's size as its first-attempt
  // capacity, so a steady HUD needs a single ui_build call.

  std::vector<UiElement> els;
  TextBlob blob;

  // 0: root column -- top bar over the body.
  els.push_back(Make(UI_ELEM_COL, -1));
  els[0].grow = 1.0f;

  // 1: top bar (a PANEL paints its background), holding a row of labels.
  {
    UiElement bar = Make(UI_ELEM_PANEL, 0);
    bar.id = kHudTopBarBackground;
    bar.fixed = kTopBarHeight;
    bar.bg_rgba = kBarBg;
    bar.pad = kGap;
    els.push_back(bar);
  }
  const int32_t bar_index = static_cast<int32_t>(els.size()) - 1;
  {
    UiElement row = Make(UI_ELEM_ROW, bar_index);
    row.grow = 1.0f;
    row.gap = 4.0f;  // small spacing between gold / speed buttons / clock
    els.push_back(row);
  }
  const int32_t bar_row = static_cast<int32_t>(els.size()) - 1;
  // Gold on the far left, then a growing spacer, then the speed cluster and the
  // clock snug together on the far right -- so the time controls sit next to the
  // clock (upper-right) rather than centred.
  AddLabel(els, blob, bar_row, "Gold  " + std::to_string(model.gold), kGoldFg,
           0.0f);
  {
    UiElement sp = Make(UI_ELEM_SPACER, bar_row);
    sp.grow = 1.0f;
    els.push_back(sp);
  }
  {
    // Sim-speed buttons. The one matching model.speed is highlighted; none are
    // ever disabled (setting the speed is always valid).
    const struct {
      uint32_t id;
      const char* label;
      float width;
      float target;
    } kSpeedBtns[] = {
        {kHudBtnPause, "Pause", kPauseBtnWidth, 0.0f},
        {kHudBtnSpeed1, "1x", kSpeedBtnWidth, 1.0f},
        {kHudBtnSpeed2, "2x", kSpeedBtnWidth, 2.0f},
        {kHudBtnSpeed4, "4x", kSpeedBtnWidth, 4.0f},
    };
    for (const auto& sb : kSpeedBtns) {
      const bool active = SpeedIs(model.speed, sb.target);
      UiElement b = Make(UI_ELEM_BUTTON, bar_row);
      b.id = sb.id;
      b.fixed = sb.width;
      b.bg_rgba = active ? kButtonActiveBg : kButtonBg;
      b.fg_rgba = active ? kTitleFg : kMutedFg;
      b.flags = UI_FLAG_ALIGN_CENTER;
      const auto [off, len] = blob.Add(std::string(sb.label));
      b.text_off = off;
      b.text_len = len;
      els.push_back(b);
    }
  }
  AddLabel(els, blob, bar_row, model.clock_text, kTextFg, 0.0f);

  // 2: body row -- viewport (empty, just claims space) + right detail panel.
  {
    UiElement body = Make(UI_ELEM_ROW, 0);
    body.grow = 1.0f;
    els.push_back(body);
  }
  const int32_t body = static_cast<int32_t>(els.size()) - 1;
  {
    UiElement viewport = Make(UI_ELEM_SPACER, body);
    viewport.grow = 1.0f;
    els.push_back(viewport);
  }

  // Right panel: ALWAYS present now (it hosts the combat log even with nothing
  // selected). A COL split into the selection detail (top half) over the combat
  // log (bottom half). The outer panel carries kHudPanelBackground so a click on
  // its chrome is consumed rather than falling through to the world.
  UiElement panel = Make(UI_ELEM_PANEL, body);
  panel.id = kHudPanelBackground;
  panel.fixed = kPanelWidth;
  panel.bg_rgba = kPanelBg;
  panel.pad = kPanelPad;
  panel.gap = kGap;
  els.push_back(panel);
  const int32_t panel_index = static_cast<int32_t>(els.size()) - 1;

  // Top half: selection detail (or a muted placeholder). grow=1 -> upper half.
  UiElement top = Make(UI_ELEM_COL, panel_index);
  top.grow = 1.0f;
  top.gap = kGap;
  els.push_back(top);
  const int32_t top_index = static_cast<int32_t>(els.size()) - 1;

  if (model.has_selection) {
    const HudSelection& sel = model.selection;
    AddLabel(els, blob, top_index, sel.title, kTitleFg, kTitleHeight);

    // A "label   value" line. A non-zero click_id makes it a navigable link:
    // wrap it in a PANEL that paints a subtle background AND carries the id (so
    // the ui crate emits a hit rect for it -- and, being deeper than the panel
    // chrome, it wins the innermost-first hit test), with the label/value in an
    // inner ROW. A static row is just the ROW.
    auto add_detail_row = [&](const std::string& label, const std::string& value,
                              uint32_t click_id) {
      int32_t container;
      if (click_id != 0) {
        UiElement p = Make(UI_ELEM_PANEL, top_index);
        p.id = click_id;
        p.fixed = kRowHeight;
        p.bg_rgba = kLinkBg;
        els.push_back(p);
        const int32_t p_index = static_cast<int32_t>(els.size()) - 1;
        UiElement r = Make(UI_ELEM_ROW, p_index);
        r.grow = 1.0f;
        els.push_back(r);
        container = static_cast<int32_t>(els.size()) - 1;
      } else {
        UiElement r = Make(UI_ELEM_ROW, top_index);
        r.fixed = kRowHeight;
        els.push_back(r);
        container = static_cast<int32_t>(els.size()) - 1;
      }
      AddLabel(els, blob, container, label, kMutedFg, 0.0f);
      AddLabel(els, blob, container, value, kTextFg, 0.0f, UI_FLAG_ALIGN_RIGHT);
    };

    for (const HudRow& r : sel.rows) {
      add_detail_row(r.label, r.value, r.click_id);
    }

    // Clickable member lists (residents / visitors): a muted heading, one
    // navigable row per entry, then a "+N more" line when the source was capped
    // (the ui crate has no scrolling yet).
    for (const HudList& list : sel.lists) {
      AddLabel(els, blob, top_index, list.heading, kMutedFg, kRowHeight);
      for (const HudRow& e : list.entries) {
        add_detail_row(e.label, e.value, e.click_id);
      }
      if (list.overflow > 0) {
        AddLabel(els, blob, top_index,
                 "+" + std::to_string(list.overflow) + " more", kMutedFg,
                 kRowHeight);
      }
    }

    // Actions. A shown-but-unavailable button is DISABLED, not omitted: it
    // still emits a hit rect, so the click is consumed instead of falling
    // through and deselecting, and the layout doesn't jump as state changes.
    if (sel.show_recruit) {
      UiElement b = Make(UI_ELEM_BUTTON, top_index);
      b.id = kHudBtnRecruit;
      b.fixed = kButtonHeight;
      b.bg_rgba = kButtonBg;
      b.fg_rgba = kTextFg;
      const auto [off, len] = blob.Add(std::string("Recruit"));
      b.text_off = off;
      b.text_len = len;
      if (!sel.can_recruit) b.flags |= UI_FLAG_DISABLED;
      els.push_back(b);
    }
    if (sel.show_destroy) {
      UiElement b = Make(UI_ELEM_BUTTON, top_index);
      b.id = kHudBtnDestroy;
      b.fixed = kButtonHeight;
      b.bg_rgba = kButtonBg;
      b.fg_rgba = kTextFg;
      const auto [off, len] = blob.Add(std::string("Destroy"));
      b.text_off = off;
      b.text_len = len;
      if (!sel.can_destroy) b.flags |= UI_FLAG_DISABLED;
      els.push_back(b);
    }
  } else {
    AddLabel(els, blob, top_index, "Nothing selected", kMutedFg, kTitleHeight);
  }

  // Bottom: the combat log, a FIXED-height region (the detail COL above grows to
  // fill the rest). kHudCombatLog so a wheel over it scrolls the log rather than
  // zooming the camera. The lines are already windowed by the view to
  // HudCombatLogCapacity() (the ui crate cannot clip/scroll).
  {
    UiElement log = Make(UI_ELEM_PANEL, panel_index);
    log.id = kHudCombatLog;
    log.fixed = kLogPanelHeight;
    log.bg_rgba = kLogBg;
    log.pad = kGap;
    els.push_back(log);
    const int32_t log_index = static_cast<int32_t>(els.size()) - 1;

    AddLabel(els, blob, log_index, "Combat Log", kMutedFg, kLogHeadingHeight);
    for (const std::string& line : model.combat_log) {
      AddLabel(els, blob, log_index, line, kTextFg, kLogRowHeight);
    }
  }

  UiBuildInput in{};
  in.elements = els.data();
  in.element_count = static_cast<uint32_t>(els.size());
  in.text_blob = blob.data.empty() ? nullptr : blob.data.data();
  in.text_blob_len = static_cast<uint32_t>(blob.data.size());
  in.viewport_w_px = viewport_w_px;
  in.viewport_h_px = viewport_h_px;
  in.scale_factor = scale;

  // Grow-and-retry sizing. `out` is the caller's persistent HudFrame, so its
  // vectors keep their capacity across frames: a steady HUD lays out in ONE
  // call (the buffers already fit). A frame whose tree grew past last frame's
  // size gets exactly one extra call -- ui_build reports the TOTAL required (a
  // value > cap means truncated, the count-then-fill idiom), so the retry is
  // sized exactly and always succeeds. Two attempts suffice.
  for (int attempt = 0; attempt < 2; ++attempt) {
    UiBuildOutput o{};
    o.quads = out.quads.empty() ? nullptr : out.quads.data();
    o.quad_cap = static_cast<uint32_t>(out.quads.size());
    o.hits = out.hits.empty() ? nullptr : out.hits.data();
    o.hit_cap = static_cast<uint32_t>(out.hits.size());
    const int32_t rc = ui_build(ctx, &in, &o);
    if (rc == UI_OK) {
      out.quads.resize(o.quad_count);  // shrink to the live count, keep capacity
      out.hits.resize(o.hit_count);
      return true;
    }
    if (rc != UI_ERR_CAPACITY) {
      spdlog::error("BuildHud: ui_build failed ({})", rc);
      out.quads.clear();
      out.hits.clear();
      return false;
    }
    // Too small: grow to exactly what it reported and try once more.
    out.quads.resize(o.quad_count);
    out.hits.resize(o.hit_count);
  }
  spdlog::error("BuildHud: ui_build did not converge");
  out.quads.clear();
  out.hits.clear();
  return false;
}

void AppendHeroProgressionRows(HudSelection& sel, const CharacterState& hero,
                               const SkillCatalog& skills) {
  if (hero.level <= 0) {
    return;  // level >= 1 marks a hero row (snapshot contract)
  }
  sel.rows.emplace_back("level", std::to_string(hero.level));
  sel.rows.emplace_back("xp", std::to_string(hero.xp) + " / " +
                                  std::to_string(hero.xp_next));
  if (hero.skill_count <= 0) {
    return;
  }
  HudList list;
  list.heading = "Skills";
  for (int32_t i = 0; i < hero.skill_count && i < kMaxSkills; ++i) {
    const int32_t id = hero.skills[i];
    const bool known = id >= 0 && id < kSkillCount;
    // The name gets its own row (a label alongside a ~31-char summary would
    // collide in the panel); the summary is a label-less value-only row below
    // it, and the effect text -- which can run well past the panel width --
    // is greedily word-wrapped onto further label-less rows.
    list.entries.emplace_back(SkillName(id), "");
    if (known) {
      list.entries.emplace_back("", SkillSummary(skills.specs[id]));
      if (!skills.specs[id].effect.empty()) {
        AppendWrapped(list, skills.specs[id].effect, kSkillTextWrapChars);
      }
    }
  }
  sel.lists.push_back(std::move(list));
}

uint32_t HudCombatLogCapacity() {
  // The log is a FIXED-height panel (kLogPanelHeight) with kGap padding on each
  // side and a heading over the lines. This mirrors the panel BuildHud lays out
  // exactly, so windowing and the panel agree. Scale cancels (numerator and
  // denominator both scale by the display density), so the count is unitless.
  const float lines_h = kLogPanelHeight - 2.0f * kGap - kLogHeadingHeight;
  if (lines_h <= 0.0f) return 0;
  const int rows = static_cast<int>(lines_h / kLogRowHeight);
  return rows > 0 ? static_cast<uint32_t>(rows) : 0u;
}

namespace {
const UiHitRect* FindHit(const HudFrame& frame, float x, float y) {
  for (const UiHitRect& r : frame.hits) {
    if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h) return &r;
  }
  return nullptr;
}
}  // namespace

uint32_t HudHitTest(const HudFrame& frame, float x, float y) {
  const UiHitRect* hit = FindHit(frame, x, y);
  return hit ? hit->id : kHudNone;
}

bool HudHitIsDisabled(const HudFrame& frame, float x, float y) {
  const UiHitRect* hit = FindHit(frame, x, y);
  return hit && (hit->flags & UI_FLAG_DISABLED) != 0;
}

}  // namespace badlands
