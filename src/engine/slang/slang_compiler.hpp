#pragma once

// Slang shader compilation, sitting ABOVE the RHI: it produces the
// target-native source and the engine-owned reflection that
// `IRhiDevice::CreateShaderModule` consumes. The RHI never invokes a compiler,
// so the dependency runs one way only.
//
// Replaces the WESL + naga path (src/crates/wesl). Probe A measured the
// tradeoff: ~60-70 ms once for the global session, ~0.01 ms per session, and
// 13-44 ms per entry point compiled cold. That is absorbed by the cache here,
// exactly as GpuPipelineGenerator's declaration hash absorbs the WESL cost
// today.
//
// Hot-reload: a Slang ISession caches modules by name and never restats the
// file, so seeing an edit means dropping the session. `InvalidateAll()` does
// that along with the cache, and createSession is cheap enough that it costs
// nothing measurable.

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "engine/rhi/rhi_pipeline.hpp"

namespace badlands::slang {

enum class ShaderTarget : uint8_t {
  Metal,  // MSL source
  Hlsl,   // HLSL source; reflection differs from Metal for structured buffers
};

const char* ToString(ShaderTarget t);

// What to compile. `features` become preprocessor defines, which is how the
// engine's 12 conditional-compilation flags (shadow_pass, instanced, ...) port
// from WESL's `@if` -- probe A confirmed that maps cleanly.
struct ShaderKey {
  std::string module;  // module name, e.g. "terrain_cluster"
  std::string entry;   // entry point, e.g. "fs_gbuffer"
  std::vector<std::string> features;

  bool operator==(const ShaderKey&) const = default;
};

struct CompiledShader {
  std::string source;              // target-native, ready for CreateShaderModule
  rhi::ShaderReflection reflection;
  ShaderTarget target = ShaderTarget::Metal;
};

class SlangCompiler {
 public:
  virtual ~SlangCompiler() = default;

  // Compiles (or returns cached). Null on failure, after logging the
  // diagnostics Slang produced.
  virtual std::shared_ptr<const CompiledShader> Get(const ShaderKey& key,
                                                    ShaderTarget target) = 0;

  // Drops every cached shader AND every Slang session, so the next Get() sees
  // edited files. Callers already holding a CompiledShader keep it alive --
  // the cache hands out shared_ptr precisely so a hot-reload cannot pull a
  // pipeline's source out from under it.
  virtual void InvalidateAll() = 0;

  virtual size_t CacheSize() const = 0;
  // Number of Get() calls that missed. Tests assert the cache is actually used
  // rather than trusting that it is.
  virtual uint64_t CompileCount() const = 0;
};

// `search_paths` resolve `import` statements, in order. Returns null (after
// logging) if the Slang global session cannot be created.
std::unique_ptr<SlangCompiler> CreateSlangCompiler(
    std::span<const std::string> search_paths);

}  // namespace badlands::slang
