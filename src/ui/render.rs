// Immediate-mode UI renderer: rects + text quads in physical pixels, drawn in
// one alpha-blended pass with shaders/ui/ui.wesl after the tonemap resolve.

use wgpu::util::DeviceExt;

use crate::gpu::frame::FrameContext;
use crate::gpu::pipelines::{
    PipelineGenerator, RenderPipelineDeclaration, VertexLayoutKind, create_bind_group,
};
use crate::ui::font::FontAtlas;

const FLOATS_PER_VERTEX: usize = 8; // pos(2) + uv(2) + color(4)

pub struct UiRenderer {
    pub atlas: FontAtlas,
    sampler: wgpu::Sampler,
    vertices: Vec<f32>,
}

impl UiRenderer {
    pub fn new(device: &wgpu::Device, queue: &wgpu::Queue, scale_factor: f32) -> UiRenderer {
        let atlas = FontAtlas::bake(device, queue, 20.0 * scale_factor);
        let sampler = device.create_sampler(&wgpu::SamplerDescriptor {
            label: Some("ui sampler"),
            mag_filter: wgpu::FilterMode::Linear,
            min_filter: wgpu::FilterMode::Linear,
            ..Default::default()
        });
        UiRenderer {
            atlas,
            sampler,
            vertices: Vec::new(),
        }
    }

    pub fn begin(&mut self) {
        self.vertices.clear();
    }

    fn push_quad(&mut self, min: [f32; 2], max: [f32; 2], uv_min: [f32; 2], uv_max: [f32; 2], color: [f32; 4]) {
        let corners = [
            ([min[0], min[1]], [uv_min[0], uv_min[1]]),
            ([min[0], max[1]], [uv_min[0], uv_max[1]]),
            ([max[0], max[1]], [uv_max[0], uv_max[1]]),
            ([max[0], min[1]], [uv_max[0], uv_min[1]]),
        ];
        for &index in &[0usize, 1, 2, 0, 2, 3] {
            let (pos, uv) = corners[index];
            self.vertices.extend_from_slice(&[
                pos[0], pos[1], uv[0], uv[1], color[0], color[1], color[2], color[3],
            ]);
        }
    }

    pub fn push_rect(&mut self, rect: &panes::Rect, color: [f32; 4]) {
        let white = self.atlas.white_uv;
        self.push_quad(
            [rect.x, rect.y],
            [rect.x + rect.w, rect.y + rect.h],
            white,
            white,
            color,
        );
    }

    // Draws `text` with the pen starting at `x`, baseline at `baseline_y`.
    pub fn push_text(&mut self, x: f32, baseline_y: f32, text: &str, color: [f32; 4]) {
        let mut pen_x = x;
        for ch in text.chars() {
            let Some(glyph) = self.atlas.glyphs.get(&ch).copied() else {
                continue;
            };
            if glyph.size_px[0] > 0.0 {
                let min = [
                    (pen_x + glyph.offset[0]).round(),
                    (baseline_y + glyph.offset[1]).round(),
                ];
                let max = [min[0] + glyph.size_px[0], min[1] + glyph.size_px[1]];
                self.push_quad(min, max, glyph.uv_min, glyph.uv_max, color);
            }
            pen_x += glyph.advance;
        }
    }

    pub fn draw(
        &self,
        device: &wgpu::Device,
        frame: &mut FrameContext,
        pipelines: &mut PipelineGenerator,
        target: &wgpu::TextureView,
        target_format: wgpu::TextureFormat,
    ) {
        if self.vertices.is_empty() {
            return;
        }
        let compiled = pipelines.get_render_pipeline(
            device,
            &RenderPipelineDeclaration {
                shader_path: "ui/ui".to_string(),
                vertex_layout: VertexLayoutKind::Ui,
                blend: Some(wgpu::BlendState::ALPHA_BLENDING),
                targets: vec![target_format],
                ..Default::default()
            },
        );
        let bind_group = create_bind_group(
            device,
            &compiled,
            0,
            "ui group 0",
            vec![
                frame.frame_uniform_buffer.as_entire_binding(),
                wgpu::BindingResource::TextureView(&self.atlas.texture_view),
                wgpu::BindingResource::Sampler(&self.sampler),
            ],
        );
        let vertex_buffer = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
            label: Some("ui vertices"),
            contents: bytemuck::cast_slice(&self.vertices),
            usage: wgpu::BufferUsages::VERTEX,
        });

        let mut pass = frame.encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
            label: Some("ui pass"),
            color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                view: target,
                resolve_target: None,
                ops: wgpu::Operations {
                    load: wgpu::LoadOp::Load,
                    store: wgpu::StoreOp::Store,
                },
                depth_slice: None,
            })],
            depth_stencil_attachment: None,
            timestamp_writes: None,
            occlusion_query_set: None,
            multiview_mask: None,
        });
        pass.set_pipeline(&compiled.pipeline);
        pass.set_bind_group(0, &bind_group, &[]);
        pass.set_vertex_buffer(0, vertex_buffer.slice(..));
        pass.draw(0..(self.vertices.len() / FLOATS_PER_VERTEX) as u32, 0..1);
    }
}
