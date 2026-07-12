// App shell: port of the SdlViewerApp main loop from weave-renderer
// (apps/src/app-framework/sdl_viewer_app.cpp) on winit — window creation,
// event handling (press-drag pan, Shift+S shader reload), and per-frame
// composition: scene forward pass -> tonemap (graph) -> UI pass -> present.

use std::path::PathBuf;
use std::sync::Arc;
use std::time::Instant;

use glam::{Vec2, Vec4};
use winit::application::ApplicationHandler;
use winit::dpi::LogicalSize;
use winit::event::{ElementState, MouseButton, WindowEvent};
use winit::event_loop::ActiveEventLoop;
use winit::keyboard::Key;
use winit::window::{Window, WindowId};

use crate::game::top_down_camera::TopDownCamera;
use crate::game::world::World;
use crate::game_ffi::Game;
use crate::gpu::context::GpuContext;
use crate::gpu::frame::FrameContext;
use crate::gpu::graph::ProcessingGraph;
use crate::gpu::pipelines::PipelineGenerator;
use crate::scene::camera::UniformData;
use crate::scene::renderer::SceneRenderer;
use crate::ui::layout::{UiLayout, compute as compute_ui_layout};
use crate::ui::render::UiRenderer;

#[derive(Default, Clone)]
pub struct RunConfig {
    pub max_frames: Option<u32>,
    pub screenshot_path: Option<PathBuf>,
}

const TICK_DT: f32 = 1.0 / 30.0;

const BRAIN_SCRIPT_MARKER: &str = "brains/warrior.noiser";

// The brain script is optional: the game runs mock-brains-only without it.
fn find_brain_script() -> Option<PathBuf> {
    crate::assets::find_asset_dir("scripts", BRAIN_SCRIPT_MARKER)
        .map(|dir| dir.join(BRAIN_SCRIPT_MARKER))
}

pub struct App {
    config: RunConfig,
    state: Option<GameState>,
}

impl App {
    pub fn new(config: RunConfig) -> App {
        App {
            config,
            state: None,
        }
    }
}

struct GameState {
    window: Arc<Window>,
    gpu: GpuContext,
    pipelines: PipelineGenerator,
    graph: ProcessingGraph,
    scene: SceneRenderer,
    ui: UiRenderer,
    world: World,
    camera: TopDownCamera,
    sim: Game,
    brain_script_path: Option<PathBuf>,

    dragging: bool,
    cursor: Vec2,
    shift_down: bool,
    frame_count: u32,
    last_tick: Instant,
    tick_accumulator: f32,
}

impl GameState {
    fn new(window: Arc<Window>) -> GameState {
        let gpu = GpuContext::new(window.clone());
        let pipelines = PipelineGenerator::new();
        let world = World::new();
        let scene = SceneRenderer::new(&gpu.device, &gpu.queue, &world, gpu.width, gpu.height);
        let ui = UiRenderer::new(&gpu.device, &gpu.queue, window.scale_factor() as f32);

        let brain_script_path = find_brain_script();
        let brain_script = brain_script_path.as_ref().and_then(|path| {
            std::fs::read_to_string(path)
                .inspect_err(|err| log::error!("failed to read {}: {err}", path.display()))
                .ok()
        });
        match &brain_script_path {
            Some(path) => log::info!("brain script: {}", path.display()),
            None => log::warn!("brain script not found; running mock brains only"),
        }
        // The Stage-2 duelists, spawned on a lane clear of the buildings;
        // their stats come from the engine's canonical descriptors.
        let mut sim = Game::new(brain_script.as_deref());
        sim.spawn(&crate::game_ffi::mercenary_desc(-8.0, -12.0));
        sim.spawn(&crate::game_ffi::goblin_desc(8.0, -12.0));

        GameState {
            window,
            gpu,
            pipelines,
            graph: ProcessingGraph::default(),
            scene,
            ui,
            world,
            camera: TopDownCamera::new(),
            sim,
            brain_script_path,
            dragging: false,
            cursor: Vec2::ZERO,
            shift_down: false,
            frame_count: 0,
            last_tick: Instant::now(),
            tick_accumulator: 0.0,
        }
    }

    // Fixed-step simulation: one tick per frame on --frames runs (so
    // screenshots are deterministic), a real-time accumulator otherwise.
    fn step_simulation(&mut self, config: &RunConfig) {
        if config.max_frames.is_some() {
            self.sim.tick(TICK_DT);
            return;
        }
        let now = Instant::now();
        self.tick_accumulator += now.duration_since(self.last_tick).as_secs_f32().min(0.25);
        self.last_tick = now;
        while self.tick_accumulator >= TICK_DT {
            self.sim.tick(TICK_DT);
            self.tick_accumulator -= TICK_DT;
        }
    }

    fn reload_brain_script(&mut self) {
        let Some(path) = &self.brain_script_path else {
            log::warn!("no brain script to reload");
            return;
        };
        match std::fs::read_to_string(path) {
            Ok(source) => {
                if self.sim.reload_script(&source) {
                    log::info!("brain script reloaded");
                } // failure keeps last-good; the C++ side already logged why
            }
            Err(err) => log::error!("failed to read {}: {err}", path.display()),
        }
    }

    fn ui_layout(&self) -> UiLayout {
        compute_ui_layout(
            self.gpu.width as f32,
            self.gpu.height as f32,
            self.window.scale_factor() as f32,
        )
    }

    fn build_uniforms(&self) -> UniformData {
        let aspect = self.gpu.width as f32 / self.gpu.height.max(1) as f32;
        let camera = self.camera.camera(aspect);
        let mut uniforms = UniformData::from_camera(&camera);
        uniforms.sun_dir = Vec4::new(0.45, 1.0, 0.3, 0.0).normalize();
        uniforms.sun_color = Vec4::new(1.0, 0.95, 0.85, 0.0) * 1.15;
        uniforms.screen_size = Vec2::new(self.gpu.width as f32, self.gpu.height as f32);
        uniforms.tonemap_mode = 0;
        uniforms.output_is_linear = u32::from(self.gpu.output_is_linear());
        uniforms
    }

    fn render(&mut self, config: &RunConfig) -> bool {
        self.step_simulation(config);
        let characters = self.sim.state();

        let uniforms = self.build_uniforms();
        let mut frame = FrameContext::begin(&self.gpu.device, &uniforms);

        let aspect = self.gpu.width as f32 / self.gpu.height.max(1) as f32;
        let camera_pos = self.camera.camera(aspect).position;
        self.scene.render(
            &self.gpu.device,
            &self.gpu.queue,
            &mut frame,
            &mut self.pipelines,
            &self.world,
            &characters,
            camera_pos,
        );

        // UI: top bar with the player's gold. Built regardless of surface
        // availability so the screenshot path can draw it too.
        let layout = self.ui_layout();
        let scale = self.window.scale_factor() as f32;
        self.ui.begin();
        self.ui
            .push_rect(&layout.top_bar, [0.09, 0.08, 0.07, 0.92]);
        let baseline = layout.top_bar.y + layout.top_bar.h * 0.5
            + (self.ui.atlas.ascent_px + self.ui.atlas.descent_px) * 0.5;
        self.ui.push_text(
            16.0 * scale,
            baseline,
            &format!("Gold: {}", self.world.gold),
            [0.98, 0.83, 0.36, 1.0],
        );
        let stats = self.sim.stats();
        if stats.noiser_bugs > 0 {
            self.ui.push_text(
                220.0 * scale,
                baseline,
                &format!("noiser bugs: {}", stats.noiser_bugs),
                [0.95, 0.35, 0.30, 1.0],
            );
        }

        // Tonemap resolve HDR -> swapchain via the processing graph, then the
        // UI pass. Skipped (not fatal) while the window is occluded.
        let surface = self.gpu.acquire_swapchain_texture();
        if surface.is_none() {
            // No present means no vsync pacing; don't spin the loop flat-out
            // while the window is occluded.
            std::thread::sleep(std::time::Duration::from_millis(50));
        }
        if let Some((_, surface_view)) = &surface {
            self.scene.add_tonemap_node(
                &mut self.graph,
                surface_view.clone(),
                self.gpu.surface_format,
            );
            self.graph
                .execute(&self.gpu.device, &mut frame, &mut self.pipelines);
            self.ui.draw(
                &self.gpu.device,
                &mut frame,
                &mut self.pipelines,
                surface_view,
                self.gpu.surface_format,
            );
        }

        // Screenshot on the last frame: re-render tonemap + UI into an
        // Rgba8UnormSrgb offscreen target and read it back as PNG.
        let is_last_frame =
            config.max_frames.is_some_and(|max| self.frame_count + 1 >= max);
        let screenshot = if is_last_frame {
            config
                .screenshot_path
                .as_ref()
                .map(|path| (path.clone(), self.prepare_screenshot(&mut frame)))
        } else {
            None
        };

        frame.submit(&self.gpu.queue);
        if let Some((surface_texture, _)) = surface {
            self.gpu.queue.present(surface_texture);
        }
        self.frame_count += 1;

        if let Some((path, pending)) = screenshot {
            pending.save(&self.gpu.device, &path);
        }
        is_last_frame
    }

    fn prepare_screenshot(&mut self, frame: &mut FrameContext) -> PendingScreenshot {
        let (width, height) = (self.gpu.width, self.gpu.height);
        // The shaders' sRGB-encode decision follows output_is_linear (set from
        // the surface format), so the screenshot target must match: shader
        // output is linear -> sRGB view encodes; shader output is already
        // sRGB-encoded -> plain Unorm view stores it as-is.
        let format = if self.gpu.output_is_linear() {
            wgpu::TextureFormat::Rgba8UnormSrgb
        } else {
            wgpu::TextureFormat::Rgba8Unorm
        };
        let texture = self.gpu.device.create_texture(&wgpu::TextureDescriptor {
            label: Some("screenshot target"),
            size: wgpu::Extent3d {
                width,
                height,
                depth_or_array_layers: 1,
            },
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            format,
            usage: wgpu::TextureUsages::RENDER_ATTACHMENT | wgpu::TextureUsages::COPY_SRC,
            view_formats: &[],
        });
        let view = texture.create_view(&wgpu::TextureViewDescriptor::default());

        // Same composition as the swapchain: tonemap + UI.
        self.scene
            .add_tonemap_node(&mut self.graph, view.clone(), format);
        self.graph
            .execute(&self.gpu.device, frame, &mut self.pipelines);
        self.ui.draw(
            &self.gpu.device,
            frame,
            &mut self.pipelines,
            &view,
            format,
        );

        let padded_bytes_per_row = (width * 4).div_ceil(256) * 256;
        let buffer = self.gpu.device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("screenshot readback"),
            size: (padded_bytes_per_row * height) as u64,
            usage: wgpu::BufferUsages::COPY_DST | wgpu::BufferUsages::MAP_READ,
            mapped_at_creation: false,
        });
        frame.encoder.copy_texture_to_buffer(
            wgpu::TexelCopyTextureInfo {
                texture: &texture,
                mip_level: 0,
                origin: wgpu::Origin3d::ZERO,
                aspect: wgpu::TextureAspect::All,
            },
            wgpu::TexelCopyBufferInfo {
                buffer: &buffer,
                layout: wgpu::TexelCopyBufferLayout {
                    offset: 0,
                    bytes_per_row: Some(padded_bytes_per_row),
                    rows_per_image: Some(height),
                },
            },
            wgpu::Extent3d {
                width,
                height,
                depth_or_array_layers: 1,
            },
        );
        PendingScreenshot {
            buffer,
            width,
            height,
            padded_bytes_per_row,
        }
    }

    fn handle_pointer_moved(&mut self, position: Vec2) {
        if self.dragging {
            let delta = position - self.cursor;
            self.camera
                .pan(delta, self.gpu.height as f32, self.world.pan_extent);
        }
        self.cursor = position;
    }

    fn handle_pointer_button(&mut self, state: ElementState) {
        match state {
            ElementState::Pressed => {
                // Drags starting on the top bar belong to the UI, not the map.
                let layout = self.ui_layout();
                let viewport = layout.viewport;
                if self.cursor.y >= viewport.y
                    && self.cursor.y <= viewport.y + viewport.h
                    && self.cursor.x >= viewport.x
                    && self.cursor.x <= viewport.x + viewport.w
                {
                    self.dragging = true;
                }
            }
            ElementState::Released => self.dragging = false,
        }
    }
}

struct PendingScreenshot {
    buffer: wgpu::Buffer,
    width: u32,
    height: u32,
    padded_bytes_per_row: u32,
}

impl PendingScreenshot {
    fn save(self, device: &wgpu::Device, path: &std::path::Path) {
        let slice = self.buffer.slice(..);
        slice.map_async(wgpu::MapMode::Read, |result| {
            result.expect("failed to map screenshot buffer");
        });
        device
            .poll(wgpu::PollType::Wait {
                submission_index: None,
                timeout: None,
            })
            .expect("device poll failed");

        let data = slice.get_mapped_range().expect("mapped range");
        let mut rows = Vec::with_capacity((self.width * self.height * 4) as usize);
        for row in 0..self.height {
            let start = (row * self.padded_bytes_per_row) as usize;
            rows.extend_from_slice(&data[start..start + (self.width * 4) as usize]);
        }
        drop(data);

        let file = std::fs::File::create(path)
            .unwrap_or_else(|err| panic!("failed to create {}: {err}", path.display()));
        let mut encoder = png::Encoder::new(std::io::BufWriter::new(file), self.width, self.height);
        encoder.set_color(png::ColorType::Rgba);
        encoder.set_depth(png::BitDepth::Eight);
        encoder
            .write_header()
            .expect("png header")
            .write_image_data(&rows)
            .expect("png data");
        log::info!("saved screenshot to {}", path.display());
    }
}

impl ApplicationHandler for App {
    fn resumed(&mut self, event_loop: &ActiveEventLoop) {
        if self.state.is_some() {
            return;
        }
        let window = Arc::new(
            event_loop
                .create_window(
                    Window::default_attributes()
                        .with_title("badlands")
                        .with_inner_size(LogicalSize::new(1600.0, 800.0)),
                )
                .expect("failed to create window"),
        );
        self.state = Some(GameState::new(window));
    }

    fn window_event(&mut self, event_loop: &ActiveEventLoop, _id: WindowId, event: WindowEvent) {
        let Some(state) = self.state.as_mut() else {
            return;
        };
        match event {
            WindowEvent::CloseRequested => event_loop.exit(),
            WindowEvent::Resized(size) => {
                state.gpu.configure_surface(size.width, size.height);
                state
                    .scene
                    .resize(&state.gpu.device, state.gpu.width, state.gpu.height);
            }
            WindowEvent::ScaleFactorChanged { scale_factor, .. } => {
                // Rebake the glyph atlas at the new DPI; layout and pen
                // positions already track the live scale factor.
                state.ui =
                    UiRenderer::new(&state.gpu.device, &state.gpu.queue, scale_factor as f32);
            }
            WindowEvent::CursorMoved { position, .. } => {
                state.handle_pointer_moved(Vec2::new(position.x as f32, position.y as f32));
            }
            WindowEvent::MouseInput {
                state: button_state,
                button: MouseButton::Left,
                ..
            } => state.handle_pointer_button(button_state),
            WindowEvent::ModifiersChanged(modifiers) => {
                state.shift_down = modifiers.state().shift_key();
            }
            WindowEvent::KeyboardInput { event, .. } => {
                if event.state == ElementState::Pressed && !event.repeat && state.shift_down {
                    // Shift+S: shader hot reload (port of the SdlViewerApp
                    // binding). Shift+N: noiser brain-script hot reload;
                    // both keep the last-good version on failure.
                    if matches!(&event.logical_key, Key::Character(c) if c.eq_ignore_ascii_case("s"))
                    {
                        state.pipelines.reload_all(&state.gpu.device);
                    }
                    if matches!(&event.logical_key, Key::Character(c) if c.eq_ignore_ascii_case("n"))
                    {
                        state.reload_brain_script();
                    }
                }
            }
            WindowEvent::RedrawRequested => {
                let done = state.render(&self.config);
                if done {
                    event_loop.exit();
                } else {
                    state.window.request_redraw();
                }
            }
            _ => {}
        }
    }

    fn about_to_wait(&mut self, _event_loop: &ActiveEventLoop) {
        if let Some(state) = &self.state {
            state.window.request_redraw();
        }
    }
}
