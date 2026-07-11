// Glyph atlas baked at startup with fontdue: printable ASCII rasterized into
// one R8Unorm texture. A small white block is reserved at the origin so solid
// rects can share the same pipeline as text.

use std::collections::HashMap;
use std::path::{Path, PathBuf};

const ATLAS_SIZE: u32 = 512;
const PADDING: u32 = 1;

#[derive(Clone, Copy)]
pub struct GlyphInfo {
    pub uv_min: [f32; 2],
    pub uv_max: [f32; 2],
    pub size_px: [f32; 2],
    // Offset from the pen position: x from pen, y down from the baseline to
    // the glyph top (screen coordinates, y-down).
    pub offset: [f32; 2],
    pub advance: f32,
}

pub struct FontAtlas {
    pub texture_view: wgpu::TextureView,
    pub glyphs: HashMap<char, GlyphInfo>,
    pub white_uv: [f32; 2],
    pub ascent_px: f32,
    pub descent_px: f32,
}

impl FontAtlas {
    pub fn bake(device: &wgpu::Device, queue: &wgpu::Queue, size_px: f32) -> FontAtlas {
        let font_path = find_asset_dir().join("fonts/Inter-Variable.ttf");
        let data = std::fs::read(&font_path)
            .unwrap_or_else(|err| panic!("failed to read {}: {err}", font_path.display()));
        let font = fontdue::Font::from_bytes(data, fontdue::FontSettings::default())
            .expect("failed to parse font");

        let mut pixels = vec![0u8; (ATLAS_SIZE * ATLAS_SIZE) as usize];
        // Reserved 4x4 white block at the origin for solid rects.
        for y in 0..4u32 {
            for x in 0..4u32 {
                pixels[(y * ATLAS_SIZE + x) as usize] = 255;
            }
        }
        let mut cursor_x: u32 = 4 + PADDING;
        let mut cursor_y: u32 = 0;
        let mut row_height: u32 = 4;

        let mut glyphs = HashMap::new();
        for ch in (32u8..127).map(char::from) {
            let (metrics, coverage) = font.rasterize(ch, size_px);
            let w = metrics.width as u32;
            let h = metrics.height as u32;
            if cursor_x + w + PADDING > ATLAS_SIZE {
                cursor_x = 0;
                cursor_y += row_height + PADDING;
                row_height = 0;
            }
            assert!(cursor_y + h <= ATLAS_SIZE, "glyph atlas overflow");
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
                    // fontdue metrics are y-up relative to the baseline;
                    // convert to y-down "top offset from baseline".
                    offset: [
                        metrics.xmin as f32,
                        -(metrics.ymin as f32 + h as f32),
                    ],
                    advance: metrics.advance_width,
                },
            );
            cursor_x += w + PADDING;
            row_height = row_height.max(h);
        }

        let texture = device.create_texture(&wgpu::TextureDescriptor {
            label: Some("ui glyph atlas"),
            size: wgpu::Extent3d {
                width: ATLAS_SIZE,
                height: ATLAS_SIZE,
                depth_or_array_layers: 1,
            },
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            format: wgpu::TextureFormat::R8Unorm,
            usage: wgpu::TextureUsages::TEXTURE_BINDING | wgpu::TextureUsages::COPY_DST,
            view_formats: &[],
        });
        queue.write_texture(
            wgpu::TexelCopyTextureInfo {
                texture: &texture,
                mip_level: 0,
                origin: wgpu::Origin3d::ZERO,
                aspect: wgpu::TextureAspect::All,
            },
            &pixels,
            wgpu::TexelCopyBufferLayout {
                offset: 0,
                bytes_per_row: Some(ATLAS_SIZE),
                rows_per_image: Some(ATLAS_SIZE),
            },
            wgpu::Extent3d {
                width: ATLAS_SIZE,
                height: ATLAS_SIZE,
                depth_or_array_layers: 1,
            },
        );

        let line_metrics = font
            .horizontal_line_metrics(size_px)
            .expect("font has no horizontal metrics");

        FontAtlas {
            texture_view: texture.create_view(&wgpu::TextureViewDescriptor::default()),
            glyphs,
            white_uv: [1.0 / ATLAS_SIZE as f32, 1.0 / ATLAS_SIZE as f32],
            ascent_px: line_metrics.ascent,
            descent_px: line_metrics.descent,
        }
    }
}

// Same walk-up strategy as the shader directory: exe-adjacent first, then the
// source tree.
fn find_asset_dir() -> PathBuf {
    let marker = Path::new("fonts/Inter-Variable.ttf");
    let mut candidates: Vec<PathBuf> = Vec::new();
    if let Ok(exe) = std::env::current_exe() {
        let mut dir = exe.parent().map(Path::to_path_buf);
        while let Some(d) = dir {
            candidates.push(d.join("assets"));
            dir = d.parent().map(Path::to_path_buf);
        }
    }
    candidates.push(PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("assets"));
    candidates.push(PathBuf::from("assets"));
    for candidate in candidates {
        if candidate.join(marker).is_file() {
            return candidate;
        }
    }
    panic!("asset directory not found (marker assets/fonts/Inter-Variable.ttf)");
}
