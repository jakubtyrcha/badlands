// Glyph atlas baked once at startup with fontdue: printable ASCII rasterized
// into a single R8 coverage texture. A small white block is reserved at the
// origin so solid rects can share the glyph pipeline (one pipeline, one draw
// call for the whole UI).
//
// Ported from the legacy Rust app's src/ui/font.rs, minus the wgpu texture
// creation — this crate is renderer-agnostic and hands the bytes to C++.

use std::collections::HashMap;

pub const ATLAS_SIZE: u32 = 1024;
const PADDING: u32 = 1;
const WHITE_BLOCK: u32 = 4;

#[derive(Clone, Copy)]
pub struct GlyphInfo {
    pub uv_min: [f32; 2],
    pub uv_max: [f32; 2],
    pub size_px: [f32; 2],
    // Offset from the pen position: x from the pen, y DOWN from the baseline to
    // the glyph top (screen coordinates, y-down).
    pub offset: [f32; 2],
    pub advance: f32,
}

pub struct FontAtlas {
    pub pixels: Vec<u8>, // ATLAS_SIZE^2, R8 coverage
    pub glyphs: HashMap<char, GlyphInfo>,
    pub white_uv: [f32; 2],
    pub ascent_px: f32,
    pub descent_px: f32,
    pub line_height_px: f32,
}

impl FontAtlas {
    pub fn bake(ttf_bytes: &[u8], px_size: f32) -> Option<FontAtlas> {
        let font = fontdue::Font::from_bytes(ttf_bytes, fontdue::FontSettings::default()).ok()?;

        let mut pixels = vec![0u8; (ATLAS_SIZE * ATLAS_SIZE) as usize];
        // Reserved solid-white block at the origin, for rect quads.
        for y in 0..WHITE_BLOCK {
            for x in 0..WHITE_BLOCK {
                pixels[(y * ATLAS_SIZE + x) as usize] = 255;
            }
        }

        let mut cursor_x: u32 = WHITE_BLOCK + PADDING;
        let mut cursor_y: u32 = 0;
        let mut row_height: u32 = WHITE_BLOCK;

        let mut glyphs = HashMap::new();
        for ch in (32u8..127).map(char::from) {
            let (metrics, coverage) = font.rasterize(ch, px_size);
            let w = metrics.width as u32;
            let h = metrics.height as u32;
            if cursor_x + w + PADDING > ATLAS_SIZE {
                cursor_x = 0;
                cursor_y += row_height + PADDING;
                row_height = 0;
            }
            if cursor_y + h > ATLAS_SIZE {
                return None; // atlas overflow: caller picked too large a px_size
            }
            for row in 0..h {
                let dst = ((cursor_y + row) * ATLAS_SIZE + cursor_x) as usize;
                let src = (row * w) as usize;
                pixels[dst..dst + w as usize].copy_from_slice(&coverage[src..src + w as usize]);
            }
            glyphs.insert(
                ch,
                GlyphInfo {
                    uv_min: [
                        cursor_x as f32 / ATLAS_SIZE as f32,
                        cursor_y as f32 / ATLAS_SIZE as f32,
                    ],
                    uv_max: [
                        (cursor_x + w) as f32 / ATLAS_SIZE as f32,
                        (cursor_y + h) as f32 / ATLAS_SIZE as f32,
                    ],
                    size_px: [w as f32, h as f32],
                    // fontdue metrics are y-up relative to the baseline; convert
                    // to a y-down "top offset from baseline".
                    offset: [metrics.xmin as f32, -(metrics.ymin as f32 + h as f32)],
                    advance: metrics.advance_width,
                },
            );
            cursor_x += w + PADDING;
            row_height = row_height.max(h);
        }

        let line_metrics = font.horizontal_line_metrics(px_size);
        let (ascent_px, descent_px, line_height_px) = match line_metrics {
            Some(m) => (m.ascent, m.descent, m.new_line_size),
            None => (px_size * 0.8, -px_size * 0.2, px_size * 1.2),
        };

        // Centre of the reserved block, so bilinear filtering can't bleed a
        // neighbouring glyph into a solid rect.
        let white = (WHITE_BLOCK as f32 * 0.5) / ATLAS_SIZE as f32;

        Some(FontAtlas {
            pixels,
            glyphs,
            white_uv: [white, white],
            ascent_px,
            descent_px,
            line_height_px,
        })
    }

    // Advance width of `text` in pixels at the baked size.
    pub fn measure(&self, text: &str) -> f32 {
        text.chars()
            .filter_map(|c| self.glyphs.get(&c))
            .map(|g| g.advance)
            .sum()
    }
}
