#include "rhi_pipelines.h"

#include <spdlog/spdlog.h>

namespace sq {

using namespace badlands::rhi;
namespace slang = badlands::slang;

namespace {

// STRAIGHT alpha: the gizmo chrome, the origin marker and the pivot are
// ordinary coloured vertices, not composited coverage. Deliberately NOT
// rhi::AlphaBlend(), whose alpha source factor is One -- that is a third
// convention, matching neither of the two this editor actually uses.
constexpr BlendState kStraightAlpha{
    .enabled = true,
    .color = {.src = BlendFactor::SrcAlpha,
              .dst = BlendFactor::OneMinusSrcAlpha,
              .op = BlendOp::Add},
    .alpha = {.src = BlendFactor::SrcAlpha,
              .dst = BlendFactor::OneMinusSrcAlpha,
              .op = BlendOp::Add}};

// PREMULTIPLIED: ground_grid_shade returns coverage already multiplied through
// (which is why gg_grazing_fade can scale all four channels), so the source
// factor is One rather than SrcAlpha.
constexpr BlendState kPremultiplied{
    .enabled = true,
    .color = {.src = BlendFactor::One,
              .dst = BlendFactor::OneMinusSrcAlpha,
              .op = BlendOp::Add},
    .alpha = {.src = BlendFactor::One,
              .dst = BlendFactor::OneMinusSrcAlpha,
              .op = BlendOp::Add}};

// Greater, not the RHI's GreaterEqual default: the metal-cpp path set
// MTL::CompareFunctionGreater against a 0.0 clear, so the port carries
// reversed-Z across unchanged and leaves equal-depth behaviour a rejection.
DepthState DepthWrite(Format depth) {
    return {.test_enabled = true,
            .write_enabled = true,
            .compare = CompareFunction::Greater,
            .format = depth};
}

DepthState DepthRead(Format depth) {
    return {.test_enabled = true,
            .write_enabled = false,
            .compare = CompareFunction::Greater,
            .format = depth};
}

// Test off, write off, format Undefined -- the chrome pass has no depth
// attachment, and a pipeline used in it must say so too.
DepthState NoDepth() { return {}; }

} // namespace

std::unique_ptr<RhiPipelines> RhiPipelines::Create(IRhiDevice& device,
                                                   slang::SlangCompiler& compiler,
                                                   Format color, Format depth) {
    auto p = std::make_unique<RhiPipelines>();
    int next = 0;

    auto load = [&](const char* module, const char* entry) -> IShaderModule* {
        auto compiled = compiler.Get({.module = module, .entry = entry},
                                     slang::ShaderTarget::Metal);
        if (!compiled) return nullptr; // the compiler logged the diagnostics
        auto m = device.CreateShaderModule(compiled->source, compiled->reflection,
                                           std::string(module) + "::" + entry);
        if (!m) return nullptr;
        p->modules_[next] = std::move(m);
        return p->modules_[next++].get();
    };

    auto* rm_vs = load("raymarch", "vs_main");
    auto* rm_fs = load("raymarch", "fs_main");
    auto* gg_vs = load("ground_grid", "vs_main");
    auto* gg_fs = load("ground_grid", "fs_main");
    auto* ms_vs = load("mesh", "vs_main");
    auto* ms_fs = load("mesh", "fs_main");
    auto* ln_vs = load("debug_lines", "vs_main");
    auto* ln_fs = load("debug_lines", "fs_main");
    if (!rm_vs || !rm_fs || !gg_vs || !gg_fs || !ms_vs || !ms_fs || !ln_vs || !ln_fs) {
        spdlog::error("shapeshifter: a shader module failed to build");
        return nullptr;
    }

    auto make = [&](IShaderModule* vs, IShaderModule* fs, DepthState d,
                    PrimitiveTopology topo, std::vector<BlendState> blend,
                    const char* label) {
        return device.CreateRenderPipeline({.vertex_shader = vs,
                                            .vertex_entry = "vs_main",
                                            .fragment_shader = fs,
                                            .fragment_entry = "fs_main",
                                            .color_formats = {color},
                                            .blend_states = std::move(blend),
                                            .depth = d,
                                            .topology = topo,
                                            // Every one of these is either a
                                            // fullscreen triangle or CPU-built
                                            // chrome with no consistent winding.
                                            .cull_mode = CullMode::None,
                                            .label = label});
    };

    p->raymarch = make(rm_vs, rm_fs, DepthWrite(depth),
                       PrimitiveTopology::TriangleList, {}, "raymarch");
    p->mesh = make(ms_vs, ms_fs, DepthWrite(depth),
                   PrimitiveTopology::TriangleList, {}, "mesh");
    p->ground = make(gg_vs, gg_fs, DepthRead(depth),
                     PrimitiveTopology::TriangleList, {kPremultiplied}, "ground");
    p->origin = make(ln_vs, ln_fs, DepthRead(depth),
                     PrimitiveTopology::TriangleList, {kStraightAlpha}, "origin");
    p->lines = make(ln_vs, ln_fs, NoDepth(), PrimitiveTopology::LineList, {},
                    "lines");
    p->blend_lines = make(ln_vs, ln_fs, NoDepth(), PrimitiveTopology::LineList,
                          {kStraightAlpha}, "blend_lines");
    p->blend_tris = make(ln_vs, ln_fs, NoDepth(), PrimitiveTopology::TriangleList,
                         {kStraightAlpha}, "blend_tris");

    if (!p->raymarch || !p->mesh || !p->ground || !p->origin || !p->lines ||
        !p->blend_lines || !p->blend_tris) {
        spdlog::error("shapeshifter: a render pipeline failed to build");
        return nullptr;
    }
    return p;
}

} // namespace sq
