// UI layout via the `panes` crate: a fixed-height top bar over a growing
// viewport. Resolved in physical pixels every frame (it's sub-microsecond).

use panes::layout;

pub struct UiLayout {
    pub top_bar: panes::Rect,
    pub viewport: panes::Rect,
}

pub fn compute(width_px: f32, height_px: f32, scale_factor: f32) -> UiLayout {
    let bar_height = 40.0 * scale_factor;
    let layout = layout! {
        col {
            panel("topbar", fixed: bar_height)
            panel("viewport", grow: 1.0)
        }
    }
    .expect("invalid UI layout");
    let resolved = layout
        .resolve(width_px, height_px)
        .expect("failed to resolve UI layout");

    let rect_of = |kind: &str| -> panes::Rect {
        let ids = resolved.by_kind(kind);
        *resolved.get(ids[0]).expect("panel missing from layout")
    };

    UiLayout {
        top_bar: rect_of("topbar"),
        viewport: rect_of("viewport"),
    }
}
