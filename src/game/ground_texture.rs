// Procedural ground texture: a 256x256 RGBA8 blend of two grass tones with
// dirt patches, generated from a small hash-based value noise (no noise crate).

const SIZE: u32 = 256;

fn hash(x: u32, y: u32, seed: u32) -> f32 {
    let mut h = x.wrapping_mul(374761393) ^ y.wrapping_mul(668265263) ^ seed.wrapping_mul(2246822519);
    h = (h ^ (h >> 13)).wrapping_mul(1274126177);
    h ^= h >> 16;
    (h & 0xffff) as f32 / 65535.0
}

fn smoothstep(t: f32) -> f32 {
    t * t * (3.0 - 2.0 * t)
}

// Value noise with bilinear-smooth interpolation, tileable over `cells`.
fn value_noise(u: f32, v: f32, cells: u32, seed: u32) -> f32 {
    let x = u * cells as f32;
    let y = v * cells as f32;
    let x0 = x.floor() as u32 % cells;
    let y0 = y.floor() as u32 % cells;
    let x1 = (x0 + 1) % cells;
    let y1 = (y0 + 1) % cells;
    let fx = smoothstep(x.fract());
    let fy = smoothstep(y.fract());
    let a = hash(x0, y0, seed);
    let b = hash(x1, y0, seed);
    let c = hash(x0, y1, seed);
    let d = hash(x1, y1, seed);
    a + (b - a) * fx + (c - a) * fy + (a - b - c + d) * fx * fy
}

pub fn create(device: &wgpu::Device, queue: &wgpu::Queue) -> wgpu::TextureView {
    let mut pixels = Vec::with_capacity((SIZE * SIZE * 4) as usize);
    // sRGB-ish tones; the texture is created as Rgba8UnormSrgb so sampling
    // yields linear values for lighting.
    let grass_dark = [0.24f32, 0.36, 0.16];
    let grass_light = [0.38f32, 0.48, 0.22];
    let dirt = [0.42f32, 0.34, 0.22];

    for y in 0..SIZE {
        for x in 0..SIZE {
            let u = x as f32 / SIZE as f32;
            let v = y as f32 / SIZE as f32;
            // Two octaves of tileable value noise for the grass blend.
            let g = 0.65 * value_noise(u, v, 8, 11) + 0.35 * value_noise(u, v, 32, 23);
            // Sparse dirt patches from a low-frequency octave.
            let d = value_noise(u, v, 5, 47);
            let dirt_mix = ((d - 0.62) * 6.0).clamp(0.0, 1.0);
            let fine = 0.9 + 0.2 * value_noise(u, v, 64, 91);

            let mut rgb = [0.0f32; 3];
            for c in 0..3 {
                let grass = grass_dark[c] + (grass_light[c] - grass_dark[c]) * g;
                rgb[c] = ((grass + (dirt[c] - grass) * dirt_mix) * fine).clamp(0.0, 1.0);
            }
            pixels.extend_from_slice(&[
                (rgb[0] * 255.0) as u8,
                (rgb[1] * 255.0) as u8,
                (rgb[2] * 255.0) as u8,
                255,
            ]);
        }
    }

    let texture = device.create_texture(&wgpu::TextureDescriptor {
        label: Some("ground texture"),
        size: wgpu::Extent3d {
            width: SIZE,
            height: SIZE,
            depth_or_array_layers: 1,
        },
        mip_level_count: 1,
        sample_count: 1,
        dimension: wgpu::TextureDimension::D2,
        format: wgpu::TextureFormat::Rgba8UnormSrgb,
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
            bytes_per_row: Some(SIZE * 4),
            rows_per_image: Some(SIZE),
        },
        wgpu::Extent3d {
            width: SIZE,
            height: SIZE,
            depth_or_array_layers: 1,
        },
    );
    texture.create_view(&wgpu::TextureViewDescriptor::default())
}
