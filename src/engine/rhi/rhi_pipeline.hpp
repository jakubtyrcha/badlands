#pragma once

// Shader modules, pipelines, and the reflection contract.
//
// The reflection struct shape is a direct consequence of probe B (see
// docs/superpowers/specs/2026-08-03-rhi-slang-exploration.md):
//
//   * Metal and D3D12 reflection of the SAME shader is byte-identical for
//     names, uniform member offsets and sizes, type kinds, varyings and
//     workgroup size. Those are normalized into engine-owned fields here.
//   * Only BINDING LOCATIONS diverge -- Metal unifies structured buffers into
//     one buffer index space while D3D12 splits srv/uav -- so the location is
//     kept as a small per-target record rather than forced into a single
//     (group, binding) pair.
//   * `[[vk::binding]]` is ignored by both targets. `ParameterBlock<T>` is the
//     only construct that maps to a Metal argument buffer AND a D3D12 register
//     space, so `group` below means "which ParameterBlock", not "which WGSL
//     @group".

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "engine/rhi/rhi_types.hpp"

namespace badlands::rhi {

// ============================================================================
// Reflection
// ============================================================================

enum class UniformType : uint8_t {
  Unknown, Int, UInt, Float, Vec2, Vec3, Vec4, Mat3, Mat4,
};

struct ReflectedUniformMember {
  std::string name;
  uint32_t offset = 0;
  uint32_t size = 0;
  UniformType type = UniformType::Unknown;
};

struct ReflectedUniformBlock {
  uint32_t group = 0;
  uint32_t slot = 0;
  std::string name;
  std::vector<ReflectedUniformMember> members;
  uint32_t total_size = 0;
};

// Where a binding actually lands on one target. Kept separate from the
// normalized fields because the two targets genuinely disagree; storing both
// is cheaper and more honest than inventing a lowest common denominator.
struct BindingLocation {
  uint32_t space = 0;  // Metal: argument-buffer index. D3D12: register space.
  uint32_t index = 0;  // index within that space, per category
};

struct ReflectedBinding {
  uint32_t group = 0;  // which ParameterBlock
  uint32_t slot = 0;   // index within the block
  std::string name;    // the graph and material system match on this
  BindingKind kind = BindingKind::UniformBuffer;

  // MVP always reports ShaderStage::All. Slang's ProgramLayout does not prune
  // globals per entry point, so per-binding visibility is NOT derivable from
  // reflection (probe B measured `vs_main` and `fs_gbuffer` returning
  // identical global lists). Binding to all stages is correct and slightly
  // wasteful; narrowing it needs a different derivation, not a different
  // struct.
  ShaderStage visibility = ShaderStage::All;

  BindingLocation location;
};

struct ReflectedEntryPoint {
  std::string name;
  ShaderStage stage = ShaderStage::None;
  uint32_t workgroup_size[3] = {1, 1, 1};  // compute only
};

struct ShaderReflection {
  std::vector<ReflectedBinding> bindings;
  std::vector<ReflectedUniformBlock> uniform_blocks;
  std::vector<ReflectedEntryPoint> entry_points;

  // Lookup by name -- the hook the render graph's auto-binding and the shared
  // binding resolver (D7/D8) attach to.
  const ReflectedBinding* FindBinding(std::string_view name) const;
  const ReflectedUniformBlock* FindUniformBlock(std::string_view name) const;
};

// ============================================================================
// Pipelines
// ============================================================================

// A compiled shader. Backends take target-native source (MSL for Metal), which
// the Slang layer produces; the RHI itself never invokes a shader compiler.
class IShaderModule {
 public:
  virtual ~IShaderModule() = default;
  virtual const ShaderReflection& GetReflection() const = 0;
  virtual const std::string& GetLabel() const = 0;
};

class IRenderPipeline {
 public:
  virtual ~IRenderPipeline() = default;
  virtual const ShaderReflection& GetReflection() const = 0;
  virtual const RenderPipelineDesc& GetDesc() const = 0;
};

class IComputePipeline {
 public:
  virtual ~IComputePipeline() = default;
  virtual const ShaderReflection& GetReflection() const = 0;
  // Convenience: the entry point's declared workgroup size, so callers can
  // compute a dispatch count without walking reflection.
  virtual void GetWorkgroupSize(uint32_t out[3]) const = 0;
};

using ShaderModulePtr = std::shared_ptr<IShaderModule>;
using RenderPipelinePtr = std::shared_ptr<IRenderPipeline>;
using ComputePipelinePtr = std::shared_ptr<IComputePipeline>;

}  // namespace badlands::rhi
