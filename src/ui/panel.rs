// Floating building-selection panel: title + Recruit/Destroy buttons + a
// visitor list. Clones the sidebar triad -- draw and hit_test both derive their
// rects from rects(), so they can never disagree. The model is built entirely
// from the render-state snapshots the app already holds (no per-building game
// query): occupancy/visitors are filtered from characters, recruitability and
// destroyability from the static class/def catalogs.

use glam::Vec2;

use crate::game::catalog;
use crate::game::class_catalog::{self, HeroClass};
use crate::game_ffi::{BuildingKind, GameBuildingState, GameCharacterState, building_def};
use crate::ui::layout::rect_contains;
use crate::ui::render::UiRenderer;

pub const PANEL_W: f32 = 220.0; // logical px
const PAD: f32 = 8.0;
const TITLE_H: f32 = 26.0;
const ROW_H: f32 = 26.0;
pub const CAPACITY: u32 = 4; // kGuildRosterCap

const PANEL_BG: [f32; 4] = [0.10, 0.10, 0.12, 0.96];
const TITLE_COLOR: [f32; 4] = [0.98, 0.83, 0.36, 1.0];
const TEXT: [f32; 4] = [0.90, 0.90, 0.92, 1.0];
const TEXT_DIM: [f32; 4] = [0.55, 0.55, 0.58, 1.0];
const BUTTON_BG: [f32; 4] = [0.20, 0.32, 0.22, 1.0];
const BUTTON_GREY: [f32; 4] = [0.16, 0.16, 0.18, 1.0];
const BUTTON_DESTROY: [f32; 4] = [0.45, 0.22, 0.20, 1.0];

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PanelButton {
    Recruit,
    Destroy,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Hit {
    Button(PanelButton),
    Consumed, // inside the panel but not a live button: swallow the click
    Miss,
}

pub struct PanelModel {
    pub building_id: u32,
    pub anchor: Vec2, // screen px, top-left (already clamped to the viewport)
    title: &'static str,
    recruit: Option<HeroClass>, // Some => this guild recruits that class
    recruit_enabled: bool,      // occupancy < capacity
    occupancy: u32,
    destroyable: bool,
    visitors: Vec<HeroClass>,
}

// Build the panel model from render-state (no game query).
pub fn build_model(
    building: &GameBuildingState,
    buildings: &[GameBuildingState],
    characters: &[GameCharacterState],
    anchor: Vec2,
) -> PanelModel {
    let kind = BuildingKind::from_i32(building.kind);
    let id = building.id as i32;

    // A hero's class is derived from its home guild's kind.
    let class_of_home = |home: i32| -> Option<HeroClass> {
        buildings
            .iter()
            .find(|b| b.id as i32 == home)
            .and_then(|b| class_catalog::guild_class(BuildingKind::from_i32(b.kind)))
    };

    let recruit = class_catalog::guild_class(kind);
    let occupancy = characters.iter().filter(|c| c.home_building_id == id).count() as u32;
    let visitors = characters
        .iter()
        .filter(|c| c.inside_building_id == id)
        .filter_map(|c| class_of_home(c.home_building_id))
        .collect();

    PanelModel {
        building_id: building.id,
        anchor,
        title: catalog::info(kind).name,
        recruit,
        recruit_enabled: recruit.is_some() && occupancy < CAPACITY,
        occupancy,
        destroyable: building_def(kind).user_destructible == 1,
        visitors,
    }
}

struct Rects {
    bg: panes::Rect,
    recruit: Option<panes::Rect>,
    destroy: Option<panes::Rect>,
    visitors_y: f32,
}

// Single source of truth for both draw and hit_test.
fn rects(model: &PanelModel, scale: f32) -> Rects {
    let pad = PAD * scale;
    let w = PANEL_W * scale;
    let row = ROW_H * scale;
    let a = model.anchor;

    let button = |y: f32| panes::Rect {
        x: a.x + pad,
        y,
        w: w - 2.0 * pad,
        h: row,
    };

    let mut y = a.y + pad + TITLE_H * scale;
    let recruit = model.recruit.map(|_| button(y));
    if recruit.is_some() {
        y += row + pad * 0.5;
    }
    let destroy = model.destroyable.then(|| button(y));
    if destroy.is_some() {
        y += row + pad * 0.5;
    }
    let visitors_y = y;
    if !model.visitors.is_empty() {
        y += row * (1 + model.visitors.len()) as f32;
    }
    let h = (y - a.y) + pad;
    Rects {
        bg: panes::Rect { x: a.x, y: a.y, w, h },
        recruit,
        destroy,
        visitors_y,
    }
}

pub fn hit_test(model: &PanelModel, scale: f32, x: f32, y: f32) -> Hit {
    let r = rects(model, scale);
    if model.recruit_enabled {
        if let Some(rr) = r.recruit {
            if rect_contains(&rr, x, y) {
                return Hit::Button(PanelButton::Recruit);
            }
        }
    }
    if let Some(rr) = r.destroy {
        if rect_contains(&rr, x, y) {
            return Hit::Button(PanelButton::Destroy);
        }
    }
    if rect_contains(&r.bg, x, y) {
        return Hit::Consumed;
    }
    Hit::Miss
}

pub fn draw(ui: &mut UiRenderer, model: &PanelModel, scale: f32) {
    let r = rects(model, scale);
    let pad = PAD * scale;
    let ascent = ui.atlas.ascent_px;
    let descent = ui.atlas.descent_px;
    ui.push_rect(&r.bg, PANEL_BG);

    let title_baseline = model.anchor.y + pad + ascent;
    ui.push_text(model.anchor.x + pad, title_baseline, model.title, TITLE_COLOR);

    let mid_baseline = |rect: &panes::Rect| rect.y + rect.h * 0.5 + (ascent + descent) * 0.5;

    if let (Some(rect), Some(cls)) = (r.recruit, model.recruit) {
        let bg = if model.recruit_enabled { BUTTON_BG } else { BUTTON_GREY };
        ui.push_rect(&rect, bg);
        let label = format!(
            "Recruit {} ({}/{})",
            class_catalog::info(cls).name,
            model.occupancy,
            CAPACITY
        );
        let color = if model.recruit_enabled { TEXT } else { TEXT_DIM };
        ui.push_text(rect.x + pad, mid_baseline(&rect), &label, color);
    }

    if let Some(rect) = r.destroy {
        ui.push_rect(&rect, BUTTON_DESTROY);
        ui.push_text(rect.x + pad, mid_baseline(&rect), "Destroy", TEXT);
    }

    if !model.visitors.is_empty() {
        let mut y = r.visitors_y + ascent;
        ui.push_text(model.anchor.x + pad, y, "Visiting:", TEXT);
        for cls in &model.visitors {
            y += ROW_H * scale;
            ui.push_text(
                model.anchor.x + pad * 2.0,
                y,
                class_catalog::info(*cls).name,
                TEXT,
            );
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn model(recruit: Option<HeroClass>, occ: u32, destroyable: bool) -> PanelModel {
        PanelModel {
            building_id: 1,
            anchor: Vec2::new(100.0, 100.0),
            title: "Test",
            recruit,
            recruit_enabled: recruit.is_some() && occ < CAPACITY,
            occupancy: occ,
            destroyable,
            visitors: Vec::new(),
        }
    }

    // Center of the recruit button (first row below the title).
    fn recruit_center(m: &PanelModel) -> (f32, f32) {
        let r = rects(m, 1.0);
        let rr = r.recruit.unwrap();
        (rr.x + rr.w * 0.5, rr.y + rr.h * 0.5)
    }

    #[test]
    fn recruit_button_hits_on_an_open_guild() {
        let m = model(Some(HeroClass::Mercenary), 2, false);
        let (x, y) = recruit_center(&m);
        assert_eq!(hit_test(&m, 1.0, x, y), Hit::Button(PanelButton::Recruit));
    }

    #[test]
    fn a_full_guild_recruit_button_is_greyed_and_consumes() {
        let m = model(Some(HeroClass::Mercenary), CAPACITY, false);
        let (x, y) = recruit_center(&m);
        // Same spot, but greyed => not a live button, just swallowed.
        assert_eq!(hit_test(&m, 1.0, x, y), Hit::Consumed);
    }

    #[test]
    fn destroy_button_only_exists_when_destroyable() {
        let destroyable = model(Some(HeroClass::Mercenary), 0, true);
        let r = rects(&destroyable, 1.0);
        let dr = r.destroy.expect("destroyable => a destroy button");
        assert_eq!(
            hit_test(&destroyable, 1.0, dr.x + dr.w * 0.5, dr.y + dr.h * 0.5),
            Hit::Button(PanelButton::Destroy)
        );

        let non_destroyable = model(None, 0, false);
        assert!(rects(&non_destroyable, 1.0).destroy.is_none());
    }

    #[test]
    fn clicks_outside_the_panel_miss() {
        let m = model(Some(HeroClass::Mercenary), 0, true);
        assert_eq!(hit_test(&m, 1.0, 1000.0, 1000.0), Hit::Miss);
    }
}
