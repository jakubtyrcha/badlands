// Right-side build panel: one button per player-placeable building. Button
// rects are computed deterministically from the sidebar layout rect so drawing
// and click hit-testing always agree.

use crate::game::catalog;
use crate::game_ffi::BuildingKind;
use crate::ui::layout::{UiLayout, rect_contains};
use crate::ui::render::UiRenderer;

const PAD: f32 = 8.0; // logical px
const TITLE_H: f32 = 30.0;
const BUTTON_H: f32 = 34.0;

const PANEL_BG: [f32; 4] = [0.10, 0.10, 0.12, 0.94];
const BUTTON_BG: [f32; 4] = [0.20, 0.21, 0.25, 1.0];
const BUTTON_SELECTED: [f32; 4] = [0.32, 0.46, 0.30, 1.0];
const TEXT: [f32; 4] = [0.90, 0.90, 0.92, 1.0];
const TITLE: [f32; 4] = [0.98, 0.83, 0.36, 1.0];

// One button rect per placeable kind, stacked below the title.
pub fn button_rects(layout: &UiLayout, scale: f32) -> Vec<(BuildingKind, panes::Rect)> {
    let pad = PAD * scale;
    let button_h = BUTTON_H * scale;
    let sidebar = &layout.sidebar;
    let mut y = sidebar.y + pad + TITLE_H * scale;
    let mut rects = Vec::with_capacity(BuildingKind::PLAYER_PLACEABLE.len());
    for &kind in &BuildingKind::PLAYER_PLACEABLE {
        rects.push((
            kind,
            panes::Rect {
                x: sidebar.x + pad,
                y,
                w: sidebar.w - 2.0 * pad,
                h: button_h,
            },
        ));
        y += button_h + pad;
    }
    rects
}

// The kind whose button contains the cursor, if any.
pub fn hit_test(layout: &UiLayout, scale: f32, cursor_x: f32, cursor_y: f32) -> Option<BuildingKind> {
    button_rects(layout, scale)
        .into_iter()
        .find(|(_, rect)| rect_contains(rect, cursor_x, cursor_y))
        .map(|(kind, _)| kind)
}

pub fn draw(ui: &mut UiRenderer, layout: &UiLayout, scale: f32, selected: Option<BuildingKind>) {
    ui.push_rect(&layout.sidebar, PANEL_BG);

    let pad = PAD * scale;
    let title_baseline = layout.sidebar.y + pad + ui.atlas.ascent_px;
    ui.push_text(layout.sidebar.x + pad, title_baseline, "Build", TITLE);

    for (kind, rect) in button_rects(layout, scale) {
        let bg = if Some(kind) == selected {
            BUTTON_SELECTED
        } else {
            BUTTON_BG
        };
        ui.push_rect(&rect, bg);
        let baseline = rect.y + rect.h * 0.5 + (ui.atlas.ascent_px + ui.atlas.descent_px) * 0.5;
        ui.push_text(rect.x + pad, baseline, catalog::info(kind).name, TEXT);
    }
}
