// rhi_lab: the MVP that proves the RHI + Slang + visibility-buffer stack.
//
// Two modes. Without --window it renders one frame to a PNG and exits, which
// keeps it reproducible and diffable and is what scripts/screenshot.sh drives.
// With --window it opens a resizable window and flies a camera around, which
// is what exercises the frame model, the swapchain and resize.
//
// ONE RENDERER, TWO DRIVERS. LabRenderer owns the pipelines, the scene's
// buffers, the frame-sized targets and the frame itself; RunHeadless records
// one frame of it into a PNG and RunWindowed drives the same object from an
// RhiAppView inside the engine's RHI app layer. The window, the loop, ImGui and
// the screenshot all belong to that layer -- the lab stopped owning them when
// it became its second consumer.
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
#include "engine/app/rhi_app.hpp"
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
    // --screenshot belongs to the APP LAYER now and never reaches here: it is
    // stripped from argv before this runs, and main folds it onto --out when
    // there is no window. Keeping an alias for it here would be a second parser
    // for one flag, and the two would drift.
    if (a == "--out") {
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
          "rhi_lab [--out FILE] [--width N] [--height N]\n"
          "        [--trees N] [--tau PX] [--seed N] [--debug-vis]\n"
          "        [--window]   open a window; WASD/QE move, right-drag turns\n"
          "\n"
          "from the app layer: --screenshot FILE, --frames N, --fixed-dt D\n"
          "(--frames/--fixed-dt need --window)\n");
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
                                .dimension = TextureDimension::Tex2DArray,
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

// The camera's field of view. STRUCTURAL: the headless shot and the windowed
// view have to agree on it or the two are not comparable, and nothing has asked
// to vary it.
constexpr float kFovDeg = 55.0f;

// ---------------------------------------------------------------------------
// The renderer: everything the lab puts on the GPU, and the frame it records.
//
// ONE IMPLEMENTATION, TWO DRIVERS. The headless path builds one of these and
// records a single frame into its own colour target for the PNG; the windowed
// path builds one inside an RhiAppView and records into the acquired backbuffer
// every frame. The only difference between them is the `present` view -- which
// is the point, because a second copy of this frame is a second thing to keep
// in step with the shaders.
// ---------------------------------------------------------------------------
class LabRenderer {
 public:
  // `present_format` is what the RESOLVE writes into: the swapchain's format
  // when there is a window -- read back from it, never assumed -- and
  // RGBA8Unorm for the offscreen target otherwise. CAMetalLayer accepts
  // BGRA8Unorm and not RGBA8Unorm; the channel order is the hardware's
  // business, so the shader is unchanged either way.
  static std::unique_ptr<LabRenderer> Create(IRhiDevice& device,
                                             slang::SlangCompiler& compiler,
                                             const lab::Scene& scene,
                                             const Options& opt,
                                             Format present_format,
                                             uint32_t width, uint32_t height);

  // Everything sized to the frame, rebuilt as one unit -- including the resolve
  // table, which NAMES the visbuffer and depth views and is immutable, so a
  // resize that rebuilt the targets alone would sample destroyed textures.
  bool Resize(uint32_t width, uint32_t height);

  // Recycles the uniform ring's slot. Belongs at BeginFrame rather than in
  // Record, so a SKIPPED frame still consumes its slot.
  void BeginFrame(uint64_t frame_index) {
    frame_alloc_->BeginFrame(frame_index);
  }

  bool UpdateFrameUniforms(uint32_t width, uint32_t height);

  // `present` selects the resolve target: the acquired backbuffer when there is
  // a window, the offscreen colour target when there is not.
  bool Record(ITextureView* present, uint32_t width, uint32_t height,
              bool want_readback);

  FlyCamera& Camera() { return cam_; }
  const Targets& GetTargets() const { return targets_; }

  // The ring slice this frame's uniforms live in. Every table binds the ring
  // with this as its dynamic offset, and the windowed self-test watches it to
  // prove the ring rotates.
  uint32_t FrameOffset() const { return frame_offset_; }

  // What the CPU selector picked for the last UpdateFrameUniforms -- the oracle
  // the GPU's cut is checked against.
  size_t CpuSelected() const { return cpu_selected_; }

  uint32_t Capacity() const { return capacity_; }
  IBuffer* DrawCounter() const { return counter_.get(); }
  IBuffer* DrawArgs() const { return args_.get(); }

 private:
  LabRenderer(IRhiDevice& device, const lab::Scene& scene, const Options& opt)
      : device_(device),
        scene_(scene),
        opt_(opt),
        capacity_(uint32_t(scene.clusters.size())) {}

  bool CreatePipelines(slang::SlangCompiler& compiler, Format present_format);
  bool CreateSceneResources();
  bool CreateBindingTables();
  bool RebuildResolveTable();
  void PlaceCamera();

  // BORROWED, and both outlive every renderer: the device is created once in
  // main and the scene is immutable render data built before either path picks
  // up. Everything this class MUTATES, it owns.
  IRhiDevice& device_;
  const lab::Scene& scene_;
  const Options& opt_;
  const uint32_t capacity_;

  ShaderModulePtr sel_module_, fin_module_, vis_vs_, vis_fs_, tree_vs_,
      tree_fs_, res_vs_, res_fs_;
  ComputePipelinePtr select_pipe_, finalize_pipe_;
  RenderPipelinePtr terrain_pipe_, tree_pipe_, resolve_pipe_;

  BufferPtr vtx_, idx_, clu_, sel_, args_, counter_;
  BufferPtr tree_vtx_, tree_idx_, tree_inst_, dummy_idx_, tree_draw_idx_;
  TexturePtr albedo_, arm_, splat0_, splat1_;
  SamplerPtr terrain_sampler_, splat_sampler_;

  BindingTablePtr select_table_, finalize_table_, terrain_table_, tree_table_,
      resolve_table_;

  // Per-frame uniforms come from a ring, not from one buffer rewritten in
  // place. With three frames in flight, writing offset 0 at the top of frame N
  // memcpys over bytes the GPU is still reading for N-1 and N-2: the camera a
  // frame renders with is whichever write happened to land, so geometry from
  // one position gets resolved with another's inv_view_proj. The tear is subtle
  // enough that the image still looks right, which is why it survived a
  // windowed run and two reviews.
  std::unique_ptr<FrameAllocator> frame_alloc_;
  // The tables name this buffer once; only the offset moves per frame.
  IBuffer* frame_ring_ = nullptr;

  Targets targets_;
  FlyCamera cam_;
  LabFrame frame_{};
  uint32_t frame_offset_ = 0;
  size_t cpu_selected_ = 0;
};

std::unique_ptr<LabRenderer> LabRenderer::Create(
    IRhiDevice& device, slang::SlangCompiler& compiler, const lab::Scene& scene,
    const Options& opt, Format present_format, uint32_t width,
    uint32_t height) {
  auto out = std::unique_ptr<LabRenderer>(new LabRenderer(device, scene, opt));
  if (!out->CreatePipelines(compiler, present_format)) return nullptr;
  if (!out->CreateSceneResources()) return nullptr;

  out->frame_alloc_ = FrameAllocator::Create(
      device, {.block_size = 64 * 1024,
               .usage = BufferUsage::Uniform,
               .label = "lab_frame_ring"});
  if (!out->frame_alloc_) return nullptr;
  out->frame_ring_ = out->frame_alloc_->PrimaryBuffer();
  if (!out->frame_ring_) return nullptr;

  if (!out->CreateBindingTables()) return nullptr;
  // Resize does the FIRST build as well as every later one, so there is one
  // place that knows what follows the frame's size.
  if (!out->Resize(width, height)) return nullptr;
  out->PlaceCamera();
  return out;
}

bool LabRenderer::CreatePipelines(slang::SlangCompiler& compiler,
                                  Format present_format) {
  auto load = [&](const char* module, const char* entry) -> ShaderModulePtr {
    auto compiled = compiler.Get({.module = module, .entry = entry},
                                 slang::ShaderTarget::Metal);
    if (!compiled) return nullptr;
    return device_.CreateShaderModule(compiled->source, compiled->reflection,
                                      std::string(module) + "::" + entry);
  };

  const char* res_entry = opt_.debug_vis ? "fs_visbuffer_debug" : "fs_resolve";
  sel_module_ = load("cluster_select", "cs_select");
  fin_module_ = load("cluster_finalize", "cs_finalize");
  vis_vs_ = load("terrain_vis", "vs_terrain");
  vis_fs_ = load("terrain_vis", "fs_vis");
  tree_vs_ = load("tree_vis", "vs_trees");
  tree_fs_ = load("tree_vis", "fs_vis");
  res_vs_ = load("resolve", "vs_fullscreen");
  res_fs_ = load("resolve", res_entry);
  if (!sel_module_ || !fin_module_ || !vis_vs_ || !vis_fs_ || !tree_vs_ ||
      !tree_fs_ || !res_vs_ || !res_fs_) {
    return false;
  }

  select_pipe_ = device_.CreateComputePipeline(
      {.shader = sel_module_.get(), .entry = "cs_select", .label = "select"});
  finalize_pipe_ = device_.CreateComputePipeline({.shader = fin_module_.get(),
                                                  .entry = "cs_finalize",
                                                  .label = "finalize"});
  terrain_pipe_ = device_.CreateRenderPipeline(
      {.vertex_shader = vis_vs_.get(), .vertex_entry = "vs_terrain",
       .fragment_shader = vis_fs_.get(), .fragment_entry = "fs_vis",
       .color_formats = {Format::R32Uint},
       .depth = {.test_enabled = true, .write_enabled = true,
                 .compare = CompareFunction::GreaterEqual,
                 .format = Format::Depth32Float},
       .cull_mode = CullMode::None, .label = "terrain_vis"});
  tree_pipe_ = device_.CreateRenderPipeline(
      {.vertex_shader = tree_vs_.get(), .vertex_entry = "vs_trees",
       .fragment_shader = tree_fs_.get(), .fragment_entry = "fs_vis",
       .color_formats = {Format::R32Uint},
       .depth = {.test_enabled = true, .write_enabled = true,
                 .compare = CompareFunction::GreaterEqual,
                 .format = Format::Depth32Float},
       .cull_mode = CullMode::None, .label = "tree_vis"});
  resolve_pipe_ = device_.CreateRenderPipeline(
      {.vertex_shader = res_vs_.get(), .vertex_entry = "vs_fullscreen",
       .fragment_shader = res_fs_.get(), .fragment_entry = res_entry,
       .color_formats = {present_format},
       .cull_mode = CullMode::None, .label = "resolve"});
  return select_pipe_ && finalize_pipe_ && terrain_pipe_ && tree_pipe_ &&
         resolve_pipe_;
}

bool LabRenderer::CreateSceneResources() {
  auto make_buffer = [&](uint64_t size, BufferUsage usage, const char* label) {
    return device_.CreateBuffer({.size = size, .usage = usage, .label = label});
  };

  vtx_ = make_buffer(scene_.dag.vertices.size() * sizeof(float),
                     BufferUsage::Storage | BufferUsage::CopyDst, "vertices");
  idx_ = make_buffer(scene_.dag.indices.size() * sizeof(uint32_t),
                     BufferUsage::Storage | BufferUsage::CopyDst, "indices");
  clu_ = make_buffer(scene_.clusters.size() * sizeof(lab::ClusterGpu),
                     BufferUsage::Storage | BufferUsage::CopyDst, "clusters");
  sel_ = make_buffer(uint64_t(capacity_) * sizeof(uint32_t),
                     BufferUsage::Storage | BufferUsage::CopyDst, "selected");
  args_ = make_buffer(sizeof(DrawIndexedIndirectArgs),
                      BufferUsage::Storage | BufferUsage::Indirect |
                          BufferUsage::CopyDst | BufferUsage::MapRead,
                      "draw_args");
  counter_ = make_buffer(sizeof(uint32_t),
                         BufferUsage::Storage | BufferUsage::CopyDst |
                             BufferUsage::MapRead,
                         "draw_counter");
  tree_vtx_ = make_buffer(scene_.tree_vertices.size() * sizeof(lab::LabVertex),
                          BufferUsage::Storage | BufferUsage::CopyDst,
                          "tree_vertices");
  tree_idx_ = make_buffer(scene_.tree_indices.size() * sizeof(uint32_t),
                          BufferUsage::Storage | BufferUsage::CopyDst,
                          "tree_indices");
  tree_inst_ = make_buffer(scene_.trees.size() * sizeof(lab::TreeInstance),
                           BufferUsage::Storage | BufferUsage::CopyDst,
                           "trees");

  // The terrain draw is instanced over a fixed index range; each instance
  // pulls its own cluster's indices, so this buffer is just 0..383.
  std::vector<uint32_t> dummy_indices(kClusterIndexBudget);
  for (uint32_t i = 0; i < kClusterIndexBudget; ++i) dummy_indices[i] = i;
  dummy_idx_ = make_buffer(dummy_indices.size() * sizeof(uint32_t),
                           BufferUsage::Index | BufferUsage::CopyDst,
                           "dummy_indices");
  tree_draw_idx_ = make_buffer(scene_.tree_indices.size() * sizeof(uint32_t),
                               BufferUsage::Index | BufferUsage::CopyDst,
                               "tree_draw_indices");

  if (!vtx_ || !idx_ || !clu_ || !sel_ || !args_ || !counter_ || !tree_vtx_ ||
      !tree_idx_ || !tree_inst_ || !dummy_idx_ || !tree_draw_idx_) {
    return false;
  }

  vtx_->Write(0, Bytes(scene_.dag.vertices));
  idx_->Write(0, Bytes(scene_.dag.indices));
  clu_->Write(0, Bytes(scene_.clusters));
  tree_vtx_->Write(0, Bytes(scene_.tree_vertices));
  tree_idx_->Write(0, Bytes(scene_.tree_indices));
  tree_inst_->Write(0, Bytes(scene_.trees));
  dummy_idx_->Write(0, Bytes(dummy_indices));
  tree_draw_idx_->Write(0, Bytes(scene_.tree_indices));
  const uint32_t zero = 0;
  counter_->Write(0, Bytes(zero));

  albedo_ = MakeLayerArray(device_, "albedo_array", /*arm=*/false);
  arm_ = MakeLayerArray(device_, "arm_array", /*arm=*/true);
  MakeSplats(device_, splat0_, splat1_);
  terrain_sampler_ = device_.CreateSampler(
      {.address_u = AddressMode::Repeat, .address_v = AddressMode::Repeat,
       .max_anisotropy = 4, .label = "terrain_sampler"});
  splat_sampler_ = device_.CreateSampler(
      {.address_u = AddressMode::ClampToEdge,
       .address_v = AddressMode::ClampToEdge, .label = "splat_sampler"});
  return albedo_ && arm_ && splat0_ && splat1_ && terrain_sampler_ &&
         splat_sampler_;
}

bool LabRenderer::CreateBindingTables() {
  const auto& sel_refl = select_pipe_->GetReflection();
  const auto& fin_refl = finalize_pipe_->GetReflection();
  const auto& terrain_refl = terrain_pipe_->GetReflection();
  const auto& tree_refl = tree_pipe_->GetReflection();

  auto sel_entries = Entries(
      {BindByName(sel_refl, "frame", frame_ring_, nullptr, nullptr, /*dynamic=*/true),
       BindByName(sel_refl, "clusters", clu_.get()),
       BindByName(sel_refl, "selected", sel_.get()),
       BindByName(sel_refl, "draw_args", args_.get()),
       BindByName(sel_refl, "draw_counter", counter_.get())});
  auto fin_entries = Entries(
      {BindByName(fin_refl, "frame", frame_ring_, nullptr, nullptr, /*dynamic=*/true),
       BindByName(fin_refl, "draw_args", args_.get()),
       BindByName(fin_refl, "draw_counter", counter_.get())});
  auto terrain_entries = Entries(
      {BindByName(terrain_refl, "frame", frame_ring_, nullptr, nullptr, /*dynamic=*/true),
       BindByName(terrain_refl, "vertices", vtx_.get()),
       BindByName(terrain_refl, "indices", idx_.get()),
       BindByName(terrain_refl, "clusters", clu_.get()),
       BindByName(terrain_refl, "selected", sel_.get())});
  auto tree_entries = Entries(
      {BindByName(tree_refl, "frame", frame_ring_, nullptr, nullptr, /*dynamic=*/true),
       BindByName(tree_refl, "trees", tree_inst_.get()),
       BindByName(tree_refl, "tree_vertices", tree_vtx_.get()),
       BindByName(tree_refl, "tree_indices", tree_idx_.get())});

  // Bail before creating a single table: a name that did not resolve has
  // already been logged, and a table built from the rest would render something
  // subtly wrong rather than nothing at all.
  if (!sel_entries || !fin_entries || !terrain_entries || !tree_entries) {
    spdlog::error("rhi_lab: binding resolution failed, aborting");
    return false;
  }

  select_table_ = device_.CreateBindingTable({.compute_pipeline = select_pipe_.get(),
                                              .entries = *sel_entries,
                                              .label = "select"});
  finalize_table_ = device_.CreateBindingTable({.compute_pipeline = finalize_pipe_.get(),
                                                .entries = *fin_entries,
                                                .label = "finalize"});
  terrain_table_ = device_.CreateBindingTable({.render_pipeline = terrain_pipe_.get(),
                                               .entries = *terrain_entries,
                                               .label = "terrain"});
  tree_table_ = device_.CreateBindingTable({.render_pipeline = tree_pipe_.get(),
                                            .entries = *tree_entries,
                                            .label = "trees"});
  return select_table_ && finalize_table_ && terrain_table_ && tree_table_;
}

bool LabRenderer::RebuildResolveTable() {
  const auto& res_refl = resolve_pipe_->GetReflection();
  // resolve.slang keeps both fragment entries, so reflection reports every
  // global whichever one is selected. Bind the full set rather than tracking
  // which entry uses what -- that is the graph's job later, not the app's.
  auto res_entries = Entries({
      BindByName(res_refl, "frame", frame_ring_, nullptr, nullptr, /*dynamic=*/true),
      BindByName(res_refl, "visbuffer", nullptr, targets_.visbuf->GetDefaultView()),
      BindByName(res_refl, "depthbuffer", nullptr, targets_.depth->GetDefaultView()),
      BindByName(res_refl, "albedo_array", nullptr, albedo_->GetDefaultView()),
      BindByName(res_refl, "arm_array", nullptr, arm_->GetDefaultView()),
      BindByName(res_refl, "biome_splat0", nullptr, splat0_->GetDefaultView()),
      BindByName(res_refl, "biome_splat1", nullptr, splat1_->GetDefaultView()),
      BindByName(res_refl, "terrain_sampler", nullptr, nullptr, terrain_sampler_.get()),
      BindByName(res_refl, "splat_sampler", nullptr, nullptr, splat_sampler_.get()),
  });
  if (!res_entries) {
    spdlog::error("rhi_lab: resolve binding resolution failed");
    return false;
  }
  resolve_table_ = device_.CreateBindingTable({.render_pipeline = resolve_pipe_.get(),
                                               .entries = *res_entries,
                                               .label = "resolve"});
  return resolve_table_ != nullptr;
}

bool LabRenderer::Resize(uint32_t width, uint32_t height) {
  if (!MakeTargets(device_, width, height, targets_)) return false;
  return RebuildResolveTable();
}

void LabRenderer::PlaceCamera() {
  const float cx = scene_.size_x_m * 0.5f, cz = scene_.size_z_m * 0.5f;
  const float dist = std::max(scene_.size_x_m, scene_.size_z_m) * 0.85f;
  const glm::vec3 eye{cx - dist * 0.6f, scene_.max_height_m + dist * 0.45f,
                      cz - dist * 0.6f};
  const glm::vec3 target{cx, scene_.max_height_m * 0.3f, cz};

  // The windowed camera starts exactly where the headless shot looks from, so
  // the two views are comparable.
  cam_.position = eye;
  const glm::vec3 dir = glm::normalize(target - eye);
  cam_.yaw = std::atan2(dir.x, dir.z);
  cam_.pitch = std::asin(std::clamp(dir.y, -1.0f, 1.0f));
  cam_.speed = std::max(20.0f, dist * 0.35f);
}

bool LabRenderer::UpdateFrameUniforms(uint32_t width, uint32_t height) {
  const float aspect = float(width) / float(std::max(1u, height));
  const glm::mat4 view = cam_.View();
  // Reversed-Z: swapping near and far in a zero-to-one projection maps
  // near->1 and far->0, which is the project-wide convention.
  const glm::mat4 proj =
      glm::perspective(glm::radians(kFovDeg), aspect, 4000.0f, 0.1f);
  frame_.view_proj = proj * view;
  frame_.inv_view_proj = glm::inverse(frame_.view_proj);
  frame_.camera_pos = glm::vec4(cam_.position, 1.0f);
  frame_.sun_direction =
      glm::vec4(glm::normalize(glm::vec3(0.45f, 0.75f, 0.35f)), 0);
  frame_.sun_color = glm::vec4(1.9f, 1.75f, 1.5f, 0.0f);
  // A plain sky-ish ambient: DC term plus a gentle vertical gradient.
  frame_.ambient_sh[0] = glm::vec4(0.32f, 0.38f, 0.48f, 0.0f);
  frame_.ambient_sh[2] = glm::vec4(0.10f, 0.12f, 0.16f, 0.0f);
  // Screen height drives the cluster error threshold, so it MUST be this
  // frame's height rather than the startup one, or a resized window selects
  // clusters for a size it is no longer rendering at.
  frame_.params = glm::vec4(opt_.tau, float(height), kFovDeg, float(capacity_));
  frame_.splat_uv = glm::vec4(1.0f / std::max(scene_.size_x_m, 1.0f),
                              1.0f / std::max(scene_.size_z_m, 1.0f), 0.0f, 0.0f);
  frame_.limits = glm::vec4(float(capacity_), 0, 0, 0);

  auto slice = frame_alloc_->Write(Bytes(frame_));
  if (!slice) {
    // Logged by the allocator. Reusing the previous offset would render the
    // previous frame's camera, which reads as stutter rather than as an error.
    spdlog::error("rhi_lab: no room for this frame's uniforms");
    return false;
  }
  frame_offset_ = uint32_t(slice->offset);

  // Oracle: the CPU selector this pass replaces. Same camera, same tau. If the
  // GPU picks a different number the port of the rule is wrong, and that shows
  // up as holes in the terrain rather than as an error.
  std::vector<uint32_t> cpu_cut;
  SelectClusters(scene_.dag, cam_.position, kFovDeg, float(height), opt_.tau,
                 cpu_cut);
  cpu_selected_ = cpu_cut.size();
  return true;
}

bool LabRenderer::Record(ITextureView* present, uint32_t width, uint32_t height,
                         bool want_readback) {
  auto encoder = device_.CreateCommandEncoder("lab_frame");
  if (!encoder) return false;

  // Selection: classify then publish. Both write the args buffer, so it is
  // declared ShaderWrite here and IndirectArg before the draw -- the sort of
  // transition Metal ignores and the validation layer checks.
  encoder->Transition(frame_ring_, ResourceState::ShaderRead);
  encoder->Transition(clu_.get(), ResourceState::ShaderRead);
  encoder->Transition(sel_.get(), ResourceState::ShaderWrite);
  encoder->Transition(args_.get(), ResourceState::ShaderWrite);
  encoder->Transition(counter_.get(), ResourceState::ShaderWrite);

  uint32_t select_wg[3] = {64, 1, 1};
  select_pipe_->GetWorkgroupSize(select_wg);
  const uint32_t groups = (capacity_ + select_wg[0] - 1) / select_wg[0];

  auto* cs = encoder->BeginComputePass("select");
  if (!cs) return false;
  cs->SetPipeline(select_pipe_.get());
  cs->SetBindingTable(0, select_table_.get(), {&frame_offset_, 1});
  cs->Dispatch(groups);
  cs->End();

  auto* cf = encoder->BeginComputePass("finalize");
  if (!cf) return false;
  cf->SetPipeline(finalize_pipe_.get());
  cf->SetBindingTable(0, finalize_table_.get(), {&frame_offset_, 1});
  cf->Dispatch(1);
  cf->End();

  // Visibility buffer: one indirect draw for all terrain, one instanced draw
  // for all trees. Neither binds a material.
  encoder->Transition(sel_.get(), ResourceState::ShaderRead);
  encoder->Transition(args_.get(), ResourceState::IndirectArg);
  encoder->Transition(vtx_.get(), ResourceState::ShaderRead);
  encoder->Transition(idx_.get(), ResourceState::ShaderRead);
  encoder->Transition(tree_vtx_.get(), ResourceState::ShaderRead);
  encoder->Transition(tree_idx_.get(), ResourceState::ShaderRead);
  encoder->Transition(tree_inst_.get(), ResourceState::ShaderRead);
  encoder->Transition(targets_.visbuf.get(), ResourceState::RenderTarget);
  encoder->Transition(targets_.depth.get(), ResourceState::DepthWrite);

  RenderPassDesc vis_pass;
  vis_pass.label = "visbuffer";
  vis_pass.color_attachments.push_back({.view = targets_.visbuf->GetDefaultView(),
                                        .load_op = LoadOp::Clear,
                                        .store_op = StoreOp::Store,
                                        .clear_color = {0, 0, 0, 0}});
  vis_pass.depth_attachment = {.view = targets_.depth->GetDefaultView(),
                               .load_op = LoadOp::Clear,
                               .store_op = StoreOp::Store,
                               .clear_depth = 0.0f};  // reversed-Z far
  auto* vp = encoder->BeginRenderPass(vis_pass);
  if (!vp) return false;
  vp->SetViewport(0, 0, float(width), float(height));
  vp->SetPipeline(terrain_pipe_.get());
  vp->SetBindingTable(0, terrain_table_.get(), {&frame_offset_, 1});
  vp->SetIndexBuffer(dummy_idx_.get(), IndexFormat::Uint32);
  vp->DrawIndexedIndirect(args_.get(), 0);

  vp->SetPipeline(tree_pipe_.get());
  vp->SetBindingTable(0, tree_table_.get(), {&frame_offset_, 1});
  vp->SetIndexBuffer(tree_draw_idx_.get(), IndexFormat::Uint32);
  vp->DrawIndexed(uint32_t(scene_.tree_indices.size()),
                  uint32_t(scene_.trees.size()));
  vp->End();

  // Resolve: every material in the scene, in screen space, from a fixed set of
  // bindings.
  encoder->Transition(targets_.visbuf.get(), ResourceState::ShaderRead);
  encoder->Transition(targets_.depth.get(), ResourceState::ShaderRead);
  encoder->Transition(albedo_.get(), ResourceState::ShaderRead);
  encoder->Transition(arm_.get(), ResourceState::ShaderRead);
  encoder->Transition(splat0_.get(), ResourceState::ShaderRead);
  encoder->Transition(splat1_.get(), ResourceState::ShaderRead);
  // The RESOLVE TARGET, which is the backbuffer when there is a window. A
  // freshly acquired drawable is a new resource every frame, so its state
  // starts Undefined every frame and has to be declared each time.
  ITexture* resolve_target =
      present ? present->GetTexture() : targets_.colour.get();
  encoder->Transition(resolve_target, ResourceState::RenderTarget);

  RenderPassDesc res_pass;
  res_pass.label = "resolve";
  res_pass.color_attachments.push_back(
      {.view = present ? present : targets_.colour->GetDefaultView(),
       .load_op = LoadOp::Clear,
       .store_op = StoreOp::Store});
  auto* rp = encoder->BeginRenderPass(res_pass);
  if (!rp) return false;
  rp->SetViewport(0, 0, float(width), float(height));
  rp->SetPipeline(resolve_pipe_.get());
  rp->SetBindingTable(0, resolve_table_.get(), {&frame_offset_, 1});
  rp->Draw(3);
  rp->End();

  if (want_readback) {
    encoder->Transition(targets_.colour.get(), ResourceState::CopySrc);
    encoder->Transition(targets_.readback.get(), ResourceState::CopyDst);
    encoder->CopyTextureToBuffer(targets_.colour.get(), 0, 0,
                                 targets_.readback.get(), 0);
    encoder->Transition(targets_.visbuf.get(), ResourceState::CopySrc);
    encoder->Transition(targets_.vis_readback.get(), ResourceState::CopyDst);
    encoder->CopyTextureToBuffer(targets_.visbuf.get(), 0, 0,
                                 targets_.vis_readback.get(), 0);
  }
  encoder->Finish();
  device_.Submit(*encoder);
  return true;
}

// What the windowed self-test observes from INSIDE the loop.
//
// Filled live rather than read afterwards: RhiApp destroys the view before Run
// returns, so a pointer to the view is dangling by the time the assertions run.
// Same shape as RunStats, and there for the same reason.
struct SelfTestObservations {
  uint32_t initial_pixel_width = 0;
  uint32_t rendered_after_resize = 0;
  uint32_t target_width = 0, target_height = 0;
  // One distinct slice per frame in flight, or the ring is not rotating.
  std::set<uint32_t> frame_offsets_seen;
};

// ---------------------------------------------------------------------------
// The windowed lab, as an RhiAppView.
//
// THE SECOND CONSUMER of RhiApp, and the one that proved the layer generalises.
// Porting it found four things object_viewer never asked for: a view could not
// request a resize, could not read the window in POINTS, could not see the
// loop's RunStats, and could not learn the swapchain's size. All four are what
// this self-test is made of.
// ---------------------------------------------------------------------------
class LabView : public rhi_app::RhiAppView {
 public:
  // `scene` outlives the app run; `obs` is the self-test's result sink and is
  // null when nothing is asserting.
  LabView(const Options& opt, const lab::Scene& scene,
          SelfTestObservations* obs)
      : opt_(opt), scene_(scene), obs_(obs) {}

  bool Initialize(const rhi_app::RhiAppContext& ctx) override {
    host_ = ctx.host;
    renderer_ = LabRenderer::Create(*ctx.device, *ctx.compiler, scene_, opt_,
                                    ctx.surface_format, ctx.width, ctx.height);
    if (!renderer_) return false;
    initial_pixel_w_ = ctx.width;
    if (obs_) obs_->initial_pixel_width = ctx.width;

    // POINTS, because that is what SDL_SetWindowSize takes; everything
    // downstream is PIXELS, which on a HiDPI display is a different number.
    int pw = 0, ph = 0;
    SDL_GetWindowSize(host_->Window(), &pw, &ph);
    test_point_w_ = pw / 2 + 64;
    test_point_h_ = ph / 2 + 32;

    spdlog::info(
        "rhi_lab: WASD/QE to move, hold right mouse to look, shift to go "
        "faster, Esc to quit");
    return true;
  }

  bool OnEvent(const SDL_Event& e) override {
    if (e.type == SDL_EVENT_MOUSE_MOTION && (e.motion.state & SDL_BUTTON_RMASK)) {
      constexpr float kLookRate = 0.0035f;
      renderer_->Camera().Turn(-e.motion.xrel * kLookRate,
                               -e.motion.yrel * kLookRate);
      return true;
    }
    return false;
  }

  void Update(const rhi_app::FrameTime& t) override {
    FlyCamera& cam = renderer_->Camera();
    if (t.keys) {
      const float boost = t.keys[SDL_SCANCODE_LSHIFT] ? 4.0f : 1.0f;
      const float step = cam.speed * boost * std::min(t.real_dt, 0.1f);
      if (t.keys[SDL_SCANCODE_W]) cam.position += cam.Forward() * step;
      if (t.keys[SDL_SCANCODE_S]) cam.position -= cam.Forward() * step;
      if (t.keys[SDL_SCANCODE_D]) cam.position += cam.Right() * step;
      if (t.keys[SDL_SCANCODE_A]) cam.position -= cam.Right() * step;
      if (t.keys[SDL_SCANCODE_E]) cam.position.y += step;
      if (t.keys[SDL_SCANCODE_Q]) cam.position.y -= step;
    }

    // Scripted resize, partway through a self-test run. Requested through the
    // SAME window-manager path a user drag uses, so the test exercises the
    // coalescing rather than bypassing it.
    if (opt_.self_test_frames > 0 &&
        t.index == uint64_t(opt_.self_test_frames / 2)) {
      host_->RequestResizePoints(uint32_t(test_point_w_),
                                 uint32_t(test_point_h_));
    }
  }

  void OnFrameBegin(uint64_t frame_index) override {
    renderer_->BeginFrame(frame_index);
  }

  bool OnResize(uint32_t w, uint32_t h) override { return renderer_->Resize(w, h); }

  bool Render(ITextureView* target, const rhi_app::FrameInfo& f) override {
    if (!renderer_->UpdateFrameUniforms(f.width, f.height)) return false;
    if (!renderer_->Record(target, f.width, f.height, /*want_readback=*/false)) {
      return false;
    }
    if (obs_) {
      obs_->frame_offsets_seen.insert(renderer_->FrameOffset());
      if (f.width != initial_pixel_w_) ++obs_->rendered_after_resize;
      // The targets' own idea of the size, recorded from the frame that used
      // them rather than read from a renderer that no longer exists.
      obs_->target_width = renderer_->GetTargets().width;
      obs_->target_height = renderer_->GetTargets().height;
    }
    return true;
  }

 private:
  const Options& opt_;
  const lab::Scene& scene_;
  SelfTestObservations* obs_ = nullptr;
  std::unique_ptr<LabRenderer> renderer_;
  rhi_app::RhiAppHost* host_ = nullptr;
  uint32_t initial_pixel_w_ = 0;
  int test_point_w_ = 0, test_point_h_ = 0;
};

// Opens the window and runs the loop. Returns a process exit code.
int RunWindowed(const Options& opt, const rhi_app::RhiAppOptions& app_opts,
                const lab::Scene& scene, IRhiDevice& device) {
  SelfTestObservations obs;

  // LENDS ITS DEVICE. The lab creates one before it knows whether it is running
  // headless or windowed -- the headless path needs it for its validation scope
  // and its readbacks -- and letting the layer make a second would have this app
  // rendering with one device into the other's drawable.
  rhi_app::RhiApp app({.title = "badlands rhi_lab",
                       .width = opt.width,
                       .height = opt.height,
                       .shader_paths = {"shaders/slang/common",
                                        "shaders/slang/rhi_lab",
                                        "shaders/slang/app",
                                        "shaders/slang/ui"},
                       .device = &device});
  rhi_app::RunStats stats;
  // The self-test owns the frame count when it is running: the scripted resize
  // fires at the halfway frame, so a different total would move it.
  rhi_app::RhiAppOptions run_opts = app_opts;
  if (opt.self_test_frames > 0) {
    run_opts.max_frames = uint64_t(opt.self_test_frames);
  }

  const int rc = app.RunParsed(
      run_opts,
      [&](const rhi_app::RhiAppContext&) {
        return std::make_unique<LabView>(opt, scene,
                                         opt.self_test_frames > 0 ? &obs : nullptr);
      },
      &stats);
  if (rc != 0) return rc;
  if (opt.self_test_frames <= 0) return 0;

  // Exit status IS the assertion: this runs as a ctest with no test framework
  // around it.
  if (stats.frames_presented == 0) {
    spdlog::error("rhi_lab self-test: no frame ever rendered");
    return 1;
  }
  // EXACTLY the frame count asked for, not merely "it stopped". The layer's
  // determinism claim is that --frames N runs N frames on any machine, and the
  // obvious check for it -- take two screenshots at different counts and
  // require different images -- is vacuous against a scene that does not
  // animate, which is every scene either of these apps has. Counting the frames
  // is the claim itself.
  if (stats.frames_begun != uint64_t(opt.self_test_frames)) {
    spdlog::error("rhi_lab self-test: asked for {} frames, the loop began {}",
                  opt.self_test_frames, stats.frames_begun);
    return 1;
  }
  if (stats.final_width == obs.initial_pixel_width) {
    spdlog::error(
        "rhi_lab self-test: the window never actually resized (still {} pixels "
        "wide) -- the test proved nothing",
        stats.final_width);
    return 1;
  }
  // The invariant: everything sized to the frame follows the reported PIXEL
  // size, whatever the backing scale turns the request into.
  if (obs.target_width != stats.final_width ||
      obs.target_height != stats.final_height) {
    spdlog::error("rhi_lab self-test: targets are {}x{}, window is {}x{}",
                  obs.target_width, obs.target_height, stats.final_width,
                  stats.final_height);
    return 1;
  }
  // FROM RunStats, not from the host after the fact: the app destroys the view
  // and the shell before returning, so reaching for either here read freed
  // memory and reported 0x0 -- which this test then blamed on the resize.
  if (stats.final_swapchain_width != stats.final_width ||
      stats.final_swapchain_height != stats.final_height) {
    spdlog::error("rhi_lab self-test: swapchain is {}x{}, window is {}x{}",
                  stats.final_swapchain_width, stats.final_swapchain_height,
                  stats.final_width, stats.final_height);
    return 1;
  }
  if (obs.rendered_after_resize == 0) {
    spdlog::error(
        "rhi_lab self-test: resize took, but nothing rendered afterwards");
    return 1;
  }
  // Distinct slices, one per frame in flight. A single reused offset means the
  // per-frame uniforms are being overwritten under the GPU.
  if (obs.frame_offsets_seen.size() < device.FramesInFlight()) {
    spdlog::error(
        "rhi_lab self-test: only {} distinct uniform slice(s) across {} frames "
        "with {} in flight -- the ring is not rotating and frame N is "
        "overwriting what N-1 is still reading",
        obs.frame_offsets_seen.size(), stats.frames_presented,
        device.FramesInFlight());
    return 1;
  }
  spdlog::info(
      "rhi_lab self-test OK: {} frames, {} after resizing {} -> {} pixels "
      "wide, {} distinct uniform slices",
      stats.frames_presented, obs.rendered_after_resize,
      obs.initial_pixel_width, stats.final_width,
      obs.frame_offsets_seen.size());
  return 0;
}

// Renders exactly one frame to a PNG, plus the reports the frame makes
// checkable: the GPU cut against the CPU oracle, and the visbuffer's coverage.
int RunHeadless(const Options& opt, const lab::Scene& scene,
                IRhiDevice& device) {
  const std::vector<std::string> shader_paths = {"shaders/slang/common",
                                                 "shaders/slang/rhi_lab"};
  auto compiler = slang::CreateSlangCompiler(shader_paths);
  if (!compiler) return 1;

  auto renderer = LabRenderer::Create(device, *compiler, scene, opt,
                                      Format::RGBA8Unorm, opt.width, opt.height);
  if (!renderer) return 1;

  device.BeginValidationScope();
  device.BeginFrame();
  renderer->BeginFrame(device.CurrentFrame());
  if (!renderer->UpdateFrameUniforms(opt.width, opt.height)) return 1;
  if (!renderer->Record(nullptr, opt.width, opt.height, /*want_readback=*/true)) {
    return 1;
  }
  device.EndFrame();
  device.WaitIdle();

  if (auto report = device.EndValidationScope()) {
    if (!report->IsClean()) {
      spdlog::warn("rhi_lab: validation observed: {}", report->violations);
    }
  } else if (device.IsValidationEnabled()) {
    // Validation is on but the scope produced no report -- a mismatched
    // Begin/End, not a clean frame. Silence here would read as success.
    spdlog::error("rhi_lab: validation scope produced no report");
  }

  // --- Report and write --------------------------------------------------
  uint32_t drawn = 0;
  renderer->DrawCounter()->Read(0, {reinterpret_cast<uint8_t*>(&drawn),
                                    sizeof(drawn)});
  DrawIndexedIndirectArgs gpu_args{};
  renderer->DrawArgs()->Read(0, {reinterpret_cast<uint8_t*>(&gpu_args),
                                 sizeof(gpu_args)});
  spdlog::info("rhi_lab: GPU selected {} of {} clusters (tau {:.1f} px) — "
               "one indirect draw of {} indices x {} instances",
               drawn, renderer->Capacity(), opt.tau, gpu_args.index_count,
               gpu_args.instance_count);
  // A mismatch means the GPU port of the selection rule has drifted from the
  // CPU reference, which shows up as holes in the terrain rather than as any
  // kind of error -- so it is checked every run, not just when something looks
  // wrong.
  if (drawn != renderer->CpuSelected()) {
    spdlog::error("rhi_lab: selector MISMATCH — GPU {} vs CPU {}. The cut is "
                  "no longer an exact cover; expect holes.",
                  drawn, renderer->CpuSelected());
  } else {
    spdlog::info("rhi_lab: selector matches the CPU oracle ({} clusters)",
                 renderer->CpuSelected());
  }

  const Targets& targets = renderer->GetTargets();
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

}  // namespace

int main(int argc, char** argv) {
  spdlog::set_pattern("%^[%l]%$ %v");

  // The app layer's own flags come out of argv FIRST, or --frames reaches the
  // lab's parser as an unknown argument and gets refused there.
  rhi_app::RhiAppOptions app_opts = rhi_app::ParseAppArgs(argc, argv);
  if (!app_opts.valid) return 1;

  Options opt;
  if (!ParseArgs(argc, argv, opt)) return 1;

  // --screenshot is now the LAYER's flag, and the lab has two capture paths for
  // it: windowed, the layer reads back the backbuffer; headless, this app was
  // already writing exactly one frame to --out, so the flag names that file.
  // One flag, one meaning, two implementations of the thing it names.
  if (!app_opts.screenshot_path.empty() && !opt.windowed) {
    opt.out = app_opts.screenshot_path;
  }
  // REFUSED rather than ignored. The headless path renders exactly one frame
  // with no loop to step, so a frame count or a fixed step here cannot do
  // anything -- and accepting them would report success for a run that did not
  // do what was asked.
  if (!opt.windowed && (app_opts.max_frames != 0 || app_opts.fixed_dt != 0.0f)) {
    spdlog::error(
        "rhi_lab: --frames and --fixed-dt need --window; without one this app "
        "renders exactly one frame to --out");
    return 1;
  }
  // Both name the frame count and they would disagree. The self-test's resize
  // fires at its halfway frame, so silently letting one win moves the event the
  // test is built around.
  if (opt.self_test_frames > 0 && app_opts.max_frames != 0) {
    spdlog::error("rhi_lab: --self-test and --frames both set the frame count; "
                  "pass one");
    return 1;
  }

  lab::Scene scene = lab::BuildScene(opt.nodes, opt.spacing, opt.trees, opt.seed);
  if (scene.clusters.empty()) {
    spdlog::error("rhi_lab: empty cluster DAG");
    return 1;
  }

  // ONE DEVICE, both paths. The headless path needs it for its validation scope
  // and its buffer readbacks; the windowed path lends it to the layer.
  auto device = CreateDevice({.backend = BackendKind::Metal,
                              .enable_validation = true,
                              .label = "rhi_lab"});
  if (!device) return 1;

  return opt.windowed ? RunWindowed(opt, app_opts, scene, *device)
                      : RunHeadless(opt, scene, *device);
}
