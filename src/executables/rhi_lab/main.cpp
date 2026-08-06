// rhi_lab: the MVP that proves the RHI + Slang + visibility-buffer stack.
//
// Two modes. Without --window it renders one frame to a PNG and exits, which
// keeps it reproducible and diffable and is what scripts/screenshot.sh drives.
// With --window it opens a resizable window and flies a camera around, which
// is what exercises the frame model, the swapchain and resize.
//
// The window loop deliberately lives here rather than in the engine's SDL app
// shell: that shell is Dawn in its signatures (RenderContext hands out a
// wgpu::Device), so the RHI-era shell is a later, separate decision.
//
// The frame is the whole architecture in miniature:
//
//   compute cs_select   -> pick the LOD cut, compact survivors  (was ~1 ms of
//   compute cs_finalize -> publish the indirect args             CPU + ~908
//   raster  visbuffer   -> ONE indirect draw for all terrain      draws)
//                          + one instanced draw for all trees
//   raster  resolve     -> materials and lighting in screen space
//
// Nothing in the raster pass binds a material, which is exactly why a
// visibility buffer makes geometry fit a render graph.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>
#include <string>
#include <SDL3/SDL.h>

#include <initializer_list>
#include <optional>
#include <set>
#include <stdexcept>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <spdlog/spdlog.h>

extern "C" {
#include "badlands_assets.h"
}

#include "engine/rhi/metal/metal_rhi.hpp"
#include "engine/rhi/rhi_device.hpp"
#include "engine/rhi/rhi_frame_allocator.hpp"
#include "engine/slang/slang_compiler.hpp"
#include "game/geometry/terrain_clusters.hpp"
#include "src/executables/rhi_lab/lab_scene.hpp"
#include "engine/app/rhi_app_shell.hpp"

using namespace badlands;
using namespace badlands::rhi;

namespace {

// Mirrors LabFrame in shaders/slang/rhi_lab/lab_common.slang.
struct LabFrame {
  glm::mat4 view_proj{1.0f};
  glm::mat4 inv_view_proj{1.0f};
  glm::vec4 camera_pos{0.0f};
  glm::vec4 sun_direction{0.0f};
  glm::vec4 sun_color{0.0f};
  glm::vec4 ambient_sh[9]{};
  glm::vec4 params{0.0f};   // tau_px, screen_h, fov_deg, cluster_count
  glm::vec4 splat_uv{0.0f};
  glm::vec4 limits{0.0f};   // x = selected-buffer capacity
};

constexpr uint32_t kClusterIndexBudget = 384;  // 128 tris, matches the shader
constexpr uint32_t kTerrainLayers = 8;
constexpr uint32_t kLayerSize = 64;
constexpr uint32_t kSplatSize = 128;

struct Options {
  std::string out = "rhi_lab.png";
  uint32_t width = 1280;
  uint32_t height = 720;
  int nodes = 129;
  float spacing = 1.0f;
  int trees = 400;
  float tau = 1.5f;
  uint32_t seed = 7;
  bool debug_vis = false;
  bool windowed = false;
  // >0 runs that many windowed frames, resizing partway, and exits with a
  // status that says whether resize actually took. Needs a display, so it
  // is an opt-in ctest rather than part of the default suite.
  int self_test_frames = 0;
};

// std::stof and friends THROW on unparseable input, so `--tau x` used to end
// the process in std::terminate with no indication which flag was wrong.
// These say what they could not parse and refuse.
bool ParseFloat(const char* text, const char* what, float& out) {
  try {
    size_t used = 0;
    const float v = std::stof(text, &used);
    if (used != std::strlen(text)) throw std::invalid_argument("trailing");
    out = v;
    return true;
  } catch (const std::exception&) {
    spdlog::error("rhi_lab: {} needs a number, got '{}'", what, text);
    return false;
  }
}

// Takes the accepted range explicitly, because the caller narrows the result
// into an int or a uint32_t. Without the range check `--nodes 4294967298`
// parses fine, truncates to 2, passes every later guard, and renders a 2x2
// terrain with exit status 0 -- a silently wrong success, which is worse than
// the std::out_of_range this replaced.
bool ParseInt(const char* text, const char* what, long lo, long hi, long& out) {
  long v = 0;
  try {
    size_t used = 0;
    v = std::stol(text, &used);
    if (used != std::strlen(text)) throw std::invalid_argument("trailing");
  } catch (const std::exception&) {
    spdlog::error("rhi_lab: {} needs an integer, got '{}'", what, text);
    return false;
  }
  if (v < lo || v > hi) {
    spdlog::error("rhi_lab: {} must be between {} and {}, got {}", what, lo, hi,
                  v);
    return false;
  }
  out = v;
  return true;
}

bool ParseArgs(int argc, char** argv, Options& o) {
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&](const char* what) -> const char* {
      if (i + 1 >= argc) {
        spdlog::error("rhi_lab: {} needs a value", what);
        return nullptr;
      }
      return argv[++i];
    };
    long n = 0;
    // --screenshot is the project-wide flag scripts/screenshot.sh passes; this
    // app is headless so it means the same thing as --out.
    if (a == "--out" || a == "--screenshot") {
      const char* v = next(a.c_str());
      if (!v) return false;
      o.out = v;
    }
    else if (a == "--width") { const char* v = next("--width"); if (!v || !ParseInt(v, "--width", 1, 16384, n)) return false; o.width = uint32_t(n); }
    else if (a == "--height") { const char* v = next("--height"); if (!v || !ParseInt(v, "--height", 1, 16384, n)) return false; o.height = uint32_t(n); }
    else if (a == "--nodes") { const char* v = next("--nodes"); if (!v || !ParseInt(v, "--nodes", 2, 8193, n)) return false; o.nodes = int(n); }
    else if (a == "--trees") { const char* v = next("--trees"); if (!v || !ParseInt(v, "--trees", 0, 1'000'000, n)) return false; o.trees = int(n); }
    else if (a == "--tau") { const char* v = next("--tau"); if (!v || !ParseFloat(v, "--tau", o.tau)) return false; }
    else if (a == "--seed") { const char* v = next("--seed"); if (!v || !ParseInt(v, "--seed", 0, 0xFFFFFFFFL, n)) return false; o.seed = uint32_t(n); }
    else if (a == "--debug-vis") { o.debug_vis = true; }
    else if (a == "--window") { o.windowed = true; }
    else if (a == "--self-test") {
      const char* v = next("--self-test");
      if (!v || !ParseInt(v, "--self-test", 2, 10000, n)) return false;
      o.self_test_frames = int(n);
      o.windowed = true;
    }
    else if (a == "--help" || a == "-h") {
      std::printf(
          "rhi_lab [--out|--screenshot FILE] [--width N] [--height N]\n"
          "        [--trees N] [--tau PX] [--seed N] [--debug-vis]\n"
          "        [--window]   open a window; WASD/QE move, right-drag turns\n");
      return false;
    } else {
      spdlog::error("rhi_lab: unknown argument '{}'", a);
      return false;
    }
  }

  // cluster_select.slang keeps a cluster when `own_error <= tau < parent_error`.
  // Cluster errors are non-negative, so a tau of 0 or less keeps NOTHING, and
  // a NaN tau fails every comparison and also keeps nothing -- either way the
  // app renders empty terrain and says nothing about why.
  if (!(o.tau > 0.0f)) {  // written to catch NaN, which fails every compare
    constexpr float kMinTau = 0.01f;
    spdlog::warn("rhi_lab: --tau {} selects no clusters at all; clamping to {}",
                 o.tau, kMinTau);
    o.tau = kMinTau;
  }
  if (o.nodes < 2 || o.trees < 0 || o.width == 0 || o.height == 0) {
    spdlog::error("rhi_lab: need --nodes >= 2, --trees >= 0, and a non-zero "
                  "--width/--height (got {}, {}, {}x{})",
                  o.nodes, o.trees, o.width, o.height);
    return false;
  }
  return true;
}

// --- Reflection-driven binding ---------------------------------------------
//
// A preview of what the render graph will do automatically (D7): resolve a
// binding by NAME against reflection rather than hard-coding slots. Getting
// this wrong by hand is precisely the class of bug the graph removes.
// Returns nullopt on a name the shader does not declare. A default-constructed
// entry would carry slot 0 and BindingKind's first enumerator -- a plausible
// binding pointing at the wrong thing, which is the failure mode that costs an
// afternoon. There is no useful way to continue, so callers bail.
std::optional<BindingEntry> BindByName(const ShaderReflection& refl,
                                       const char* name,
                                       IBuffer* buffer = nullptr,
                                       ITextureView* view = nullptr,
                                       ISampler* sampler = nullptr,
                                       bool dynamic = false) {
  const auto* b = refl.FindBinding(name);
  if (!b) {
    spdlog::error("rhi_lab: shader has no binding named '{}'", name);
    return std::nullopt;
  }
  BindingEntry e;
  e.slot = b->slot;
  e.kind = b->kind;
  e.buffer = buffer;
  e.texture_view = view;
  e.sampler = sampler;
  e.dynamic_offset = dynamic;
  return e;
}

// Collapses a table's worth of lookups: nullopt if ANY name failed, so a
// half-built table never reaches CreateBindingTable.
std::optional<std::vector<BindingEntry>> Entries(
    std::initializer_list<std::optional<BindingEntry>> es) {
  std::vector<BindingEntry> out;
  out.reserve(es.size());
  for (const auto& e : es) {
    if (!e) return std::nullopt;
    out.push_back(*e);
  }
  return out;
}

template <typename T>
std::span<const uint8_t> Bytes(const std::vector<T>& v) {
  return {reinterpret_cast<const uint8_t*>(v.data()), v.size() * sizeof(T)};
}
template <typename T>
std::span<const uint8_t> Bytes(const T& v) {
  return {reinterpret_cast<const uint8_t*>(&v), sizeof(T)};
}

// --- Procedural material set ------------------------------------------------
//
// Eight distinct layers so the splat blend is visible. Real packs would come
// from MaterialLibrary; the point here is the blend, not the textures.
// Everything whose size follows the frame. Rebuilt as one unit on resize, at
// one point in the frame, so half a frame can never use each size.
struct Targets {
  uint32_t width = 0, height = 0;
  TexturePtr visbuf, depth, colour;
  BufferPtr readback, vis_readback;
};

bool MakeTargets(IRhiDevice& dev, uint32_t w, uint32_t h, Targets& out) {
  // Destroy() rather than drop: deferred deletion keeps the old ones alive for
  // frames still in flight, which is what lets a live resize skip WaitIdle and
  // not hitch.
  if (out.visbuf) out.visbuf->Destroy();
  if (out.depth) out.depth->Destroy();
  if (out.colour) out.colour->Destroy();
  if (out.readback) out.readback->Destroy();
  if (out.vis_readback) out.vis_readback->Destroy();

  out.width = w;
  out.height = h;
  out.visbuf = dev.CreateTexture({.width = w, .height = h,
                                  .format = Format::R32Uint,
                                  .usage = TextureUsage::RenderTarget |
                                           TextureUsage::Sampled |
                                           TextureUsage::CopySrc,
                                  .label = "visbuffer"});
  out.depth = dev.CreateTexture({.width = w, .height = h,
                                 .format = Format::Depth32Float,
                                 .usage = TextureUsage::DepthStencil |
                                          TextureUsage::Sampled,
                                 .label = "depth"});
  out.colour = dev.CreateTexture({.width = w, .height = h,
                                  .format = Format::RGBA8Unorm,
                                  .usage = TextureUsage::RenderTarget |
                                           TextureUsage::CopySrc,
                                  .label = "colour"});
  out.readback = dev.CreateBuffer(
      {.size = uint64_t(w) * h * 4,
       .usage = BufferUsage::CopyDst | BufferUsage::MapRead,
       .label = "readback"});
  out.vis_readback = dev.CreateBuffer(
      {.size = uint64_t(w) * h * 4,
       .usage = BufferUsage::CopyDst | BufferUsage::MapRead,
       .label = "vis_readback"});
  if (!out.visbuf || !out.depth || !out.colour || !out.readback ||
      !out.vis_readback) {
    spdlog::error("rhi_lab: could not create {}x{} targets", w, h);
    return false;
  }
  return true;
}

// A free-fly camera. WASD moves in the look plane, QE up and down, and the
// right mouse button turns.
struct FlyCamera {
  glm::vec3 position{0, 0, 0};
  float yaw = 0.0f;    // radians, around +Y
  float pitch = 0.0f;  // radians, clamped away from straight up/down
  float speed = 60.0f;

  glm::vec3 Forward() const {
    return glm::normalize(glm::vec3(std::cos(pitch) * std::sin(yaw),
                                    std::sin(pitch),
                                    std::cos(pitch) * std::cos(yaw)));
  }
  glm::vec3 Right() const {
    return glm::normalize(glm::cross(Forward(), glm::vec3(0, 1, 0)));
  }
  glm::mat4 View() const {
    return glm::lookAt(position, position + Forward(), glm::vec3(0, 1, 0));
  }
  void Turn(float dyaw, float dpitch) {
    yaw += dyaw;
    // Clamped short of vertical: at exactly +-90 degrees the up vector and the
    // look direction are parallel and lookAt produces NaNs.
    constexpr float kLimit = 1.55f;
    pitch = std::clamp(pitch + dpitch, -kLimit, kLimit);
  }
};

TexturePtr MakeLayerArray(IRhiDevice& dev, const char* label, bool arm) {
  auto tex = dev.CreateTexture({.width = kLayerSize,
                                .height = kLayerSize,
                                .array_layers = kTerrainLayers,
                                .format = Format::RGBA8Unorm,
                                .usage = TextureUsage::Sampled | TextureUsage::CopyDst,
                                .label = label});
  if (!tex) return nullptr;

  std::vector<uint8_t> pixels(size_t(kLayerSize) * kLayerSize * 4);
  for (uint32_t layer = 0; layer < kTerrainLayers; ++layer) {
    const float hue = float(layer) / float(kTerrainLayers);
    for (uint32_t y = 0; y < kLayerSize; ++y) {
      for (uint32_t x = 0; x < kLayerSize; ++x) {
        const size_t o = (size_t(y) * kLayerSize + x) * 4;
        // A little per-texel variation so tiling is visible in the output.
        const float g = 0.85f + 0.15f * std::sin(float(x) * 0.7f + float(y) * 0.4f);
        if (arm) {
          pixels[o + 0] = 235;                                   // AO
          pixels[o + 1] = uint8_t(120 + 100 * hue);              // roughness
          pixels[o + 2] = 0;                                     // metal
          // Displacement drives the height-lerp; varying it per layer is what
          // makes the blend interlock rather than cross-fade.
          pixels[o + 3] = uint8_t(60 + 180 * std::fabs(std::sin(hue * 6.0f + g)));
        } else {
          const float r = 0.5f + 0.5f * std::cos(6.2831853f * (hue + 0.00f));
          const float gg = 0.5f + 0.5f * std::cos(6.2831853f * (hue + 0.33f));
          const float b = 0.5f + 0.5f * std::cos(6.2831853f * (hue + 0.67f));
          pixels[o + 0] = uint8_t(std::clamp(r * g, 0.0f, 1.0f) * 255);
          pixels[o + 1] = uint8_t(std::clamp(gg * g, 0.0f, 1.0f) * 255);
          pixels[o + 2] = uint8_t(std::clamp(b * g, 0.0f, 1.0f) * 255);
          pixels[o + 3] = 255;
        }
      }
    }
    tex->Write(0, layer, Bytes(pixels));
  }
  return tex;
}

// Two splat textures carrying eight biome weights in world XZ. This is the
// decision that keeps terrain material off bindless: weights come from here,
// not from vertex data, so the resolve needs only world position.
void MakeSplats(IRhiDevice& dev, TexturePtr& s0, TexturePtr& s1) {
  auto make = [&](const char* label) {
    return dev.CreateTexture({.width = kSplatSize,
                              .height = kSplatSize,
                              .format = Format::RGBA8Unorm,
                              .usage = TextureUsage::Sampled | TextureUsage::CopyDst,
                              .label = label});
  };
  s0 = make("biome_splat0");
  s1 = make("biome_splat1");
  if (!s0 || !s1) return;

  std::vector<uint8_t> a(size_t(kSplatSize) * kSplatSize * 4, 0);
  std::vector<uint8_t> b(size_t(kSplatSize) * kSplatSize * 4, 0);
  for (uint32_t y = 0; y < kSplatSize; ++y) {
    for (uint32_t x = 0; x < kSplatSize; ++x) {
      const size_t o = (size_t(y) * kSplatSize + x) * 4;
      const float u = float(x) / float(kSplatSize - 1);
      const float v = float(y) / float(kSplatSize - 1);
      // Smooth, overlapping bands so several layers are genuinely blended
      // rather than one winning everywhere.
      float w[8];
      float sum = 0.0f;
      for (int i = 0; i < 8; ++i) {
        const float centre = (float(i) + 0.5f) / 8.0f;
        const float d = std::fabs((u * 0.6f + v * 0.4f) - centre);
        w[i] = std::max(0.0f, 1.0f - d * 5.0f);
        sum += w[i];
      }
      if (sum <= 0.0f) { w[0] = 1.0f; sum = 1.0f; }
      for (int i = 0; i < 4; ++i) a[o + i] = uint8_t(w[i] / sum * 255.0f);
      for (int i = 0; i < 4; ++i) b[o + i] = uint8_t(w[i + 4] / sum * 255.0f);
    }
  }
  s0->Write(0, 0, Bytes(a));
  s1->Write(0, 0, Bytes(b));
}

}  // namespace

int main(int argc, char** argv) {
  spdlog::set_pattern("%^[%l]%$ %v");
  Options opt;
  if (!ParseArgs(argc, argv, opt)) return 1;

  // --- Scene -------------------------------------------------------------
  lab::Scene scene = lab::BuildScene(opt.nodes, opt.spacing, opt.trees, opt.seed);
  if (scene.clusters.empty()) {
    spdlog::error("rhi_lab: empty cluster DAG");
    return 1;
  }

  // --- Device and shaders ------------------------------------------------
  auto device = CreateDevice({.backend = BackendKind::Metal,
                              .enable_validation = true,
                              .label = "rhi_lab"});
  if (!device) return 1;

  const std::vector<std::string> shader_paths = {"shaders/slang/rhi_lab"};
  auto compiler = slang::CreateSlangCompiler(shader_paths);
  if (!compiler) return 1;

  auto load = [&](const char* module, const char* entry) -> ShaderModulePtr {
    auto compiled = compiler->Get({.module = module, .entry = entry},
                                  slang::ShaderTarget::Metal);
    if (!compiled) return nullptr;
    return device->CreateShaderModule(compiled->source, compiled->reflection,
                                      std::string(module) + "::" + entry);
  };

  auto sel_module = load("cluster_select", "cs_select");
  auto fin_module = load("cluster_finalize", "cs_finalize");
  auto vis_vs = load("terrain_vis", "vs_terrain");
  auto vis_fs = load("terrain_vis", "fs_vis");
  auto tree_vs = load("tree_vis", "vs_trees");
  auto tree_fs = load("tree_vis", "fs_vis");
  auto res_vs = load("resolve", "vs_fullscreen");
  auto res_fs = load("resolve", opt.debug_vis ? "fs_visbuffer_debug" : "fs_resolve");
  if (!sel_module || !fin_module || !vis_vs || !vis_fs || !tree_vs || !tree_fs ||
      !res_vs || !res_fs) {
    return 1;
  }

  auto select_pipe = device->CreateComputePipeline(
      {.shader = sel_module.get(), .entry = "cs_select", .label = "select"});
  auto finalize_pipe = device->CreateComputePipeline(
      {.shader = fin_module.get(), .entry = "cs_finalize", .label = "finalize"});
  auto terrain_pipe = device->CreateRenderPipeline(
      {.vertex_shader = vis_vs.get(), .vertex_entry = "vs_terrain",
       .fragment_shader = vis_fs.get(), .fragment_entry = "fs_vis",
       .color_formats = {Format::R32Uint},
       .depth = {.test_enabled = true, .write_enabled = true,
                 .compare = CompareFunction::GreaterEqual,
                 .format = Format::Depth32Float},
       .cull_mode = CullMode::None, .label = "terrain_vis"});
  auto tree_pipe = device->CreateRenderPipeline(
      {.vertex_shader = tree_vs.get(), .vertex_entry = "vs_trees",
       .fragment_shader = tree_fs.get(), .fragment_entry = "fs_vis",
       .color_formats = {Format::R32Uint},
       .depth = {.test_enabled = true, .write_enabled = true,
                 .compare = CompareFunction::GreaterEqual,
                 .format = Format::Depth32Float},
       .cull_mode = CullMode::None, .label = "tree_vis"});
  // The resolve writes straight into the backbuffer when there is a window, so
  // its colour format has to match one. CAMetalLayer accepts BGRA8Unorm and
  // not RGBA8Unorm -- the channel order is the hardware's business, so the
  // shader is unchanged either way.
  const Format present_format =
      opt.windowed ? Format::BGRA8Unorm : Format::RGBA8Unorm;
  auto resolve_pipe = device->CreateRenderPipeline(
      {.vertex_shader = res_vs.get(), .vertex_entry = "vs_fullscreen",
       .fragment_shader = res_fs.get(),
       .fragment_entry = opt.debug_vis ? "fs_visbuffer_debug" : "fs_resolve",
       .color_formats = {present_format},
       .cull_mode = CullMode::None, .label = "resolve"});
  if (!select_pipe || !finalize_pipe || !terrain_pipe || !tree_pipe ||
      !resolve_pipe) {
    return 1;
  }

  // --- Buffers -----------------------------------------------------------
  const uint32_t capacity = uint32_t(scene.clusters.size());
  auto make_buffer = [&](uint64_t size, BufferUsage usage, const char* label) {
    return device->CreateBuffer({.size = size, .usage = usage, .label = label});
  };

  auto vtx = make_buffer(scene.dag.vertices.size() * sizeof(float),
                         BufferUsage::Storage | BufferUsage::CopyDst, "vertices");
  auto idx = make_buffer(scene.dag.indices.size() * sizeof(uint32_t),
                         BufferUsage::Storage | BufferUsage::CopyDst, "indices");
  auto clu = make_buffer(scene.clusters.size() * sizeof(lab::ClusterGpu),
                         BufferUsage::Storage | BufferUsage::CopyDst, "clusters");
  auto sel = make_buffer(uint64_t(capacity) * sizeof(uint32_t),
                         BufferUsage::Storage | BufferUsage::CopyDst, "selected");
  auto args = make_buffer(sizeof(DrawIndexedIndirectArgs),
                          BufferUsage::Storage | BufferUsage::Indirect |
                              BufferUsage::CopyDst | BufferUsage::MapRead,
                          "draw_args");
  auto counter = make_buffer(sizeof(uint32_t),
                             BufferUsage::Storage | BufferUsage::CopyDst |
                                 BufferUsage::MapRead,
                             "draw_counter");
  auto frame_ubo = make_buffer(sizeof(LabFrame),
                               BufferUsage::Uniform | BufferUsage::CopyDst,
                               "frame");
  auto tree_vtx = make_buffer(scene.tree_vertices.size() * sizeof(lab::LabVertex),
                              BufferUsage::Storage | BufferUsage::CopyDst,
                              "tree_vertices");
  auto tree_idx = make_buffer(scene.tree_indices.size() * sizeof(uint32_t),
                              BufferUsage::Storage | BufferUsage::CopyDst,
                              "tree_indices");
  auto tree_inst = make_buffer(scene.trees.size() * sizeof(lab::TreeInstance),
                               BufferUsage::Storage | BufferUsage::CopyDst,
                               "trees");

  // The terrain draw is instanced over a fixed index range; each instance
  // pulls its own cluster's indices, so this buffer is just 0..383.
  std::vector<uint32_t> dummy_indices(kClusterIndexBudget);
  for (uint32_t i = 0; i < kClusterIndexBudget; ++i) dummy_indices[i] = i;
  auto dummy_idx = make_buffer(dummy_indices.size() * sizeof(uint32_t),
                               BufferUsage::Index | BufferUsage::CopyDst,
                               "dummy_indices");
  auto tree_draw_idx = make_buffer(scene.tree_indices.size() * sizeof(uint32_t),
                                   BufferUsage::Index | BufferUsage::CopyDst,
                                   "tree_draw_indices");

  vtx->Write(0, Bytes(scene.dag.vertices));
  idx->Write(0, Bytes(scene.dag.indices));
  clu->Write(0, Bytes(scene.clusters));
  tree_vtx->Write(0, Bytes(scene.tree_vertices));
  tree_idx->Write(0, Bytes(scene.tree_indices));
  tree_inst->Write(0, Bytes(scene.trees));
  dummy_idx->Write(0, Bytes(dummy_indices));
  tree_draw_idx->Write(0, Bytes(scene.tree_indices));
  const uint32_t zero = 0;
  counter->Write(0, Bytes(zero));

  // --- Textures ----------------------------------------------------------
  auto albedo = MakeLayerArray(*device, "albedo_array", /*arm=*/false);
  auto arm = MakeLayerArray(*device, "arm_array", /*arm=*/true);
  TexturePtr splat0, splat1;
  MakeSplats(*device, splat0, splat1);
  auto terrain_sampler = device->CreateSampler(
      {.address_u = AddressMode::Repeat, .address_v = AddressMode::Repeat,
       .max_anisotropy = 4, .label = "terrain_sampler"});
  auto splat_sampler = device->CreateSampler(
      {.address_u = AddressMode::ClampToEdge,
       .address_v = AddressMode::ClampToEdge, .label = "splat_sampler"});
  if (!albedo || !arm || !splat0 || !splat1) return 1;

  // Everything sized to the frame lives in one struct, so a resize rebuilds
  // exactly this set and nothing else -- including the resolve table, which
  // references the visbuffer and depth views and is immutable.
  Targets targets;
  if (!MakeTargets(*device, opt.width, opt.height, targets)) return 1;

  // --- Frame constants ---------------------------------------------------
  LabFrame f{};
  const float cx = scene.size_x_m * 0.5f, cz = scene.size_z_m * 0.5f;
  const float dist = std::max(scene.size_x_m, scene.size_z_m) * 0.85f;
  const glm::vec3 start_eye{cx - dist * 0.6f, scene.max_height_m + dist * 0.45f,
                            cz - dist * 0.6f};
  const glm::vec3 start_target{cx, scene.max_height_m * 0.3f, cz};
  const float fov_deg = 55.0f;

  // The windowed camera starts exactly where the headless shot looks from, so
  // the two views are comparable.
  FlyCamera cam;
  cam.position = start_eye;
  {
    const glm::vec3 dir = glm::normalize(start_target - start_eye);
    cam.yaw = std::atan2(dir.x, dir.z);
    cam.pitch = std::asin(std::clamp(dir.y, -1.0f, 1.0f));
    cam.speed = std::max(20.0f, dist * 0.35f);
  }

  // Per-frame uniforms come from the ring, not from one buffer rewritten in
  // place. With three frames in flight, `frame_ubo->Write(0, ...)` at the top
  // of frame N memcpys over bytes the GPU is still reading for N-1 and N-2:
  // the camera a frame renders with is whichever write happened to land, so
  // geometry from one position gets resolved with another's inv_view_proj.
  // The tear is subtle enough that the image still looks right, which is why
  // it survived a windowed run and two reviews.
  auto frame_alloc = FrameAllocator::Create(
      *device, {.block_size = 64 * 1024,
                .usage = BufferUsage::Uniform,
                .label = "lab_frame_ring"});
  if (!frame_alloc) return 1;

  // The tables name this buffer once; only the offset moves per frame.
  IBuffer* frame_ring = frame_alloc->PrimaryBuffer();
  if (!frame_ring) return 1;

  size_t cpu_selected = 0;
  // The slice this frame's uniforms live in. Every table binds the ring with
  // this as a dynamic offset.
  uint32_t frame_offset = 0;
  auto update_frame_uniforms = [&](uint32_t w, uint32_t h) {
    const float aspect = float(w) / float(std::max(1u, h));
    const glm::mat4 view = cam.View();
    // Reversed-Z: swapping near and far in a zero-to-one projection maps
    // near->1 and far->0, which is the project-wide convention.
    const glm::mat4 proj =
        glm::perspective(glm::radians(fov_deg), aspect, 4000.0f, 0.1f);
    f.view_proj = proj * view;
    f.inv_view_proj = glm::inverse(f.view_proj);
    f.camera_pos = glm::vec4(cam.position, 1.0f);
    f.sun_direction =
        glm::vec4(glm::normalize(glm::vec3(0.45f, 0.75f, 0.35f)), 0);
    f.sun_color = glm::vec4(1.9f, 1.75f, 1.5f, 0.0f);
    // A plain sky-ish ambient: DC term plus a gentle vertical gradient.
    f.ambient_sh[0] = glm::vec4(0.32f, 0.38f, 0.48f, 0.0f);
    f.ambient_sh[2] = glm::vec4(0.10f, 0.12f, 0.16f, 0.0f);
    // Screen height drives the cluster error threshold, so it MUST be this
    // frame's height rather than the startup one, or a resized window selects
    // clusters for a size it is no longer rendering at.
    f.params = glm::vec4(opt.tau, float(h), fov_deg, float(capacity));
    f.splat_uv = glm::vec4(1.0f / std::max(scene.size_x_m, 1.0f),
                           1.0f / std::max(scene.size_z_m, 1.0f), 0.0f, 0.0f);
    f.limits = glm::vec4(float(capacity), 0, 0, 0);

    auto slice = frame_alloc->Write(Bytes(f));
    if (!slice) {
      // Logged by the allocator. Reusing the previous offset would render the
      // previous frame's camera, which reads as stutter rather than as an
      // error.
      spdlog::error("rhi_lab: no room for this frame's uniforms");
      return false;
    }
    frame_offset = uint32_t(slice->offset);

    // Oracle: the CPU selector this pass replaces. Same camera, same tau. If
    // the GPU picks a different number the port of the rule is wrong, and that
    // shows up as holes in the terrain rather than as an error.
    std::vector<uint32_t> cpu_cut;
    SelectClusters(scene.dag, cam.position, fov_deg, float(h), opt.tau,
                   cpu_cut);
    cpu_selected = cpu_cut.size();
    return true;
  };

  // --- Binding tables ----------------------------------------------------
  const auto& sel_refl = select_pipe->GetReflection();
  const auto& fin_refl = finalize_pipe->GetReflection();
  const auto& terrain_refl = terrain_pipe->GetReflection();
  const auto& tree_refl = tree_pipe->GetReflection();
  const auto& res_refl = resolve_pipe->GetReflection();

  auto sel_entries =
      Entries({BindByName(sel_refl, "frame", frame_ring, nullptr, nullptr, /*dynamic=*/true),
               BindByName(sel_refl, "clusters", clu.get()),
               BindByName(sel_refl, "selected", sel.get()),
               BindByName(sel_refl, "draw_args", args.get()),
               BindByName(sel_refl, "draw_counter", counter.get())});
  auto fin_entries =
      Entries({BindByName(fin_refl, "frame", frame_ring, nullptr, nullptr, /*dynamic=*/true),
               BindByName(fin_refl, "draw_args", args.get()),
               BindByName(fin_refl, "draw_counter", counter.get())});
  auto terrain_entries =
      Entries({BindByName(terrain_refl, "frame", frame_ring, nullptr, nullptr, /*dynamic=*/true),
               BindByName(terrain_refl, "vertices", vtx.get()),
               BindByName(terrain_refl, "indices", idx.get()),
               BindByName(terrain_refl, "clusters", clu.get()),
               BindByName(terrain_refl, "selected", sel.get())});
  auto tree_entries =
      Entries({BindByName(tree_refl, "frame", frame_ring, nullptr, nullptr, /*dynamic=*/true),
               BindByName(tree_refl, "trees", tree_inst.get()),
               BindByName(tree_refl, "tree_vertices", tree_vtx.get()),
               BindByName(tree_refl, "tree_indices", tree_idx.get())});
  // resolve.slang keeps both fragment entries, so reflection reports every
  // global whichever one is selected. Bind the full set rather than tracking
  // which entry uses what -- that is the graph's job later, not the app's.
  //
  // A lambda because this is the ONLY table referencing size-dependent views.
  // Binding tables are immutable, so a resize cannot patch it -- it has to be
  // built again against the new visbuffer and depth.
  BindingTablePtr resolve_table;
  auto rebuild_resolve = [&]() {
    auto res_entries = Entries({
        BindByName(res_refl, "frame", frame_ring, nullptr, nullptr, /*dynamic=*/true),
        BindByName(res_refl, "visbuffer", nullptr,
                   targets.visbuf->GetDefaultView()),
        BindByName(res_refl, "depthbuffer", nullptr,
                   targets.depth->GetDefaultView()),
        BindByName(res_refl, "albedo_array", nullptr, albedo->GetDefaultView()),
        BindByName(res_refl, "arm_array", nullptr, arm->GetDefaultView()),
        BindByName(res_refl, "biome_splat0", nullptr, splat0->GetDefaultView()),
        BindByName(res_refl, "biome_splat1", nullptr, splat1->GetDefaultView()),
        BindByName(res_refl, "terrain_sampler", nullptr, nullptr,
                   terrain_sampler.get()),
        BindByName(res_refl, "splat_sampler", nullptr, nullptr,
                   splat_sampler.get()),
    });
    if (!res_entries) {
      spdlog::error("rhi_lab: resolve binding resolution failed");
      return false;
    }
    resolve_table = device->CreateBindingTable(
        {.render_pipeline = resolve_pipe.get(), .entries = *res_entries,
         .label = "resolve"});
    return resolve_table != nullptr;
  };

  // Bail before creating a single table: a name that did not resolve has
  // already been logged, and a table built from the rest would render
  // something subtly wrong rather than nothing at all.
  if (!sel_entries || !fin_entries || !terrain_entries || !tree_entries) {
    spdlog::error("rhi_lab: binding resolution failed, aborting");
    return 1;
  }

  auto select_table = device->CreateBindingTable(
      {.compute_pipeline = select_pipe.get(), .entries = *sel_entries,
       .label = "select"});
  auto finalize_table = device->CreateBindingTable(
      {.compute_pipeline = finalize_pipe.get(), .entries = *fin_entries,
       .label = "finalize"});
  auto terrain_table = device->CreateBindingTable(
      {.render_pipeline = terrain_pipe.get(), .entries = *terrain_entries,
       .label = "terrain"});
  auto tree_table = device->CreateBindingTable(
      {.render_pipeline = tree_pipe.get(), .entries = *tree_entries,
       .label = "trees"});

  if (!select_table || !finalize_table || !terrain_table || !tree_table) {
    return 1;
  }
  if (!rebuild_resolve()) return 1;

  // --- Frame -------------------------------------------------------------
  //
  // One body, two callers: the headless path renders into `colour` and reads
  // it back for the PNG; the windowed path renders straight into the acquired
  // backbuffer. `present` selects which.
  auto record_frame = [&](ITextureView* present, uint32_t w, uint32_t h,
                          bool want_readback) {
  auto encoder = device->CreateCommandEncoder("lab_frame");

  // Selection: classify then publish. Both write the args buffer, so it is
  // declared ShaderWrite here and IndirectArg before the draw -- the sort of
  // transition Metal ignores and the validation layer checks.
  encoder->Transition(frame_ring, ResourceState::ShaderRead);
  encoder->Transition(clu.get(), ResourceState::ShaderRead);
  encoder->Transition(sel.get(), ResourceState::ShaderWrite);
  encoder->Transition(args.get(), ResourceState::ShaderWrite);
  encoder->Transition(counter.get(), ResourceState::ShaderWrite);

  uint32_t select_wg[3] = {64, 1, 1};
  select_pipe->GetWorkgroupSize(select_wg);
  const uint32_t groups = (capacity + select_wg[0] - 1) / select_wg[0];

  auto* cs = encoder->BeginComputePass("select");
  cs->SetPipeline(select_pipe.get());
  cs->SetBindingTable(0, select_table.get(), {&frame_offset, 1});
  cs->Dispatch(groups);
  cs->End();

  auto* cf = encoder->BeginComputePass("finalize");
  cf->SetPipeline(finalize_pipe.get());
  cf->SetBindingTable(0, finalize_table.get(), {&frame_offset, 1});
  cf->Dispatch(1);
  cf->End();

  // Visibility buffer: one indirect draw for all terrain, one instanced draw
  // for all trees. Neither binds a material.
  encoder->Transition(sel.get(), ResourceState::ShaderRead);
  encoder->Transition(args.get(), ResourceState::IndirectArg);
  encoder->Transition(vtx.get(), ResourceState::ShaderRead);
  encoder->Transition(idx.get(), ResourceState::ShaderRead);
  encoder->Transition(tree_vtx.get(), ResourceState::ShaderRead);
  encoder->Transition(tree_idx.get(), ResourceState::ShaderRead);
  encoder->Transition(tree_inst.get(), ResourceState::ShaderRead);
  encoder->Transition(targets.visbuf.get(), ResourceState::RenderTarget);
  encoder->Transition(targets.depth.get(), ResourceState::DepthWrite);

  RenderPassDesc vis_pass;
  vis_pass.label = "visbuffer";
  vis_pass.color_attachments.push_back({.view = targets.visbuf->GetDefaultView(),
                                        .load_op = LoadOp::Clear,
                                        .store_op = StoreOp::Store,
                                        .clear_color = {0, 0, 0, 0}});
  vis_pass.depth_attachment = {.view = targets.depth->GetDefaultView(),
                               .load_op = LoadOp::Clear,
                               .store_op = StoreOp::Store,
                               .clear_depth = 0.0f};  // reversed-Z far
  auto* vp = encoder->BeginRenderPass(vis_pass);
  vp->SetViewport(0, 0, float(w), float(h));
  vp->SetPipeline(terrain_pipe.get());
  vp->SetBindingTable(0, terrain_table.get(), {&frame_offset, 1});
  vp->SetIndexBuffer(dummy_idx.get(), IndexFormat::Uint32);
  vp->DrawIndexedIndirect(args.get(), 0);

  vp->SetPipeline(tree_pipe.get());
  vp->SetBindingTable(0, tree_table.get(), {&frame_offset, 1});
  vp->SetIndexBuffer(tree_draw_idx.get(), IndexFormat::Uint32);
  vp->DrawIndexed(uint32_t(scene.tree_indices.size()),
                  uint32_t(scene.trees.size()));
  vp->End();

  // Resolve: every material in the scene, in screen space, from a fixed set of
  // bindings.
  encoder->Transition(targets.visbuf.get(), ResourceState::ShaderRead);
  encoder->Transition(targets.depth.get(), ResourceState::ShaderRead);
  encoder->Transition(albedo.get(), ResourceState::ShaderRead);
  encoder->Transition(arm.get(), ResourceState::ShaderRead);
  encoder->Transition(splat0.get(), ResourceState::ShaderRead);
  encoder->Transition(splat1.get(), ResourceState::ShaderRead);
  // The RESOLVE TARGET, which is the backbuffer when there is a window. A
  // freshly acquired drawable is a new resource every frame, so its state
  // starts Undefined every frame and has to be declared each time.
  ITexture* resolve_target =
      present ? present->GetTexture() : targets.colour.get();
  encoder->Transition(resolve_target, ResourceState::RenderTarget);

  RenderPassDesc res_pass;
  res_pass.label = "resolve";
  res_pass.color_attachments.push_back(
      {.view = present ? present : targets.colour->GetDefaultView(),
       .load_op = LoadOp::Clear,
       .store_op = StoreOp::Store});
  auto* rp = encoder->BeginRenderPass(res_pass);
  if (!rp) return;
  rp->SetViewport(0, 0, float(w), float(h));
  rp->SetPipeline(resolve_pipe.get());
  rp->SetBindingTable(0, resolve_table.get(), {&frame_offset, 1});
  rp->Draw(3);
  rp->End();

  if (want_readback) {
    encoder->Transition(targets.colour.get(), ResourceState::CopySrc);
    encoder->Transition(targets.readback.get(), ResourceState::CopyDst);
    encoder->CopyTextureToBuffer(targets.colour.get(), 0, 0,
                                 targets.readback.get(), 0);
    encoder->Transition(targets.visbuf.get(), ResourceState::CopySrc);
    encoder->Transition(targets.vis_readback.get(), ResourceState::CopyDst);
    encoder->CopyTextureToBuffer(targets.visbuf.get(), 0, 0,
                                 targets.vis_readback.get(), 0);
  }
  encoder->Finish();
  device->Submit(*encoder);
  };  // record_frame

  if (opt.windowed) {
    // The window, the swapchain, the resize coalescing and the frame pacing all
    // live in AppShell now -- shared with object_viewer, because a second copy
    // of that loop is a second copy of every macOS input and HiDPI bug it took
    // several rounds to get right.
    auto shell = rhi_app::AppShell::Create(
        *device, {.title = "badlands rhi_lab",
                  .width = opt.width,
                  .height = opt.height,
                  .present_format = Format::BGRA8Unorm});
    if (!shell) return 1;
    if (!MakeTargets(*device, shell->Width(), shell->Height(), targets)) return 1;
    if (!rebuild_resolve()) return 1;

    spdlog::info(
        "rhi_lab: WASD/QE to move, hold right mouse to look, shift to go "
        "faster, Esc to quit");

    // Self-test bookkeeping. The request is in POINTS, because that is what
    // SDL_SetWindowSize takes; everything downstream is in PIXELS, which on a
    // HiDPI display is a different number. Asserting against the requested
    // value compares the two and fails on a correct implementation -- which is
    // exactly what this test did on its first run.
    int point_w = 0, point_h = 0;
    SDL_GetWindowSize(shell->Window(), &point_w, &point_h);
    const int test_point_w = point_w / 2 + 64;
    const int test_point_h = point_h / 2 + 32;
    const uint32_t initial_pixel_w = shell->Width();
    uint32_t rendered_after_resize = 0;
    // Which uniform slices the frames actually used. If consecutive frames
    // share one, the CPU is overwriting bytes the GPU may still be reading --
    // the hazard the ring exists to remove, and one that renders a plausible
    // image while it happens.
    std::set<uint32_t> frame_offsets_seen;

    rhi_app::AppShellCallbacks cb;
    cb.OnEvent = [&](const SDL_Event& e) {
      if (e.type == SDL_EVENT_MOUSE_MOTION && (e.motion.state & SDL_BUTTON_RMASK)) {
        constexpr float kLookRate = 0.0035f;
        cam.Turn(-e.motion.xrel * kLookRate, -e.motion.yrel * kLookRate);
        return true;
      }
      return false;
    };
    cb.OnUpdate = [&](const rhi_app::FrameInfo& f) {
      const float boost = f.keys[SDL_SCANCODE_LSHIFT] ? 4.0f : 1.0f;
      const float step = cam.speed * boost * std::min(f.dt, 0.1f);
      if (f.keys[SDL_SCANCODE_W]) cam.position += cam.Forward() * step;
      if (f.keys[SDL_SCANCODE_S]) cam.position -= cam.Forward() * step;
      if (f.keys[SDL_SCANCODE_D]) cam.position += cam.Right() * step;
      if (f.keys[SDL_SCANCODE_A]) cam.position -= cam.Right() * step;
      if (f.keys[SDL_SCANCODE_E]) cam.position.y += step;
      if (f.keys[SDL_SCANCODE_Q]) cam.position.y -= step;

      // Scripted resize, partway through a self-test run. Requested through the
      // SAME window-manager path a user drag uses, so the test exercises the
      // coalescing rather than bypassing it.
      if (opt.self_test_frames > 0 &&
          f.index == uint64_t(opt.self_test_frames / 2)) {
        shell->RequestResizePoints(uint32_t(test_point_w),
                                   uint32_t(test_point_h));
      }
    };
    cb.OnFrameBegin = [&](uint64_t frame_index) {
      // Recycles this frame's slot. Safe without a check: the shell's
      // BeginFrame already blocked until the frame that owned it retired.
      frame_alloc->BeginFrame(frame_index);
    };
    cb.OnResize = [&](uint32_t w, uint32_t h) {
      if (!MakeTargets(*device, w, h, targets)) return false;
      // The resolve table is immutable and holds the OLD visbuffer and depth
      // views, so it has to be rebuilt too.
      return rebuild_resolve();
    };
    cb.OnRender = [&](ITextureView* target, const rhi_app::FrameInfo& f) {
      if (!update_frame_uniforms(f.width, f.height)) return false;
      record_frame(target, f.width, f.height, /*want_readback=*/false);
      frame_offsets_seen.insert(frame_offset);
      if (f.width != initial_pixel_w) ++rendered_after_resize;
      return true;
    };

    const auto stats = shell->Run(cb, opt.self_test_frames);

    if (opt.self_test_frames > 0) {
      // Exit status IS the assertion: this runs as a ctest with no test
      // framework around it.
      if (stats.frames_presented == 0) {
        spdlog::error("rhi_lab self-test: no frame ever rendered");
        return 1;
      }
      if (stats.final_width == initial_pixel_w) {
        spdlog::error(
            "rhi_lab self-test: the window never actually resized (still {} "
            "pixels wide) -- the test proved nothing",
            stats.final_width);
        return 1;
      }
      // The invariant: everything sized to the frame follows the reported
      // PIXEL size, whatever the backing scale turns the request into.
      if (targets.width != stats.final_width ||
          targets.height != stats.final_height) {
        spdlog::error("rhi_lab self-test: targets are {}x{}, window is {}x{}",
                      targets.width, targets.height, stats.final_width,
                      stats.final_height);
        return 1;
      }
      if (shell->Swapchain()->GetWidth() != stats.final_width ||
          shell->Swapchain()->GetHeight() != stats.final_height) {
        spdlog::error("rhi_lab self-test: swapchain is {}x{}, window is {}x{}",
                      shell->Swapchain()->GetWidth(),
                      shell->Swapchain()->GetHeight(), stats.final_width,
                      stats.final_height);
        return 1;
      }
      if (rendered_after_resize == 0) {
        spdlog::error(
            "rhi_lab self-test: resize took, but nothing rendered afterwards");
        return 1;
      }
      // Distinct slices, one per frame in flight. A single reused offset means
      // the per-frame uniforms are being overwritten under the GPU.
      if (frame_offsets_seen.size() < device->FramesInFlight()) {
        spdlog::error(
            "rhi_lab self-test: only {} distinct uniform slice(s) across {} "
            "frames with {} in flight -- the ring is not rotating and frame "
            "N is overwriting what N-1 is still reading",
            frame_offsets_seen.size(), stats.frames_presented,
            device->FramesInFlight());
        return 1;
      }
      spdlog::info(
          "rhi_lab self-test OK: {} frames, {} after resizing {} -> {} pixels "
          "wide, {} distinct uniform slices",
          stats.frames_presented, rendered_after_resize, initial_pixel_w,
          stats.final_width, frame_offsets_seen.size());
    }
    return 0;
  }

  device->BeginValidationScope();
  device->BeginFrame();
  frame_alloc->BeginFrame(device->CurrentFrame());
  if (!update_frame_uniforms(opt.width, opt.height)) return 1;
  record_frame(nullptr, opt.width, opt.height, /*want_readback=*/true);
  device->EndFrame();
  device->WaitIdle();

  if (auto report = device->EndValidationScope()) {
    if (!report->IsClean()) {
      spdlog::warn("rhi_lab: validation observed: {}", report->violations);
    }
  } else if (device->IsValidationEnabled()) {
    // Validation is on but the scope produced no report -- a mismatched
    // Begin/End, not a clean frame. Silence here would read as success.
    spdlog::error("rhi_lab: validation scope produced no report");
  }

  // --- Report and write --------------------------------------------------
  uint32_t drawn = 0;
  counter->Read(0, {reinterpret_cast<uint8_t*>(&drawn), sizeof(drawn)});
  DrawIndexedIndirectArgs gpu_args{};
  args->Read(0, {reinterpret_cast<uint8_t*>(&gpu_args), sizeof(gpu_args)});
  spdlog::info("rhi_lab: GPU selected {} of {} clusters (tau {:.1f} px) — "
               "one indirect draw of {} indices x {} instances",
               drawn, capacity, opt.tau, gpu_args.index_count,
               gpu_args.instance_count);
  // A mismatch means the GPU port of the selection rule has drifted from the
  // CPU reference, which shows up as holes in the terrain rather than as any
  // kind of error -- so it is checked every run, not just when something looks
  // wrong.
  if (drawn != cpu_selected) {
    spdlog::error("rhi_lab: selector MISMATCH — GPU {} vs CPU {}. The cut is "
                  "no longer an exact cover; expect holes.",
                  drawn, cpu_selected);
  } else {
    spdlog::info("rhi_lab: selector matches the CPU oracle ({} clusters)",
                 cpu_selected);
  }

  std::vector<uint32_t> vis(size_t(opt.width) * opt.height, 0);
  if (targets.vis_readback->Read(0, {reinterpret_cast<uint8_t*>(vis.data()),
                             vis.size() * 4})) {
    size_t nonzero = 0;
    uint32_t sample = 0;
    for (uint32_t v : vis) {
      if (v != 0) { ++nonzero; if (!sample) sample = v; }
    }
    spdlog::info("rhi_lab: visbuffer {} / {} texels written (sample 0x{:x})",
                 nonzero, vis.size(), sample);
  }

  std::vector<uint8_t> pixels(size_t(opt.width) * opt.height * 4);
  if (!targets.readback->Read(0, pixels)) {
    spdlog::error("rhi_lab: readback failed");
    return 1;
  }
  badlands_write_png(opt.out.c_str(), pixels.data(), opt.width, opt.height);
  spdlog::info("rhi_lab: wrote {} ({}x{})", opt.out, opt.width, opt.height);
  return 0;
}
