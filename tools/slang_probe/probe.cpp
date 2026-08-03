// Probe harness for the RHI/Slang exploration.
//
//   Probe A (--timing)  : is runtime compilation viable, or must we go offline?
//   Probe B (--reflect) : does Slang reflection produce what the engine consumes,
//                         and does it survive a target change?
//
// Run from the repo root; shader paths resolve relative to cwd.
// See tools/slang_probe/README.md.

#include <slang.h>
#include <slang-com-ptr.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#if PROBE_HAVE_WESL
extern "C" {
#include "wesl_ffi.h"
}
#endif

using Clock = std::chrono::steady_clock;
static double MsSince(Clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

namespace {

const char* kSearchPaths[] = {
    "tools/slang_probe/shaders/common",
    "tools/slang_probe/shaders/compute",
    "tools/slang_probe/shaders/material",
};

struct TargetSpec {
  const char* name;
  SlangCompileTarget format;
};

const TargetSpec kTargets[] = {
    {"metal", SLANG_METAL},
    {"hlsl", SLANG_HLSL},
};

// One entry point we want compiled and/or reflected.
struct ShaderCase {
  const char* module_name;  // Slang module (file stem)
  const char* entry;
  bool shadow_pass;  // sets the SHADOW_PASS macro
};

const ShaderCase kCases[] = {
    {"terrain_cluster", "vs_main", false},
    {"terrain_cluster", "fs_gbuffer", false},
    {"terrain_cluster", "vs_main", true},
    {"instance_classify", "main", false},
    // Probe B follow-up: does ParameterBlock give us the (group, binding) model
    // that [[vk::binding]] did not?
    {"binding_models", "fs_main", false},
};

void Diag(slang::IBlob* blob, const char* what) {
  if (blob && blob->getBufferSize() > 0) {
    std::fprintf(stderr, "[%s] %s\n", what,
                 static_cast<const char*>(blob->getBufferPointer()));
  }
}

Slang::ComPtr<slang::ISession> MakeSession(slang::IGlobalSession* global,
                                           SlangCompileTarget format,
                                           bool shadow_pass) {
  slang::TargetDesc target = {};
  target.format = format;
  target.profile = global->findProfile("sm_6_6");

  slang::PreprocessorMacroDesc macro = {"SHADOW_PASS", "1"};

  slang::SessionDesc desc = {};
  desc.targets = &target;
  desc.targetCount = 1;
  desc.searchPaths = kSearchPaths;
  desc.searchPathCount = SLANG_COUNT_OF(kSearchPaths);
  if (shadow_pass) {
    desc.preprocessorMacros = &macro;
    desc.preprocessorMacroCount = 1;
  }

  Slang::ComPtr<slang::ISession> session;
  global->createSession(desc, session.writeRef());
  return session;
}

// Load + compose + link + codegen one entry point. Returns generated size in
// bytes, or 0 on failure. `out_layout` optionally receives the linked layout.
size_t CompileEntry(slang::ISession* session, const ShaderCase& c,
                    Slang::ComPtr<slang::IComponentType>* out_linked) {
  Slang::ComPtr<slang::IBlob> diag;
  slang::IModule* module = session->loadModule(c.module_name, diag.writeRef());
  Diag(diag, "loadModule");
  if (!module) return 0;

  Slang::ComPtr<slang::IEntryPoint> entry;
  if (SLANG_FAILED(module->findEntryPointByName(c.entry, entry.writeRef()))) {
    std::fprintf(stderr, "[entry] '%s' not found in %s\n", c.entry, c.module_name);
    return 0;
  }

  slang::IComponentType* parts[] = {module, entry.get()};
  Slang::ComPtr<slang::IComponentType> composed;
  diag = nullptr;
  session->createCompositeComponentType(parts, 2, composed.writeRef(),
                                        diag.writeRef());
  Diag(diag, "compose");
  if (!composed) return 0;

  Slang::ComPtr<slang::IComponentType> linked;
  diag = nullptr;
  composed->link(linked.writeRef(), diag.writeRef());
  Diag(diag, "link");
  if (!linked) return 0;

  Slang::ComPtr<slang::IBlob> code;
  diag = nullptr;
  linked->getEntryPointCode(0, 0, code.writeRef(), diag.writeRef());
  Diag(diag, "codegen");
  if (!code) return 0;

  if (out_linked) *out_linked = linked;
  return code->getBufferSize();
}

// ---------------------------------------------------------------------------
// Probe A: timing
// ---------------------------------------------------------------------------

void ProbeTiming() {
  std::printf("\n=== PROBE A: compile timing ===\n\n");

  auto t0 = Clock::now();
  Slang::ComPtr<slang::IGlobalSession> global;
  slang::createGlobalSession(global.writeRef());
  const double global_ms = MsSince(t0);
  std::printf("createGlobalSession (once per process) : %8.2f ms\n\n", global_ms);

  std::printf("%-8s %-20s %-11s %10s %10s %10s\n", "target", "module", "entry",
              "session", "compile", "warm");
  std::printf("%s\n", std::string(76, '-').c_str());

  for (const auto& t : kTargets) {
    for (const auto& c : kCases) {
      // Cold: a fresh session, so no module is cached.
      auto ts = Clock::now();
      auto session = MakeSession(global, t.format, c.shadow_pass);
      const double session_ms = MsSince(ts);
      if (!session) {
        std::printf("%-8s %-20s %-11s   SESSION FAILED\n", t.name, c.module_name,
                    c.entry);
        continue;
      }

      auto tc = Clock::now();
      const size_t bytes = CompileEntry(session, c, nullptr);
      const double cold_ms = MsSince(tc);

      // Warm: same session, same module -- exercises the session module cache.
      auto tw = Clock::now();
      CompileEntry(session, c, nullptr);
      const double warm_ms = MsSince(tw);

      const std::string label =
          std::string(c.entry) + (c.shadow_pass ? " [shadow]" : "");
      std::printf("%-8s %-20s %-11s %9.2fm %9.2fm %9.2fm%s\n", t.name,
                  c.module_name, label.c_str(), session_ms, cold_ms, warm_ms,
                  bytes ? "" : "  (FAILED)");
    }
  }

  std::printf("\nAll times ms. 'session' = createSession, 'compile' = "
              "loadModule+compose+link+codegen\non a fresh session, 'warm' = the "
              "same call again on that session (module cache hit).\n");

#if PROBE_HAVE_WESL
  std::printf("\n--- WESL baseline (same shaders, current pipeline) ---\n");
  struct WeslCase { const char* path; const char* label; };
  const WeslCase wesl_cases[] = {
      {"material/terrain_cluster", "terrain_cluster"},
      {"compute/instance_classify", "instance_classify"},
  };
  for (const auto& w : wesl_cases) {
    auto tw = Clock::now();
    WeslCompileResult r = wesl_compile_file_with_dirs("shaders", w.path, nullptr,
                                                      0, nullptr, 0);
    const double ms = MsSince(tw);
    const bool ok = r.wgsl != nullptr;
    std::printf("%-24s %9.2fms %s\n", w.label, ms, ok ? "" : "(FAILED)");
    if (!ok && r.error) std::fprintf(stderr, "  wesl: %s\n", r.error);
    wesl_free_result(r);
  }
  std::printf("NOTE: libwesl_ffi here is whatever the main build produced; if "
              "that is a debug\nbuild the comparison flatters Slang.\n");
#else
  std::printf("\n(WESL baseline unavailable: libwesl_ffi not built)\n");
#endif
}

// ---------------------------------------------------------------------------
// Probe B: reflection
// ---------------------------------------------------------------------------

const char* KindName(slang::TypeReflection::Kind k) {
  using K = slang::TypeReflection::Kind;
  switch (k) {
    case K::None: return "none";
    case K::Struct: return "struct";
    case K::Array: return "array";
    case K::Matrix: return "matrix";
    case K::Vector: return "vector";
    case K::Scalar: return "scalar";
    case K::ConstantBuffer: return "cbuffer";
    case K::Resource: return "resource";
    case K::SamplerState: return "sampler";
    case K::TextureBuffer: return "texbuffer";
    case K::ShaderStorageBuffer: return "ssbo";
    case K::ParameterBlock: return "paramblock";
    case K::GenericTypeParameter: return "generic";
    case K::Interface: return "interface";
    case K::Pointer: return "pointer";
    default: return "?";
  }
}

const char* CategoryName(slang::ParameterCategory c) {
  switch (c) {
    case slang::ParameterCategory::None: return "none";
    case slang::ParameterCategory::Mixed: return "mixed";
    case slang::ParameterCategory::ConstantBuffer: return "cbuffer";
    case slang::ParameterCategory::ShaderResource: return "srv";
    case slang::ParameterCategory::UnorderedAccess: return "uav";
    case slang::ParameterCategory::VaryingInput: return "varying-in";
    case slang::ParameterCategory::VaryingOutput: return "varying-out";
    case slang::ParameterCategory::SamplerState: return "sampler";
    case slang::ParameterCategory::Uniform: return "uniform";
    case slang::ParameterCategory::DescriptorTableSlot: return "descr-slot";
    case slang::ParameterCategory::RegisterSpace: return "space";
    // NOTE: Slang's Metal*/ parameter categories are ALIASES of the D3D ones
    // (MetalBuffer == ConstantBuffer == 2, MetalTexture == ShaderResource == 3).
    // That aliasing is itself a probe-B finding: the category enum does not
    // distinguish targets, so a reflected category means different things
    // depending on which target produced it.
    case slang::ParameterCategory::MetalArgumentBufferElement: return "mtl-argbuf";
    case slang::ParameterCategory::SubElementRegisterSpace: return "sub-space";
    case slang::ParameterCategory::GenericResource: return "generic-res";
    default: {
      // Print the raw enum so an unmapped category is still diagnosable.
      static char buf[32];
      std::snprintf(buf, sizeof(buf), "cat#%d", (int)c);
      return buf;
    }
  }
}

const char* StageName(SlangStage s) {
  switch (s) {
    case SLANG_STAGE_VERTEX: return "vertex";
    case SLANG_STAGE_FRAGMENT: return "fragment";
    case SLANG_STAGE_COMPUTE: return "compute";
    default: return "?";
  }
}

// Dump a uniform-buffer struct's members: the engine's ReflectedUniformBuffer
// needs {name, offset, size, type} per member plus a total size.
void DumpMembers(slang::TypeLayoutReflection* tl, int indent) {
  if (!tl) return;
  if (tl->getKind() == slang::TypeReflection::Kind::ConstantBuffer ||
      tl->getKind() == slang::TypeReflection::Kind::ParameterBlock) {
    tl = tl->getElementTypeLayout();
    if (!tl) return;
  }
  if (tl->getKind() != slang::TypeReflection::Kind::Struct) return;

  const unsigned n = tl->getFieldCount();
  for (unsigned i = 0; i < n; ++i) {
    slang::VariableLayoutReflection* f = tl->getFieldByIndex(i);
    slang::TypeLayoutReflection* ftl = f->getTypeLayout();
    std::printf("%*s.%-18s off=%-5zu size=%-5zu kind=%-8s\n", indent, "",
                f->getName() ? f->getName() : "?",
                f->getOffset(slang::ParameterCategory::Uniform),
                ftl ? ftl->getSize(slang::ParameterCategory::Uniform) : 0,
                ftl ? KindName(ftl->getKind()) : "?");
  }
}

void DumpParam(slang::VariableLayoutReflection* p, bool with_members) {
  slang::TypeLayoutReflection* tl = p->getTypeLayout();
  const unsigned cat_count = p->getCategoryCount();

  std::printf("  %-18s ", p->getName() ? p->getName() : "?");
  for (unsigned c = 0; c < cat_count; ++c) {
    const slang::ParameterCategory cat = p->getCategoryByIndex(c);
    std::printf("[%s space=%zu index=%zu] ", CategoryName(cat),
                (size_t)p->getBindingSpace(cat), (size_t)p->getOffset(cat));
  }
  std::printf("kind=%s", tl ? KindName(tl->getKind()) : "?");
  if (tl && tl->getKind() == slang::TypeReflection::Kind::Resource) {
    std::printf(" shape=0x%x access=%u", (unsigned)tl->getResourceShape(),
                (unsigned)tl->getResourceAccess());
  }
  std::printf("\n");

  if (with_members) DumpMembers(tl, 4);
}

void DumpVaryings(slang::EntryPointReflection* ep) {
  const unsigned n = ep->getParameterCount();
  for (unsigned i = 0; i < n; ++i) {
    slang::VariableLayoutReflection* p = ep->getParameterByIndex(i);
    slang::TypeLayoutReflection* tl = p->getTypeLayout();
    // A struct parameter carries the real varyings in its fields.
    if (tl && tl->getKind() == slang::TypeReflection::Kind::Struct) {
      for (unsigned f = 0; f < tl->getFieldCount(); ++f) {
        slang::VariableLayoutReflection* fv = tl->getFieldByIndex(f);
        std::printf("    in  %-16s loc=%-3zu sem=%-12s kind=%s\n",
                    fv->getName() ? fv->getName() : "?",
                    fv->getOffset(slang::ParameterCategory::VaryingInput),
                    fv->getSemanticName() ? fv->getSemanticName() : "-",
                    fv->getTypeLayout() ? KindName(fv->getTypeLayout()->getKind()) : "?");
      }
    } else {
      std::printf("    in  %-16s loc=%-3zu sem=%s\n",
                  p->getName() ? p->getName() : "?",
                  p->getOffset(slang::ParameterCategory::VaryingInput),
                  p->getSemanticName() ? p->getSemanticName() : "-");
    }
  }

  slang::VariableLayoutReflection* res = ep->getResultVarLayout();
  if (!res) return;
  slang::TypeLayoutReflection* rtl = res->getTypeLayout();
  if (rtl && rtl->getKind() == slang::TypeReflection::Kind::Struct) {
    for (unsigned f = 0; f < rtl->getFieldCount(); ++f) {
      slang::VariableLayoutReflection* fv = rtl->getFieldByIndex(f);
      std::printf("    out %-16s loc=%-3zu sem=%-12s kind=%s\n",
                  fv->getName() ? fv->getName() : "?",
                  fv->getOffset(slang::ParameterCategory::VaryingOutput),
                  fv->getSemanticName() ? fv->getSemanticName() : "-",
                  fv->getTypeLayout() ? KindName(fv->getTypeLayout()->getKind()) : "?");
    }
  } else if (rtl) {
    std::printf("    out %-16s loc=%-3zu sem=%s\n", "<result>",
                res->getOffset(slang::ParameterCategory::VaryingOutput),
                res->getSemanticName() ? res->getSemanticName() : "-");
  }
}

void ProbeReflection() {
  std::printf("\n=== PROBE B: reflection, per target ===\n");

  Slang::ComPtr<slang::IGlobalSession> global;
  slang::createGlobalSession(global.writeRef());

  for (const auto& t : kTargets) {
    for (const auto& c : kCases) {
      auto session = MakeSession(global, t.format, c.shadow_pass);
      if (!session) continue;

      // std::addressof: ComPtr deletes operator&.
      Slang::ComPtr<slang::IComponentType> linked;
      if (!CompileEntry(session, c, std::addressof(linked))) continue;

      slang::ProgramLayout* layout = linked->getLayout(0);
      if (!layout) continue;

      std::printf("\n--- %s | %s :: %s%s ---\n", t.name, c.module_name, c.entry,
                  c.shadow_pass ? " [SHADOW_PASS]" : "");

      std::printf("global params (%u):\n", layout->getParameterCount());
      for (unsigned i = 0; i < layout->getParameterCount(); ++i) {
        DumpParam(layout->getParameterByIndex(i), /*with_members=*/true);
      }

      for (unsigned e = 0; e < layout->getEntryPointCount(); ++e) {
        slang::EntryPointReflection* ep = layout->getEntryPointByIndex(e);
        std::printf("entry '%s' stage=%s", ep->getName(),
                    StageName(ep->getStage()));
        if (ep->getStage() == SLANG_STAGE_COMPUTE) {
          SlangUInt sz[3] = {0, 0, 0};
          ep->getComputeThreadGroupSize(3, sz);
          std::printf(" threadgroup=(%llu,%llu,%llu)",
                      (unsigned long long)sz[0], (unsigned long long)sz[1],
                      (unsigned long long)sz[2]);
        }
        std::printf("\n");
        DumpVaryings(ep);
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Probe A follow-up: does a session notice a changed source file?
//
// Hot-reload today is GpuPipelineGenerator::InvalidateAll() dropping its
// pipeline caches. If a Slang ISession caches modules by name and never
// restats the file, hot-reload must drop the session too -- which is only
// acceptable if createSession is cheap.
// ---------------------------------------------------------------------------

const char* kHotPath = "tools/slang_probe/shaders/common/hotreload_tmp.slang";

bool WriteHotShader(float value) {
  std::FILE* f = std::fopen(kHotPath, "w");
  if (!f) return false;
  std::fprintf(f,
               "// Generated by slang_probe --hotreload. Safe to delete.\n"
               "[shader(\"fragment\")]\n"
               "float4 fs_main() : SV_Target { return float4(%.1f); }\n",
               value);
  std::fclose(f);
  return true;
}

size_t CompileHot(slang::ISession* session, std::string* out_code) {
  Slang::ComPtr<slang::IBlob> diag;
  slang::IModule* m = session->loadModule("hotreload_tmp", diag.writeRef());
  if (!m) { Diag(diag, "hot loadModule"); return 0; }
  Slang::ComPtr<slang::IEntryPoint> ep;
  if (SLANG_FAILED(m->findEntryPointByName("fs_main", ep.writeRef()))) return 0;
  slang::IComponentType* parts[] = {m, ep.get()};
  Slang::ComPtr<slang::IComponentType> composed;
  session->createCompositeComponentType(parts, 2, composed.writeRef(), nullptr);
  if (!composed) return 0;
  Slang::ComPtr<slang::IComponentType> linked;
  composed->link(linked.writeRef(), nullptr);
  if (!linked) return 0;
  Slang::ComPtr<slang::IBlob> code;
  linked->getEntryPointCode(0, 0, code.writeRef(), nullptr);
  if (!code) return 0;
  if (out_code) {
    out_code->assign(static_cast<const char*>(code->getBufferPointer()),
                     code->getBufferSize());
  }
  return code->getBufferSize();
}

bool Mentions(const std::string& code, const char* needle) {
  return code.find(needle) != std::string::npos;
}

void ProbeHotReload() {
  std::printf("\n=== PROBE A follow-up: source change visibility ===\n\n");

  Slang::ComPtr<slang::IGlobalSession> global;
  slang::createGlobalSession(global.writeRef());

  if (!WriteHotShader(1.0f)) {
    std::fprintf(stderr, "cannot write %s (run from the repo root)\n", kHotPath);
    return;
  }

  auto sessionA = MakeSession(global, SLANG_METAL, false);
  std::string codeA;
  if (!CompileHot(sessionA, &codeA)) { std::printf("initial compile FAILED\n"); return; }
  std::printf("session A, source = 1.0 -> emits 1.0: %s\n",
              Mentions(codeA, "1.0") ? "yes" : "NO");

  WriteHotShader(2.0f);

  std::string codeA2;
  CompileHot(sessionA, &codeA2);
  const bool same_session_sees = Mentions(codeA2, "2.0");
  std::printf("source changed to 2.0, SAME session  -> sees change: %s\n",
              same_session_sees ? "yes" : "NO (module cached)");

  auto ts = Clock::now();
  auto sessionB = MakeSession(global, SLANG_METAL, false);
  const double new_session_ms = MsSince(ts);
  std::string codeB;
  CompileHot(sessionB, &codeB);
  const bool new_session_sees = Mentions(codeB, "2.0");
  std::printf("source changed to 2.0, NEW session   -> sees change: %s "
              "(createSession %.3f ms)\n",
              new_session_sees ? "yes" : "NO", new_session_ms);

  std::remove(kHotPath);
  std::printf("\nVERDICT: hot-reload %s\n",
              same_session_sees
                  ? "can reuse one session"
                  : (new_session_sees
                         ? "must drop the ISession, which createSession makes cheap"
                         : "NEEDS INVESTIGATION -- neither path saw the change"));
}

}  // namespace

int main(int argc, char** argv) {
  bool timing = false, reflect = false, hot = false;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--timing") timing = true;
    else if (a == "--reflect") reflect = true;
    else if (a == "--hotreload") hot = true;
    else if (a == "--all") { timing = reflect = hot = true; }
    else {
      std::fprintf(stderr,
                   "usage: slang_probe [--timing] [--reflect] [--hotreload] [--all]\n");
      return 2;
    }
  }
  if (!timing && !reflect && !hot) { timing = reflect = hot = true; }

  if (timing) ProbeTiming();
  if (hot) ProbeHotReload();
  if (reflect) ProbeReflection();
  return 0;
}
