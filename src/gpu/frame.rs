// Port of weave-renderer src/rendering/context/frame_context.{hpp,cpp}
// (class FrameContext): per-frame command encoder, the frame uniform buffer
// (@group(0) @binding(0) in common/frame.wesl) and the dynamic uniform buffer
// used for per-object data via dynamic offsets (AllocateDynamicUniform).

use wgpu::util::DeviceExt;

use crate::scene::camera::UniformData;

pub const DYNAMIC_UNIFORM_ALIGNMENT: u64 = 256;
// Port of FrameContext::ReserveDynamicUniform — sized up front so the buffer
// never reallocates mid-frame (which would invalidate bind groups). At 256
// bytes/slot this holds 4096 objects/frame; every sim building, poppable,
// character, and the placement ghost each take a slot, so the ceiling has to
// clear a busy late-game map (a truly unbounded map needs instanced draws).
const DYNAMIC_UNIFORM_CAPACITY: u64 = 1024 * 1024;

pub struct FrameContext {
    pub encoder: wgpu::CommandEncoder,
    pub frame_uniform_buffer: wgpu::Buffer,
    pub dynamic_uniform_buffer: wgpu::Buffer,
    dynamic_cursor: u64,
}

impl FrameContext {
    // Port of GpuContext::BeginFrame(UniformData).
    pub fn begin(device: &wgpu::Device, uniforms: &UniformData) -> FrameContext {
        let frame_uniform_buffer = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
            label: Some("frame uniforms"),
            contents: bytemuck::bytes_of(uniforms),
            usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
        });
        let dynamic_uniform_buffer = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("dynamic uniforms"),
            size: DYNAMIC_UNIFORM_CAPACITY,
            usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        let encoder = device.create_command_encoder(&wgpu::CommandEncoderDescriptor {
            label: Some("frame encoder"),
        });
        FrameContext {
            encoder,
            frame_uniform_buffer,
            dynamic_uniform_buffer,
            dynamic_cursor: 0,
        }
    }

    // Port of FrameContext::AllocateDynamicUniform(size, data) -> offset.
    // Returns the dynamic offset to pass to set_bind_group.
    pub fn allocate_dynamic_uniform(&mut self, queue: &wgpu::Queue, data: &[u8]) -> u32 {
        let offset = self.dynamic_cursor;
        assert!(
            offset + data.len() as u64 <= DYNAMIC_UNIFORM_CAPACITY,
            "dynamic uniform buffer exhausted"
        );
        queue.write_buffer(&self.dynamic_uniform_buffer, offset, data);
        self.dynamic_cursor =
            (offset + data.len() as u64).div_ceil(DYNAMIC_UNIFORM_ALIGNMENT) * DYNAMIC_UNIFORM_ALIGNMENT;
        offset as u32
    }

    // Port of GpuContext::SubmitFrame(frame): End() + queue submit.
    pub fn submit(self, queue: &wgpu::Queue) {
        queue.submit([self.encoder.finish()]);
    }
}
