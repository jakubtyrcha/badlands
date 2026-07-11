// Minimal port of weave-renderer src/image_processing/processing_graph.{hpp,cpp}
// + tasks/fullscreen_shader_node.*: a list of fullscreen shader nodes executed
// in insertion order against imported texture views. The MVP only uses it for
// the tonemapping resolve (tasks/tonemapping_node.*), but this keeps the
// "graph rendering" structure to grow into (blur/readback/compute nodes).

use crate::gpu::frame::FrameContext;
use crate::gpu::pipelines::{
    PipelineGenerator, RenderPipelineDeclaration, VertexLayoutKind, create_bind_group,
};

pub struct FullscreenNode {
    pub shader_path: String,
    // Bound to @group(0) @binding(1..) in declaration order; binding 0 is the
    // frame uniform buffer (common/frame.wesl).
    pub inputs: Vec<wgpu::TextureView>,
    pub target: wgpu::TextureView,
    pub target_format: wgpu::TextureFormat,
    pub clear: Option<wgpu::Color>, // None = load existing contents
}

#[derive(Default)]
pub struct ProcessingGraph {
    nodes: Vec<FullscreenNode>,
}

impl ProcessingGraph {
    // Port of ProcessingGraph::AddTask (fullscreen variant).
    pub fn add_fullscreen_node(&mut self, node: FullscreenNode) {
        self.nodes.push(node);
    }

    // Port of ProcessingGraph::RecordInto(encoder): executes nodes in order and
    // drains the graph.
    pub fn execute(
        &mut self,
        device: &wgpu::Device,
        frame: &mut FrameContext,
        pipelines: &mut PipelineGenerator,
    ) {
        for node in self.nodes.drain(..) {
            let compiled = pipelines.get_render_pipeline(
                device,
                &RenderPipelineDeclaration {
                    shader_path: node.shader_path.clone(),
                    vertex_layout: VertexLayoutKind::None,
                    targets: vec![node.target_format],
                    ..Default::default()
                },
            );

            let mut resources = vec![frame.frame_uniform_buffer.as_entire_binding()];
            for input in &node.inputs {
                resources.push(wgpu::BindingResource::TextureView(input));
            }
            let bind_group =
                create_bind_group(device, &compiled, 0, &node.shader_path, resources);

            let mut pass = frame.encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
                label: Some(&node.shader_path),
                color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                    view: &node.target,
                    resolve_target: None,
                    ops: wgpu::Operations {
                        load: match node.clear {
                            Some(color) => wgpu::LoadOp::Clear(color),
                            None => wgpu::LoadOp::Load,
                        },
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
            pass.draw(0..3, 0..1);
        }
    }
}
