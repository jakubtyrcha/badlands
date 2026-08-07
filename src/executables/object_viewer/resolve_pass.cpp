#include "executables/object_viewer/resolve_pass.hpp"

#include "executables/object_viewer/visbuffer_pass.hpp"

#include <cmath>
#include <cstring>

#include <spdlog/spdlog.h>

namespace badlands::object_viewer {

using namespace badlands::rhi;

namespace {

// Slot numbers are the DECLARATION ORDER of the shader's globals, which is what
// the Slang reflection layer assigns. Named here so a reordering in
// resolve.slang is a one-line fix rather than a hunt.
constexpr uint32_t kFrameSlot = 0;
constexpr uint32_t kVisbufferSlot = 1;
constexpr uint32_t kVerticesSlot = 2;
constexpr uint32_t kIndicesSlot = 3;
constexpr uint32_t kDrawsSlot = 4;
constexpr uint32_t kAlbedoSlot = 5;
constexpr uint32_t kNormalSlot = 6;
constexpr uint32_t kArmSlot = 7;
constexpr uint32_t kDispSlot = 8;
constexpr uint32_t kSamplerSlot = 9;
constexpr uint32_t kPrefilteredSlot = 10;
constexpr uint32_t kBrdfLutSlot = 11;
constexpr uint32_t kIblSamplerSlot = 12;

std::span<const uint8_t> Bytes(const void* p, size_t n) {
  return {static_cast<const uint8_t*>(p), n};
}

}  // namespace

const std::array<DebugViewInfo, size_t(DebugView::kCount)>& DebugViews() {
  // In ENUM ORDER, and the index is the shader constant. One table for both
  // surfaces, so the CLI name and the UI label cannot describe different modes.
  static const std::array<DebugViewInfo, size_t(DebugView::kCount)> kViews = {{
      {"lit", "Lit"},
      {"triangle-id", "Triangle ID"},
      {"barycentric", "Barycentric"},
      {"uv", "UV"},
      {"depth", "Depth"},
      {"albedo", "Albedo"},
      {"normal", "Normal"},
      {"roughness", "Roughness"},
      {"metallic", "Metallic"},
      {"displacement", "Displacement"},
      {"ao", "AO"},
  }};
  return kViews;
}

DebugView DebugViewFromName(std::string_view name) {
  const auto& views = DebugViews();
  for (size_t i = 0; i < views.size(); ++i) {
    if (views[i].cli == name) return DebugView(i);
  }
  return DebugView::kCount;
}

glm::vec3 SunDirection(const SunSettings& sun) {
  const float el = glm::radians(sun.elevation_deg);
  const float az = glm::radians(sun.azimuth_deg);
  // Elevation 90 must be straight up: the y term is sin(elevation), and the
  // horizontal radius shrinks to zero as it approaches the pole. Getting these
  // the other way round puts the sun on the horizon at noon.
  return glm::normalize(glm::vec3(std::cos(el) * std::sin(az), std::sin(el),
                                  -std::cos(el) * std::cos(az)));
}

std::unique_ptr<ResolvePass> ResolvePass::Create(IRhiDevice& device,
                                                 slang::SlangCompiler& compiler,
                                                 const MaterialPack& pack,
                                                 Format target_format) {
  auto pass = std::unique_ptr<ResolvePass>(new ResolvePass());
  pass->device_ = &device;
  pass->pack_ = &pack;

  auto load = [&](const char* entry) -> ShaderModulePtr {
    auto compiled = compiler.Get({.module = "resolve", .entry = entry},
                                 slang::ShaderTarget::Metal);
    if (!compiled) return nullptr;  // the compiler logged the diagnostics
    return device.CreateShaderModule(compiled->source, compiled->reflection,
                                     std::string("resolve::") + entry);
  };
  pass->vs_ = load("vs_fullscreen");
  pass->fs_ = load("fs_resolve");
  if (!pass->vs_ || !pass->fs_) return nullptr;

  pass->pipeline_ = device.CreateRenderPipeline(
      {.vertex_shader = pass->vs_.get(),
       .vertex_entry = "vs_fullscreen",
       .fragment_shader = pass->fs_.get(),
       .fragment_entry = "fs_resolve",
       .color_formats = {target_format},
       // DEPTH-TESTED, not branchy. The triangle sits at z = 0 and compares
       // Less against the visbuffer's reversed-Z depth (cleared to 0 = far), so
       // the expensive material path -- five fetches, analytic gradients, a
       // full BRDF -- never runs on a background pixel. Write is off: this pass
       // reads the depth the raster pass produced and does not own it.
       .depth = {.test_enabled = true,
                 .write_enabled = false,
                 .compare = CompareFunction::Less,
                 .format = VisbufferPass::kDepthFormat},
       .cull_mode = CullMode::None,
       .label = "resolve"});
  if (!pass->pipeline_) return nullptr;

  // THE DEPTH SPLIT. `Less` at z = 0 against a reversed-Z buffer cleared to 0
  // runs only where geometry was rasterized; the background's `GreaterEqual`
  // runs only where it was not. Write is off in both -- neither draw owns the
  // depth, they only read it.
  auto load_bg = [&](const char* entry) -> ShaderModulePtr {
    auto compiled = compiler.Get({.module = "background", .entry = entry},
                                 slang::ShaderTarget::Metal);
    if (!compiled) return nullptr;
    return device.CreateShaderModule(compiled->source, compiled->reflection,
                                     std::string("background::") + entry);
  };
  pass->bg_vs_ = load_bg("vs_background");
  pass->bg_fs_ = load_bg("fs_background");
  if (!pass->bg_vs_ || !pass->bg_fs_) return nullptr;
  pass->bg_pipeline_ = device.CreateRenderPipeline(
      {.vertex_shader = pass->bg_vs_.get(),
       .vertex_entry = "vs_background",
       .fragment_shader = pass->bg_fs_.get(),
       .fragment_entry = "fs_background",
       .color_formats = {target_format},
       .depth = {.test_enabled = true,
                 .write_enabled = false,
                 .compare = CompareFunction::GreaterEqual,
                 .format = VisbufferPass::kDepthFormat},
       .cull_mode = CullMode::None,
       .label = "background"});
  if (!pass->bg_pipeline_) return nullptr;

  pass->alloc_ = FrameAllocator::Create(
      device, {.block_size = 64 * 1024,
               .usage = BufferUsage::Uniform,
               .label = "resolve_frame"});
  if (!pass->alloc_) return nullptr;

  // TRILINEAR, and the mip filter is the load-bearing part: the prefiltered
  // chain stores one roughness per mip, so a Nearest mip filter would step
  // between roughness levels in visible bands instead of sweeping.
  pass->ibl_sampler_ = device.CreateSampler(
      {.mag_filter = FilterMode::Linear,
       .min_filter = FilterMode::Linear,
       .mip_filter = FilterMode::Linear,
       .address_u = AddressMode::ClampToEdge,
       .address_v = AddressMode::ClampToEdge,
       .label = "ibl"});
  // 1x1 stand-ins for "no environment". A table entry cannot be empty and a
  // destroyed view cannot be bound, so the absence of IBL is a real tiny
  // texture plus the frame flag -- never a missing binding.
  pass->dummy_cube_ = device.CreateTexture(
      {.width = 1, .height = 1, .array_layers = 6,
       .format = Format::RGBA16Float,
       .usage = TextureUsage::Sampled | TextureUsage::CopyDst,
       .dimension = TextureDimension::Cube,
       .label = "ibl_absent_cube"});
  pass->dummy_lut_ = device.CreateTexture(
      {.width = 1, .height = 1,
       .format = Format::RG16Float,
       .usage = TextureUsage::Sampled | TextureUsage::CopyDst,
       .label = "ibl_absent_lut"});
  if (!pass->ibl_sampler_ || !pass->dummy_cube_ || !pass->dummy_lut_) {
    return nullptr;
  }
  pass->env_view_ = pass->dummy_cube_->CreateView(
      {.dimension = TextureViewDimension::Cube, .label = "ibl_absent_cube"});
  pass->lut_view_ = pass->dummy_lut_->GetDefaultView();
  if (!pass->env_view_ || !pass->lut_view_) return nullptr;

  // A neutral sky-ish ambient, so the lit view is not black before anything
  // sets one. ENGINE convention: the diffuse convolution is baked into the
  // coefficients, so this is a raw-basis constant term and nothing else.
  pass->frame_.ambient_sh[0] = glm::vec4(0.12f, 0.14f, 0.18f, 0.0f);
  return pass;
}

void ResolvePass::BeginFrame(uint64_t frame_index) {
  alloc_->BeginFrame(frame_index);
}

bool ResolvePass::BuildTable(ITexture* visbuffer, IBuffer* vertices,
                             IBuffer* indices, IBuffer* draws,
                             IBuffer* frame_buffer) {
  // A pack without a displacement map still has to fill the slot -- an entry a
  // shader declares and a table omits is a refusal, not a default. The ARM map
  // stands in, and the Displacement view then shows AO, which is why
  // LoadMaterialPack logs whether a pack has one.
  ITexture* disp = pack_->gpu_displacement ? pack_->gpu_displacement.get()
                                           : pack_->gpu_arm.get();
  auto table = device_->CreateBindingTable(
      {.render_pipeline = pipeline_.get(),
       .entries =
           {{.slot = kFrameSlot,
             .kind = BindingKind::UniformBuffer,
             .buffer = frame_buffer,
             // The ring moves the uniform every frame.
             .dynamic_offset = true},
            {.slot = kVisbufferSlot,
             .kind = BindingKind::SampledTexture,
             .texture_view = visbuffer->GetDefaultView()},
            {.slot = kVerticesSlot,
             .kind = BindingKind::ReadOnlyStorageBuffer,
             .buffer = vertices},
            {.slot = kIndicesSlot,
             .kind = BindingKind::ReadOnlyStorageBuffer,
             .buffer = indices},
            {.slot = kDrawsSlot,
             .kind = BindingKind::ReadOnlyStorageBuffer,
             .buffer = draws},
            {.slot = kAlbedoSlot,
             .kind = BindingKind::SampledTexture,
             .texture_view = pack_->gpu_albedo->GetDefaultView()},
            {.slot = kNormalSlot,
             .kind = BindingKind::SampledTexture,
             .texture_view = pack_->gpu_normal->GetDefaultView()},
            {.slot = kArmSlot,
             .kind = BindingKind::SampledTexture,
             .texture_view = pack_->gpu_arm->GetDefaultView()},
            {.slot = kDispSlot,
             .kind = BindingKind::SampledTexture,
             .texture_view = disp->GetDefaultView()},
            {.slot = kPrefilteredSlot,
             .kind = BindingKind::SampledTexture,
             .texture_view = env_view_},
            {.slot = kBrdfLutSlot,
             .kind = BindingKind::SampledTexture,
             .texture_view = lut_view_},
            {.slot = kIblSamplerSlot,
             .kind = BindingKind::Sampler,
             .sampler = ibl_sampler_.get()},
            {.slot = kSamplerSlot,
             .kind = BindingKind::Sampler,
             .sampler = pack_->sampler.get()}},
       .label = "resolve"});
  if (!table) return false;  // CreateBindingTable logged why
  table_ = std::move(table);
  table_visbuffer_ = visbuffer;
  table_frame_buffer_ = frame_buffer;
  return true;
}

void ResolvePass::SetCamera(const glm::mat4& view, const glm::mat4& proj,
                            glm::vec3 camera_world_pos, float near_m,
                            float far_m) {
  frame_.view_proj = proj * view;
  frame_.camera_pos = glm::vec4(camera_world_pos, 0.0f);
  frame_.params.y = near_m;
  frame_.params.z = far_m;
}

void ResolvePass::SetSun(const SunSettings& sun) {
  frame_.sun_direction = glm::vec4(SunDirection(sun), 0.0f);
  frame_.sun_color = glm::vec4(sun.color * sun.intensity, 0.0f);
}

void ResolvePass::SetAmbient(const glm::vec4 sh[9]) {
  for (int i = 0; i < 9; ++i) frame_.ambient_sh[i] = sh[i];
}

void ResolvePass::SetEnvironment(ITextureView* prefiltered, uint32_t mip_count,
                                 ITextureView* brdf_lut, float intensity) {
  // EITHER missing clears the flag. A cube with no LUT (or a LUT with no cube)
  // is half a split sum, and half a split sum is a plausible-looking wrong
  // image rather than a failure -- so the pair is all-or-nothing.
  if (!prefiltered || !brdf_lut) {
    if (prefiltered || brdf_lut) {
      spdlog::warn(
          "object_viewer: the IBL chain needs both a prefiltered cube and a "
          "BRDF LUT; got only one, so ambient specular stays off");
    }
    env_view_ = dummy_cube_->CreateView(
        {.dimension = TextureViewDimension::Cube, .label = "ibl_absent_cube"});
    lut_view_ = dummy_lut_->GetDefaultView();
    frame_.ibl = glm::vec4(1.0f, intensity, 0.0f, 0.0f);
  } else {
    env_view_ = prefiltered;
    lut_view_ = brdf_lut;
    frame_.ibl = glm::vec4(float(mip_count), intensity, 1.0f, 0.0f);
  }
  // The tables NAME the views, and a table is immutable -- so swapping the
  // environment has to drop BOTH and let AddToGraph rebuild them.
  table_.reset();
  bg_table_.reset();
}

void ResolvePass::SetViewRays(glm::vec3 forward, glm::vec3 right,
                              glm::vec3 up) {
  frame_.ray_forward = glm::vec4(forward, 0.0f);
  frame_.ray_right = glm::vec4(right, 0.0f);
  frame_.ray_up = glm::vec4(up, 0.0f);
}

bool ResolvePass::AddToGraph(graph::RenderGraph& graph,
                             graph::ResourceHandle visbuffer,
                             ITexture* visbuffer_texture,
                             graph::ResourceHandle depth,
                             graph::ResourceHandle vertices,
                             graph::ResourceHandle indices,
                             graph::ResourceHandle draws,
                             graph::ResourceHandle target,
                             IBuffer* vertex_buffer, IBuffer* index_buffer,
                             IBuffer* draw_buffer) {
  frame_.params.x = float(uint32_t(view_));
  auto uniform = alloc_->Write(Bytes(&frame_, sizeof(frame_)));
  if (!uniform) {
    spdlog::error("object_viewer: could not allocate the resolve frame uniform");
    return false;
  }
  frame_offset_ = uint32_t(uniform->offset);
  frame_buffer_ = uniform->buffer;

  // Rebuilt when the visbuffer is recreated (a resize) OR when the ring grows
  // past its block and the uniform lands on a different buffer.
  if (table_visbuffer_ != visbuffer_texture ||
      table_frame_buffer_ != frame_buffer_ || !table_) {
    if (!BuildTable(visbuffer_texture, vertex_buffer, index_buffer, draw_buffer,
                    frame_buffer_)) {
      return false;
    }
  }

  // Every sampled resource declared, not just bound. The material maps are
  // imported here rather than at Create because a graph is rebuilt per frame
  // and handles belong to one graph -- and an undeclared texture is what the
  // validation layer catches as "in state Undefined but SetBindingTable
  // requires ShaderRead", which is exactly how the ImGui atlas was caught.
  // UNDEFINED on entry, not ShaderRead. The maps were filled by
  // ITexture::Write outside any pass, so nothing has transitioned them --
  // claiming they arrive ShaderRead makes the graph skip the barrier (state
  // already equals the target) and the validation layer reports exactly that.
  // The third time this shape has appeared, after the ImGui atlas and the
  // output pass's uniform: an entry state is a claim about what already
  // happened, not a request.
  auto import_tex = [&](ITexture* t, const char* name) {
    return graph.ImportTexture(t, ResourceState::Undefined, name);
  };
  auto uni = graph.ImportBuffer(frame_buffer_, ResourceState::Undefined,
                                "resolve_frame");
  auto alb = import_tex(pack_->gpu_albedo.get(), "mat_albedo");
  auto nrm = import_tex(pack_->gpu_normal.get(), "mat_normal");
  auto arm = import_tex(pack_->gpu_arm.get(), "mat_arm");
  if (!uni.IsValid() || !alb.IsValid() || !nrm.IsValid() || !arm.IsValid()) {
    return false;
  }

  auto pass = graph.AddRasterPass("resolve");
  pass.ColorTarget(target, LoadOp::Clear, StoreOp::Store)
      .Reads(visbuffer)
      .Reads(vertices)
      .Reads(indices)
      .Reads(draws)
      .Reads(uni)
      .Reads(alb)
      .Reads(nrm)
      .Reads(arm);
  if (pack_->gpu_displacement) {
    auto dsp = import_tex(pack_->gpu_displacement.get(), "mat_disp");
    if (!dsp.IsValid()) return false;
    pass.Reads(dsp);
  }
  // The IBL pair, declared like every other sampled resource. The validation
  // layer caught their absence immediately: a table that NAMES a texture the
  // graph never heard of has no transition derived for it, which Metal forgives
  // and DX12 will not.
  //
  // Declared even when the flag is off, because the 1x1 stand-ins are still
  // bound -- the shader branches on the flag, the BINDING is unconditional.
  if (env_view_ && env_view_->GetTexture()) {
    auto env = import_tex(env_view_->GetTexture(), "ibl_prefiltered");
    if (!env.IsValid()) return false;
    pass.Reads(env);
  }
  if (lut_view_ && lut_view_->GetTexture()) {
    auto lut = import_tex(lut_view_->GetTexture(), "ibl_brdf_lut");
    if (!lut.IsValid()) return false;
    pass.Reads(lut);
  }
  // READ-ONLY: both draws test this depth and neither writes it. Declaring it
  // as a target rather than a Reads() is what tells the graph it is an
  // ATTACHMENT -- a sampled read would derive the wrong transition.
  pass.DepthReadOnly(depth);

  // The background's own table. A table resolves slots against ONE pipeline's
  // reflection, and the background shader declares three bindings where the
  // resolve declares thirteen -- so sharing one would resolve every slot to the
  // wrong index.
  if (bg_table_frame_ != frame_buffer_ || !bg_table_) {
    auto bg = device_->CreateBindingTable(
        {.render_pipeline = bg_pipeline_.get(),
         .entries = {{.slot = 0,
                      .kind = BindingKind::UniformBuffer,
                      .buffer = frame_buffer_,
                      .dynamic_offset = true},
                     {.slot = 1,
                      .kind = BindingKind::SampledTexture,
                      .texture_view = env_view_},
                     {.slot = 2,
                      .kind = BindingKind::Sampler,
                      .sampler = ibl_sampler_.get()}},
         .label = "background"});
    if (!bg) return false;  // CreateBindingTable logged why
    bg_table_ = std::move(bg);
    bg_table_frame_ = frame_buffer_;
  }

  pass.Execute([this](const graph::RasterContext& ctx) {
    const uint32_t offsets[1] = {frame_offset_};
    // TWO DISJOINT DRAWS, partitioned by the depth test rather than by a
    // branch: the resolve covers what was rasterized, the background covers
    // what was not, and between them they cover the target exactly once.
    ctx.pass->SetPipeline(pipeline_.get());
    ctx.pass->SetBindingTable(0, table_.get(), offsets);
    ctx.pass->Draw(3);  // one fullscreen triangle
    ctx.pass->SetPipeline(bg_pipeline_.get());
    ctx.pass->SetBindingTable(0, bg_table_.get(), offsets);
    ctx.pass->Draw(3);
  });
  return true;
}

}  // namespace badlands::object_viewer
