// Port of weave-renderer src/rendering/shader/shader_reflection.{hpp,cpp}:
// derive bind group layouts from the compiled WGSL instead of declaring them
// by hand. Group 1 uniform buffers are forced to dynamic offsets
// (CreateLayoutsFromReflection {.force_group1_dynamic_offsets = true}), which
// is how per-object uniforms work (FrameContext::allocate_dynamic_uniform).

use std::collections::BTreeMap;

pub struct ReflectedLayouts {
    // group index -> entries, sorted by binding
    pub groups: Vec<Vec<wgpu::BindGroupLayoutEntry>>,
}

pub fn reflect_bind_group_layouts(module: &naga::Module) -> ReflectedLayouts {
    let mut groups: BTreeMap<u32, BTreeMap<u32, wgpu::BindGroupLayoutEntry>> = BTreeMap::new();

    for (_, var) in module.global_variables.iter() {
        let Some(ref binding) = var.binding else {
            continue;
        };
        let ty = match binding_type_for(module, var, binding.group) {
            Some(ty) => ty,
            None => continue,
        };
        groups.entry(binding.group).or_default().insert(
            binding.binding,
            wgpu::BindGroupLayoutEntry {
                binding: binding.binding,
                visibility: wgpu::ShaderStages::VERTEX | wgpu::ShaderStages::FRAGMENT,
                ty,
                count: None,
            },
        );
    }

    let max_group = groups.keys().max().copied().unwrap_or(0);
    let mut out = Vec::new();
    for group in 0..=max_group {
        out.push(
            groups
                .get(&group)
                .map(|entries| entries.values().copied().collect())
                .unwrap_or_default(),
        );
    }
    ReflectedLayouts { groups: out }
}

fn binding_type_for(
    module: &naga::Module,
    var: &naga::GlobalVariable,
    group: u32,
) -> Option<wgpu::BindingType> {
    let inner = &module.types[var.ty].inner;
    match var.space {
        naga::AddressSpace::Uniform => Some(wgpu::BindingType::Buffer {
            ty: wgpu::BufferBindingType::Uniform,
            // force_group1_dynamic_offsets: per-object uniforms live in group 1
            has_dynamic_offset: group == 1,
            min_binding_size: None,
        }),
        naga::AddressSpace::Storage { access } => Some(wgpu::BindingType::Buffer {
            ty: wgpu::BufferBindingType::Storage {
                read_only: !access.contains(naga::StorageAccess::STORE),
            },
            has_dynamic_offset: false,
            min_binding_size: None,
        }),
        naga::AddressSpace::Handle => match *inner {
            naga::TypeInner::Image {
                dim,
                arrayed,
                class,
            } => {
                let view_dimension = view_dimension_for(dim, arrayed);
                match class {
                    naga::ImageClass::Sampled { kind, multi } => Some(wgpu::BindingType::Texture {
                        sample_type: match kind {
                            naga::ScalarKind::Float => {
                                wgpu::TextureSampleType::Float { filterable: true }
                            }
                            naga::ScalarKind::Sint => wgpu::TextureSampleType::Sint,
                            naga::ScalarKind::Uint => wgpu::TextureSampleType::Uint,
                            _ => wgpu::TextureSampleType::Float { filterable: true },
                        },
                        view_dimension,
                        multisampled: multi,
                    }),
                    naga::ImageClass::Depth { multi } => Some(wgpu::BindingType::Texture {
                        sample_type: wgpu::TextureSampleType::Depth,
                        view_dimension,
                        multisampled: multi,
                    }),
                    naga::ImageClass::External => None,
                    naga::ImageClass::Storage { format, access } => {
                        Some(wgpu::BindingType::StorageTexture {
                            access: if access.contains(naga::StorageAccess::LOAD)
                                && access.contains(naga::StorageAccess::STORE)
                            {
                                wgpu::StorageTextureAccess::ReadWrite
                            } else if access.contains(naga::StorageAccess::STORE) {
                                wgpu::StorageTextureAccess::WriteOnly
                            } else {
                                wgpu::StorageTextureAccess::ReadOnly
                            },
                            format: storage_format_for(format),
                            view_dimension,
                        })
                    }
                }
            }
            naga::TypeInner::Sampler { comparison } => {
                Some(wgpu::BindingType::Sampler(if comparison {
                    wgpu::SamplerBindingType::Comparison
                } else {
                    wgpu::SamplerBindingType::Filtering
                }))
            }
            _ => None,
        },
        _ => None,
    }
}

fn view_dimension_for(dim: naga::ImageDimension, arrayed: bool) -> wgpu::TextureViewDimension {
    match (dim, arrayed) {
        (naga::ImageDimension::D1, _) => wgpu::TextureViewDimension::D1,
        (naga::ImageDimension::D2, false) => wgpu::TextureViewDimension::D2,
        (naga::ImageDimension::D2, true) => wgpu::TextureViewDimension::D2Array,
        (naga::ImageDimension::D3, _) => wgpu::TextureViewDimension::D3,
        (naga::ImageDimension::Cube, false) => wgpu::TextureViewDimension::Cube,
        (naga::ImageDimension::Cube, true) => wgpu::TextureViewDimension::CubeArray,
    }
}

fn storage_format_for(format: naga::StorageFormat) -> wgpu::TextureFormat {
    // Only the formats the MVP could plausibly touch; extend as needed.
    match format {
        naga::StorageFormat::Rgba8Unorm => wgpu::TextureFormat::Rgba8Unorm,
        naga::StorageFormat::Rgba16Float => wgpu::TextureFormat::Rgba16Float,
        naga::StorageFormat::R32Float => wgpu::TextureFormat::R32Float,
        naga::StorageFormat::Rgba32Float => wgpu::TextureFormat::Rgba32Float,
        other => unimplemented!("storage texture format {other:?}"),
    }
}
