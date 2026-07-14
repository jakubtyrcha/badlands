//! FFI bindings for the WESL shader compiler and WGSL reflection.
//!
//! This crate provides C-compatible functions that allow C++ code to:
//! - Compile WESL shaders to WGSL at runtime
//! - Reflect WGSL shaders to extract binding information
//!
//! Ported from sampo's `wesl-ffi` crate (`../sampo/wesl-ffi/src/lib.rs`),
//! replacing badlands' original minimal `badlands_wesl_compile`-only ABI
//! (which used wesl 0.4's `Wesl::new(dir).compile(&ModulePath{..})`). This
//! version adds the naga-based WGSL reflection ABI
//! (`wgsl_reflect`/`wgsl_reflect_uniforms`/`wgsl_reflect_vertex_inputs`/
//! `wgsl_reflect_fragment_outputs`) that `sampo/src/rendering/shader/{shader_
//! reflection,gpu_pipeline_generator}.cpp` depend on, and pins to wesl 0.2 to
//! match sampo's `Wesl::new_barebones` + custom-`Resolver` compile path.

use std::borrow::Cow;
use std::collections::HashSet;
use std::ffi::{CStr, CString};
use std::os::raw::c_char;
use std::ptr;

use naga::front::wgsl;
use wesl::{
    Feature, FileResolver, ManglerKind, ModulePath, ResolveError, Resolver, VirtualResolver, Wesl,
};

/// Result of a WESL compilation.
/// The caller is responsible for freeing both pointers using `wesl_free_string`.
#[repr(C)]
pub struct WeslCompileResult {
    /// On success, contains the compiled WGSL. On failure, this is null.
    pub wgsl: *mut c_char,
    /// On failure, contains the error message. On success, this is null.
    pub error: *mut c_char,
}

/// Compile a WESL shader file to WGSL.
///
/// # Arguments
/// * `shader_dir` - Path to the directory containing shaders (null-terminated C string)
/// * `module_path` - Module path to compile, e.g., "main" or "effects::bloom" (null-terminated)
///
/// # Returns
/// A `WeslCompileResult` with either `wgsl` or `error` populated.
/// The caller must free both strings using `wesl_free_string`.
///
/// # Safety
/// Both pointers must be valid null-terminated C strings.
#[no_mangle]
pub unsafe extern "C" fn wesl_compile_file(
    shader_dir: *const c_char,
    module_path: *const c_char,
) -> WeslCompileResult {
    let result = std::panic::catch_unwind(|| {
        if shader_dir.is_null() || module_path.is_null() {
            return WeslCompileResult {
                wgsl: ptr::null_mut(),
                error: to_c_string("Null pointer passed to wesl_compile_file"),
            };
        }

        let shader_dir = match CStr::from_ptr(shader_dir).to_str() {
            Ok(s) => s,
            Err(e) => {
                return WeslCompileResult {
                    wgsl: ptr::null_mut(),
                    error: to_c_string(&format!("Invalid shader_dir UTF-8: {}", e)),
                }
            }
        };

        let module_path = match CStr::from_ptr(module_path).to_str() {
            Ok(s) => s,
            Err(e) => {
                return WeslCompileResult {
                    wgsl: ptr::null_mut(),
                    error: to_c_string(&format!("Invalid module_path UTF-8: {}", e)),
                }
            }
        };

        compile_wesl_file_internal(shader_dir, module_path)
    });

    match result {
        Ok(r) => r,
        Err(_) => WeslCompileResult {
            wgsl: ptr::null_mut(),
            error: to_c_string("WESL compilation panicked"),
        },
    }
}

/// Compile a WESL shader file to WGSL with feature flags for conditional compilation.
///
/// # Arguments
/// * `shader_dir` - Path to the directory containing shaders (null-terminated C string)
/// * `module_path` - Module path to compile, e.g., "main" or "effects::bloom" (null-terminated)
/// * `features` - Array of feature names (null-terminated C strings) to enable
/// * `feature_count` - Number of features in the array
///
/// # Returns
/// A `WeslCompileResult` with either `wgsl` or `error` populated.
/// The caller must free both strings using `wesl_free_string`.
///
/// # Safety
/// All pointers must be valid null-terminated C strings.
#[no_mangle]
pub unsafe extern "C" fn wesl_compile_file_with_features(
    shader_dir: *const c_char,
    module_path: *const c_char,
    features: *const *const c_char,
    feature_count: usize,
) -> WeslCompileResult {
    let result = std::panic::catch_unwind(|| {
        if shader_dir.is_null() || module_path.is_null() {
            return WeslCompileResult {
                wgsl: ptr::null_mut(),
                error: to_c_string("Null pointer passed to wesl_compile_file_with_features"),
            };
        }

        let shader_dir = match CStr::from_ptr(shader_dir).to_str() {
            Ok(s) => s,
            Err(e) => {
                return WeslCompileResult {
                    wgsl: ptr::null_mut(),
                    error: to_c_string(&format!("Invalid shader_dir UTF-8: {}", e)),
                }
            }
        };

        let module_path = match CStr::from_ptr(module_path).to_str() {
            Ok(s) => s,
            Err(e) => {
                return WeslCompileResult {
                    wgsl: ptr::null_mut(),
                    error: to_c_string(&format!("Invalid module_path UTF-8: {}", e)),
                }
            }
        };

        // Parse feature flags
        let mut feature_set = HashSet::new();
        if !features.is_null() && feature_count > 0 {
            let feature_slice = std::slice::from_raw_parts(features, feature_count);
            for &feature_ptr in feature_slice {
                if !feature_ptr.is_null() {
                    if let Ok(feature_str) = CStr::from_ptr(feature_ptr).to_str() {
                        feature_set.insert(feature_str.to_string());
                    }
                }
            }
        }

        compile_wesl_file_internal_with_features(shader_dir, module_path, feature_set)
    });

    match result {
        Ok(r) => r,
        Err(_) => WeslCompileResult {
            wgsl: ptr::null_mut(),
            error: to_c_string("WESL compilation panicked"),
        },
    }
}

/// Compile a WESL shader file to WGSL with additional search directories for imports.
///
/// The primary directory is tried first for the module itself and all imports.
/// Additional directories provide fallback locations for import resolution.
///
/// # Arguments
/// * `shader_dir` - Primary directory containing shaders (null-terminated C string)
/// * `module_path` - Module path to compile (null-terminated)
/// * `features` - Array of feature names to enable (null if none)
/// * `feature_count` - Number of features in the array
/// * `additional_dirs` - Array of additional directory paths for import resolution (null if none)
/// * `additional_dir_count` - Number of additional directories
///
/// # Safety
/// All pointers must be valid null-terminated C strings.
#[no_mangle]
pub unsafe extern "C" fn wesl_compile_file_with_dirs(
    shader_dir: *const c_char,
    module_path: *const c_char,
    features: *const *const c_char,
    feature_count: usize,
    additional_dirs: *const *const c_char,
    additional_dir_count: usize,
) -> WeslCompileResult {
    let result = std::panic::catch_unwind(|| {
        if shader_dir.is_null() || module_path.is_null() {
            return WeslCompileResult {
                wgsl: ptr::null_mut(),
                error: to_c_string("Null pointer passed to wesl_compile_file_with_dirs"),
            };
        }

        let shader_dir = match CStr::from_ptr(shader_dir).to_str() {
            Ok(s) => s,
            Err(e) => {
                return WeslCompileResult {
                    wgsl: ptr::null_mut(),
                    error: to_c_string(&format!("Invalid shader_dir UTF-8: {}", e)),
                }
            }
        };

        let module_path = match CStr::from_ptr(module_path).to_str() {
            Ok(s) => s,
            Err(e) => {
                return WeslCompileResult {
                    wgsl: ptr::null_mut(),
                    error: to_c_string(&format!("Invalid module_path UTF-8: {}", e)),
                }
            }
        };

        let mut feature_set = HashSet::new();
        if !features.is_null() && feature_count > 0 {
            let feature_slice = std::slice::from_raw_parts(features, feature_count);
            for &feature_ptr in feature_slice {
                if !feature_ptr.is_null() {
                    if let Ok(feature_str) = CStr::from_ptr(feature_ptr).to_str() {
                        feature_set.insert(feature_str.to_string());
                    }
                }
            }
        }

        let mut extra_dirs = Vec::new();
        if !additional_dirs.is_null() && additional_dir_count > 0 {
            let dir_slice = std::slice::from_raw_parts(additional_dirs, additional_dir_count);
            for &dir_ptr in dir_slice {
                if !dir_ptr.is_null() {
                    if let Ok(dir_str) = CStr::from_ptr(dir_ptr).to_str() {
                        extra_dirs.push(dir_str);
                    }
                }
            }
        }

        compile_wesl_file_internal_with_features_and_dirs(
            shader_dir,
            module_path,
            feature_set,
            &extra_dirs,
        )
    });

    match result {
        Ok(r) => r,
        Err(_) => WeslCompileResult {
            wgsl: ptr::null_mut(),
            error: to_c_string("WESL compilation panicked"),
        },
    }
}

/// Compile WESL source code directly (without file I/O).
///
/// # Arguments
/// * `source` - WESL source code (null-terminated C string)
/// * `filename` - Virtual filename for error messages (null-terminated)
///
/// # Returns
/// A `WeslCompileResult` with either `wgsl` or `error` populated.
///
/// # Safety
/// Both pointers must be valid null-terminated C strings.
#[no_mangle]
pub unsafe extern "C" fn wesl_compile_source(
    source: *const c_char,
    filename: *const c_char,
) -> WeslCompileResult {
    let result = std::panic::catch_unwind(|| {
        if source.is_null() || filename.is_null() {
            return WeslCompileResult {
                wgsl: ptr::null_mut(),
                error: to_c_string("Null pointer passed to wesl_compile_source"),
            };
        }

        let source = match CStr::from_ptr(source).to_str() {
            Ok(s) => s,
            Err(e) => {
                return WeslCompileResult {
                    wgsl: ptr::null_mut(),
                    error: to_c_string(&format!("Invalid source UTF-8: {}", e)),
                }
            }
        };

        let filename = match CStr::from_ptr(filename).to_str() {
            Ok(s) => s,
            Err(e) => {
                return WeslCompileResult {
                    wgsl: ptr::null_mut(),
                    error: to_c_string(&format!("Invalid filename UTF-8: {}", e)),
                }
            }
        };

        compile_wesl_source_internal(source, filename)
    });

    match result {
        Ok(r) => r,
        Err(_) => WeslCompileResult {
            wgsl: ptr::null_mut(),
            error: to_c_string("WESL compilation panicked"),
        },
    }
}

/// Free a string returned by WESL functions.
///
/// # Safety
/// The pointer must have been returned by a WESL function, or be null.
#[no_mangle]
pub unsafe extern "C" fn wesl_free_string(s: *mut c_char) {
    if !s.is_null() {
        drop(CString::from_raw(s));
    }
}

/// Free a WeslCompileResult (frees both wgsl and error strings).
///
/// # Safety
/// The result must have been returned by a WESL compile function.
#[no_mangle]
pub unsafe extern "C" fn wesl_free_result(result: WeslCompileResult) {
    wesl_free_string(result.wgsl);
    wesl_free_string(result.error);
}

// Internal helper to convert a Rust string to a C string pointer
fn to_c_string(s: &str) -> *mut c_char {
    match CString::new(s) {
        Ok(cs) => cs.into_raw(),
        Err(_) => {
            // String contains null bytes, replace them
            let cleaned: String = s.chars().filter(|&c| c != '\0').collect();
            CString::new(cleaned)
                .unwrap_or_else(|_| CString::new("Error converting string").unwrap())
                .into_raw()
        }
    }
}

/// Resolver that tries multiple directories in order for import resolution.
struct MultiDirResolver {
    resolvers: Vec<FileResolver>,
}

impl MultiDirResolver {
    fn new(dirs: &[&str]) -> Self {
        Self {
            resolvers: dirs.iter().map(|d| FileResolver::new(d)).collect(),
        }
    }
}

impl Resolver for MultiDirResolver {
    fn resolve_source<'a>(
        &'a self,
        path: &ModulePath,
    ) -> Result<Cow<'a, str>, ResolveError> {
        let mut last_err = None;
        for resolver in &self.resolvers {
            match resolver.resolve_source(path) {
                Ok(source) => return Ok(source),
                Err(e) => last_err = Some(e),
            }
        }
        Err(last_err.unwrap_or_else(|| {
            ResolveError::ModuleNotFound(path.clone(), "no directories configured".to_string())
        }))
    }

    fn display_name(&self, path: &ModulePath) -> Option<String> {
        self.resolvers.iter().find_map(|r| r.display_name(path))
    }

    fn fs_path(&self, path: &ModulePath) -> Option<std::path::PathBuf> {
        self.resolvers.iter().find_map(|r| r.fs_path(path))
    }
}

fn compile_wesl_file_internal(shader_dir: &str, module_path: &str) -> WeslCompileResult {
    compile_wesl_file_internal_with_features_and_dirs(shader_dir, module_path, HashSet::new(), &[])
}

fn compile_wesl_file_internal_with_features(
    shader_dir: &str,
    module_path: &str,
    features: HashSet<String>,
) -> WeslCompileResult {
    compile_wesl_file_internal_with_features_and_dirs(shader_dir, module_path, features, &[])
}

fn compile_wesl_file_internal_with_features_and_dirs(
    shader_dir: &str,
    module_path: &str,
    features: HashSet<String>,
    additional_dirs: &[&str],
) -> WeslCompileResult {
    // Use Wesl::new_barebones with a FileResolver to avoid strict validation
    // that rejects Dawn-specific formats like r8unorm.
    // The standard Wesl::new() validates texture formats against the WGSL spec,
    // but r8unorm requires the chromium_internal_graphite extension which isn't
    // recognized by the WESL validator.
    let mut dirs: Vec<&str> = vec![shader_dir];
    dirs.extend_from_slice(additional_dirs);
    let resolver = MultiDirResolver::new(&dirs);
    let mut compiler = Wesl::new_barebones().set_custom_resolver(resolver);

    // Enable necessary extensions for proper compilation
    compiler.use_imports(true);       // Enable import resolution (mandatory extension)
    compiler.use_condcomp(true);      // Enable conditional compilation (optional extension)
    compiler.use_stripping(true);     // Remove unused declarations
    compiler.set_mangler(ManglerKind::Escape);  // Enable name mangling for imports

    // Set feature flags
    for feature in &features {
        compiler.set_feature(feature, Feature::Enable);
    }

    // Parse the module path - WESL requires "package::module" format
    let full_module_path = format!("package::{}", module_path);
    let module_specifier: ModulePath = match full_module_path.parse() {
        Ok(spec) => spec,
        Err(e) => {
            return WeslCompileResult {
                wgsl: ptr::null_mut(),
                error: to_c_string(&format!("Invalid module path '{}': {}", full_module_path, e)),
            }
        }
    };

    match compiler.compile(&module_specifier) {
        Ok(compiled) => {
            let wgsl = compiled.to_string();
            WeslCompileResult {
                wgsl: to_c_string(&wgsl),
                error: ptr::null_mut(),
            }
        }
        Err(e) => WeslCompileResult {
            wgsl: ptr::null_mut(),
            error: to_c_string(&format!("{}", e)),
        },
    }
}

fn compile_wesl_source_internal(source: &str, _filename: &str) -> WeslCompileResult {
    // Create a virtual resolver with the source
    let mut resolver = VirtualResolver::new();

    // Parse module path for "main"
    let module_path: ModulePath = match "package::main".parse() {
        Ok(mp) => mp,
        Err(e) => {
            return WeslCompileResult {
                wgsl: ptr::null_mut(),
                error: to_c_string(&format!("Internal error parsing module path: {}", e)),
            }
        }
    };

    resolver.add_module(module_path.clone(), Cow::Owned(source.to_string()));

    // Use barebones compiler with custom resolver
    let compiler = Wesl::new_barebones().set_custom_resolver(resolver);

    match compiler.compile(&module_path) {
        Ok(compiled) => {
            let wgsl = compiled.to_string();
            WeslCompileResult {
                wgsl: to_c_string(&wgsl),
                error: ptr::null_mut(),
            }
        }
        Err(e) => WeslCompileResult {
            wgsl: ptr::null_mut(),
            error: to_c_string(&format!("{}", e)),
        },
    }
}

// ============================================================================
// WGSL Reflection via Naga
// ============================================================================

/// Binding type constants (matches C++ enum values)
pub mod binding_type {
    pub const UNIFORM: u32 = 0;
    pub const STORAGE: u32 = 1;
    pub const STORAGE_READ_ONLY: u32 = 2;
    pub const TEXTURE: u32 = 3;
    pub const STORAGE_TEXTURE: u32 = 4;
    pub const SAMPLER: u32 = 5;
}

/// Texture sample type constants
pub mod texture_sample_type {
    pub const FLOAT: u32 = 0;
    pub const DEPTH: u32 = 1;
    pub const SINT: u32 = 2;
    pub const UINT: u32 = 3;
    pub const UNFILTERABLE_FLOAT: u32 = 4;
}

/// Texture dimension constants
pub mod texture_dimension {
    pub const D1: u32 = 0;
    pub const D2: u32 = 1;
    pub const D2_ARRAY: u32 = 2;
    pub const D3: u32 = 3;
    pub const CUBE: u32 = 4;
    pub const CUBE_ARRAY: u32 = 5;
}

/// Sampler type constants
pub mod sampler_type {
    pub const FILTERING: u32 = 0;
    pub const NON_FILTERING: u32 = 1;
    pub const COMPARISON: u32 = 2;
}

/// Shader stage visibility flags (can be OR'd together)
pub mod shader_stage {
    pub const VERTEX: u32 = 1;
    pub const FRAGMENT: u32 = 2;
    pub const COMPUTE: u32 = 4;
}

/// Storage texture format constants (matches WgslStorageTextureFormat in C header)
pub mod storage_texture_format {
    pub const UNDEFINED: u32 = 0;
    pub const R32FLOAT: u32 = 1;
    pub const R32UINT: u32 = 2;
    pub const R32SINT: u32 = 3;
    pub const RG32FLOAT: u32 = 4;
    pub const RG32UINT: u32 = 5;
    pub const RG32SINT: u32 = 6;
    pub const RGBA8UNORM: u32 = 7;
    pub const RGBA8SNORM: u32 = 8;
    pub const RGBA8UINT: u32 = 9;
    pub const RGBA8SINT: u32 = 10;
    pub const BGRA8UNORM: u32 = 11;
    pub const RGBA16FLOAT: u32 = 12;
    pub const RGBA16UINT: u32 = 13;
    pub const RGBA16SINT: u32 = 14;
    pub const RGBA32FLOAT: u32 = 15;
    pub const RGBA32UINT: u32 = 16;
    pub const RGBA32SINT: u32 = 17;
    pub const R16FLOAT: u32 = 18;
    pub const RG16FLOAT: u32 = 19;
    pub const R8UNORM: u32 = 20;
    pub const RG8UNORM: u32 = 21;
}

/// Storage texture access constants
pub mod storage_texture_access {
    pub const WRITE_ONLY: u32 = 0;
    pub const READ_ONLY: u32 = 1;
    pub const READ_WRITE: u32 = 2;
}

/// Uniform type constants for struct members
pub mod uniform_type {
    pub const INT: u32 = 0;
    pub const UINT: u32 = 1;
    pub const FLOAT: u32 = 2;
    pub const VEC2: u32 = 3;
    pub const VEC3: u32 = 4;
    pub const VEC4: u32 = 5;
    pub const MAT4: u32 = 6;
    pub const UNKNOWN: u32 = 255;
}

/// Vertex format constants (matches wgpu::VertexFormat)
pub mod vertex_format {
    pub const FLOAT32: u32 = 0;
    pub const FLOAT32X2: u32 = 1;
    pub const FLOAT32X3: u32 = 2;
    pub const FLOAT32X4: u32 = 3;
    pub const SINT32: u32 = 4;
    pub const SINT32X2: u32 = 5;
    pub const SINT32X3: u32 = 6;
    pub const SINT32X4: u32 = 7;
    pub const UINT32: u32 = 8;
    pub const UINT32X2: u32 = 9;
    pub const UINT32X3: u32 = 10;
    pub const UINT32X4: u32 = 11;
    pub const UNDEFINED: u32 = 255;
}

/// A single binding extracted from a WGSL shader.
#[repr(C)]
pub struct WgslBinding {
    pub group: u32,
    pub binding: u32,
    pub name: *mut c_char,
    pub binding_type: u32,
    pub texture_sample_type: u32,
    pub texture_dimension: u32,
    pub sampler_type: u32,
    pub visibility: u32,
    pub storage_texture_format: u32,
    pub storage_texture_access: u32,
}

/// Result of WGSL reflection.
#[repr(C)]
pub struct WgslReflectionResult {
    pub bindings: *mut WgslBinding,
    pub binding_count: usize,
    pub error: *mut c_char,
    pub workgroup_size: [u32; 3],
}

/// A single member of a uniform buffer struct.
#[repr(C)]
pub struct WgslUniformMember {
    pub name: *mut c_char,  // Member name (e.g., "displacement_scale")
    pub offset: u32,        // Byte offset in buffer
    pub size: u32,          // Size in bytes
    pub type_id: u32,       // uniform_type constant
}

/// A uniform buffer with its struct members.
#[repr(C)]
pub struct WgslUniformBuffer {
    pub group: u32,
    pub binding: u32,
    pub name: *mut c_char,              // Struct/var name (e.g., "object")
    pub members: *mut WgslUniformMember,
    pub member_count: usize,
    pub total_size: u32,                // Total buffer size
}

/// Result of uniform buffer reflection.
#[repr(C)]
pub struct WgslUniformReflectionResult {
    pub buffers: *mut WgslUniformBuffer,
    pub buffer_count: usize,
    pub error: *mut c_char,
}

/// A vertex input extracted from a WGSL shader (e.g., @location(0) pos: vec3<f32>).
#[repr(C)]
pub struct WgslVertexInput {
    pub location: u32,
    pub name: *mut c_char,
    pub format: u32,  // vertex_format constant
}

/// Result of vertex input reflection.
#[repr(C)]
pub struct WgslVertexInputReflectionResult {
    pub inputs: *mut WgslVertexInput,
    pub input_count: usize,
    pub error: *mut c_char,
}

/// A fragment output extracted from a WGSL shader (e.g., @location(0) -> vec4<f32>).
#[repr(C)]
pub struct WgslFragmentOutput {
    pub location: u32,
    pub name: *mut c_char,
    pub component_count: u32,  // 1-4
}

/// Result of fragment output reflection.
#[repr(C)]
pub struct WgslFragmentOutputReflectionResult {
    pub outputs: *mut WgslFragmentOutput,
    pub output_count: usize,
    pub error: *mut c_char,
}

/// Reflect a WGSL shader to extract binding information.
///
/// # Safety
/// The source pointer must be a valid null-terminated C string.
#[no_mangle]
pub unsafe extern "C" fn wgsl_reflect(wgsl_source: *const c_char) -> WgslReflectionResult {
    let result = std::panic::catch_unwind(|| {
        if wgsl_source.is_null() {
            return WgslReflectionResult {
                bindings: ptr::null_mut(),
                binding_count: 0,
                error: to_c_string("Null pointer passed to wgsl_reflect"),
                workgroup_size: [0, 0, 0],
            };
        }

        let source = match CStr::from_ptr(wgsl_source).to_str() {
            Ok(s) => s,
            Err(e) => {
                return WgslReflectionResult {
                    bindings: ptr::null_mut(),
                    binding_count: 0,
                    error: to_c_string(&format!("Invalid source UTF-8: {}", e)),
                    workgroup_size: [0, 0, 0],
                }
            }
        };

        reflect_wgsl_internal(source)
    });

    match result {
        Ok(r) => r,
        Err(_) => WgslReflectionResult {
            bindings: ptr::null_mut(),
            binding_count: 0,
            error: to_c_string("WGSL reflection panicked"),
            workgroup_size: [0, 0, 0],
        },
    }
}

/// Free a WgslReflectionResult.
///
/// # Safety
/// The result must have been returned by wgsl_reflect.
#[no_mangle]
pub unsafe extern "C" fn wgsl_free_reflection(result: WgslReflectionResult) {
    if !result.bindings.is_null() && result.binding_count > 0 {
        // Free name strings before dropping the Vec
        let bindings = std::slice::from_raw_parts(result.bindings, result.binding_count);
        for b in bindings {
            wesl_free_string(b.name);
        }
        let _ = Vec::from_raw_parts(result.bindings, result.binding_count, result.binding_count);
    }
    wesl_free_string(result.error);
}

/// Reflect a WGSL shader to extract uniform buffer member information.
///
/// # Safety
/// The source pointer must be a valid null-terminated C string.
#[no_mangle]
pub unsafe extern "C" fn wgsl_reflect_uniforms(wgsl_source: *const c_char) -> WgslUniformReflectionResult {
    let result = std::panic::catch_unwind(|| {
        if wgsl_source.is_null() {
            return WgslUniformReflectionResult {
                buffers: ptr::null_mut(),
                buffer_count: 0,
                error: to_c_string("Null pointer passed to wgsl_reflect_uniforms"),
            };
        }

        let source = match CStr::from_ptr(wgsl_source).to_str() {
            Ok(s) => s,
            Err(e) => {
                return WgslUniformReflectionResult {
                    buffers: ptr::null_mut(),
                    buffer_count: 0,
                    error: to_c_string(&format!("Invalid source UTF-8: {}", e)),
                }
            }
        };

        reflect_uniforms_internal(source)
    });

    match result {
        Ok(r) => r,
        Err(_) => WgslUniformReflectionResult {
            buffers: ptr::null_mut(),
            buffer_count: 0,
            error: to_c_string("WGSL uniform reflection panicked"),
        },
    }
}

/// Free a WgslUniformReflectionResult.
///
/// # Safety
/// The result must have been returned by wgsl_reflect_uniforms.
#[no_mangle]
pub unsafe extern "C" fn wgsl_free_uniform_reflection(result: WgslUniformReflectionResult) {
    if !result.buffers.is_null() && result.buffer_count > 0 {
        let buffers = Vec::from_raw_parts(result.buffers, result.buffer_count, result.buffer_count);
        for buffer in buffers {
            wesl_free_string(buffer.name);
            if !buffer.members.is_null() && buffer.member_count > 0 {
                let members = Vec::from_raw_parts(buffer.members, buffer.member_count, buffer.member_count);
                for member in members {
                    wesl_free_string(member.name);
                }
            }
        }
    }
    wesl_free_string(result.error);
}

/// Reflect a WGSL shader to extract vertex input information.
///
/// # Safety
/// The source pointer must be a valid null-terminated C string.
#[no_mangle]
pub unsafe extern "C" fn wgsl_reflect_vertex_inputs(wgsl_source: *const c_char) -> WgslVertexInputReflectionResult {
    let result = std::panic::catch_unwind(|| {
        if wgsl_source.is_null() {
            return WgslVertexInputReflectionResult {
                inputs: ptr::null_mut(),
                input_count: 0,
                error: to_c_string("Null pointer passed to wgsl_reflect_vertex_inputs"),
            };
        }

        let source = match CStr::from_ptr(wgsl_source).to_str() {
            Ok(s) => s,
            Err(e) => {
                return WgslVertexInputReflectionResult {
                    inputs: ptr::null_mut(),
                    input_count: 0,
                    error: to_c_string(&format!("Invalid source UTF-8: {}", e)),
                }
            }
        };

        reflect_vertex_inputs_internal(source)
    });

    match result {
        Ok(r) => r,
        Err(_) => WgslVertexInputReflectionResult {
            inputs: ptr::null_mut(),
            input_count: 0,
            error: to_c_string("WGSL vertex input reflection panicked"),
        },
    }
}

/// Free a WgslVertexInputReflectionResult.
///
/// # Safety
/// The result must have been returned by wgsl_reflect_vertex_inputs.
#[no_mangle]
pub unsafe extern "C" fn wgsl_free_vertex_input_reflection(result: WgslVertexInputReflectionResult) {
    if !result.inputs.is_null() && result.input_count > 0 {
        let inputs = Vec::from_raw_parts(result.inputs, result.input_count, result.input_count);
        for input in inputs {
            wesl_free_string(input.name);
        }
    }
    wesl_free_string(result.error);
}

/// Reflect a WGSL shader to extract fragment output information.
///
/// # Safety
/// The source pointer must be a valid null-terminated C string.
#[no_mangle]
pub unsafe extern "C" fn wgsl_reflect_fragment_outputs(wgsl_source: *const c_char) -> WgslFragmentOutputReflectionResult {
    let result = std::panic::catch_unwind(|| {
        if wgsl_source.is_null() {
            return WgslFragmentOutputReflectionResult {
                outputs: ptr::null_mut(),
                output_count: 0,
                error: to_c_string("Null pointer passed to wgsl_reflect_fragment_outputs"),
            };
        }

        let source = match CStr::from_ptr(wgsl_source).to_str() {
            Ok(s) => s,
            Err(e) => {
                return WgslFragmentOutputReflectionResult {
                    outputs: ptr::null_mut(),
                    output_count: 0,
                    error: to_c_string(&format!("Invalid source UTF-8: {}", e)),
                }
            }
        };

        reflect_fragment_outputs_internal(source)
    });

    match result {
        Ok(r) => r,
        Err(_) => WgslFragmentOutputReflectionResult {
            outputs: ptr::null_mut(),
            output_count: 0,
            error: to_c_string("WGSL fragment output reflection panicked"),
        },
    }
}

/// Free a WgslFragmentOutputReflectionResult.
///
/// # Safety
/// The result must have been returned by wgsl_reflect_fragment_outputs.
#[no_mangle]
pub unsafe extern "C" fn wgsl_free_fragment_output_reflection(result: WgslFragmentOutputReflectionResult) {
    if !result.outputs.is_null() && result.output_count > 0 {
        let outputs = Vec::from_raw_parts(result.outputs, result.output_count, result.output_count);
        for output in outputs {
            wesl_free_string(output.name);
        }
    }
    wesl_free_string(result.error);
}

fn reflect_vertex_inputs_internal(source: &str) -> WgslVertexInputReflectionResult {
    // Parse WGSL with Naga
    let module = match wgsl::parse_str(source) {
        Ok(m) => m,
        Err(e) => {
            return WgslVertexInputReflectionResult {
                inputs: ptr::null_mut(),
                input_count: 0,
                error: to_c_string(&format!("WGSL parse error: {}", e.emit_to_string(source))),
            }
        }
    };

    // Find vertex shader entry point and extract its input arguments
    let mut inputs = Vec::new();

    for entry_point in &module.entry_points {
        if entry_point.stage != naga::ShaderStage::Vertex {
            continue;
        }

        // Check function arguments for @location bindings
        for arg in &entry_point.function.arguments {
            if let Some(binding) = &arg.binding {
                if let naga::Binding::Location { location, .. } = binding {
                    let arg_name = arg.name.as_deref().unwrap_or("_unnamed");
                    let arg_ty = &module.types[arg.ty];
                    let format = naga_type_to_vertex_format(arg_ty);

                    inputs.push(WgslVertexInput {
                        location: *location,
                        name: to_c_string(arg_name),
                        format,
                    });
                }
            } else {
                // Argument might be a struct - check its members
                let arg_ty = &module.types[arg.ty];
                if let naga::TypeInner::Struct { members, .. } = &arg_ty.inner {
                    for member in members {
                        if let Some(naga::Binding::Location { location, .. }) = &member.binding {
                            let member_name = member.name.as_deref().unwrap_or("_unnamed");
                            let member_ty = &module.types[member.ty];
                            let format = naga_type_to_vertex_format(member_ty);

                            inputs.push(WgslVertexInput {
                                location: *location,
                                name: to_c_string(member_name),
                                format,
                            });
                        }
                    }
                }
            }
        }
    }

    // Sort by location
    inputs.sort_by_key(|input| input.location);

    // Convert to C-compatible array
    let input_count = inputs.len();
    let inputs_ptr = if inputs.is_empty() {
        ptr::null_mut()
    } else {
        let mut boxed = inputs.into_boxed_slice();
        let ptr = boxed.as_mut_ptr();
        std::mem::forget(boxed);
        ptr
    };

    WgslVertexInputReflectionResult {
        inputs: inputs_ptr,
        input_count,
        error: ptr::null_mut(),
    }
}

fn naga_type_to_vertex_format(ty: &naga::Type) -> u32 {
    match &ty.inner {
        naga::TypeInner::Scalar(scalar) => {
            match scalar.kind {
                naga::ScalarKind::Float => vertex_format::FLOAT32,
                naga::ScalarKind::Sint => vertex_format::SINT32,
                naga::ScalarKind::Uint => vertex_format::UINT32,
                _ => vertex_format::UNDEFINED,
            }
        }
        naga::TypeInner::Vector { size, scalar } => {
            let vec_size = match size {
                naga::VectorSize::Bi => 2,
                naga::VectorSize::Tri => 3,
                naga::VectorSize::Quad => 4,
            };
            match (scalar.kind, vec_size) {
                (naga::ScalarKind::Float, 2) => vertex_format::FLOAT32X2,
                (naga::ScalarKind::Float, 3) => vertex_format::FLOAT32X3,
                (naga::ScalarKind::Float, 4) => vertex_format::FLOAT32X4,
                (naga::ScalarKind::Sint, 2) => vertex_format::SINT32X2,
                (naga::ScalarKind::Sint, 3) => vertex_format::SINT32X3,
                (naga::ScalarKind::Sint, 4) => vertex_format::SINT32X4,
                (naga::ScalarKind::Uint, 2) => vertex_format::UINT32X2,
                (naga::ScalarKind::Uint, 3) => vertex_format::UINT32X3,
                (naga::ScalarKind::Uint, 4) => vertex_format::UINT32X4,
                _ => vertex_format::UNDEFINED,
            }
        }
        _ => vertex_format::UNDEFINED,
    }
}

/// Get component count from a Naga type (1 for scalar, 2-4 for vectors).
fn naga_type_component_count(ty: &naga::Type) -> u32 {
    match &ty.inner {
        naga::TypeInner::Scalar(_) => 1,
        naga::TypeInner::Vector { size, .. } => match size {
            naga::VectorSize::Bi => 2,
            naga::VectorSize::Tri => 3,
            naga::VectorSize::Quad => 4,
        },
        _ => 0,
    }
}

fn reflect_fragment_outputs_internal(source: &str) -> WgslFragmentOutputReflectionResult {
    let module = match wgsl::parse_str(source) {
        Ok(m) => m,
        Err(e) => {
            return WgslFragmentOutputReflectionResult {
                outputs: ptr::null_mut(),
                output_count: 0,
                error: to_c_string(&format!("WGSL parse error: {}", e.emit_to_string(source))),
            }
        }
    };

    let mut outputs = Vec::new();

    for entry_point in &module.entry_points {
        if entry_point.stage != naga::ShaderStage::Fragment {
            continue;
        }

        if let Some(ref result) = entry_point.function.result {
            if let Some(ref binding) = result.binding {
                // Direct return: -> @location(0) vec4<f32>
                if let naga::Binding::Location { location, .. } = binding {
                    let result_ty = &module.types[result.ty];
                    let name = entry_point.function.name.as_deref().unwrap_or("_output");
                    outputs.push(WgslFragmentOutput {
                        location: *location,
                        name: to_c_string(name),
                        component_count: naga_type_component_count(result_ty),
                    });
                }
            } else {
                // Struct return: -> GBufferOutput
                let result_ty = &module.types[result.ty];
                if let naga::TypeInner::Struct { members, .. } = &result_ty.inner {
                    for member in members {
                        if let Some(naga::Binding::Location { location, .. }) = &member.binding {
                            let member_name = member.name.as_deref().unwrap_or("_unnamed");
                            let member_ty = &module.types[member.ty];
                            outputs.push(WgslFragmentOutput {
                                location: *location,
                                name: to_c_string(member_name),
                                component_count: naga_type_component_count(member_ty),
                            });
                        }
                    }
                }
            }
        }
    }

    // Sort by location
    outputs.sort_by_key(|o| o.location);

    let output_count = outputs.len();
    let outputs_ptr = if outputs.is_empty() {
        ptr::null_mut()
    } else {
        let mut boxed = outputs.into_boxed_slice();
        let ptr = boxed.as_mut_ptr();
        std::mem::forget(boxed);
        ptr
    };

    WgslFragmentOutputReflectionResult {
        outputs: outputs_ptr,
        output_count,
        error: ptr::null_mut(),
    }
}

fn reflect_uniforms_internal(source: &str) -> WgslUniformReflectionResult {
    // Parse WGSL with Naga
    let module = match wgsl::parse_str(source) {
        Ok(m) => m,
        Err(e) => {
            return WgslUniformReflectionResult {
                buffers: ptr::null_mut(),
                buffer_count: 0,
                error: to_c_string(&format!("WGSL parse error: {}", e.emit_to_string(source))),
            }
        }
    };

    // Collect uniform buffers with their struct members
    let mut buffers = Vec::new();

    for (_handle, var) in module.global_variables.iter() {
        // Only process uniform buffers with bindings
        if var.space != naga::AddressSpace::Uniform {
            continue;
        }
        let binding = match &var.binding {
            Some(b) => b,
            None => continue,
        };

        let ty = &module.types[var.ty];

        // Get struct members if this is a struct type
        if let naga::TypeInner::Struct { members, span } = &ty.inner {
            let var_name = var.name.as_deref().unwrap_or("uniform");

            // Extract member information
            let mut uniform_members = Vec::new();
            for member in members {
                let member_name = member.name.as_deref().unwrap_or("_unnamed");
                let member_ty = &module.types[member.ty];
                let (type_id, size) = classify_uniform_member_type(member_ty);

                uniform_members.push(WgslUniformMember {
                    name: to_c_string(member_name),
                    offset: member.offset,
                    size,
                    type_id,
                });
            }

            let member_count = uniform_members.len();
            let members_ptr = if uniform_members.is_empty() {
                ptr::null_mut()
            } else {
                let mut boxed = uniform_members.into_boxed_slice();
                let ptr = boxed.as_mut_ptr();
                std::mem::forget(boxed);
                ptr
            };

            buffers.push(WgslUniformBuffer {
                group: binding.group,
                binding: binding.binding,
                name: to_c_string(var_name),
                members: members_ptr,
                member_count,
                total_size: *span,
            });
        }
    }

    // Convert to C-compatible array
    let buffer_count = buffers.len();
    let buffers_ptr = if buffers.is_empty() {
        ptr::null_mut()
    } else {
        let mut boxed = buffers.into_boxed_slice();
        let ptr = boxed.as_mut_ptr();
        std::mem::forget(boxed);
        ptr
    };

    WgslUniformReflectionResult {
        buffers: buffers_ptr,
        buffer_count,
        error: ptr::null_mut(),
    }
}

/// Classify a uniform buffer member type and return (type_id, size_in_bytes)
fn classify_uniform_member_type(ty: &naga::Type) -> (u32, u32) {
    match &ty.inner {
        naga::TypeInner::Scalar(scalar) => {
            match scalar.kind {
                naga::ScalarKind::Sint => (uniform_type::INT, 4),
                naga::ScalarKind::Uint => (uniform_type::UINT, 4),
                naga::ScalarKind::Float => (uniform_type::FLOAT, 4),
                _ => (uniform_type::UNKNOWN, 4),
            }
        }
        naga::TypeInner::Vector { size, scalar } => {
            let elem_size = 4u32;  // All scalar types we support are 4 bytes
            let vec_size = match size {
                naga::VectorSize::Bi => 2,
                naga::VectorSize::Tri => 3,
                naga::VectorSize::Quad => 4,
            };
            let type_id = match (scalar.kind, vec_size) {
                (naga::ScalarKind::Float, 2) => uniform_type::VEC2,
                (naga::ScalarKind::Float, 3) => uniform_type::VEC3,
                (naga::ScalarKind::Float, 4) => uniform_type::VEC4,
                _ => uniform_type::UNKNOWN,
            };
            (type_id, vec_size * elem_size)
        }
        naga::TypeInner::Matrix { columns, rows, scalar } => {
            // mat4x4<f32> is the most common case
            let cols = match columns {
                naga::VectorSize::Bi => 2,
                naga::VectorSize::Tri => 3,
                naga::VectorSize::Quad => 4,
            };
            let row_count = match rows {
                naga::VectorSize::Bi => 2,
                naga::VectorSize::Tri => 3,
                naga::VectorSize::Quad => 4,
            };
            if cols == 4 && row_count == 4 && scalar.kind == naga::ScalarKind::Float {
                (uniform_type::MAT4, 64)
            } else {
                // Other matrix sizes - return as unknown with computed size
                (uniform_type::UNKNOWN, cols * row_count * 4)
            }
        }
        _ => (uniform_type::UNKNOWN, 0),
    }
}

fn reflect_wgsl_internal(source: &str) -> WgslReflectionResult {
    // Parse WGSL with Naga
    let module = match wgsl::parse_str(source) {
        Ok(m) => m,
        Err(e) => {
            return WgslReflectionResult {
                bindings: ptr::null_mut(),
                binding_count: 0,
                error: to_c_string(&format!("WGSL parse error: {}", e.emit_to_string(source))),
                workgroup_size: [0, 0, 0],
            }
        }
    };


    // Build visibility map: which entry points use which global variables
    let mut var_visibility: std::collections::HashMap<naga::Handle<naga::GlobalVariable>, u32> =
        std::collections::HashMap::new();

    for (_, entry_point) in module.entry_points.iter().enumerate() {
        let stage_flag = match entry_point.stage {
            naga::ShaderStage::Vertex => shader_stage::VERTEX,
            naga::ShaderStage::Fragment => shader_stage::FRAGMENT,
            naga::ShaderStage::Compute => shader_stage::COMPUTE,
        };

        // Collect all global variables used by this entry point's function
        collect_function_globals(&module, &entry_point.function, &mut |handle| {
            *var_visibility.entry(handle).or_insert(0) |= stage_flag;
        });
    }

    // Compute fallback visibility from all entry points present in the module
    let module_stages: u32 = module.entry_points.iter().fold(0, |acc, ep| {
        acc | match ep.stage {
            naga::ShaderStage::Vertex => shader_stage::VERTEX,
            naga::ShaderStage::Fragment => shader_stage::FRAGMENT,
            naga::ShaderStage::Compute => shader_stage::COMPUTE,
        }
    });

    // Extract bindings from global variables
    let mut bindings = Vec::new();

    for (handle, var) in module.global_variables.iter() {
        if let Some(binding) = &var.binding {
            let ty = &module.types[var.ty];
            let mut classified = classify_type(&module, ty);

            // Refine buffer binding type based on address space
            if classified.binding_type == binding_type::UNIFORM {
                match var.space {
                    naga::AddressSpace::Storage { access } => {
                        if access.contains(naga::StorageAccess::STORE) {
                            classified.binding_type = binding_type::STORAGE;
                        } else {
                            classified.binding_type = binding_type::STORAGE_READ_ONLY;
                        }
                    }
                    naga::AddressSpace::Uniform => {} // already correct
                    _ => {}
                }
            }

            // Get visibility from actual entry point usage analysis.
            // Bindings not referenced by any entry point get no visibility
            // (rather than all stages), which is critical for staying within
            // the WebGPU 10-storage-buffer-per-stage limit.
            let visibility = var_visibility
                .get(&handle)
                .copied()
                .unwrap_or(0);

            bindings.push(WgslBinding {
                group: binding.group,
                binding: binding.binding,
                name: to_c_string(var.name.as_deref().unwrap_or("")),
                binding_type: classified.binding_type,
                texture_sample_type: classified.texture_sample_type,
                texture_dimension: classified.texture_dimension,
                sampler_type: classified.sampler_type,
                visibility,
                storage_texture_format: classified.storage_format,
                storage_texture_access: classified.storage_access,
            });
        }
    }


    // Extract workgroup_size from the first compute entry point
    let workgroup_size = module
        .entry_points
        .iter()
        .find(|ep| ep.stage == naga::ShaderStage::Compute)
        .map(|ep| ep.workgroup_size)
        .unwrap_or([1, 1, 1]);

    // Convert to C-compatible array
    let binding_count = bindings.len();
    let bindings_ptr = if bindings.is_empty() {
        ptr::null_mut()
    } else {
        let mut boxed = bindings.into_boxed_slice();
        let ptr = boxed.as_mut_ptr();
        std::mem::forget(boxed);
        ptr
    };

    WgslReflectionResult {
        bindings: bindings_ptr,
        binding_count,
        error: ptr::null_mut(),
        workgroup_size,
    }
}

fn collect_function_globals<F>(
    module: &naga::Module,
    function: &naga::Function,
    callback: &mut F,
) where
    F: FnMut(naga::Handle<naga::GlobalVariable>),
{
    // Recursively resolve expression to find GlobalVariable references
    fn resolve_to_global(
        function: &naga::Function,
        expr_handle: naga::Handle<naga::Expression>,
    ) -> Option<naga::Handle<naga::GlobalVariable>> {
        match &function.expressions[expr_handle] {
            naga::Expression::GlobalVariable(handle) => Some(*handle),
            naga::Expression::Access { base, .. }
            | naga::Expression::AccessIndex { base, .. } => resolve_to_global(function, *base),
            _ => None,
        }
    }

    // Check all expressions for global variable references
    for (_, expr) in function.expressions.iter() {
        match expr {
            naga::Expression::GlobalVariable(handle) => {
                callback(*handle);
            }
            // Handle texture sampling - the image/sampler arguments reference globals
            naga::Expression::ImageSample {
                image, sampler, ..
            } => {
                if let Some(handle) = resolve_to_global(function, *image) {
                    callback(handle);
                }
                if let Some(handle) = resolve_to_global(function, *sampler) {
                    callback(handle);
                }
            }
            naga::Expression::ImageLoad { image, .. }
            | naga::Expression::ImageQuery { image, .. } => {
                if let Some(handle) = resolve_to_global(function, *image) {
                    callback(handle);
                }
            }
            _ => {}
        }
    }

    // Also check function calls recursively (for named functions)
    for (_, expr) in function.expressions.iter() {
        if let naga::Expression::CallResult(func_handle) = expr {
            let called_func = &module.functions[*func_handle];
            collect_function_globals(module, called_func, callback);
        }
    }

    // Walk statements to find globals referenced only from statements
    // (e.g., textureStore is Statement::ImageStore, not an Expression)
    fn walk_block<F>(
        module: &naga::Module,
        function: &naga::Function,
        block: &naga::Block,
        callback: &mut F,
    ) where
        F: FnMut(naga::Handle<naga::GlobalVariable>),
    {
        for stmt in block.iter() {
            match stmt {
                naga::Statement::ImageStore { image, .. } => {
                    if let Some(handle) = resolve_to_global(function, *image) {
                        callback(handle);
                    }
                }
                naga::Statement::Store { pointer, .. } => {
                    if let Some(handle) = resolve_to_global(function, *pointer) {
                        callback(handle);
                    }
                }
                naga::Statement::Call { function: func_handle, .. } => {
                    let called_func = &module.functions[*func_handle];
                    collect_function_globals(module, called_func, callback);
                }
                naga::Statement::If {
                    accept, reject, ..
                } => {
                    walk_block(module, function, accept, callback);
                    walk_block(module, function, reject, callback);
                }
                naga::Statement::Switch { cases, .. } => {
                    for case in cases {
                        walk_block(module, function, &case.body, callback);
                    }
                }
                naga::Statement::Loop {
                    body, continuing, ..
                } => {
                    walk_block(module, function, body, callback);
                    walk_block(module, function, continuing, callback);
                }
                naga::Statement::Block(block) => {
                    walk_block(module, function, block, callback);
                }
                _ => {}
            }
        }
    }

    walk_block(module, function, &function.body, callback);
}

/// Classified binding info from Naga type analysis
struct ClassifiedBinding {
    binding_type: u32,
    texture_sample_type: u32,
    texture_dimension: u32,
    sampler_type: u32,
    storage_format: u32,
    storage_access: u32,
}

fn map_storage_format(format: naga::StorageFormat) -> u32 {
    match format {
        naga::StorageFormat::R32Float => storage_texture_format::R32FLOAT,
        naga::StorageFormat::R32Uint => storage_texture_format::R32UINT,
        naga::StorageFormat::R32Sint => storage_texture_format::R32SINT,
        naga::StorageFormat::Rg32Float => storage_texture_format::RG32FLOAT,
        naga::StorageFormat::Rg32Uint => storage_texture_format::RG32UINT,
        naga::StorageFormat::Rg32Sint => storage_texture_format::RG32SINT,
        naga::StorageFormat::Rgba8Unorm => storage_texture_format::RGBA8UNORM,
        naga::StorageFormat::Rgba8Snorm => storage_texture_format::RGBA8SNORM,
        naga::StorageFormat::Rgba8Uint => storage_texture_format::RGBA8UINT,
        naga::StorageFormat::Rgba8Sint => storage_texture_format::RGBA8SINT,
        naga::StorageFormat::Bgra8Unorm => storage_texture_format::BGRA8UNORM,
        naga::StorageFormat::Rgba16Float => storage_texture_format::RGBA16FLOAT,
        naga::StorageFormat::Rgba16Uint => storage_texture_format::RGBA16UINT,
        naga::StorageFormat::Rgba16Sint => storage_texture_format::RGBA16SINT,
        naga::StorageFormat::Rgba32Float => storage_texture_format::RGBA32FLOAT,
        naga::StorageFormat::Rgba32Uint => storage_texture_format::RGBA32UINT,
        naga::StorageFormat::Rgba32Sint => storage_texture_format::RGBA32SINT,
        naga::StorageFormat::R16Float => storage_texture_format::R16FLOAT,
        naga::StorageFormat::Rg16Float => storage_texture_format::RG16FLOAT,
        naga::StorageFormat::R8Unorm => storage_texture_format::R8UNORM,
        naga::StorageFormat::Rg8Unorm => storage_texture_format::RG8UNORM,
        _ => storage_texture_format::UNDEFINED,
    }
}

fn map_storage_access(access: naga::StorageAccess) -> u32 {
    let has_load = access.contains(naga::StorageAccess::LOAD);
    let has_store = access.contains(naga::StorageAccess::STORE);
    match (has_load, has_store) {
        (true, true) => storage_texture_access::READ_WRITE,
        (true, false) => storage_texture_access::READ_ONLY,
        _ => storage_texture_access::WRITE_ONLY,
    }
}

fn classify_type(
    module: &naga::Module,
    ty: &naga::Type,
) -> ClassifiedBinding {
    match &ty.inner {
        naga::TypeInner::Scalar { .. }
        | naga::TypeInner::Vector { .. }
        | naga::TypeInner::Matrix { .. }
        | naga::TypeInner::Struct { .. }
        | naga::TypeInner::Array { .. } => {
            ClassifiedBinding {
                binding_type: binding_type::UNIFORM,
                texture_sample_type: 0,
                texture_dimension: 0,
                sampler_type: 0,
                storage_format: 0,
                storage_access: 0,
            }
        }

        naga::TypeInner::Image { dim, arrayed, class } => {
            let texture_dim = match (dim, arrayed) {
                (naga::ImageDimension::D1, false) => texture_dimension::D1,
                (naga::ImageDimension::D2, false) => texture_dimension::D2,
                (naga::ImageDimension::D2, true) => texture_dimension::D2_ARRAY,
                (naga::ImageDimension::D3, false) => texture_dimension::D3,
                (naga::ImageDimension::Cube, false) => texture_dimension::CUBE,
                (naga::ImageDimension::Cube, true) => texture_dimension::CUBE_ARRAY,
                _ => texture_dimension::D2,
            };

            match class {
                naga::ImageClass::Sampled { kind, multi: _ } => {
                    let st = match kind {
                        naga::ScalarKind::Float => texture_sample_type::FLOAT,
                        naga::ScalarKind::Sint => texture_sample_type::SINT,
                        naga::ScalarKind::Uint => texture_sample_type::UINT,
                        _ => texture_sample_type::FLOAT,
                    };
                    ClassifiedBinding {
                        binding_type: binding_type::TEXTURE,
                        texture_sample_type: st,
                        texture_dimension: texture_dim,
                        sampler_type: 0,
                        storage_format: 0,
                        storage_access: 0,
                    }
                }
                naga::ImageClass::Depth { multi: _ } => {
                    ClassifiedBinding {
                        binding_type: binding_type::TEXTURE,
                        texture_sample_type: texture_sample_type::DEPTH,
                        texture_dimension: texture_dim,
                        sampler_type: 0,
                        storage_format: 0,
                        storage_access: 0,
                    }
                }
                naga::ImageClass::Storage { format, access } => {
                    ClassifiedBinding {
                        binding_type: binding_type::STORAGE_TEXTURE,
                        texture_sample_type: 0,
                        texture_dimension: texture_dim,
                        sampler_type: 0,
                        storage_format: map_storage_format(*format),
                        storage_access: map_storage_access(*access),
                    }
                }
            }
        }

        naga::TypeInner::Sampler { comparison } => {
            let st = if *comparison {
                sampler_type::COMPARISON
            } else {
                sampler_type::FILTERING
            };
            ClassifiedBinding {
                binding_type: binding_type::SAMPLER,
                texture_sample_type: 0,
                texture_dimension: 0,
                sampler_type: st,
                storage_format: 0,
                storage_access: 0,
            }
        }

        naga::TypeInner::BindingArray { base, .. } => {
            let base_ty = &module.types[*base];
            classify_type(module, base_ty)
        }

        _ => ClassifiedBinding {
            binding_type: binding_type::UNIFORM,
            texture_sample_type: 0,
            texture_dimension: 0,
            sampler_type: 0,
            storage_format: 0,
            storage_access: 0,
        },
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::ffi::CString;

    #[test]
    fn test_compile_simple_wgsl() {
        // WESL is a superset of WGSL, so plain WGSL should compile
        let source = CString::new(
            r#"
@vertex
fn vs_main() -> @builtin(position) vec4f {
    return vec4f(0.0, 0.0, 0.0, 1.0);
}

@fragment
fn fs_main() -> @location(0) vec4f {
    return vec4f(1.0, 0.0, 0.0, 1.0);
}
"#,
        )
        .unwrap();
        let filename = CString::new("test.wesl").unwrap();

        unsafe {
            let result = wesl_compile_source(source.as_ptr(), filename.as_ptr());
            if !result.error.is_null() {
                let error = CStr::from_ptr(result.error).to_str().unwrap();
                panic!("Compilation failed: {}", error);
            }
            assert!(!result.wgsl.is_null());
            let wgsl = CStr::from_ptr(result.wgsl).to_str().unwrap();
            assert!(wgsl.contains("vs_main"));
            wesl_free_result(result);
        }
    }
}

#[cfg(test)]
mod enable_directive_tests {
    use super::*;

    /// Verifies that WESL passes through the `enable` directive for
    /// Dawn/Chromium extensions like `chromium_internal_graphite`.
    #[test]
    fn test_enable_directive_passthrough() {
        let source = r#"
enable chromium_internal_graphite;

@group(0) @binding(0) var ao_output: texture_storage_2d<r8unorm, write>;

@compute @workgroup_size(8, 8, 1)
fn main(@builtin(global_invocation_id) global_id: vec3<u32>) {
    let pixel_coord = vec2<i32>(global_id.xy);
    textureStore(ao_output, pixel_coord, vec4<f32>(1.0));
}
"#;
        let filename = "test.wesl";
        let result = compile_wesl_source_internal(source, filename);

        if !result.error.is_null() {
            unsafe {
                let error = std::ffi::CStr::from_ptr(result.error).to_str().unwrap();
                panic!("WESL compilation failed: {}", error);
            }
        }

        unsafe {
            let wgsl = std::ffi::CStr::from_ptr(result.wgsl).to_str().unwrap();

            assert!(
                wgsl.contains("enable chromium_internal_graphite"),
                "Enable directive should be preserved in output. Got:\n{}", wgsl
            );
            assert!(wgsl.contains("r8unorm"), "r8unorm format should be in output");

            wesl_free_result(result);
        }
    }
}

#[cfg(test)]
mod shader_compilation_tests {
    use super::*;

    #[test]
    fn test_reflection_populates_binding_names() {
        let source = r#"
struct Params { value: f32 }
@group(0) @binding(0) var<uniform> my_params: Params;
@group(0) @binding(1) var my_texture: texture_2d<f32>;
@group(1) @binding(0) var<storage, read> my_data: array<u32>;

@group(0) @binding(2) var dst: texture_storage_2d<rgba8unorm, write>;

@compute @workgroup_size(1)
fn main(@builtin(global_invocation_id) gid: vec3<u32>) {
    let v = my_params.value;
    let t = textureLoad(my_texture, vec2<i32>(0, 0), 0);
    let d = my_data[0];
    textureStore(dst, vec2<i32>(gid.xy), vec4(v, f32(d), t.r, 1.0));
}
"#;
        let c_source = std::ffi::CString::new(source).unwrap();
        let result = unsafe { wgsl_reflect(c_source.as_ptr()) };
        if !result.error.is_null() {
            unsafe {
                let err = std::ffi::CStr::from_ptr(result.error).to_str().unwrap();
                panic!("reflection failed: {}", err);
            }
        }
        assert_eq!(result.binding_count, 4);

        unsafe {
            let bindings = std::slice::from_raw_parts(result.bindings, result.binding_count);
            let names: Vec<String> = bindings.iter().map(|b| {
                std::ffi::CStr::from_ptr(b.name).to_str().unwrap().to_string()
            }).collect();

            assert!(names.contains(&"my_params".to_string()), "my_params not found: {:?}", names);
            assert!(names.contains(&"my_texture".to_string()), "my_texture not found: {:?}", names);
            assert!(names.contains(&"my_data".to_string()), "my_data not found: {:?}", names);
            assert!(names.contains(&"dst".to_string()), "dst not found: {:?}", names);

            wgsl_free_reflection(result);
        }
    }

    /// Compiles badlands' own `shaders/common/frame.wesl` (sampo's shader
    /// library — geometry/screen_rect_shadow.wesl, compute/glitch.wesl, etc.
    /// — isn't present in this repo, so those upstream tests don't carry
    /// over) through the real `wesl_compile_file_with_dirs` C ABI entry
    /// point, then feeds the resulting WGSL through `wgsl_reflect_uniforms`
    /// and checks the FrameUniforms uniform buffer comes back out. This is
    /// the exact WESL-compile -> naga-reflect round trip
    /// `GpuPipelineGenerator::GetPipeline` performs at runtime.
    #[test]
    fn test_frame_wesl_compile_with_dirs_and_reflect_uniforms() {
        // Cargo always runs unit test binaries with cwd set to the crate's
        // manifest directory (src/crates/wesl), not the directory `cargo
        // test` was invoked from. Use CARGO_MANIFEST_DIR to find the
        // repo-root shaders/ directory reliably regardless of invocation cwd.
        let shader_dir = CString::new(concat!(env!("CARGO_MANIFEST_DIR"), "/../../../shaders"))
            .unwrap();
        let module_path = CString::new("common/frame").unwrap();

        let result = unsafe {
            wesl_compile_file_with_dirs(
                shader_dir.as_ptr(),
                module_path.as_ptr(),
                ptr::null(),
                0,
                ptr::null(),
                0,
            )
        };

        if !result.error.is_null() {
            unsafe {
                let error = CStr::from_ptr(result.error).to_str().unwrap();
                panic!(
                    "common/frame.wesl compilation via wesl_compile_file_with_dirs failed: {}",
                    error
                );
            }
        }

        let wgsl_owned = unsafe {
            let wgsl = CStr::from_ptr(result.wgsl).to_str().unwrap();
            assert!(
                wgsl.contains("FrameUniforms"),
                "FrameUniforms struct missing from compiled WGSL:\n{}",
                wgsl
            );
            wgsl.to_string()
        };
        unsafe { wesl_free_result(result) };

        let wgsl_c = CString::new(wgsl_owned).unwrap();
        let uniforms = unsafe { wgsl_reflect_uniforms(wgsl_c.as_ptr()) };
        if !uniforms.error.is_null() {
            unsafe {
                let error = CStr::from_ptr(uniforms.error).to_str().unwrap();
                panic!("wgsl_reflect_uniforms failed: {}", error);
            }
        }
        assert!(uniforms.buffer_count >= 1, "expected at least one uniform buffer");

        unsafe {
            let buffers = std::slice::from_raw_parts(uniforms.buffers, uniforms.buffer_count);
            let frame_buffer = buffers.iter().find(|b| {
                !b.name.is_null() && CStr::from_ptr(b.name).to_str().unwrap() == "frame"
            });
            assert!(
                frame_buffer.is_some(),
                "expected a `frame` uniform buffer binding (FrameUniforms) among: {:?}",
                buffers.iter().map(|b| CStr::from_ptr(b.name).to_str().unwrap()).collect::<Vec<_>>()
            );
            assert!(
                frame_buffer.unwrap().member_count > 0,
                "FrameUniforms should have reflected struct members"
            );

            wgsl_free_uniform_reflection(uniforms);
        }
    }
}

#[cfg(test)]
mod storage_texture_visibility_tests {
    use super::*;

    #[test]
    fn test_compute_storage_texture_gets_compute_visibility() {
        let source = r#"
@group(0) @binding(0) var output_tex: texture_storage_2d<rgba8unorm, write>;

@compute @workgroup_size(8, 8)
fn main(@builtin(global_invocation_id) id: vec3<u32>) {
    textureStore(output_tex, vec2<i32>(id.xy), vec4<f32>(1.0, 0.0, 0.0, 1.0));
}
"#;
        let result = reflect_wgsl_internal(source);
        assert!(result.error.is_null(), "Reflection should succeed");
        assert_eq!(result.binding_count, 1);

        unsafe {
            let binding = &*result.bindings;
            assert_eq!(binding.group, 0);
            assert_eq!(binding.binding, 0);
            assert_eq!(
                binding.binding_type,
                binding_type::STORAGE_TEXTURE,
                "Should be classified as STORAGE_TEXTURE"
            );
            assert_eq!(
                binding.visibility,
                shader_stage::COMPUTE,
                "Write-only storage texture in compute shader must have COMPUTE visibility"
            );
            assert_eq!(
                binding.storage_texture_format,
                storage_texture_format::RGBA8UNORM,
                "Format should be RGBA8UNORM"
            );
            assert_eq!(
                binding.storage_texture_access,
                storage_texture_access::WRITE_ONLY,
                "Access should be WRITE_ONLY"
            );

            wgsl_free_reflection(result);
        }
    }

    #[test]
    fn test_storage_texture_write_only_no_query() {
        let source = r#"
@group(0) @binding(0) var output_tex: texture_storage_2d<rgba8unorm, write>;

@compute @workgroup_size(8, 8)
fn main(@builtin(global_invocation_id) id: vec3<u32>) {
    textureStore(output_tex, vec2<i32>(id.xy), vec4<f32>(1.0, 0.0, 0.0, 1.0));
}
"#;
        let result = reflect_wgsl_internal(source);
        assert!(result.error.is_null(), "Reflection should succeed");
        assert_eq!(result.binding_count, 1);

        unsafe {
            let binding = &*result.bindings;
            assert_eq!(binding.group, 0);
            assert_eq!(binding.binding, 0);
            assert_eq!(
                binding.binding_type,
                binding_type::STORAGE_TEXTURE,
                "Should be classified as STORAGE_TEXTURE"
            );
            assert_eq!(
                binding.visibility,
                shader_stage::COMPUTE,
                "Write-only storage texture with no expression-level references must have COMPUTE visibility"
            );

            wgsl_free_reflection(result);
        }
    }

    #[test]
    fn test_mixed_entry_point_visibility() {
        let source = r#"
struct Params { value: f32 }
@group(0) @binding(0) var<uniform> params: Params;

@vertex fn vs_main() -> @builtin(position) vec4<f32> {
    return vec4<f32>(params.value, 0.0, 0.0, 1.0);
}

@fragment fn fs_main() -> @location(0) vec4<f32> {
    return vec4<f32>(params.value, 0.0, 0.0, 1.0);
}

@compute @workgroup_size(1)
fn cs_main() {
    // no-op, but compute entry point exists
}
"#;
        let result = reflect_wgsl_internal(source);
        assert!(result.error.is_null(), "Reflection should succeed");
        assert_eq!(result.binding_count, 1);

        unsafe {
            let binding = &*result.bindings;
            assert_eq!(binding.group, 0);
            assert_eq!(binding.binding, 0);
            assert_eq!(
                binding.binding_type,
                binding_type::UNIFORM,
                "Should be classified as UNIFORM"
            );
            assert_eq!(
                binding.visibility,
                shader_stage::VERTEX | shader_stage::FRAGMENT,
                "Uniform used in vertex and fragment but not compute should have VERTEX | FRAGMENT visibility"
            );

            wgsl_free_reflection(result);
        }
    }
}
