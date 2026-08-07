// Slang compiler tests.
//
// Fixtures are written to a temp directory by the test itself rather than
// pointing at shaders/ -- the project's convention is to test the MECHANISM
// with test-local fixtures, never to assert on shipped data files. That also
// means these tests keep passing when the real shaders change.

#include <catch_amalgamated.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "engine/slang/slang_compiler.hpp"

using namespace badlands;
using badlands::slang::ShaderKey;
using badlands::slang::ShaderTarget;

namespace {

namespace fs = std::filesystem;

// Writes the fixture tree once per process and hands back its path.
class Fixtures {
 public:
  static const fs::path& Dir() {
    static const fs::path dir = Build();
    return dir;
  }

 private:
  static void Put(const fs::path& p, const char* text) {
    std::ofstream out(p);
    out << text;
  }

  static fs::path Build() {
    const fs::path dir =
        fs::temp_directory_path() / "badlands_slang_fixtures";
    fs::create_directories(dir);

    // A shared module, so import resolution through search paths is exercised
    // rather than assumed.
    Put(dir / "fixture_common.slang", R"(
module fixture_common;

public struct Params
{
    public float4x4 transform;
    public float4   tint;
    public float    scale;
};

public float3 apply_tint(float3 c, float4 tint) { return c * tint.rgb; }
)");

    // A cube sample, which is the one thing in the IBL chain the toolchain had
    // never been asked for. Slang reflects any non-buffer Resource as a sampled
    // texture, so reflection alone cannot say whether the EMITTED MSL declares
    // a texturecube or a texture2d -- and a texture2d bound to a cube handle is
    // wrong pixels, not a compile error.
    Put(dir / "fixture_cube.slang", R"(
module fixture_cube;

TextureCube<float4> env;
SamplerState env_sampler;

struct VOut { float4 pos : SV_Position; float3 dir : TEXCOORD0; };

[shader("vertex")]
VOut vs_cube(uint vid : SV_VertexID)
{
    let uv = float2(float((vid << 1) & 2), float(vid & 2));
    VOut o;
    o.pos = float4(uv * 2.0 - 1.0, 0.0, 1.0);
    o.dir = float3(uv * 2.0 - 1.0, 1.0);
    return o;
}

[shader("fragment")]
float4 fs_cube(VOut i) : SV_Target
{
    return env.SampleLevel(env_sampler, normalize(i.dir), 1.5);
}
)");

    Put(dir / "fixture_compute.slang", R"(
import fixture_common;

ConstantBuffer<Params> params;
StructuredBuffer<uint> input;
RWStructuredBuffer<uint> output;

[shader("compute")]
[numthreads(48, 2, 1)]
void cs_main(uint3 gid : SV_DispatchThreadID)
{
    output[gid.x] = input[gid.x] + uint(params.scale);
}
)");

    // Conditional compilation, standing in for the engine's 12 `@if` flags.
    Put(dir / "fixture_variants.slang", R"(
import fixture_common;

ConstantBuffer<Params> params;

struct VOut { float4 pos : SV_Position; };

[shader("vertex")]
VOut vs_main(uint vid : SV_VertexID)
{
    VOut o;
    o.pos = mul(params.transform, float4(float(vid), 0.0, 0.0, 1.0));
    return o;
}

[shader("fragment")]
float4 fs_main(VOut i) : SV_Target
{
#ifdef TINTED
    return float4(apply_tint(float3(1.0, 1.0, 1.0), params.tint), 1.0);
#else
    return float4(1.0, 1.0, 1.0, 1.0);
#endif
}
)");
    return dir;
  }
};

std::unique_ptr<badlands::slang::SlangCompiler> MakeCompiler() {
  const std::vector<std::string> paths = {Fixtures::Dir().string()};
  return badlands::slang::CreateSlangCompiler(paths);
}

}  // namespace

TEST_CASE("slang: compiles a compute entry to MSL with reflection",
          "[slang]") {
  auto compiler = MakeCompiler();
  REQUIRE(compiler);

  auto shader = compiler->Get({.module = "fixture_compute", .entry = "cs_main"},
                              ShaderTarget::Metal);
  REQUIRE(shader);
  CHECK(shader->target == ShaderTarget::Metal);
  CHECK_FALSE(shader->source.empty());
  // MSL, not WGSL or HLSL.
  CHECK(shader->source.find("metal_stdlib") != std::string::npos);

  // Workgroup size comes from reflection, not from the caller guessing.
  REQUIRE(shader->reflection.entry_points.size() >= 1);
  const auto& ep = shader->reflection.entry_points[0];
  CHECK(ep.stage == rhi::ShaderStage::Compute);
  CHECK(ep.workgroup_size[0] == 48);
  CHECK(ep.workgroup_size[1] == 2);
  CHECK(ep.workgroup_size[2] == 1);
}

// The one link in the IBL toolchain that nothing else can vouch for.
//
// KindFromLayout maps every non-buffer Resource to SampledTexture, so a cube
// and a 2D texture reflect IDENTICALLY -- which means reflection passing proves
// nothing about what Slang emitted. A texture2d declared where the RHI binds a
// cube handle is wrong pixels rather than a compile error, and it would surface
// as "the prefilter looks smeared" three tasks later.
TEST_CASE("slang: a TextureCube is emitted as a cube, not a 2D texture",
          "[slang]") {
  auto compiler = MakeCompiler();
  REQUIRE(compiler);

  auto shader = compiler->Get({.module = "fixture_cube", .entry = "fs_cube"},
                              ShaderTarget::Metal);
  REQUIRE(shader);
  INFO(shader->source);
  CHECK(shader->source.find("texturecube") != std::string::npos);
  // Not merely "a cube appears somewhere": the 2D form must be absent, or a
  // fixture that declared both would pass while binding the wrong one.
  CHECK(shader->source.find("texture2d") == std::string::npos);

  // Reflection still has to see it as a sampled texture, because that is what
  // the RHI binding table keys on.
  bool found = false;
  for (const auto& b : shader->reflection.bindings) {
    if (b.name == "env") {
      found = true;
      CHECK(b.kind == rhi::BindingKind::SampledTexture);
    }
  }
  CHECK(found);
}

TEST_CASE("slang: reflects bindings by name and kind", "[slang]") {
  auto compiler = MakeCompiler();
  REQUIRE(compiler);
  auto shader = compiler->Get({.module = "fixture_compute", .entry = "cs_main"},
                              ShaderTarget::Metal);
  REQUIRE(shader);

  const auto& r = shader->reflection;
  // Name lookup is the hook the render graph's auto-binding attaches to.
  const auto* params = r.FindBinding("params");
  const auto* input = r.FindBinding("input");
  const auto* output = r.FindBinding("output");
  REQUIRE(params != nullptr);
  REQUIRE(input != nullptr);
  REQUIRE(output != nullptr);

  CHECK(params->kind == rhi::BindingKind::UniformBuffer);
  CHECK(input->kind == rhi::BindingKind::ReadOnlyStorageBuffer);
  CHECK(output->kind == rhi::BindingKind::StorageBuffer);

  // Visibility is always All: ProgramLayout does not prune globals per entry
  // point, so it is not derivable. Pinned so a future change is deliberate.
  CHECK(params->visibility == rhi::ShaderStage::All);

  // Distinct bindings land at distinct Metal buffer indices.
  CHECK(params->location.index != input->location.index);
  CHECK(input->location.index != output->location.index);
}

TEST_CASE("slang: reflects uniform member offsets", "[slang]") {
  auto compiler = MakeCompiler();
  REQUIRE(compiler);
  auto shader = compiler->Get({.module = "fixture_compute", .entry = "cs_main"},
                              ShaderTarget::Metal);
  REQUIRE(shader);

  const auto* block = shader->reflection.FindUniformBlock("params");
  REQUIRE(block != nullptr);
  REQUIRE(block->members.size() == 3);

  // float4x4 at 0, float4 at 64, float at 80 -- std140-ish packing, and the
  // property the engine's UniformData mirrors depend on.
  CHECK(block->members[0].name == "transform");
  CHECK(block->members[0].offset == 0);
  CHECK(block->members[0].type == rhi::UniformType::Mat4);
  CHECK(block->members[1].name == "tint");
  CHECK(block->members[1].offset == 64);
  CHECK(block->members[1].type == rhi::UniformType::Vec4);
  CHECK(block->members[2].name == "scale");
  CHECK(block->members[2].offset == 80);
  CHECK(block->members[2].type == rhi::UniformType::Float);
}

TEST_CASE("slang: features select different code", "[slang]") {
  auto compiler = MakeCompiler();
  REQUIRE(compiler);

  auto plain = compiler->Get({.module = "fixture_variants", .entry = "fs_main"},
                             ShaderTarget::Metal);
  auto tinted = compiler->Get(
      {.module = "fixture_variants", .entry = "fs_main", .features = {"TINTED"}},
      ShaderTarget::Metal);
  REQUIRE(plain);
  REQUIRE(tinted);

  // This is how the engine's 12 conditional-compilation flags port from WESL's
  // @if: preprocessor defines carried on the session.
  CHECK(plain->source != tinted->source);
  CHECK(compiler->CacheSize() == 2);
}

TEST_CASE("slang: the cache serves repeated requests", "[slang]") {
  auto compiler = MakeCompiler();
  REQUIRE(compiler);

  const ShaderKey key{.module = "fixture_compute", .entry = "cs_main"};
  auto a = compiler->Get(key, ShaderTarget::Metal);
  REQUIRE(a);
  const uint64_t after_first = compiler->CompileCount();

  for (int i = 0; i < 5; ++i) {
    auto b = compiler->Get(key, ShaderTarget::Metal);
    REQUIRE(b);
    CHECK(b.get() == a.get());  // same object, not merely equal
  }
  // Asserted rather than assumed: a cache that silently misses would still
  // pass every other test here, just slowly.
  CHECK(compiler->CompileCount() == after_first);
  CHECK(compiler->CacheSize() == 1);
}

TEST_CASE("slang: feature order does not split the cache", "[slang]") {
  auto compiler = MakeCompiler();
  REQUIRE(compiler);
  auto a = compiler->Get({.module = "fixture_variants", .entry = "fs_main",
                          .features = {"TINTED", "OTHER"}},
                         ShaderTarget::Metal);
  auto b = compiler->Get({.module = "fixture_variants", .entry = "fs_main",
                          .features = {"OTHER", "TINTED"}},
                         ShaderTarget::Metal);
  REQUIRE(a);
  REQUIRE(b);
  CHECK(a.get() == b.get());
  CHECK(compiler->CacheSize() == 1);
}

TEST_CASE("slang: a held shader survives InvalidateAll", "[slang]") {
  // The hot-reload contract. InvalidateAll drops the cache AND every ISession,
  // because a session never restats its files -- but a caller already holding
  // a CompiledShader must keep working, which is why the cache hands out
  // shared_ptr rather than a raw reference into its own storage.
  auto compiler = MakeCompiler();
  REQUIRE(compiler);

  const ShaderKey key{.module = "fixture_compute", .entry = "cs_main"};
  auto held = compiler->Get(key, ShaderTarget::Metal);
  REQUIRE(held);
  const std::string source_before = held->source;

  compiler->InvalidateAll();
  CHECK(compiler->CacheSize() == 0);

  // Still readable, still intact.
  CHECK(held->source == source_before);
  CHECK(held->reflection.entry_points[0].workgroup_size[0] == 48);

  // And the next request recompiles rather than serving a dropped entry.
  const uint64_t before = compiler->CompileCount();
  auto fresh = compiler->Get(key, ShaderTarget::Metal);
  REQUIRE(fresh);
  CHECK(compiler->CompileCount() == before + 1);
  CHECK(fresh.get() != held.get());
  CHECK(fresh->source == source_before);
}

TEST_CASE("slang: Metal and HLSL agree on everything except binding locations",
          "[slang]") {
  // Probe B's central finding, pinned as a test: the parts the engine
  // normalizes really are identical across targets, and the part it keeps
  // per-target really does differ.
  auto compiler = MakeCompiler();
  REQUIRE(compiler);

  const ShaderKey key{.module = "fixture_compute", .entry = "cs_main"};
  auto metal = compiler->Get(key, ShaderTarget::Metal);
  auto hlsl = compiler->Get(key, ShaderTarget::Hlsl);
  REQUIRE(metal);
  REQUIRE(hlsl);
  CHECK(metal->source != hlsl->source);

  const auto& mr = metal->reflection;
  const auto& hr = hlsl->reflection;

  // Identical: names, kinds, workgroup size, uniform layout.
  REQUIRE(mr.bindings.size() == hr.bindings.size());
  for (const auto& mb : mr.bindings) {
    const auto* hb = hr.FindBinding(mb.name);
    REQUIRE(hb != nullptr);
    CHECK(hb->kind == mb.kind);
  }
  CHECK(mr.entry_points[0].workgroup_size[0] ==
        hr.entry_points[0].workgroup_size[0]);

  const auto* mblock = mr.FindUniformBlock("params");
  const auto* hblock = hr.FindUniformBlock("params");
  REQUIRE(mblock != nullptr);
  REQUIRE(hblock != nullptr);
  REQUIRE(mblock->members.size() == hblock->members.size());
  for (size_t i = 0; i < mblock->members.size(); ++i) {
    CHECK(mblock->members[i].name == hblock->members[i].name);
    CHECK(mblock->members[i].offset == hblock->members[i].offset);
  }

  // Divergent: Metal unifies structured buffers into one buffer index space
  // while D3D12 splits srv/uav, so the read-only and read-write buffers share
  // an index on D3D12 but not on Metal. This is exactly why BindingLocation is
  // per-target rather than a single (group, binding).
  const auto* m_in = mr.FindBinding("input");
  const auto* m_out = mr.FindBinding("output");
  const auto* h_in = hr.FindBinding("input");
  const auto* h_out = hr.FindBinding("output");
  REQUIRE(m_in);
  REQUIRE(m_out);
  REQUIRE(h_in);
  REQUIRE(h_out);
  CHECK(m_in->location.index != m_out->location.index);
  INFO("metal in/out = " << m_in->location.index << "/" << m_out->location.index
       << ", hlsl in/out = " << h_in->location.index << "/"
       << h_out->location.index);
  CHECK(h_in->location.index == 0);   // srv space
  CHECK(h_out->location.index == 0);  // uav space -- same index, different space
}

TEST_CASE("slang: a missing module or entry point fails cleanly", "[slang]") {
  auto compiler = MakeCompiler();
  REQUIRE(compiler);
  CHECK(compiler->Get({.module = "no_such_module", .entry = "cs_main"},
                      ShaderTarget::Metal) == nullptr);
  CHECK(compiler->Get({.module = "fixture_compute", .entry = "no_such_entry"},
                      ShaderTarget::Metal) == nullptr);
  // Failures are not cached, so fixing the shader and retrying works.
  CHECK(compiler->CacheSize() == 0);
}
