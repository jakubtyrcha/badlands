#include "engine/slang/slang_compiler.hpp"

#include <slang.h>
#include <slang-com-ptr.h>

#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <spdlog/spdlog.h>

namespace badlands::slang {
namespace {

namespace sl = ::slang;

// ---------------------------------------------------------------------------
// Slang reflection -> engine reflection
//
// Probe B established what is and is not portable here. Names, uniform member
// offsets and sizes, type kinds, workgroup size: identical across Metal and
// D3D12, so they become plain engine fields. Binding LOCATIONS are not --
// Metal unifies structured buffers into one buffer index space while D3D12
// splits srv/uav -- so those stay in a per-target BindingLocation instead of
// being forced into a single (group, binding) pair.
// ---------------------------------------------------------------------------

rhi::BindingKind KindFromLayout(sl::TypeLayoutReflection* tl) {
  if (!tl) return rhi::BindingKind::UniformBuffer;
  switch (tl->getKind()) {
    case sl::TypeReflection::Kind::ConstantBuffer:
    case sl::TypeReflection::Kind::ParameterBlock:
      return rhi::BindingKind::UniformBuffer;
    case sl::TypeReflection::Kind::SamplerState:
      return rhi::BindingKind::Sampler;
    case sl::TypeReflection::Kind::Resource: {
      const SlangResourceShape shape =
          (SlangResourceShape)(tl->getResourceShape() & SLANG_RESOURCE_BASE_SHAPE_MASK);
      const SlangResourceAccess access = tl->getResourceAccess();
      if (shape == SLANG_STRUCTURED_BUFFER || shape == SLANG_BYTE_ADDRESS_BUFFER) {
        return access == SLANG_RESOURCE_ACCESS_READ
                   ? rhi::BindingKind::ReadOnlyStorageBuffer
                   : rhi::BindingKind::StorageBuffer;
      }
      return rhi::BindingKind::SampledTexture;
    }
    default:
      return rhi::BindingKind::UniformBuffer;
  }
}

rhi::UniformType UniformTypeFromLayout(sl::TypeLayoutReflection* tl) {
  if (!tl) return rhi::UniformType::Unknown;
  switch (tl->getKind()) {
    case sl::TypeReflection::Kind::Scalar:
      switch (tl->getScalarType()) {
        case sl::TypeReflection::ScalarType::Int32: return rhi::UniformType::Int;
        case sl::TypeReflection::ScalarType::UInt32: return rhi::UniformType::UInt;
        case sl::TypeReflection::ScalarType::Float32: return rhi::UniformType::Float;
        default: return rhi::UniformType::Unknown;
      }
    case sl::TypeReflection::Kind::Vector:
      switch (tl->getElementCount()) {
        case 2: return rhi::UniformType::Vec2;
        case 3: return rhi::UniformType::Vec3;
        case 4: return rhi::UniformType::Vec4;
        default: return rhi::UniformType::Unknown;
      }
    case sl::TypeReflection::Kind::Matrix:
      if (tl->getRowCount() == 4 && tl->getColumnCount() == 4) {
        return rhi::UniformType::Mat4;
      }
      if (tl->getRowCount() == 3 && tl->getColumnCount() == 3) {
        return rhi::UniformType::Mat3;
      }
      return rhi::UniformType::Unknown;
    default:
      return rhi::UniformType::Unknown;
  }
}

rhi::ShaderStage StageFrom(SlangStage s) {
  switch (s) {
    case SLANG_STAGE_VERTEX: return rhi::ShaderStage::Vertex;
    case SLANG_STAGE_FRAGMENT: return rhi::ShaderStage::Fragment;
    case SLANG_STAGE_COMPUTE: return rhi::ShaderStage::Compute;
    default: return rhi::ShaderStage::None;
  }
}

void CollectUniformMembers(sl::TypeLayoutReflection* tl,
                           rhi::ReflectedUniformBlock& out) {
  if (!tl) return;
  if (tl->getKind() == sl::TypeReflection::Kind::ConstantBuffer ||
      tl->getKind() == sl::TypeReflection::Kind::ParameterBlock) {
    tl = tl->getElementTypeLayout();
    if (!tl) return;
  }
  if (tl->getKind() != sl::TypeReflection::Kind::Struct) return;

  out.total_size = uint32_t(tl->getSize(SLANG_PARAMETER_CATEGORY_UNIFORM));
  const unsigned n = tl->getFieldCount();
  for (unsigned i = 0; i < n; ++i) {
    sl::VariableLayoutReflection* f = tl->getFieldByIndex(i);
    if (!f) continue;
    sl::TypeLayoutReflection* ftl = f->getTypeLayout();
    rhi::ReflectedUniformMember m;
    m.name = f->getName() ? f->getName() : "";
    m.offset = uint32_t(f->getOffset(SLANG_PARAMETER_CATEGORY_UNIFORM));
    m.size = ftl ? uint32_t(ftl->getSize(SLANG_PARAMETER_CATEGORY_UNIFORM)) : 0;
    m.type = UniformTypeFromLayout(ftl);
    out.members.push_back(std::move(m));
  }
}

rhi::ShaderReflection Extract(sl::ProgramLayout* layout) {
  rhi::ShaderReflection out;
  if (!layout) return out;

  const unsigned params = layout->getParameterCount();
  for (unsigned i = 0; i < params; ++i) {
    sl::VariableLayoutReflection* p = layout->getParameterByIndex(i);
    if (!p) continue;
    sl::TypeLayoutReflection* tl = p->getTypeLayout();

    rhi::ReflectedBinding b;
    b.name = p->getName() ? p->getName() : "";
    b.kind = KindFromLayout(tl);
    // `slot` is the engine's stable identifier and MUST be unique within a
    // group; `location.index` is the target's, and is NOT unique because Slang
    // numbers per category -- a constant buffer, a texture and a sampler can
    // all report index 0. Conflating the two collapses distinct bindings onto
    // one slot and silently drops all but the first.
    b.group = 0;
    b.slot = i;
    if (p->getCategoryCount() > 0) {
      const sl::ParameterCategory cat = p->getCategoryByIndex(0);
      b.location.space = uint32_t(p->getBindingSpace(cat));
      b.location.index = uint32_t(p->getOffset(cat));
    }
    // Always All: ProgramLayout reports every module global regardless of
    // entry point, so per-stage visibility is not derivable (probe B measured
    // vs_main and fs_gbuffer returning identical global lists).
    b.visibility = rhi::ShaderStage::All;
    out.bindings.push_back(std::move(b));

    if (tl && (tl->getKind() == sl::TypeReflection::Kind::ConstantBuffer ||
               tl->getKind() == sl::TypeReflection::Kind::ParameterBlock)) {
      rhi::ReflectedUniformBlock ub;
      ub.group = out.bindings.back().group;
      ub.slot = out.bindings.back().slot;
      ub.name = out.bindings.back().name;
      CollectUniformMembers(tl, ub);
      out.uniform_blocks.push_back(std::move(ub));
    }
  }

  const unsigned eps = layout->getEntryPointCount();
  for (unsigned i = 0; i < eps; ++i) {
    sl::EntryPointReflection* ep = layout->getEntryPointByIndex(i);
    if (!ep) continue;
    rhi::ReflectedEntryPoint out_ep;
    out_ep.name = ep->getName() ? ep->getName() : "";
    out_ep.stage = StageFrom(ep->getStage());
    if (ep->getStage() == SLANG_STAGE_COMPUTE) {
      SlangUInt sz[3] = {1, 1, 1};
      ep->getComputeThreadGroupSize(3, sz);
      out_ep.workgroup_size[0] = uint32_t(sz[0]);
      out_ep.workgroup_size[1] = uint32_t(sz[1]);
      out_ep.workgroup_size[2] = uint32_t(sz[2]);
    }
    out.entry_points.push_back(std::move(out_ep));
  }
  return out;
}

// ---------------------------------------------------------------------------
// Compiler
// ---------------------------------------------------------------------------

std::string DiagText(sl::IBlob* blob) {
  if (!blob || blob->getBufferSize() == 0) return {};
  return std::string(static_cast<const char*>(blob->getBufferPointer()));
}

// Cache key: the shader key plus the target. Features are sorted so that two
// callers listing the same flags in different orders hit the same entry.
std::string MakeCacheKey(const ShaderKey& k, ShaderTarget t) {
  std::vector<std::string> features = k.features;
  std::sort(features.begin(), features.end());
  std::string s = k.module + "#" + k.entry + "#" + ToString(t);
  for (const auto& f : features) s += "#" + f;
  return s;
}

class SlangCompilerImpl final : public SlangCompiler {
 public:
  SlangCompilerImpl(Slang::ComPtr<sl::IGlobalSession> global,
                    std::vector<std::string> search_paths)
      : global_(std::move(global)), search_paths_(std::move(search_paths)) {
    for (const auto& p : search_paths_) search_path_ptrs_.push_back(p.c_str());
  }

  std::shared_ptr<const CompiledShader> Get(const ShaderKey& key,
                                            ShaderTarget target) override {
    const std::string cache_key = MakeCacheKey(key, target);
    if (auto it = cache_.find(cache_key); it != cache_.end()) return it->second;

    ++compiles_;
    auto compiled = Compile(key, target);
    if (!compiled) return nullptr;
    cache_.emplace(cache_key, compiled);
    return compiled;
  }

  void InvalidateAll() override {
    // Sessions go too, not just the cache: an ISession caches modules by name
    // and never restats the file, so keeping one would mean the next Get()
    // silently returned pre-edit source.
    cache_.clear();
    sessions_.clear();
  }

  size_t CacheSize() const override { return cache_.size(); }
  uint64_t CompileCount() const override { return compiles_; }

 private:
  // One session per (target, feature set). Sessions are ~0.01 ms to create but
  // cache parsed modules, so sharing one across every shader built with the
  // same flags is what keeps a multi-shader frame off the parser.
  sl::ISession* SessionFor(ShaderTarget target,
                           const std::vector<std::string>& features) {
    std::vector<std::string> sorted = features;
    std::sort(sorted.begin(), sorted.end());
    std::string key = std::string(ToString(target));
    for (const auto& f : sorted) key += "#" + f;

    if (auto it = sessions_.find(key); it != sessions_.end()) {
      return it->second.get();
    }

    sl::TargetDesc td = {};
    td.format = target == ShaderTarget::Metal ? SLANG_METAL : SLANG_HLSL;
    td.profile = global_->findProfile("sm_6_6");

    std::vector<sl::PreprocessorMacroDesc> macros;
    macros.reserve(sorted.size());
    for (const auto& f : sorted) macros.push_back({f.c_str(), "1"});

    sl::SessionDesc sd = {};
    sd.targets = &td;
    sd.targetCount = 1;
    sd.searchPaths = search_path_ptrs_.data();
    sd.searchPathCount = SlangInt(search_path_ptrs_.size());
    // glm is column-major and the engine uploads glm matrices verbatim, so the
    // shader side must agree. Slang's default is row-major, which silently
    // transposes every matrix in a uniform buffer -- geometry then lands
    // somewhere off screen with no error anywhere.
    sd.defaultMatrixLayoutMode = SLANG_MATRIX_LAYOUT_COLUMN_MAJOR;
    if (!macros.empty()) {
      sd.preprocessorMacros = macros.data();
      sd.preprocessorMacroCount = SlangInt(macros.size());
    }

    Slang::ComPtr<sl::ISession> session;
    global_->createSession(sd, session.writeRef());
    if (!session) {
      spdlog::error("slang: createSession failed for {}", key);
      return nullptr;
    }
    auto* raw = session.get();
    sessions_.emplace(std::move(key), std::move(session));
    return raw;
  }

  std::shared_ptr<CompiledShader> Compile(const ShaderKey& key,
                                          ShaderTarget target) {
    sl::ISession* session = SessionFor(target, key.features);
    if (!session) return nullptr;

    Slang::ComPtr<sl::IBlob> diag;
    sl::IModule* module = session->loadModule(key.module.c_str(), diag.writeRef());
    if (!module) {
      spdlog::error("slang: loadModule('{}') failed: {}", key.module,
                    DiagText(diag));
      return nullptr;
    }

    Slang::ComPtr<sl::IEntryPoint> entry;
    if (SLANG_FAILED(
            module->findEntryPointByName(key.entry.c_str(), entry.writeRef()))) {
      spdlog::error("slang: '{}' has no entry point '{}'", key.module, key.entry);
      return nullptr;
    }

    sl::IComponentType* parts[] = {module, entry.get()};
    Slang::ComPtr<sl::IComponentType> composed;
    diag = nullptr;
    session->createCompositeComponentType(parts, 2, composed.writeRef(),
                                          diag.writeRef());
    if (!composed) {
      spdlog::error("slang: compose('{}::{}') failed: {}", key.module, key.entry,
                    DiagText(diag));
      return nullptr;
    }

    Slang::ComPtr<sl::IComponentType> linked;
    diag = nullptr;
    composed->link(linked.writeRef(), diag.writeRef());
    if (!linked) {
      spdlog::error("slang: link('{}::{}') failed: {}", key.module, key.entry,
                    DiagText(diag));
      return nullptr;
    }

    Slang::ComPtr<sl::IBlob> code;
    diag = nullptr;
    linked->getEntryPointCode(0, 0, code.writeRef(), diag.writeRef());
    if (!code) {
      spdlog::error("slang: codegen('{}::{}') failed: {}", key.module, key.entry,
                    DiagText(diag));
      return nullptr;
    }

    auto out = std::make_shared<CompiledShader>();
    out->target = target;
    out->source.assign(static_cast<const char*>(code->getBufferPointer()),
                       code->getBufferSize());
    // getEntryPointCode may include a trailing NUL; the backends want a plain
    // source string.
    while (!out->source.empty() && out->source.back() == '\0') {
      out->source.pop_back();
    }
    out->reflection = Extract(linked->getLayout(0));
    return out;
  }

  Slang::ComPtr<sl::IGlobalSession> global_;
  std::vector<std::string> search_paths_;
  std::vector<const char*> search_path_ptrs_;
  std::map<std::string, Slang::ComPtr<sl::ISession>> sessions_;
  std::map<std::string, std::shared_ptr<const CompiledShader>> cache_;
  uint64_t compiles_ = 0;
};

}  // namespace

const char* ToString(ShaderTarget t) {
  switch (t) {
    case ShaderTarget::Metal: return "metal";
    case ShaderTarget::Hlsl: return "hlsl";
  }
  return "?";
}

std::unique_ptr<SlangCompiler> CreateSlangCompiler(
    std::span<const std::string> search_paths) {
  Slang::ComPtr<sl::IGlobalSession> global;
  // Expensive (~60-70 ms, probe A) because it loads the core module -- hence
  // once per process, never per compile.
  sl::createGlobalSession(global.writeRef());
  if (!global) {
    spdlog::error("slang: createGlobalSession failed");
    return nullptr;
  }
  return std::make_unique<SlangCompilerImpl>(
      std::move(global),
      std::vector<std::string>(search_paths.begin(), search_paths.end()));
}

}  // namespace badlands::slang
