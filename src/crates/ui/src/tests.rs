// Layout/validation tests. These exercise the pure-Rust core (element ->
// rects -> quads); the C ABI itself is exercised from the C++ side.
//
// Deliberately NOT pinned: exact glyph advances or quad counts for a given
// string — those track the font file and fontdue's rasterizer, not our logic.
// We assert structural invariants instead.

use super::*;
use crate::element::{Element, Kind, ValidateError, validate};
use panes::Rect;

fn el(kind: Kind, parent: i32, fixed: f32, grow: f32) -> Element {
    Element {
        id: 0,
        kind,
        parent,
        fixed,
        grow,
        pad: 0.0,
        gap: 0.0,
        bg_rgba: 0,
        fg_rgba: 0xffff_ffff,
        text_off: 0,
        text_len: 0,
        flags: 0,
    }
}

// The HUD skeleton this system exists to draw: a fixed-height top bar over a
// row of [viewport grows, right panel fixed].
fn hud_skeleton() -> Vec<Element> {
    vec![
        el(Kind::Col, -1, 0.0, 1.0),   // 0 root
        el(Kind::Panel, 0, 40.0, 0.0), // 1 top bar
        el(Kind::Row, 0, 0.0, 1.0),    // 2 body
        el(Kind::Spacer, 2, 0.0, 1.0), // 3 viewport
        el(Kind::Panel, 2, 220.0, 0.0), // 4 right panel
    ]
}

#[test]
fn solves_top_bar_and_right_panel() {
    let e = hud_skeleton();
    let vp = Rect { x: 0.0, y: 0.0, w: 1600.0, h: 900.0 };
    let r = layout::solve(&e, vp, 1.0).expect("layout");

    // Top bar: full width, fixed height, at the origin.
    assert_eq!(r[1].x, 0.0);
    assert_eq!(r[1].y, 0.0);
    assert_eq!(r[1].w, 1600.0);
    assert_eq!(r[1].h, 40.0);

    // Body fills the rest, directly under the bar.
    assert_eq!(r[2].y, 40.0);
    assert_eq!(r[2].h, 860.0);

    // Right panel: fixed width, flush to the right edge, full body height.
    assert_eq!(r[4].w, 220.0);
    assert_eq!(r[4].x, 1600.0 - 220.0);
    assert_eq!(r[4].h, 860.0);

    // Viewport takes what's left.
    assert_eq!(r[3].w, 1600.0 - 220.0);
}

#[test]
fn scale_factor_multiplies_fixed_sizes_but_not_the_viewport() {
    let e = hud_skeleton();
    let vp = Rect { x: 0.0, y: 0.0, w: 1600.0, h: 900.0 };
    let r = layout::solve(&e, vp, 2.0).expect("layout");

    // Fixed sizes are authored in logical px and scale with the display.
    assert_eq!(r[1].h, 80.0);
    assert_eq!(r[4].w, 440.0);
    // The viewport is already physical, so it is untouched.
    assert_eq!(r[1].w, 1600.0);
}

#[test]
fn container_rect_is_exact_not_the_union_of_its_children() {
    // The bug the level-by-level solve exists to prevent: a fixed-height bar
    // holding one narrow label must keep its FULL width, not shrink to the
    // label's extent (which is what reconstructing a container rect from the
    // union of its children would give).
    let mut e = vec![
        el(Kind::Col, -1, 0.0, 1.0),    // 0 root
        el(Kind::Panel, 0, 40.0, 0.0),  // 1 top bar
        el(Kind::Row, 1, 0.0, 1.0),     // 2 bar contents
        el(Kind::Label, 2, 60.0, 0.0),  // 3 a 60px-wide label
    ];
    e[1].bg_rgba = 0x1122_33ff;
    let vp = Rect { x: 0.0, y: 0.0, w: 800.0, h: 600.0 };
    let r = layout::solve(&e, vp, 1.0).expect("layout");

    assert_eq!(r[1].w, 800.0, "bar spans the full width");
    assert_eq!(r[1].h, 40.0, "bar keeps its fixed height");
    assert_eq!(r[3].w, 60.0, "label keeps its own width");
    assert!(r[3].w < r[1].w, "the bar is wider than its only child");
}

#[test]
fn fixed_sizes_the_main_axis_of_the_parent() {
    // Flexbox semantics, and what the ABI documents: `fixed` is the MAIN-axis
    // size, so the same element sizes its height under a Col and its width
    // under a Row. Pinned because getting this backwards is silent.
    let col = vec![el(Kind::Col, -1, 0.0, 1.0), el(Kind::Label, 0, 50.0, 0.0)];
    let row = vec![el(Kind::Row, -1, 0.0, 1.0), el(Kind::Label, 0, 50.0, 0.0)];
    let vp = Rect { x: 0.0, y: 0.0, w: 400.0, h: 300.0 };

    let rc = layout::solve(&col, vp, 1.0).expect("layout");
    assert_eq!(rc[1].h, 50.0);
    assert_eq!(rc[1].w, 400.0, "cross axis stretches");

    let rr = layout::solve(&row, vp, 1.0).expect("layout");
    assert_eq!(rr[1].w, 50.0);
    assert_eq!(rr[1].h, 300.0, "cross axis stretches");
}

#[test]
fn padding_insets_children_on_both_sides() {
    let mut e = vec![
        el(Kind::Panel, -1, 0.0, 1.0),
        el(Kind::Label, 0, 0.0, 1.0),
    ];
    e[0].pad = 10.0;
    let vp = Rect { x: 0.0, y: 0.0, w: 200.0, h: 100.0 };
    let r = layout::solve(&e, vp, 1.0).expect("layout");

    assert_eq!(r[1].x, 10.0);
    assert_eq!(r[1].y, 10.0);
    assert_eq!(r[1].w, 180.0);
    assert_eq!(r[1].h, 80.0);
}

#[test]
fn gap_separates_siblings() {
    let mut e = vec![
        el(Kind::Col, -1, 0.0, 1.0),
        el(Kind::Label, 0, 20.0, 0.0),
        el(Kind::Label, 0, 20.0, 0.0),
    ];
    e[0].gap = 8.0;
    let vp = Rect { x: 0.0, y: 0.0, w: 100.0, h: 200.0 };
    let r = layout::solve(&e, vp, 1.0).expect("layout");

    assert_eq!(r[1].y, 0.0);
    assert_eq!(r[2].y, 28.0, "second child clears the first plus the gap");
}

#[test]
fn rejects_a_parent_that_does_not_precede_its_child() {
    // Forward references would break layout::solve's single forward pass and
    // make cycles representable.
    let e = vec![el(Kind::Col, -1, 0.0, 1.0), el(Kind::Label, 2, 0.0, 1.0), el(Kind::Label, 0, 0.0, 1.0)];
    assert_eq!(validate(&e, ""), Err(ValidateError::BadTree));
}

#[test]
fn rejects_a_root_with_a_parent() {
    let e = vec![el(Kind::Col, 0, 0.0, 1.0)];
    assert_eq!(validate(&e, ""), Err(ValidateError::BadTree));
}

#[test]
fn rejects_a_text_run_outside_the_blob() {
    let mut e = vec![el(Kind::Label, -1, 0.0, 1.0)];
    e[0].text_off = 3;
    e[0].text_len = 10;
    assert_eq!(validate(&e, "abc"), Err(ValidateError::BadText));
}

#[test]
fn rejects_a_text_run_splitting_a_utf8_char() {
    let mut e = vec![el(Kind::Label, -1, 0.0, 1.0)];
    e[0].text_off = 0;
    e[0].text_len = 1; // 'é' is two bytes
    assert_eq!(validate(&e, "é"), Err(ValidateError::BadText));
}

#[test]
fn accepts_a_valid_tree_and_text() {
    let mut e = hud_skeleton();
    e[1].text_off = 0;
    e[1].text_len = 4;
    assert_eq!(validate(&e, "Gold"), Ok(()));
}

#[test]
fn buttons_and_ided_elements_emit_hit_rects_innermost_first() {
    let mut e = vec![
        el(Kind::Col, -1, 0.0, 1.0),
        el(Kind::Panel, 0, 0.0, 1.0),
        el(Kind::Button, 1, 30.0, 0.0),
    ];
    e[1].id = 7; // the panel: consumes clicks that miss the button
    e[2].id = 9; // the button
    e[1].bg_rgba = 0x0000_00ff;

    let vp = Rect { x: 0.0, y: 0.0, w: 200.0, h: 200.0 };
    let rects = layout::solve(&e, vp, 1.0).expect("layout");
    let atlas = test_atlas();
    let out = draw::emit(&e, &rects, "", &atlas);

    let ids: Vec<u32> = out.hits.iter().map(|h| h.id).collect();
    assert_eq!(ids, vec![9, 7], "innermost first, so the button wins the click");
}

#[test]
fn a_disabled_button_still_emits_a_hit_rect() {
    // Otherwise a click on a greyed button would fall through the panel and
    // deselect/repick the world behind it.
    let mut e = vec![el(Kind::Panel, -1, 0.0, 1.0), el(Kind::Button, 0, 30.0, 0.0)];
    e[1].id = 5;
    e[1].flags = element::FLAG_DISABLED;

    let vp = Rect { x: 0.0, y: 0.0, w: 200.0, h: 200.0 };
    let rects = layout::solve(&e, vp, 1.0).expect("layout");
    let out = draw::emit(&e, &rects, "", &test_atlas());

    let hit = out.hits.iter().find(|h| h.id == 5).expect("disabled button emits a hit rect");
    assert_ne!(hit.flags & element::FLAG_DISABLED, 0, "flag survives so callers can skip the action");
}

#[test]
fn a_background_emits_exactly_one_quad_covering_its_rect() {
    let mut e = vec![el(Kind::Panel, -1, 0.0, 1.0)];
    e[0].bg_rgba = 0x1020_30ff;

    let vp = Rect { x: 0.0, y: 0.0, w: 120.0, h: 60.0 };
    let rects = layout::solve(&e, vp, 1.0).expect("layout");
    let out = draw::emit(&e, &rects, "", &test_atlas());

    assert_eq!(out.quads.len(), 1);
    let q = out.quads[0];
    assert_eq!((q.x, q.y, q.w, q.h), (0.0, 0.0, 120.0, 60.0));
    assert_eq!(q.rgba, 0x1020_30ff);
    assert_eq!((q.u0, q.v0), (q.u1, q.v1), "solid rects sample the reserved white texel");
}

#[test]
fn a_transparent_background_emits_nothing() {
    let mut e = vec![el(Kind::Panel, -1, 0.0, 1.0)];
    e[0].bg_rgba = 0x1020_3000; // alpha 0
    let vp = Rect { x: 0.0, y: 0.0, w: 120.0, h: 60.0 };
    let rects = layout::solve(&e, vp, 1.0).expect("layout");
    assert!(draw::emit(&e, &rects, "", &test_atlas()).quads.is_empty());
}

#[test]
fn text_measure_grows_with_length() {
    let atlas = test_atlas();
    assert!(atlas.measure("Gold: 1000") > atlas.measure("Gold: 1"));
    assert_eq!(atlas.measure(""), 0.0);
}

// draw::text_run is the positioned single-run shaper behind the ui_text_run C
// ABI (floating world labels): a run laid from a baseline-left origin (0,0),
// which the C++ caller then scales + translates to a screen anchor.
#[test]
fn text_run_reports_measured_width_and_full_text_height() {
    let atlas = test_atlas();
    let run = draw::text_run("AB", 0x1234_56ff, &atlas);
    // Width is the same advance the layout engine measures with.
    assert_eq!(run.width, atlas.measure("AB"));
    // Height is the whole text box: ascent above + descent below the baseline.
    assert_eq!(run.height, atlas.ascent_px - atlas.descent_px);
    // One quad per inked glyph (both letters here), carrying the requested color.
    assert_eq!(run.quads.len(), 2);
    assert!(run.quads.iter().all(|q| q.rgba == 0x1234_56ff));
}

#[test]
fn text_run_is_anchored_baseline_left_at_the_origin() {
    let atlas = test_atlas();
    let run = draw::text_run("A", 0xffff_ffff, &atlas);
    let q = run.quads[0];
    // The pen starts at x=0; the first glyph sits within a side-bearing of it
    // (a glyph's ink may extend slightly left of the pen, so not strictly >= 0).
    assert!(q.x.abs() < atlas.line_height_px, "first glyph is anchored at the left origin");
    assert!(q.y < 0.0, "the glyph sits ABOVE the baseline at y=0");
}

#[test]
fn text_run_skips_glyphs_with_no_ink_but_still_advances() {
    // A space has advance but no quad, so "A B" measures wider than "AB" yet
    // still emits only the two inked glyphs.
    let atlas = test_atlas();
    let spaced = draw::text_run("A B", 0xffff_ffff, &atlas);
    assert_eq!(spaced.quads.len(), 2);
    assert!(spaced.width > draw::text_run("AB", 0xffff_ffff, &atlas).width);
}

#[test]
fn text_run_of_empty_string_is_empty() {
    let atlas = test_atlas();
    let run = draw::text_run("", 0xffff_ffff, &atlas);
    assert!(run.quads.is_empty());
    assert_eq!(run.width, 0.0);
}

// The real shipping font, so the atlas geometry under test is the one the game
// actually bakes. Tests run from the crate dir, hence the walk up to the root.
fn test_atlas() -> font::FontAtlas {
    let path = concat!(
        env!("CARGO_MANIFEST_DIR"),
        "/../../../assets/fonts/IM_Fell_DW_Pica/IMFellDWPica-Regular.ttf"
    );
    let bytes = std::fs::read(path).expect("shipping font is present (git-lfs checked out)");
    font::FontAtlas::bake(&bytes, 20.0).expect("atlas bakes")
}

#[test]
fn atlas_reserves_a_solid_white_texel() {
    let atlas = test_atlas();
    assert_eq!(atlas.pixels.len(), (font::ATLAS_SIZE * font::ATLAS_SIZE) as usize);
    assert_eq!(atlas.pixels[0], 255, "origin block is opaque for solid rects");
    assert!(atlas.ascent_px > 0.0);
    assert!(atlas.descent_px <= 0.0, "fontdue reports descent below the baseline");
}
