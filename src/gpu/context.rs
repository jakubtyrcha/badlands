// Port of weave-renderer src/rendering/gpu_context.{hpp,cpp} (class GpuContext).
// Owns the wgpu instance/adapter/device/queue/surface and the surface format
// selection. Prefers an RGBA16Float (HDR, linear) surface when available,
// matching GpuContextOptions::prefer_hdr_surface + GetPreferredFormat().

use std::sync::Arc;

use winit::window::Window;

pub struct GpuContext {
    #[allow(dead_code)] // kept alive alongside the surface, as in the C++ GpuContext
    pub instance: wgpu::Instance,
    pub adapter: wgpu::Adapter,
    pub device: wgpu::Device,
    pub queue: wgpu::Queue,
    pub surface: wgpu::Surface<'static>,
    pub surface_format: wgpu::TextureFormat,
    pub width: u32,
    pub height: u32,
}

impl GpuContext {
    pub fn new(window: Arc<Window>) -> GpuContext {
        let size = window.inner_size();
        let instance =
            wgpu::Instance::new(wgpu::InstanceDescriptor::new_without_display_handle_from_env());
        let surface = instance
            .create_surface(window)
            .expect("failed to create surface");

        let adapter = pollster::block_on(instance.request_adapter(&wgpu::RequestAdapterOptions {
            power_preference: wgpu::PowerPreference::HighPerformance,
            compatible_surface: Some(&surface),
            ..Default::default()
        }))
        .expect("no suitable GPU adapter");

        let (device, queue) = pollster::block_on(adapter.request_device(&wgpu::DeviceDescriptor {
            label: Some("badlands device"),
            ..Default::default()
        }))
        .expect("failed to create device");

        let surface_format = Self::preferred_format(&surface, &adapter);
        log::info!("surface format: {surface_format:?}");

        let mut ctx = GpuContext {
            instance,
            adapter,
            device,
            queue,
            surface,
            surface_format,
            width: size.width.max(1),
            height: size.height.max(1),
        };
        ctx.configure_surface(ctx.width, ctx.height);
        ctx
    }

    // Port of GpuContext::GetPreferredFormat(): RGBA16Float (HDR) when the
    // surface supports it, otherwise an 8-bit sRGB format.
    fn preferred_format(
        surface: &wgpu::Surface<'_>,
        adapter: &wgpu::Adapter,
    ) -> wgpu::TextureFormat {
        let caps = surface.get_capabilities(adapter);
        if caps
            .formats
            .contains(&wgpu::TextureFormat::Rgba16Float)
        {
            return wgpu::TextureFormat::Rgba16Float;
        }
        // Non-sRGB on purpose: with output_is_linear == 0 the tonemap and UI
        // shaders sRGB-encode manually (tonemapping.wesl), so an -Srgb target
        // would encode twice. Matches the C++ BGRA8Unorm default.
        if caps.formats.contains(&wgpu::TextureFormat::Bgra8Unorm) {
            return wgpu::TextureFormat::Bgra8Unorm;
        }
        caps.formats[0]
    }

    // 1 = linear (RGBA16Float) output, 0 = sRGB — feeds UniformData::output_is_linear.
    pub fn output_is_linear(&self) -> bool {
        self.surface_format == wgpu::TextureFormat::Rgba16Float
    }

    // Port of GpuContext::ConfigureSurface().
    pub fn configure_surface(&mut self, width: u32, height: u32) {
        self.width = width.max(1);
        self.height = height.max(1);
        let caps = self.surface.get_capabilities(&self.adapter);
        self.surface.configure(
            &self.device,
            &wgpu::SurfaceConfiguration {
                usage: wgpu::TextureUsages::RENDER_ATTACHMENT,
                format: self.surface_format,
                // Auto = ExtendedSrgbLinear for Rgba16Float (HDR), sRGB otherwise.
                color_space: wgpu::SurfaceColorSpace::Auto,
                width: self.width,
                height: self.height,
                present_mode: wgpu::PresentMode::Fifo,
                alpha_mode: caps.alpha_modes[0],
                view_formats: vec![],
                desired_maximum_frame_latency: 2,
            },
        );
    }

    // Port of GpuContext::AcquireSwapchainTexture(). Outdated/Lost surfaces
    // are reconfigured so the next acquire can recover; Occluded/Timeout just
    // skip the present.
    pub fn acquire_swapchain_texture(
        &mut self,
    ) -> Option<(wgpu::SurfaceTexture, wgpu::TextureView)> {
        match self.surface.get_current_texture() {
            wgpu::CurrentSurfaceTexture::Success(tex)
            | wgpu::CurrentSurfaceTexture::Suboptimal(tex) => {
                let view = tex
                    .texture
                    .create_view(&wgpu::TextureViewDescriptor::default());
                Some((tex, view))
            }
            status @ (wgpu::CurrentSurfaceTexture::Outdated
            | wgpu::CurrentSurfaceTexture::Lost) => {
                log::warn!("swapchain {status:?}; reconfiguring surface");
                self.configure_surface(self.width, self.height);
                None
            }
            status => {
                log::warn!("failed to acquire swapchain texture: {status:?}");
                None
            }
        }
    }
}
