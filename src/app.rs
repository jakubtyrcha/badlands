// App shell: port of the SdlViewerApp main loop from weave-renderer
// (apps/src/app-framework/sdl_viewer_app.cpp) on winit — window creation,
// event handling (press-drag pan, Shift+S shader reload), and per-frame
// composition: scene forward pass -> tonemap (graph) -> UI pass -> present.

use std::path::PathBuf;
use std::sync::Arc;
use std::time::Instant;

use glam::{Vec2, Vec3, Vec4};
use winit::application::ApplicationHandler;
use winit::dpi::LogicalSize;
use winit::event::{ElementState, MouseButton, WindowEvent};
use winit::event_loop::ActiveEventLoop;
use winit::keyboard::{Key, NamedKey};
use winit::window::{Window, WindowId};

use crate::game::angled_camera::AngledCamera;
use crate::game::catalog;
use crate::game_ffi::{
    BuildingKind, GRID_HALF_EXTENT_TILES, Game, GameBuildingState, GameCharacterState,
    GameGridTriangle, GamePlacementDesc, building_at_world, render_box,
};
use crate::gpu::context::GpuContext;
use crate::gpu::frame::FrameContext;
use crate::gpu::graph::ProcessingGraph;
use crate::gpu::pipelines::PipelineGenerator;
use crate::scene::camera::{Camera, UniformData};
use crate::scene::overlay;
use crate::scene::renderer::{GhostInstance, SceneRenderer};
use crate::ui::layout::{UiLayout, compute as compute_ui_layout, rect_contains};
use crate::ui::panel::{self, Hit, PanelButton};
use crate::ui::render::UiRenderer;
use crate::ui::sidebar;

#[derive(Default, Clone)]
pub struct RunConfig {
    pub max_frames: Option<u32>,
    pub screenshot_path: Option<PathBuf>,
}

const TICK_DT: f32 = 1.0 / 30.0;

// How far the camera center may pan from the origin (keeps the map edge mostly
// off-screen). A presentation concern, so it lives on the render side.
const PAN_EXTENT: f32 = 14.0;
const GROUND_HALF_EXTENT: f32 = GRID_HALF_EXTENT_TILES as f32;

// The building being placed: kind + current rotation (0..3 -> 0/45/90/135 deg).
struct PlacementMode {
    kind: BuildingKind,
    rotation_index: i32,
}

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
    camera: AngledCamera,
    sim: Game,
    brain_script_path: Option<PathBuf>,

    placement: Option<PlacementMode>,
    selected_building: Option<u32>,
    probe_triangles: Vec<GameGridTriangle>,

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
        let scene = SceneRenderer::new(&gpu.device, &gpu.queue, GROUND_HALF_EXTENT, gpu.width, gpu.height);
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
            camera: AngledCamera::new(),
            sim,
            brain_script_path,
            placement: None,
            selected_building: None,
            probe_triangles: Vec::new(),
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

    // The placement request under the cursor for the active tool. Shared by the
    // preview probe and the actual place so the ghost and the placed building
    // can never disagree.
    fn placement_desc(&self, mode: &PlacementMode, screen: Vec2) -> GamePlacementDesc {
        let ground = self.camera.screen_to_ground(self.cursor, screen);
        GamePlacementDesc {
            kind: mode.kind as i32,
            rotation_index: mode.rotation_index,
            world_x: ground.x,
            world_z: ground.y,
        }
    }

    // Projects each building's top to screen space and draws its name centered
    // above it (a camera-facing debug label; the UI pass has no depth test, so
    // labels always render on top).
    fn push_building_labels(&mut self, camera: &Camera, buildings: &[GameBuildingState], screen: Vec2) {
        let view_proj = camera.proj() * camera.view_at_origin();
        for building in buildings {
            let info = catalog::info(BuildingKind::from_i32(building.kind));
            let anchor = Vec3::new(building.center_x, info.height + 0.5, building.center_z);
            let clip = view_proj * (anchor - camera.position).extend(1.0);
            if clip.w <= 1e-4 {
                continue; // behind the camera
            }
            let ndc = clip.truncate() / clip.w;
            if ndc.x < -1.0 || ndc.x > 1.0 || ndc.y < -1.0 || ndc.y > 1.0 {
                continue; // off-screen: don't bleed labels onto the UI edges
            }
            let px = (ndc.x * 0.5 + 0.5) * screen.x;
            let py = (1.0 - (ndc.y * 0.5 + 0.5)) * screen.y - 8.0;
            let width = self.ui.atlas.measure(info.name);
            self.ui
                .push_text(px - width * 0.5, py, info.name, [0.96, 0.96, 0.99, 1.0]);
        }
    }

    // Screen anchor (top-left) for the selected building's floating panel,
    // clamped inside the viewport; a fixed corner if the center projects behind
    // the camera.
    fn panel_anchor(&self, camera: &Camera, center: Vec3, screen: Vec2, layout: &UiLayout, scale: f32) -> Vec2 {
        let vp = &layout.viewport;
        let pad = 8.0 * scale;
        let max_x = (vp.x + vp.w - panel::PANEL_W * scale - pad).max(vp.x + pad);
        let max_y = (vp.y + vp.h - 160.0 * scale).max(vp.y + pad);
        match camera.world_to_screen(center, screen) {
            Some(p) => Vec2::new(
                p.x.clamp(vp.x + pad, max_x),
                (p.y + 12.0 * scale).clamp(vp.y + pad, max_y),
            ),
            None => Vec2::new(vp.x + pad, vp.y + pad),
        }
    }

    // The selection panel model from in-hand snapshots, or None if the selection
    // no longer refers to a live building.
    fn selected_panel_model(
        &self,
        camera: &Camera,
        buildings: &[GameBuildingState],
        characters: &[GameCharacterState],
        screen: Vec2,
        layout: &UiLayout,
        scale: f32,
    ) -> Option<panel::PanelModel> {
        let id = self.selected_building?;
        let building = buildings.iter().find(|b| b.id == id)?;
        let info = catalog::info(BuildingKind::from_i32(building.kind));
        let center = Vec3::new(building.center_x, info.height + 0.5, building.center_z);
        let anchor = self.panel_anchor(camera, center, screen, layout, scale);
        Some(panel::build_model(building, buildings, characters, anchor))
    }

    fn render(&mut self, config: &RunConfig) -> bool {
        self.step_simulation(config);
        let characters = self.sim.state();
        let buildings = self.sim.buildings();

        let aspect = self.gpu.width as f32 / self.gpu.height.max(1) as f32;
        let camera = self.camera.camera(aspect);
        let camera_pos = camera.position;
        let screen = Vec2::new(self.gpu.width as f32, self.gpu.height as f32);
        let layout = self.ui_layout();

        // Placement preview: a ghost cube + grid overlay from a per-frame probe
        // (read-only, so it self-heals on pan/rotate/place).
        let mut ghost = None;
        let mut overlay_vertices: Vec<f32> = Vec::new();
        if let Some(mode) = &self.placement {
            if rect_contains(&layout.viewport, self.cursor.x, self.cursor.y) {
                let desc = self.placement_desc(mode, screen);
                let probe = self.sim.probe_placement(&desc, &mut self.probe_triangles);
                overlay_vertices = overlay::build_vertices(&self.probe_triangles);
                let bbox = render_box(mode.kind as i32, mode.rotation_index);
                let info = catalog::info(mode.kind);
                let color = if probe.valid == 1 {
                    info.color.extend(1.0)
                } else {
                    Vec4::new(0.85, 0.32, 0.30, 1.0) // pale red = can't place
                };
                ghost = Some(GhostInstance {
                    pos: Vec3::new(probe.snapped_x, 0.0, probe.snapped_z),
                    size: Vec3::new(bbox.size_x, info.height, bbox.size_z),
                    yaw: bbox.yaw_radians,
                    color,
                });
            }
        }

        let uniforms = self.build_uniforms();
        let mut frame = FrameContext::begin(&self.gpu.device, &uniforms);

        // Heroes hidden inside a building are excluded from the scene (but stay
        // in the panel's visitor list).
        let visible: Vec<GameCharacterState> = characters
            .iter()
            .copied()
            .filter(|c| c.inside_building_id < 0)
            .collect();

        self.scene.render(
            &self.gpu.device,
            &self.gpu.queue,
            &mut frame,
            &mut self.pipelines,
            &buildings,
            &visible,
            ghost.as_ref(),
            &overlay_vertices,
            camera_pos,
        );

        // UI: top bar (gold), the build sidebar, and floating building labels.
        // Built regardless of surface availability so the screenshot path draws
        // it too.
        let scale = self.window.scale_factor() as f32;
        let world = self.sim.world();
        self.ui.begin();
        self.ui
            .push_rect(&layout.top_bar, [0.09, 0.08, 0.07, 0.92]);
        let baseline = layout.top_bar.y + layout.top_bar.h * 0.5
            + (self.ui.atlas.ascent_px + self.ui.atlas.descent_px) * 0.5;
        self.ui.push_text(
            16.0 * scale,
            baseline,
            &format!("Gold: {}", world.gold),
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
        let selected = self.placement.as_ref().map(|m| m.kind);
        sidebar::draw(&mut self.ui, &layout, scale, selected);
        self.push_building_labels(&camera, &buildings, screen);

        // The floating selection panel (recruit/destroy + visitor list).
        if let Some(model) =
            self.selected_panel_model(&camera, &buildings, &characters, screen, &layout, scale)
        {
            panel::draw(&mut self.ui, &model, scale);
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
            let screen = Vec2::new(self.gpu.width as f32, self.gpu.height as f32);
            self.camera.pan(self.cursor, position, screen, PAN_EXTENT);
        }
        self.cursor = position;
    }

    fn handle_pointer_button(&mut self, state: ElementState) {
        match state {
            ElementState::Pressed => {
                let layout = self.ui_layout();
                let scale = self.window.scale_factor() as f32;
                let screen = Vec2::new(self.gpu.width as f32, self.gpu.height as f32);

                // 1. Sidebar: select a placement tool (clicking the active one
                // cancels).
                if rect_contains(&layout.sidebar, self.cursor.x, self.cursor.y) {
                    if let Some(kind) = sidebar::hit_test(&layout, scale, self.cursor.x, self.cursor.y) {
                        let active = self.placement.as_ref().map(|m| m.kind) == Some(kind);
                        self.placement = (!active).then_some(PlacementMode {
                            kind,
                            rotation_index: 0,
                        });
                    }
                    return;
                }

                // 2. Open selection panel: recruit / destroy / swallow the click.
                if self.selected_building.is_some() {
                    let aspect = self.gpu.width as f32 / self.gpu.height.max(1) as f32;
                    let camera = self.camera.camera(aspect);
                    let buildings = self.sim.buildings();
                    let characters = self.sim.state();
                    match self
                        .selected_panel_model(&camera, &buildings, &characters, screen, &layout, scale)
                    {
                        Some(model) => {
                            match panel::hit_test(&model, scale, self.cursor.x, self.cursor.y) {
                                Hit::Button(PanelButton::Recruit) => {
                                    self.sim.recruit(model.building_id);
                                    return;
                                }
                                Hit::Button(PanelButton::Destroy) => {
                                    self.sim.destroy_building(model.building_id);
                                    self.selected_building = None;
                                    return;
                                }
                                Hit::Consumed => return,
                                Hit::Miss => {}
                            }
                        }
                        // Selection went stale (destroyed elsewhere).
                        None => self.selected_building = None,
                    }
                }

                // 3. Viewport: place with the active tool, else pick a building,
                // else start a pan drag (empty ground clears the selection).
                if rect_contains(&layout.viewport, self.cursor.x, self.cursor.y) {
                    if let Some(mode) = &self.placement {
                        let desc = self.placement_desc(mode, screen);
                        self.sim.place_building(&desc); // invalid spot is a no-op
                    } else {
                        let ground = self.camera.screen_to_ground(self.cursor, screen);
                        let buildings = self.sim.buildings();
                        match building_at_world(&buildings, ground) {
                            Some(id) => self.selected_building = Some(id),
                            None => {
                                self.selected_building = None;
                                self.dragging = true;
                            }
                        }
                    }
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
                if event.state == ElementState::Pressed && !event.repeat {
                    if state.shift_down {
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
                    } else {
                        // Placement controls: 'r' rotates 45 deg, Esc cancels.
                        match &event.logical_key {
                            Key::Character(c) if c.eq_ignore_ascii_case("r") => {
                                if let Some(mode) = &mut state.placement {
                                    mode.rotation_index = (mode.rotation_index + 1) % 4;
                                }
                            }
                            Key::Named(NamedKey::Escape) => {
                                state.placement = None;
                                state.selected_building = None;
                            }
                            _ => {}
                        }
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
