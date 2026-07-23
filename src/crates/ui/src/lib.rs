// Game-UI feature-lib: flexbox layout (`panes`) + glyph rasterization/text
// layout (`fontdue`) behind a deliberately coarse C ABI — one call per frame.
// See include/badlands_ui.h for the contract.
//
// This crate is the GAME UI surface, separate from the Dear ImGui DEBUG UI (see
// CLAUDE.md). It knows about panels, labels and buttons; never about heroes,
// gold or buildings. It is renderer-agnostic: it emits quads in physical pixels,
// never vertices, so it stays decoupled from shaders/ui/ui.wesl.

pub mod draw;
pub mod element;
pub mod font;
pub mod layout;

use std::ffi::c_char;
use std::panic::{AssertUnwindSafe, catch_unwind};

use element::{Element, Kind};
use font::{ATLAS_SIZE, FontAtlas};
use panes::Rect;

// Must match include/badlands_ui.h.
pub const UI_OK: i32 = 0;
pub const UI_ERR_NULL: i32 = -1;
pub const UI_ERR_BAD_TREE: i32 = -2;
pub const UI_ERR_BAD_TEXT: i32 = -3;
pub const UI_ERR_LAYOUT: i32 = -4;
pub const UI_ERR_CAPACITY: i32 = -5;
pub const UI_ERR_PANIC: i32 = -6;

pub struct UiContext {
    atlas: FontAtlas,
}

// ---------------------------------------------------------------------------
// C-layout mirrors of the header's structs. Field order/types must match
// badlands_ui.h exactly.
// ---------------------------------------------------------------------------

#[repr(C)]
pub struct CUiElement {
    pub id: u32,
    pub kind: u8,
    pub parent: i32,
    pub fixed: f32,
    pub grow: f32,
    pub pad: f32,
    pub gap: f32,
    pub bg_rgba: u32,
    pub fg_rgba: u32,
    pub text_off: u32,
    pub text_len: u32,
    pub flags: u32,
}

#[repr(C)]
pub struct CUiQuad {
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

#[repr(C)]
pub struct CUiHitRect {
    pub id: u32,
    pub x: f32,
    pub y: f32,
    pub w: f32,
    pub h: f32,
    pub flags: u32,
}

#[repr(C)]
pub struct CUiFontInfo {
    pub atlas_size: u32,
    pub ascent_px: f32,
    pub descent_px: f32,
    pub line_height_px: f32,
    pub white_u: f32,
    pub white_v: f32,
}

#[repr(C)]
pub struct CUiBuildInput {
    pub elements: *const CUiElement,
    pub element_count: u32,
    pub text_blob: *const c_char,
    pub text_blob_len: u32,
    pub viewport_w_px: f32,
    pub viewport_h_px: f32,
    pub scale_factor: f32,
}

#[repr(C)]
pub struct CUiBuildOutput {
    pub quads: *mut CUiQuad,
    pub quad_cap: u32,
    pub quad_count: u32,
    pub hits: *mut CUiHitRect,
    pub hit_cap: u32,
    pub hit_count: u32,
}

// ---------------------------------------------------------------------------
// Entry points
// ---------------------------------------------------------------------------

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ui_create(
    ttf_bytes: *const u8,
    ttf_len: u32,
    px_size: f32,
) -> *mut UiContext {
    if ttf_bytes.is_null() || ttf_len == 0 || !(px_size > 0.0) {
        return std::ptr::null_mut();
    }
    let result = catch_unwind(AssertUnwindSafe(|| {
        let bytes = unsafe { std::slice::from_raw_parts(ttf_bytes, ttf_len as usize) };
        let atlas = FontAtlas::bake(bytes, px_size)?;
        Some(Box::into_raw(Box::new(UiContext { atlas })))
    }));
    match result {
        Ok(Some(ptr)) => ptr,
        _ => std::ptr::null_mut(),
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ui_destroy(ctx: *mut UiContext) {
    if ctx.is_null() {
        return;
    }
    let _ = catch_unwind(AssertUnwindSafe(|| {
        drop(unsafe { Box::from_raw(ctx) });
    }));
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ui_atlas(
    ctx: *const UiContext,
    out_r8: *mut u8,
    cap: u32,
    out_info: *mut CUiFontInfo,
) -> i32 {
    if ctx.is_null() || out_info.is_null() {
        return UI_ERR_NULL;
    }
    let result = catch_unwind(AssertUnwindSafe(|| {
        let atlas = &unsafe { &*ctx }.atlas;
        // Filled even on the capacity error, so the caller learns the size it
        // needs from the same call that told it the buffer was too small.
        unsafe {
            *out_info = CUiFontInfo {
                atlas_size: ATLAS_SIZE,
                ascent_px: atlas.ascent_px,
                descent_px: atlas.descent_px,
                line_height_px: atlas.line_height_px,
                white_u: atlas.white_uv[0],
                white_v: atlas.white_uv[1],
            };
        }
        let needed = atlas.pixels.len();
        if out_r8.is_null() || (cap as usize) < needed {
            return UI_ERR_CAPACITY;
        }
        unsafe { std::ptr::copy_nonoverlapping(atlas.pixels.as_ptr(), out_r8, needed) };
        UI_OK
    }));
    result.unwrap_or(UI_ERR_PANIC)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ui_build(
    ctx: *const UiContext,
    input: *const CUiBuildInput,
    output: *mut CUiBuildOutput,
) -> i32 {
    if ctx.is_null() || input.is_null() || output.is_null() {
        return UI_ERR_NULL;
    }
    let result = catch_unwind(AssertUnwindSafe(|| {
        let atlas = &unsafe { &*ctx }.atlas;
        let input = unsafe { &*input };
        let output = unsafe { &mut *output };
        output.quad_count = 0;
        output.hit_count = 0;

        if input.element_count > 0 && input.elements.is_null() {
            return UI_ERR_NULL;
        }

        // Text blob: borrowed, must be valid UTF-8 for the whole call.
        let blob: &str = if input.text_blob_len == 0 {
            ""
        } else if input.text_blob.is_null() {
            return UI_ERR_NULL;
        } else {
            let bytes = unsafe {
                std::slice::from_raw_parts(
                    input.text_blob as *const u8,
                    input.text_blob_len as usize,
                )
            };
            match std::str::from_utf8(bytes) {
                Ok(s) => s,
                Err(_) => return UI_ERR_BAD_TEXT,
            }
        };

        let raw = if input.element_count == 0 {
            &[][..]
        } else {
            unsafe {
                std::slice::from_raw_parts(input.elements, input.element_count as usize)
            }
        };

        let mut elements = Vec::with_capacity(raw.len());
        for e in raw {
            let Some(kind) = Kind::from_u8(e.kind) else {
                return UI_ERR_BAD_TREE;
            };
            elements.push(Element {
                id: e.id,
                kind,
                parent: e.parent,
                fixed: e.fixed,
                grow: e.grow,
                pad: e.pad,
                gap: e.gap,
                bg_rgba: e.bg_rgba,
                fg_rgba: e.fg_rgba,
                text_off: e.text_off,
                text_len: e.text_len,
                flags: e.flags,
            });
        }

        match element::validate(&elements, blob) {
            Ok(()) => {}
            Err(element::ValidateError::BadTree) => return UI_ERR_BAD_TREE,
            Err(element::ValidateError::BadText) => return UI_ERR_BAD_TEXT,
        }

        let scale = if input.scale_factor > 0.0 { input.scale_factor } else { 1.0 };
        let viewport = Rect {
            x: 0.0,
            y: 0.0,
            w: input.viewport_w_px,
            h: input.viewport_h_px,
        };
        let Some(rects) = layout::solve(&elements, viewport, scale) else {
            return UI_ERR_LAYOUT;
        };

        let out = draw::emit(&elements, &rects, blob, atlas);

        // Truncation idiom (game_state/game_buildings): report the TOTAL needed,
        // write at most `cap`, never past it.
        output.quad_count = out.quads.len() as u32;
        output.hit_count = out.hits.len() as u32;

        let quad_n = out.quads.len().min(output.quad_cap as usize);
        if quad_n > 0 && !output.quads.is_null() {
            for (i, q) in out.quads[..quad_n].iter().enumerate() {
                unsafe {
                    *output.quads.add(i) = CUiQuad {
                        x: q.x,
                        y: q.y,
                        w: q.w,
                        h: q.h,
                        u0: q.u0,
                        v0: q.v0,
                        u1: q.u1,
                        v1: q.v1,
                        rgba: q.rgba,
                    };
                }
            }
        }
        let hit_n = out.hits.len().min(output.hit_cap as usize);
        if hit_n > 0 && !output.hits.is_null() {
            for (i, h) in out.hits[..hit_n].iter().enumerate() {
                unsafe {
                    *output.hits.add(i) = CUiHitRect {
                        id: h.id,
                        x: h.x,
                        y: h.y,
                        w: h.w,
                        h: h.h,
                        flags: h.flags,
                    };
                }
            }
        }

        if out.quads.len() > output.quad_cap as usize || out.hits.len() > output.hit_cap as usize {
            return UI_ERR_CAPACITY;
        }
        UI_OK
    }));
    result.unwrap_or(UI_ERR_PANIC)
}

#[cfg(test)]
mod tests;
