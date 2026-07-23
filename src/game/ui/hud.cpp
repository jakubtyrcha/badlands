#include "game/ui/hud.hpp"

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

// 0xRRGGBBAA. Warm near-black chrome so the HUD reads against both the sunlit
// and night palettes without competing with the scene.
constexpr uint32_t kBarBg = 0x171512ecu;
constexpr uint32_t kPanelBg = 0x1a1816ee;
constexpr uint32_t kButtonBg = 0x3a352cff;
constexpr uint32_t kGoldFg = 0xfad45cff;
constexpr uint32_t kTextFg = 0xe8e2d4ff;
constexpr uint32_t kMutedFg = 0x9d9689ff;
constexpr uint32_t kTitleFg = 0xf2e9d2ff;

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
    els.push_back(row);
  }
  const int32_t bar_row = static_cast<int32_t>(els.size()) - 1;
  AddLabel(els, blob, bar_row, "Gold  " + std::to_string(model.gold), kGoldFg,
           0.0f);
  AddLabel(els, blob, bar_row, model.clock_text, kTextFg, 0.0f,
           UI_FLAG_ALIGN_RIGHT);

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

  if (model.has_selection) {
    const HudSelection& sel = model.selection;
    UiElement panel = Make(UI_ELEM_PANEL, body);
    panel.id = kHudPanelBackground;  // consumes clicks on the chrome
    panel.fixed = kPanelWidth;
    panel.bg_rgba = kPanelBg;
    panel.pad = kPanelPad;
    panel.gap = kGap;
    els.push_back(panel);
    const int32_t panel_index = static_cast<int32_t>(els.size()) - 1;

    AddLabel(els, blob, panel_index, sel.title, kTitleFg, kTitleHeight);
    for (const auto& [label, value] : sel.rows) {
      // One row per line: "label   value", right-aligned value.
      UiElement row = Make(UI_ELEM_ROW, panel_index);
      row.fixed = kRowHeight;
      els.push_back(row);
      const int32_t row_index = static_cast<int32_t>(els.size()) - 1;
      AddLabel(els, blob, row_index, label, kMutedFg, 0.0f);
      AddLabel(els, blob, row_index, value, kTextFg, 0.0f,
               UI_FLAG_ALIGN_RIGHT);
    }

    // Actions. A shown-but-unavailable button is DISABLED, not omitted: it
    // still emits a hit rect, so the click is consumed instead of falling
    // through and deselecting, and the layout doesn't jump as state changes.
    if (sel.show_recruit) {
      UiElement b = Make(UI_ELEM_BUTTON, panel_index);
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
      UiElement b = Make(UI_ELEM_BUTTON, panel_index);
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

    // Trailing filler so the content sits at the top of the panel rather than
    // being stretched to fill it.
    UiElement filler = Make(UI_ELEM_SPACER, panel_index);
    filler.grow = 1.0f;
    els.push_back(filler);
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
