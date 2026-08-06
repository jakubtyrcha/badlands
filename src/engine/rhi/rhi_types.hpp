#pragma once

// Shared vocabulary for the RHI. No backend types, no `wgpu::`, no Metal.
//
// The vocabulary is deliberately WebGPU-shaped (decision D2 in
// docs/superpowers/specs/2026-08-03-rhi-slang-exploration.md): ~89% of the
// engine's ~3,350 existing `wgpu::` references are either target-neutral or
// plain handle renames, so keeping the shape makes the eventual port
// mechanical rather than a rewrite. Where WebGPU's shape is a liability it is
// dropped rather than copied -- see `ResourceState` below, which WebGPU has no
// equivalent for because it hides hazards entirely.
//
// MVP scope is documented per-item. Items marked "not in MVP" are named here
// so the enum does not have to change shape when they land; nothing in the
// terrain path needs them.

#include <cstdint>
#include <string>
#include <vector>

namespace badlands::rhi {

// ============================================================================
// Formats
// ============================================================================

// Only the formats the MVP path actually uses, plus the surface formats. Add
// as needed -- an unlisted format is a compile error rather than a silent
// fallback.
enum class Format : uint8_t {
  Undefined = 0,

  // Color, 8-bit
  R8Unorm,
  RGBA8Unorm,
  RGBA8UnormSrgb,
  BGRA8Unorm,
  BGRA8UnormSrgb,

  // Color, 16-bit float
  RG16Float,
  RGBA16Float,

  // Color, 32-bit
  R32Float,
  R32Uint,   // the visibility buffer's cluster/triangle id target
  RG32Uint,  // Metal requires this for 64-bit texture atomics (D3 / Apple8+)
  RGBA32Float,

  // Depth. Reversed-Z everywhere: clears to 0.0, compares GreaterEqual.
  Depth32Float,
};

// Bytes per texel for uncompressed formats. Returns 0 for Undefined.
constexpr uint32_t FormatByteSize(Format f) {
  switch (f) {
    case Format::R8Unorm: return 1;
    case Format::RG16Float: return 4;
    case Format::RGBA8Unorm:
    case Format::RGBA8UnormSrgb:
    case Format::BGRA8Unorm:
    case Format::BGRA8UnormSrgb:
    case Format::R32Float:
    case Format::R32Uint:
    case Format::Depth32Float: return 4;
    case Format::RGBA16Float:
    case Format::RG32Uint: return 8;
    case Format::RGBA32Float: return 16;
    case Format::Undefined: return 0;
  }
  return 0;
}

constexpr bool IsDepthFormat(Format f) { return f == Format::Depth32Float; }

// ============================================================================
// Usage flags
// ============================================================================

enum class BufferUsage : uint32_t {
  None = 0,
  CopySrc = 1u << 0,
  CopyDst = 1u << 1,
  Index = 1u << 2,
  Uniform = 1u << 3,
  Storage = 1u << 4,
  Indirect = 1u << 5,
  MapRead = 1u << 6,
  // No Vertex: the MVP pulls vertex data from storage buffers via the vertex
  // id, so there are no vertex input layouts at all. Vertex buffers land with
  // the wider port if anything still needs them.
};

enum class TextureUsage : uint32_t {
  None = 0,
  CopySrc = 1u << 0,
  CopyDst = 1u << 1,
  Sampled = 1u << 2,
  RenderTarget = 1u << 3,
  DepthStencil = 1u << 4,
  Storage = 1u << 5,  // not in MVP; GTAO's R8 storage target is the first user
};

enum class ShaderStage : uint32_t {
  None = 0,
  Vertex = 1u << 0,
  Fragment = 1u << 1,
  Compute = 1u << 2,
  // Mesh/Task land with the visibility-buffer hardware path (D10).

  // MVP binds every table to all stages, because Slang's ProgramLayout does
  // not prune globals per entry point so per-binding visibility is not
  // derivable (probe B). Correct, slightly wasteful, revisited only on
  // evidence.
  AllGraphics = Vertex | Fragment,
  All = Vertex | Fragment | Compute,
};

// Bitwise helpers. Defined per-enum rather than via a template so an
// unintended enum never silently gains operators.
#define BADLANDS_RHI_FLAGS(E)                                              \
  constexpr E operator|(E a, E b) {                                        \
    return static_cast<E>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b)); \
  }                                                                        \
  constexpr E operator&(E a, E b) {                                        \
    return static_cast<E>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b)); \
  }                                                                        \
  constexpr E& operator|=(E& a, E b) { a = a | b; return a; }              \
  constexpr bool Any(E v) { return static_cast<uint32_t>(v) != 0u; }       \
  constexpr bool Has(E v, E bit) { return Any(v & bit); }

BADLANDS_RHI_FLAGS(BufferUsage)
BADLANDS_RHI_FLAGS(TextureUsage)
BADLANDS_RHI_FLAGS(ShaderStage)

// ============================================================================
// Resource state -- the one place we deliberately do NOT follow WebGPU
// ============================================================================

// WebGPU hides hazards; Metal tracks them automatically; DX12 does neither.
// So the front end declares intent explicitly, the Metal backend ignores it,
// and the validation decorator CHECKS it as bookkeeping over the command
// stream -- with no GPU involved.
//
// This is why the check exists at all: Metal renders correctly whether or not
// intent was declared, so Metal can never reveal a missing declaration. The
// decorator can, on any machine, in the fast test suite. DX12 may still arrive
// with barrier bugs; the point is that the assertions that pin them down
// already exist by then.
enum class ResourceState : uint8_t {
  Undefined = 0,
  ShaderRead,    // sampled or read-only storage, any stage
  ShaderWrite,   // read-write storage
  RenderTarget,  // color attachment
  DepthWrite,    // depth attachment, writing
  DepthRead,     // depth attachment, read-only / sampled depth
  IndirectArg,   // source of a DrawIndirect / DispatchIndirect
  CopySrc,
  CopyDst,
};

const char* ToString(ResourceState s);

// ============================================================================
// Pipeline state
// ============================================================================

enum class CompareFunction : uint8_t {
  Never, Less, LessEqual, Greater, GreaterEqual, Equal, NotEqual, Always,
};

enum class CullMode : uint8_t { None, Front, Back };
enum class FrontFace : uint8_t { Ccw, Cw };
enum class PrimitiveTopology : uint8_t { TriangleList, TriangleStrip, LineList };
enum class IndexFormat : uint8_t { Uint16, Uint32 };

enum class FilterMode : uint8_t { Nearest, Linear };
enum class AddressMode : uint8_t { ClampToEdge, Repeat, MirrorRepeat };

enum class LoadOp : uint8_t { Load, Clear, DontCare };
enum class StoreOp : uint8_t { Store, Discard };

// ============================================================================
// Descriptors
// ============================================================================

struct BufferDesc {
  uint64_t size = 0;
  BufferUsage usage = BufferUsage::None;
  std::string label;  // debug only; backends may surface it in captures
};

struct TextureDesc {
  uint32_t width = 1;
  uint32_t height = 1;
  uint32_t array_layers = 1;  // >1 makes it a 2D array (terrain layer packs)
  uint32_t mip_levels = 1;
  Format format = Format::Undefined;
  TextureUsage usage = TextureUsage::None;
  std::string label;
};

struct SamplerDesc {
  FilterMode mag_filter = FilterMode::Linear;
  FilterMode min_filter = FilterMode::Linear;
  FilterMode mip_filter = FilterMode::Linear;
  AddressMode address_u = AddressMode::Repeat;
  AddressMode address_v = AddressMode::Repeat;
  uint16_t max_anisotropy = 1;
  std::string label;
};

// A view onto a texture. Defaults select the whole resource.
struct TextureViewDesc {
  uint32_t base_mip = 0;
  uint32_t mip_count = 0;    // 0 = all remaining
  uint32_t base_layer = 0;
  uint32_t layer_count = 0;  // 0 = all remaining
  std::string label;
};

struct ColorAttachment {
  class ITextureView* view = nullptr;
  LoadOp load_op = LoadOp::Clear;
  StoreOp store_op = StoreOp::Store;
  float clear_color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
};

struct DepthAttachment {
  class ITextureView* view = nullptr;
  LoadOp load_op = LoadOp::Clear;
  StoreOp store_op = StoreOp::Store;
  // Reversed-Z: far is 0.0. See the project-wide invariant.
  float clear_depth = 0.0f;
  bool read_only = false;
};

struct RenderPassDesc {
  std::vector<ColorAttachment> color_attachments;
  DepthAttachment depth_attachment;  // view == nullptr means no depth
  std::string label;
};

struct DepthState {
  bool test_enabled = false;
  bool write_enabled = false;
  // Reversed-Z: opaque geometry compares GreaterEqual; only a shadow pass
  // would use Less.
  CompareFunction compare = CompareFunction::GreaterEqual;
  Format format = Format::Undefined;
};

struct RenderPipelineDesc {
  class IShaderModule* vertex_shader = nullptr;
  std::string vertex_entry = "vs_main";
  class IShaderModule* fragment_shader = nullptr;  // null = depth-only pass
  std::string fragment_entry = "fs_main";

  std::vector<Format> color_formats;
  DepthState depth;
  PrimitiveTopology topology = PrimitiveTopology::TriangleList;
  CullMode cull_mode = CullMode::Back;
  FrontFace front_face = FrontFace::Ccw;
  // No blend state and no vertex layout in the MVP: the terrain path is opaque
  // and pulls its vertices from storage buffers.
  std::string label;
};

struct ComputePipelineDesc {
  class IShaderModule* shader = nullptr;
  std::string entry = "main";
  std::string label;
};

// ============================================================================
// Binding
// ============================================================================

// One entry in a binding table. Exactly one of the pointers is set; `kind`
// says which, so the validation decorator can check it against reflection
// without inspecting the union.
//
// The pointers are BORROWED for the duration of the CreateBindingTable call
// only: the table takes a share of ownership of everything it is given, so a
// caller may drop its own handle immediately afterwards. Without that, dropping
// the last caller handle frees the object while the table still points at it --
// a heap-use-after-free the first ASan run caught.
enum class BindingKind : uint8_t {
  UniformBuffer, StorageBuffer, ReadOnlyStorageBuffer, SampledTexture, Sampler,
};

// The most dynamic offsets one table may declare.
//
// Metal is flexible here, but a DX12 root signature has a 64-DWORD budget and
// a root CBV costs 2, so eight leaves ample room for everything else a
// signature carries. Bounded on the Mac, where exceeding it is cheap to fix,
// rather than discovered on the DX12 machine later.
inline constexpr uint32_t kMaxDynamicOffsetsPerTable = 8;

struct BindingEntry {
  uint32_t slot = 0;
  BindingKind kind = BindingKind::UniformBuffer;
  class IBuffer* buffer = nullptr;
  // Base offset, fixed for the life of the table.
  uint64_t buffer_offset = 0;
  // When true, SetBindingTable supplies an ADDITIONAL offset per call, and
  // this entry consumes one value from its `dynamic_offsets` span. Buffers
  // only; a texture or sampler cannot be re-pointed this way.
  bool dynamic_offset = false;
  uint64_t buffer_size = 0;  // 0 = to the end of the buffer
  class ITextureView* texture_view = nullptr;
  class ISampler* sampler = nullptr;
};

// A binding table is one `ParameterBlock<T>` in Slang terms -- probe B found
// that is the only construct mapping to BOTH a Metal argument buffer and a
// D3D12 register space, so it is the RHI's unit of binding. `group` is the
// block index; `slot` above is the index within it.
struct BindingTableDesc {
  class IRenderPipeline* render_pipeline = nullptr;   // one of these two
  class IComputePipeline* compute_pipeline = nullptr;
  uint32_t group = 0;
  std::vector<BindingEntry> entries;
  std::string label;
};

// ============================================================================
// Indirect argument layouts -- must match what the GPU writes
// ============================================================================

// Mirrors the standard 20-byte DrawIndexed indirect args, so a compute shader
// can write it directly.
struct DrawIndexedIndirectArgs {
  uint32_t index_count = 0;
  uint32_t instance_count = 0;
  uint32_t first_index = 0;
  int32_t base_vertex = 0;
  uint32_t first_instance = 0;
};
static_assert(sizeof(DrawIndexedIndirectArgs) == 20,
              "indirect args must stay the standard 20-byte layout");

// Mirrors the standard 12-byte dispatch indirect args. A compute shader writes
// this to drive a later dispatch from a count only the GPU knows -- how many
// points survived a cull, how many splats an SDF produced.
//
// y and z default to 0, NOT to the 1 that `Dispatch(x, y = 1, z = 1)` uses,
// and the difference is a trap worth knowing about: `DispatchIndirectArgs a{};
// a.x = n;` dispatches NOTHING, and nothing reports it -- the counts live in
// GPU memory, so the validation layer deliberately does not check them.
//
// Kept at 0 anyway, because this struct mirrors what the GPU writes rather
// than what a host would like to type. A convenience default here would be a
// value no shader ever produces, and it would read as "the layout says y is 1"
// to anyone matching this against a shader. A host seeding it sets all three;
// a shader writing it sets all three (see shaders/slang/splat/finalize.slang).
struct DispatchIndirectArgs {
  uint32_t x = 0;
  uint32_t y = 0;
  uint32_t z = 0;
};
static_assert(sizeof(DispatchIndirectArgs) == 12,
              "dispatch indirect args must stay the standard 12-byte layout");

}  // namespace badlands::rhi
