// Turns solved rects + text into draw quads (physical px) and hit rects.
//
// Quads, not vertices: emitting a vertex array here would couple this crate to
// shaders/ui/ui.wesl's vertex layout. Quads keep the crate renderer-agnostic
// (which is `panes`' whole point) and keep this stage testable in plain Rust.
// Expanding a quad into two triangles is the caller's ten lines.
//
// Emission order IS draw order: backgrounds before their own text, parents
// before children. Hit rects are emitted innermost-last and reversed at the end,
// so a caller taking the first containing rect gets the most specific hit.

use panes::Rect;

use crate::element::{Element, Kind, FLAG_ALIGN_CENTER, FLAG_ALIGN_RIGHT};
use crate::font::FontAtlas;

#[derive(Clone, Copy)]
pub struct Quad {
    pub x: f32,
    pub y: f32,
    pub w: f32,
    pub h: f32,
    pub u0: f32,
    pub v0: f32,
    pub u1: f32,
    pub v1: f32,
    pub rgba: u32,
}

#[derive(Clone, Copy)]
pub struct HitRect {
    pub id: u32,
    pub x: f32,
    pub y: f32,
    pub w: f32,
    pub h: f32,
    pub flags: u32,
}

// A greyed-out element's text and background are drawn at reduced alpha. The
// element still emits a hit rect (see Element::is_interactive) so the click is
// consumed rather than falling through to the world.
const DISABLED_ALPHA_SCALE: f32 = 0.4;

pub struct DrawOutput {
    pub quads: Vec<Quad>,
    pub hits: Vec<HitRect>,
}

pub fn emit(elements: &[Element], rects: &[Rect], blob: &str, atlas: &FontAtlas) -> DrawOutput {
    let mut quads = Vec::new();
    let mut hits = Vec::new();

    for (e, r) in elements.iter().zip(rects) {
        let disabled = e.flags & crate::element::FLAG_DISABLED != 0;

        if e.kind.has_background() && alpha_of(e.bg_rgba) > 0 {
            quads.push(solid(*r, scaled_alpha(e.bg_rgba, disabled), atlas));
        }

        let text = e.text(blob);
        if !text.is_empty() && matches!(e.kind, Kind::Label | Kind::Button) {
            push_text(&mut quads, e, *r, text, scaled_alpha(e.fg_rgba, disabled), atlas);
        }

        if e.is_interactive() {
            hits.push(HitRect {
                id: e.id,
                x: r.x,
                y: r.y,
                w: r.w,
                h: r.h,
                flags: e.flags,
            });
        }
    }

    // Parents precede children in `elements`, so reversing puts the innermost
    // (most specific) hit first.
    hits.reverse();
    DrawOutput { quads, hits }
}

fn alpha_of(rgba: u32) -> u8 {
    (rgba & 0xff) as u8
}

fn scaled_alpha(rgba: u32, disabled: bool) -> u32 {
    if !disabled {
        return rgba;
    }
    let a = (alpha_of(rgba) as f32 * DISABLED_ALPHA_SCALE) as u32;
    (rgba & 0xffff_ff00) | (a & 0xff)
}

fn solid(r: Rect, rgba: u32, atlas: &FontAtlas) -> Quad {
    let [u, v] = atlas.white_uv;
    Quad {
        x: r.x,
        y: r.y,
        w: r.w,
        h: r.h,
        u0: u,
        v0: v,
        u1: u,
        v1: v,
        rgba,
    }
}

fn push_text(
    quads: &mut Vec<Quad>,
    e: &Element,
    r: Rect,
    text: &str,
    rgba: u32,
    atlas: &FontAtlas,
) {
    let width = atlas.measure(text);
    // Buttons centre their label by default; labels honour the align flags and
    // are otherwise left-aligned.
    let centered = e.flags & FLAG_ALIGN_CENTER != 0 || (e.kind == Kind::Button && e.flags & (FLAG_ALIGN_RIGHT | FLAG_ALIGN_CENTER) == 0);
    let pen_x = if centered {
        r.x + (r.w - width) * 0.5
    } else if e.flags & FLAG_ALIGN_RIGHT != 0 {
        r.x + r.w - width
    } else {
        r.x
    };

    // Vertically centre the text box (ascent above the baseline, descent below;
    // fontdue reports descent negative) within the element's rect.
    let text_height = atlas.ascent_px - atlas.descent_px;
    let baseline_y = r.y + (r.h - text_height) * 0.5 + atlas.ascent_px;

    emit_glyphs(quads, text, pen_x, baseline_y, rgba, atlas);
}

// Lays `text`'s glyphs from a pen at `pen_x` on the baseline `baseline_y`,
// appending one quad per inked glyph. The shared core of both HUD text layout
// (push_text) and the positioned single-run shaper (text_run).
pub fn emit_glyphs(
    quads: &mut Vec<Quad>,
    text: &str,
    pen_x: f32,
    baseline_y: f32,
    rgba: u32,
    atlas: &FontAtlas,
) {
    let mut pen = pen_x;
    for ch in text.chars() {
        let Some(g) = atlas.glyphs.get(&ch) else {
            continue;
        };
        if g.size_px[0] > 0.0 {
            let x = (pen + g.offset[0]).round();
            let y = (baseline_y + g.offset[1]).round();
            quads.push(Quad {
                x,
                y,
                w: g.size_px[0],
                h: g.size_px[1],
                u0: g.uv_min[0],
                v0: g.uv_min[1],
                u1: g.uv_max[0],
                v1: g.uv_max[1],
                rgba,
            });
        }
        pen += g.advance;
    }
}

// One shaped text run, laid from a baseline-left origin (0,0): glyph quads sit
// at x >= 0 and (for the parts above the baseline) y < 0. Backs the ui_text_run
// C ABI for floating world labels -- the caller scales + translates these quads
// to a projected screen anchor, using `width`/`height` to anchor (centre/right)
// the box. `height` is the full text box (ascent above + descent below).
pub struct TextRun {
    pub quads: Vec<Quad>,
    pub width: f32,
    pub height: f32,
}

pub fn text_run(text: &str, rgba: u32, atlas: &FontAtlas) -> TextRun {
    let mut quads = Vec::new();
    emit_glyphs(&mut quads, text, 0.0, 0.0, rgba, atlas);
    TextRun {
        quads,
        width: atlas.measure(text),
        height: atlas.ascent_px - atlas.descent_px,
    }
}
