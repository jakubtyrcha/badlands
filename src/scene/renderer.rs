// Minimal forward-pass port of sampo src/rendering/scene_renderer.{hpp,cpp}:
// owns the RGBA16Float HDR accumulation target + reversed-Z Depth32Float
// buffer, draws the ground quad and the building cubes (following
// rendering/passes/render_textured_mesh.cpp), then resolves to the swapchain
// with the tonemapping node on the processing graph.

use glam::{Mat4, Vec3, Vec4};

use crate::game::ground_texture;
use crate::game::world::World;
use crate::gpu::frame::FrameContext;
use crate::gpu::graph::{FullscreenNode, ProcessingGraph};
use crate::gpu::pipelines::{
    DEPTH_REVERSED_Z, PipelineGenerator, RenderPipelineDeclaration, VertexLayoutKind,
    create_bind_group,
};
use crate::scene::mesh;

pub const ACCUMULATION_FORMAT: wgpu::TextureFormat = wgpu::TextureFormat::Rgba16Float;

// Port of ObjectUniforms (sampo rendering/components/transform.hpp), extended
// with a per-building color; bound at @group(1) @binding(0) with a dynamic
// offset from FrameContext::allocate_dynamic_uniform.
#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
struct ObjectUniforms {
    model_matrix: Mat4, // LocalSpace -> WorldCameraOffsetedSpace
    color: Vec4,
}

pub struct SceneRenderer {
    hdr_view: wgpu::TextureView,
    depth_view: wgpu::TextureView,
    width: u32,
    height: u32,

    ground_vertex_buffer: wgpu::Buffer,
    ground_vertex_count: u32,
    cube_vertex_buffer: wgpu::Buffer,
    cube_vertex_count: u32,
    ground_texture_view: wgpu::TextureView,
    ground_sampler: wgpu::Sampler,
}

impl SceneRenderer {
    pub fn new(device: &wgpu::Device, queue: &wgpu::Queue, world: &World, width: u32, height: u32) -> SceneRenderer {
        let (hdr_view, depth_view) = create_targets(device, width, height);

        let ground = mesh::build_ground_quad(world.ground_half_extent, world.ground_half_extent / 4.0);
        let cube = mesh::build_unit_cube();
        let ground_texture_view = ground_texture::create(device, queue);

        let ground_sampler = device.create_sampler(&wgpu::SamplerDescriptor {
            label: Some("ground sampler"),
            address_mode_u: wgpu::AddressMode::Repeat,
            address_mode_v: wgpu::AddressMode::Repeat,
            mag_filter: wgpu::FilterMode::Linear,
            min_filter: wgpu::FilterMode::Linear,
            mipmap_filter: wgpu::MipmapFilterMode::Linear,
            ..Default::default()
        });

        SceneRenderer {
            hdr_view,
            depth_view,
            width,
            height,
            ground_vertex_count: ground.vertex_count,
            ground_vertex_buffer: ground.upload(device, "ground vertices"),
            cube_vertex_count: cube.vertex_count,
            cube_vertex_buffer: cube.upload(device, "cube vertices"),
            ground_texture_view,
            ground_sampler,
        }
    }

    pub fn resize(&mut self, device: &wgpu::Device, width: u32, height: u32) {
        if width == self.width && height == self.height {
            return;
        }
        let (hdr_view, depth_view) = create_targets(device, width, height);
        self.hdr_view = hdr_view;
        self.depth_view = depth_view;
        self.width = width;
        self.height = height;
    }

    // The forward scene pass into the HDR accumulation target.
    pub fn render(
        &self,
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        frame: &mut FrameContext,
        pipelines: &mut PipelineGenerator,
        world: &World,
        camera_world_pos: Vec3,
    ) {
        let ground_pipeline = pipelines.get_render_pipeline(
            device,
            &RenderPipelineDeclaration {
                shader_path: "game/ground".to_string(),
                vertex_layout: VertexLayoutKind::TexturedMesh,
                cull_mode: Some(wgpu::Face::Back),
                depth: Some(DEPTH_REVERSED_Z),
                targets: vec![ACCUMULATION_FORMAT],
                ..Default::default()
            },
        );
        let building_pipeline = pipelines.get_render_pipeline(
            device,
            &RenderPipelineDeclaration {
                shader_path: "game/building".to_string(),
                vertex_layout: VertexLayoutKind::TexturedMesh,
                cull_mode: Some(wgpu::Face::Back),
                depth: Some(DEPTH_REVERSED_Z),
                targets: vec![ACCUMULATION_FORMAT],
                ..Default::default()
            },
        );

        let ground_bind_group = create_bind_group(
            device,
            &ground_pipeline,
            0,
            "ground group 0",
            vec![
                frame.frame_uniform_buffer.as_entire_binding(),
                wgpu::BindingResource::TextureView(&self.ground_texture_view),
                wgpu::BindingResource::Sampler(&self.ground_sampler),
            ],
        );
        let building_frame_group = create_bind_group(
            device,
            &building_pipeline,
            0,
            "building group 0",
            vec![frame.frame_uniform_buffer.as_entire_binding()],
        );
        // Group 1: the shared dynamic uniform buffer, one 256-aligned slot per
        // building (render_textured_mesh.cpp BindPerObject pattern).
        let building_object_group = device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("building group 1"),
            layout: &building_pipeline.bind_group_layouts[1],
            entries: &[wgpu::BindGroupEntry {
                binding: 0,
                resource: wgpu::BindingResource::Buffer(wgpu::BufferBinding {
                    buffer: &frame.dynamic_uniform_buffer,
                    offset: 0,
                    size: wgpu::BufferSize::new(std::mem::size_of::<ObjectUniforms>() as u64),
                }),
            }],
        });

        // Per-object uniforms: model translation rebased to camera-offset
        // space (world_pos - camera_world_pos), as in render_textured_mesh.cpp.
        let mut building_offsets = Vec::with_capacity(world.buildings.len());
        for building in &world.buildings {
            let world_pos = Vec3::new(building.pos.x, 0.0, building.pos.y);
            let model = Mat4::from_translation(world_pos - camera_world_pos)
                * Mat4::from_scale(building.size);
            let uniforms = ObjectUniforms {
                model_matrix: model,
                color: building.color.extend(1.0),
            };
            building_offsets
                .push(frame.allocate_dynamic_uniform(queue, bytemuck::bytes_of(&uniforms)));
        }

        let mut pass = frame.encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
            label: Some("scene forward pass"),
            color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                view: &self.hdr_view,
                resolve_target: None,
                ops: wgpu::Operations {
                    load: wgpu::LoadOp::Clear(wgpu::Color {
                        r: 0.005,
                        g: 0.006,
                        b: 0.008,
                        a: 1.0,
                    }),
                    store: wgpu::StoreOp::Store,
                },
                depth_slice: None,
            })],
            depth_stencil_attachment: Some(wgpu::RenderPassDepthStencilAttachment {
                view: &self.depth_view,
                depth_ops: Some(wgpu::Operations {
                    // Reversed-Z: clear to 0.0 (far), test Greater.
                    load: wgpu::LoadOp::Clear(0.0),
                    store: wgpu::StoreOp::Store,
                }),
                stencil_ops: None,
            }),
            timestamp_writes: None,
            occlusion_query_set: None,
            multiview_mask: None,
        });

        pass.set_pipeline(&ground_pipeline.pipeline);
        pass.set_bind_group(0, &ground_bind_group, &[]);
        pass.set_vertex_buffer(0, self.ground_vertex_buffer.slice(..));
        pass.draw(0..self.ground_vertex_count, 0..1);

        pass.set_pipeline(&building_pipeline.pipeline);
        pass.set_bind_group(0, &building_frame_group, &[]);
        pass.set_vertex_buffer(0, self.cube_vertex_buffer.slice(..));
        for offset in &building_offsets {
            pass.set_bind_group(1, &building_object_group, &[*offset]);
            pass.draw(0..self.cube_vertex_count, 0..1);
        }
    }

    // Queue the tonemapping resolve (port of TonemappingNode) onto the graph.
    pub fn add_tonemap_node(
        &self,
        graph: &mut ProcessingGraph,
        target: wgpu::TextureView,
        target_format: wgpu::TextureFormat,
    ) {
        graph.add_fullscreen_node(FullscreenNode {
            shader_path: "passes/tonemapping".to_string(),
            inputs: vec![self.hdr_view.clone()],
            target,
            target_format,
            clear: Some(wgpu::Color::BLACK),
        });
    }
}

// Port of ColorRenderTarget (color_render_target.cpp): RGBA16Float color +
// Depth32Float depth of matching size.
fn create_targets(
    device: &wgpu::Device,
    width: u32,
    height: u32,
) -> (wgpu::TextureView, wgpu::TextureView) {
    let size = wgpu::Extent3d {
        width: width.max(1),
        height: height.max(1),
        depth_or_array_layers: 1,
    };
    let hdr = device.create_texture(&wgpu::TextureDescriptor {
        label: Some("hdr accumulation"),
        size,
        mip_level_count: 1,
        sample_count: 1,
        dimension: wgpu::TextureDimension::D2,
        format: ACCUMULATION_FORMAT,
        usage: wgpu::TextureUsages::RENDER_ATTACHMENT | wgpu::TextureUsages::TEXTURE_BINDING,
        view_formats: &[],
    });
    let depth = device.create_texture(&wgpu::TextureDescriptor {
        label: Some("scene depth"),
        size,
        mip_level_count: 1,
        sample_count: 1,
        dimension: wgpu::TextureDimension::D2,
        format: wgpu::TextureFormat::Depth32Float,
        usage: wgpu::TextureUsages::RENDER_ATTACHMENT,
        view_formats: &[],
    });
    (
        hdr.create_view(&wgpu::TextureViewDescriptor::default()),
        depth.create_view(&wgpu::TextureViewDescriptor::default()),
    )
}
