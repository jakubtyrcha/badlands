// Port of weave-renderer src/rendering/shader/gpu_pipeline_generator.{hpp,cpp}
// (class GpuPipelineGenerator, sync GetPipeline API) plus the slim remains of
// src/rendering/material/material.hpp: a RenderPipelineDeclaration is compiled
// (WESL -> WGSL via the `wesl` crate, the Rust library the C++ engine wrapped
// through wesl-ffi), reflected (naga -> bind group layouts), cached by
// declaration, and rebuilt on reload_all() (Shift+S hot reload).

use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::sync::Arc;

use wesl::{ModulePath, StandardResolver, Wesl, syntax::PathOrigin};

use crate::gpu::reflection::reflect_bind_group_layouts;

#[derive(Clone, Copy, Debug)]
pub enum VertexLayoutKind {
    // Fullscreen passes: no vertex buffer (vertex id driven).
    None,
    // Port of VertexLayout::kTexturedMesh: pos(3) + uv(2) + normal(3) + tangent(3).
    TexturedMesh,
    // UI quads: pos_px(2) + uv(2) + color(4).
    Ui,
}

#[derive(Clone, Copy, Debug)]
pub struct DepthConfig {
    pub format: wgpu::TextureFormat,
    pub write: bool,
    pub compare: wgpu::CompareFunction,
}

// Reversed-Z scene depth (RenderTarget::kColorDepthGreater).
pub const DEPTH_REVERSED_Z: DepthConfig = DepthConfig {
    format: wgpu::TextureFormat::Depth32Float,
    write: true,
    compare: wgpu::CompareFunction::Greater,
};

#[derive(Clone, Debug)]
pub struct RenderPipelineDeclaration {
    // Shader module path relative to shaders/, without extension, e.g. "game/ground".
    pub shader_path: String,
    pub vertex_layout: VertexLayoutKind,
    pub cull_mode: Option<wgpu::Face>,
    pub depth: Option<DepthConfig>,
    pub blend: Option<wgpu::BlendState>,
    pub targets: Vec<wgpu::TextureFormat>,
}

impl Default for RenderPipelineDeclaration {
    fn default() -> Self {
        RenderPipelineDeclaration {
            shader_path: String::new(),
            vertex_layout: VertexLayoutKind::None,
            cull_mode: None,
            depth: None,
            blend: None,
            targets: vec![],
        }
    }
}

pub struct CompiledPipeline {
    pub pipeline: wgpu::RenderPipeline,
    pub bind_group_layouts: Vec<wgpu::BindGroupLayout>,
}

struct CacheEntry {
    decl: RenderPipelineDeclaration,
    pipeline: Arc<CompiledPipeline>,
}

pub struct PipelineGenerator {
    shader_dir: PathBuf,
    wesl: Wesl<StandardResolver>,
    cache: HashMap<String, CacheEntry>,
}

impl PipelineGenerator {
    pub fn new() -> PipelineGenerator {
        let shader_dir = find_shader_directory();
        log::info!("shader directory: {}", shader_dir.display());
        PipelineGenerator {
            wesl: Wesl::new(&shader_dir),
            shader_dir,
            cache: HashMap::new(),
        }
    }

    // Port of GpuPipelineGenerator::ReloadAll(): recompiles every cached
    // pipeline from disk, keeping the last-good pipeline (and logging) when a
    // shader fails to compile — a broken shader must not kill the session.
    pub fn reload_all(&mut self, device: &wgpu::Device) {
        log::info!("reloading shaders");
        for entry in self.cache.values_mut() {
            match Self::build_pipeline_impl(&self.wesl, &self.shader_dir, device, &entry.decl) {
                Ok(pipeline) => entry.pipeline = Arc::new(pipeline),
                Err(err) => log::error!(
                    "shader reload failed, keeping previous pipeline for '{}':\n{err}",
                    entry.decl.shader_path
                ),
            }
        }
    }

    // Port of the sync GpuPipelineGenerator::GetPipeline(declaration, targets).
    // The first compile of a declaration is fatal on error (broken shader at
    // startup); reload_all handles subsequent errors gracefully.
    pub fn get_render_pipeline(
        &mut self,
        device: &wgpu::Device,
        decl: &RenderPipelineDeclaration,
    ) -> Arc<CompiledPipeline> {
        let key = format!("{decl:?}");
        if let Some(cached) = self.cache.get(&key) {
            return cached.pipeline.clone();
        }
        let compiled = Arc::new(
            Self::build_pipeline_impl(&self.wesl, &self.shader_dir, device, decl)
                .unwrap_or_else(|err| panic!("{err}")),
        );
        self.cache.insert(
            key,
            CacheEntry {
                decl: decl.clone(),
                pipeline: compiled.clone(),
            },
        );
        compiled
    }

    // Port of GpuPipelineGenerator::CompileWesl (via wesl-ffi in C++).
    fn compile_wesl(
        wesl: &Wesl<StandardResolver>,
        shader_dir: &Path,
        shader_path: &str,
    ) -> Result<String, String> {
        let module = ModulePath {
            origin: PathOrigin::Absolute,
            components: shader_path.split('/').map(str::to_string).collect(),
        };
        match wesl.compile(&module) {
            Ok(result) => Ok(result.to_string()),
            Err(err) => Err(format!(
                "WESL compilation of '{shader_path}' (in {}) failed:\n{err}",
                shader_dir.display()
            )),
        }
    }

    fn build_pipeline_impl(
        wesl: &Wesl<StandardResolver>,
        shader_dir: &Path,
        device: &wgpu::Device,
        decl: &RenderPipelineDeclaration,
    ) -> Result<CompiledPipeline, String> {
        let wgsl = Self::compile_wesl(wesl, shader_dir, &decl.shader_path)?;

        let naga_module = naga::front::wgsl::parse_str(&wgsl).map_err(|err| {
            format!(
                "naga failed to parse WGSL for '{}':\n{}",
                decl.shader_path,
                err.emit_to_string(&wgsl)
            )
        })?;
        for entry_point in ["vs_main", "fs_main"] {
            if !naga_module.entry_points.iter().any(|ep| ep.name == entry_point) {
                return Err(format!(
                    "shader '{}' is missing entry point '{entry_point}'",
                    decl.shader_path
                ));
            }
        }
        let reflected = reflect_bind_group_layouts(&naga_module);

        let shader = device.create_shader_module(wgpu::ShaderModuleDescriptor {
            label: Some(&decl.shader_path),
            source: wgpu::ShaderSource::Wgsl(wgsl.into()),
        });

        let bind_group_layouts: Vec<wgpu::BindGroupLayout> = reflected
            .groups
            .iter()
            .enumerate()
            .map(|(group, entries)| {
                device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
                    label: Some(&format!("{} group {group}", decl.shader_path)),
                    entries,
                })
            })
            .collect();
        let layout_refs: Vec<Option<&wgpu::BindGroupLayout>> =
            bind_group_layouts.iter().map(Some).collect();
        let pipeline_layout = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
            label: Some(&decl.shader_path),
            bind_group_layouts: &layout_refs,
            immediate_size: 0,
        });

        let vertex_buffers: Vec<Option<wgpu::VertexBufferLayout<'static>>> =
            vertex_buffer_layouts(decl.vertex_layout)
                .into_iter()
                .map(Some)
                .collect();
        let targets: Vec<Option<wgpu::ColorTargetState>> = decl
            .targets
            .iter()
            .map(|format| {
                Some(wgpu::ColorTargetState {
                    format: *format,
                    blend: decl.blend,
                    write_mask: wgpu::ColorWrites::ALL,
                })
            })
            .collect();

        let pipeline = device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
            label: Some(&decl.shader_path),
            layout: Some(&pipeline_layout),
            vertex: wgpu::VertexState {
                module: &shader,
                entry_point: Some("vs_main"),
                compilation_options: Default::default(),
                buffers: &vertex_buffers,
            },
            primitive: wgpu::PrimitiveState {
                topology: wgpu::PrimitiveTopology::TriangleList,
                front_face: wgpu::FrontFace::Ccw,
                cull_mode: decl.cull_mode,
                ..Default::default()
            },
            depth_stencil: decl.depth.map(|depth| wgpu::DepthStencilState {
                format: depth.format,
                depth_write_enabled: Some(depth.write),
                depth_compare: Some(depth.compare),
                stencil: Default::default(),
                bias: Default::default(),
            }),
            multisample: Default::default(),
            fragment: Some(wgpu::FragmentState {
                module: &shader,
                entry_point: Some("fs_main"),
                compilation_options: Default::default(),
                targets: &targets,
            }),
            multiview_mask: None,
            cache: None,
        });

        Ok(CompiledPipeline {
            pipeline,
            bind_group_layouts,
        })
    }
}

fn vertex_buffer_layouts(kind: VertexLayoutKind) -> Vec<wgpu::VertexBufferLayout<'static>> {
    const TEXTURED_MESH: [wgpu::VertexAttribute; 4] = wgpu::vertex_attr_array![
        0 => Float32x3, // position
        1 => Float32x2, // uv
        2 => Float32x3, // normal
        3 => Float32x3, // tangent
    ];
    const UI: [wgpu::VertexAttribute; 3] = wgpu::vertex_attr_array![
        0 => Float32x2, // position (physical pixels)
        1 => Float32x2, // uv
        2 => Float32x4, // color (sRGB)
    ];
    match kind {
        VertexLayoutKind::None => vec![],
        VertexLayoutKind::TexturedMesh => vec![wgpu::VertexBufferLayout {
            array_stride: 11 * 4,
            step_mode: wgpu::VertexStepMode::Vertex,
            attributes: &TEXTURED_MESH,
        }],
        VertexLayoutKind::Ui => vec![wgpu::VertexBufferLayout {
            array_stride: 8 * 4,
            step_mode: wgpu::VertexStepMode::Vertex,
            attributes: &UI,
        }],
    }
}

// Helper mirroring the free function CreateBindGroup(device, compiled, group,
// entries) in gpu_pipeline_generator.hpp: resources are matched positionally
// against the reflected (binding-sorted) layout entries of the group.
pub fn create_bind_group(
    device: &wgpu::Device,
    compiled: &CompiledPipeline,
    group: u32,
    label: &str,
    resources: Vec<wgpu::BindingResource<'_>>,
) -> wgpu::BindGroup {
    // Reflection sorts entries by binding id and our shaders use dense
    // 0..n bindings, so positional matching is faithful to the C++ helper.
    let entries: Vec<wgpu::BindGroupEntry> = resources
        .into_iter()
        .enumerate()
        .map(|(index, resource)| wgpu::BindGroupEntry {
            binding: index as u32,
            resource,
        })
        .collect();
    device.create_bind_group(&wgpu::BindGroupDescriptor {
        label: Some(label),
        layout: &compiled.bind_group_layouts[group as usize],
        entries: &entries,
    })
}

// Port of GpuContext::FindAndSetShaderDirectory() / FindExeRelativeAssetDir:
// walk up from the executable looking for a directory containing the marker
// shader, then fall back to the source tree and CWD.
fn find_shader_directory() -> PathBuf {
    crate::assets::find_asset_dir("shaders", "common/frame.wesl")
        .expect("shader directory not found (marker common/frame.wesl)")
}
