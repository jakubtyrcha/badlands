// C ABI for the `wesl_ffi` Rust crate (src/crates/wesl): compiles .wesl
// shader modules to WGSL and reflects WGSL bindings/uniforms/vertex-inputs/
// fragment-outputs via naga. Linked into the badlands C++ app via Corrosion.
//
// Ported verbatim from sampo's `src/wesl_ffi.h`.
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// WESL Compilation
// ============================================================================

typedef struct {
  char* wgsl;   // On success: compiled WGSL, otherwise null
  char* error;  // On failure: error message, otherwise null
} WeslCompileResult;

// Compile a WESL shader file to WGSL.
// shader_dir: path to the directory containing shaders
// module_path: module path to compile, e.g., "main" or "effects::bloom"
WeslCompileResult wesl_compile_file(const char* shader_dir,
                                    const char* module_path);

// Compile a WESL shader file to WGSL with feature flags for conditional
// compilation. shader_dir: path to the directory containing shaders
// module_path: module path to compile, e.g., "main" or "effects::bloom"
// features: array of feature names (null-terminated strings) to enable
// feature_count: number of features in the array
WeslCompileResult wesl_compile_file_with_features(const char* shader_dir,
                                                  const char* module_path,
                                                  const char** features,
                                                  size_t feature_count);

// Compile a WESL shader file to WGSL with additional search directories.
// shader_dir: primary directory containing shaders
// module_path: module path to compile
// features: array of feature names (null-terminated strings) to enable (null if none)
// feature_count: number of features in the array
// additional_dirs: array of additional directory paths for import resolution (null if
// none) additional_dir_count: number of additional directories
WeslCompileResult wesl_compile_file_with_dirs(const char* shader_dir,
                                               const char* module_path,
                                               const char** features,
                                               size_t feature_count,
                                               const char** additional_dirs,
                                               size_t additional_dir_count);

// Compile WESL source code directly.
// source: WESL source code
// filename: virtual filename for error messages
WeslCompileResult wesl_compile_source(const char* source, const char* filename);

// Free a string returned by WESL functions.
void wesl_free_string(char* s);

// Free a WeslCompileResult (frees both wgsl and error strings).
void wesl_free_result(WeslCompileResult result);

// ============================================================================
// WGSL Reflection (via Naga)
// ============================================================================

// Binding type constants
enum WgslBindingType {
  WGSL_BINDING_UNIFORM = 0,
  WGSL_BINDING_STORAGE = 1,
  WGSL_BINDING_STORAGE_READ_ONLY = 2,
  WGSL_BINDING_TEXTURE = 3,
  WGSL_BINDING_STORAGE_TEXTURE = 4,
  WGSL_BINDING_SAMPLER = 5,
};

// Texture sample type constants
enum WgslTextureSampleType {
  WGSL_TEXTURE_SAMPLE_FLOAT = 0,
  WGSL_TEXTURE_SAMPLE_DEPTH = 1,
  WGSL_TEXTURE_SAMPLE_SINT = 2,
  WGSL_TEXTURE_SAMPLE_UINT = 3,
  WGSL_TEXTURE_SAMPLE_UNFILTERABLE_FLOAT = 4,
};

// Texture dimension constants
enum WgslTextureDimension {
  WGSL_TEXTURE_DIM_1D = 0,
  WGSL_TEXTURE_DIM_2D = 1,
  WGSL_TEXTURE_DIM_2D_ARRAY = 2,
  WGSL_TEXTURE_DIM_3D = 3,
  WGSL_TEXTURE_DIM_CUBE = 4,
  WGSL_TEXTURE_DIM_CUBE_ARRAY = 5,
};

// Sampler type constants
enum WgslSamplerType {
  WGSL_SAMPLER_FILTERING = 0,
  WGSL_SAMPLER_NON_FILTERING = 1,
  WGSL_SAMPLER_COMPARISON = 2,
};

// Storage texture format constants (matches naga::StorageFormat subset)
enum WgslStorageTextureFormat {
  WGSL_STORAGE_FORMAT_UNDEFINED = 0,
  WGSL_STORAGE_FORMAT_R32FLOAT = 1,
  WGSL_STORAGE_FORMAT_R32UINT = 2,
  WGSL_STORAGE_FORMAT_R32SINT = 3,
  WGSL_STORAGE_FORMAT_RG32FLOAT = 4,
  WGSL_STORAGE_FORMAT_RG32UINT = 5,
  WGSL_STORAGE_FORMAT_RG32SINT = 6,
  WGSL_STORAGE_FORMAT_RGBA8UNORM = 7,
  WGSL_STORAGE_FORMAT_RGBA8SNORM = 8,
  WGSL_STORAGE_FORMAT_RGBA8UINT = 9,
  WGSL_STORAGE_FORMAT_RGBA8SINT = 10,
  WGSL_STORAGE_FORMAT_BGRA8UNORM = 11,
  WGSL_STORAGE_FORMAT_RGBA16FLOAT = 12,
  WGSL_STORAGE_FORMAT_RGBA16UINT = 13,
  WGSL_STORAGE_FORMAT_RGBA16SINT = 14,
  WGSL_STORAGE_FORMAT_RGBA32FLOAT = 15,
  WGSL_STORAGE_FORMAT_RGBA32UINT = 16,
  WGSL_STORAGE_FORMAT_RGBA32SINT = 17,
  WGSL_STORAGE_FORMAT_R16FLOAT = 18,
  WGSL_STORAGE_FORMAT_RG16FLOAT = 19,
  WGSL_STORAGE_FORMAT_R8UNORM = 20,
  WGSL_STORAGE_FORMAT_RG8UNORM = 21,
};

// Storage texture access constants
enum WgslStorageTextureAccess {
  WGSL_STORAGE_ACCESS_WRITE_ONLY = 0,
  WGSL_STORAGE_ACCESS_READ_ONLY = 1,
  WGSL_STORAGE_ACCESS_READ_WRITE = 2,
};

// Shader stage visibility flags (can be OR'd together)
enum WgslShaderStage {
  WGSL_STAGE_VERTEX = 1,
  WGSL_STAGE_FRAGMENT = 2,
  WGSL_STAGE_COMPUTE = 4,
};

// Uniform member type constants
enum WgslUniformType {
  WGSL_UNIFORM_TYPE_INT = 0,
  WGSL_UNIFORM_TYPE_UINT = 1,
  WGSL_UNIFORM_TYPE_FLOAT = 2,
  WGSL_UNIFORM_TYPE_VEC2 = 3,
  WGSL_UNIFORM_TYPE_VEC3 = 4,
  WGSL_UNIFORM_TYPE_VEC4 = 5,
  WGSL_UNIFORM_TYPE_MAT4 = 6,
  WGSL_UNIFORM_TYPE_UNKNOWN = 255,
};

// Vertex format constants (matches wgpu::VertexFormat)
enum WgslVertexFormat {
  WGSL_VERTEX_FORMAT_FLOAT32 = 0,
  WGSL_VERTEX_FORMAT_FLOAT32X2 = 1,
  WGSL_VERTEX_FORMAT_FLOAT32X3 = 2,
  WGSL_VERTEX_FORMAT_FLOAT32X4 = 3,
  WGSL_VERTEX_FORMAT_SINT32 = 4,
  WGSL_VERTEX_FORMAT_SINT32X2 = 5,
  WGSL_VERTEX_FORMAT_SINT32X3 = 6,
  WGSL_VERTEX_FORMAT_SINT32X4 = 7,
  WGSL_VERTEX_FORMAT_UINT32 = 8,
  WGSL_VERTEX_FORMAT_UINT32X2 = 9,
  WGSL_VERTEX_FORMAT_UINT32X3 = 10,
  WGSL_VERTEX_FORMAT_UINT32X4 = 11,
  WGSL_VERTEX_FORMAT_UNDEFINED = 255,
};

// A single binding extracted from a WGSL shader
typedef struct {
  uint32_t group;
  uint32_t binding;
  char* name;                         // Variable name (freed by wgsl_free_reflection)
  uint32_t binding_type;              // WgslBindingType
  uint32_t texture_sample_type;       // WgslTextureSampleType (for textures)
  uint32_t texture_dimension;         // WgslTextureDimension (for textures)
  uint32_t sampler_type;              // WgslSamplerType (for samplers)
  uint32_t visibility;                // WgslShaderStage flags
  uint32_t storage_texture_format;    // WgslStorageTextureFormat (for storage textures)
  uint32_t storage_texture_access;    // WgslStorageTextureAccess (for storage textures)
} WgslBinding;

// Result of WGSL reflection
typedef struct {
  WgslBinding* bindings;  // Array of bindings (null on error)
  size_t binding_count;   // Number of bindings
  char* error;            // Error message (null on success)
  uint32_t workgroup_size[3];  // Compute shader workgroup size (0,0,0 on error)
} WgslReflectionResult;

// Reflect a WGSL shader to extract binding information.
// wgsl_source: WGSL source code (null-terminated)
WgslReflectionResult wgsl_reflect(const char* wgsl_source);

// Free a WgslReflectionResult (frees bindings array and error string).
void wgsl_free_reflection(WgslReflectionResult result);

// ============================================================================
// Uniform Buffer Reflection
// ============================================================================

// A single member of a uniform buffer struct
typedef struct {
  char* name;        // Member name (e.g., "displacement_scale")
  uint32_t offset;   // Byte offset in buffer
  uint32_t size;     // Size in bytes
  uint32_t type_id;  // WgslUniformType
} WgslUniformMember;

// A uniform buffer with its struct members
typedef struct {
  uint32_t group;
  uint32_t binding;
  char* name;                  // Struct/var name (e.g., "object")
  WgslUniformMember* members;  // Array of members
  size_t member_count;         // Number of members
  uint32_t total_size;         // Total buffer size
} WgslUniformBuffer;

// Result of uniform buffer reflection
typedef struct {
  WgslUniformBuffer* buffers;  // Array of uniform buffers (null on error)
  size_t buffer_count;         // Number of buffers
  char* error;                 // Error message (null on success)
} WgslUniformReflectionResult;

// Reflect a WGSL shader to extract uniform buffer member information.
// wgsl_source: WGSL source code (null-terminated)
WgslUniformReflectionResult wgsl_reflect_uniforms(const char* wgsl_source);

// Free a WgslUniformReflectionResult (frees all nested allocations).
void wgsl_free_uniform_reflection(WgslUniformReflectionResult result);

// ============================================================================
// Vertex Input Reflection
// ============================================================================

// A vertex input extracted from a WGSL shader
typedef struct {
  uint32_t location;
  char* name;
  uint32_t format;  // WgslVertexFormat
} WgslVertexInput;

// Result of vertex input reflection
typedef struct {
  WgslVertexInput* inputs;  // Array of vertex inputs (null on error)
  size_t input_count;       // Number of inputs
  char* error;              // Error message (null on success)
} WgslVertexInputReflectionResult;

// Reflect a WGSL shader to extract vertex input information.
// wgsl_source: WGSL source code (null-terminated)
WgslVertexInputReflectionResult wgsl_reflect_vertex_inputs(
    const char* wgsl_source);

// Free a WgslVertexInputReflectionResult (frees all nested allocations).
void wgsl_free_vertex_input_reflection(WgslVertexInputReflectionResult result);

// ============================================================================
// Fragment Output Reflection
// ============================================================================

// A fragment output extracted from a WGSL shader
typedef struct {
  uint32_t location;
  char* name;
  uint32_t component_count;  // 1-4
} WgslFragmentOutput;

// Result of fragment output reflection
typedef struct {
  WgslFragmentOutput* outputs;  // Array of fragment outputs (null on error)
  size_t output_count;          // Number of outputs
  char* error;                  // Error message (null on success)
} WgslFragmentOutputReflectionResult;

// Reflect a WGSL shader to extract fragment output information.
// wgsl_source: WGSL source code (null-terminated)
WgslFragmentOutputReflectionResult wgsl_reflect_fragment_outputs(
    const char* wgsl_source);

// Free a WgslFragmentOutputReflectionResult (frees all nested allocations).
void wgsl_free_fragment_output_reflection(
    WgslFragmentOutputReflectionResult result);

#ifdef __cplusplus
}
#endif
