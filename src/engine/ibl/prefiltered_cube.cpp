#include "engine/ibl/prefiltered_cube.hpp"

#include <algorithm>
#include <cstring>
#include <vector>

#include <spdlog/spdlog.h>

namespace badlands::ibl {

using namespace badlands::rhi;

namespace {

// Slot numbers are the DECLARATION ORDER of the shader's globals, which is what
// the Slang reflection layer assigns. Named here so a reordering in
// prefilter.slang is a one-line fix rather than a hunt.
constexpr uint32_t kFrameSlot = 0;
constexpr uint32_t kSourceSlot = 1;
constexpr uint32_t kSamplerSlot = 2;

constexpr Format kFormat = Format::RGBA16Float;

struct PrefilterParams {
  float params[4] = {0.0f, 0.0f, 0.0f, 0.0f};  // roughness, face, size, pad
};

ShaderModulePtr LoadShader(IRhiDevice& device, slang::SlangCompiler& compiler,
                           const char* module, const char* entry) {
  auto compiled = compiler.Get({.module = module, .entry = entry},
                               slang::ShaderTarget::Metal);
  if (!compiled) return nullptr;  // the compiler logged the diagnostics
  return device.CreateShaderModule(compiled->source, compiled->reflection,
                                   std::string(module) + "::" + entry);
}

}  // namespace

std::unique_ptr<PrefilteredCube> PrefilteredCube::Create(
    IRhiDevice& device, slang::SlangCompiler& compiler) {
  auto out = std::unique_ptr<PrefilteredCube>(new PrefilteredCube());
  out->device_ = &device;

  out->vs_ = LoadShader(device, compiler, "prefilter", "vs_prefilter");
  out->fs_ = LoadShader(device, compiler, "prefilter", "fs_prefilter");
  if (!out->vs_ || !out->fs_) return nullptr;

  out->pipeline_ = device.CreateRenderPipeline(
      {.vertex_shader = out->vs_.get(),
       .vertex_entry = "vs_prefilter",
       .fragment_shader = out->fs_.get(),
       .fragment_entry = "fs_prefilter",
       .color_formats = {kFormat},
       .cull_mode = CullMode::None,
       .label = "ibl_prefilter"});
  if (!out->pipeline_) return nullptr;

  out->texture_ = device.CreateTexture(
      {.width = kFaceSize,
       .height = kFaceSize,
       .array_layers = 6,
       .mip_levels = kMipCount,
       .format = kFormat,
       .usage = TextureUsage::Sampled | TextureUsage::RenderTarget |
                TextureUsage::CopySrc,
       .dimension = TextureDimension::Cube,
       .label = "ibl_prefiltered"});
  if (!out->texture_) return nullptr;

  out->cube_view_ = out->texture_->CreateView(
      {.dimension = TextureViewDimension::Cube, .label = "ibl_prefiltered.cube"});
  if (!out->cube_view_) return nullptr;  // CreateView logged why

  // ONE buffer holding every (face, mip)'s params at its own offset, rather
  // than a ring: this is not per-frame data, and all 30 draws are recorded into
  // one encoder before anything is submitted. Writing a single slot per draw
  // would have every draw read the last one's values.
  out->params_ = device.CreateBuffer(
      {.size = sizeof(PrefilterParams) * 6 * kMipCount,
       .usage = BufferUsage::Uniform | BufferUsage::CopyDst,
       .label = "ibl_prefilter_params"});
  if (!out->params_) return nullptr;

  return out;
}

bool PrefilteredCube::Generate(ITexture* source_cube,
                               ISampler* source_sampler) {
  if (!source_cube || !source_sampler) {
    spdlog::error("ibl: prefilter needs a source cube and a sampler (got {})",
                  source_cube ? "no sampler" : "no cube");
    return false;
  }
  ITextureView* source_view = source_cube->CreateView(
      {.dimension = TextureViewDimension::Cube, .label = "ibl_prefilter.src"});
  if (!source_view) return false;  // CreateView logged why

  // The offset a dynamic binding takes must satisfy the device's alignment, so
  // the stride is the aligned size rather than sizeof.
  const uint64_t align = std::max<uint64_t>(1, device_->MinBufferOffsetAlignment());
  const uint64_t stride = ((sizeof(PrefilterParams) + align - 1) / align) * align;
  if (stride * 6 * kMipCount > params_->GetSize()) {
    // Recreate at the aligned stride rather than writing past the end. Refuse
    // and log if even that fails -- a short params buffer means every draw past
    // the first reads someone else's roughness.
    params_ = device_->CreateBuffer(
        {.size = stride * 6 * kMipCount,
         .usage = BufferUsage::Uniform | BufferUsage::CopyDst,
         .label = "ibl_prefilter_params"});
    if (!params_) return false;
  }

  std::vector<uint8_t> staging(size_t(stride) * 6 * kMipCount, 0);
  for (uint32_t mip = 0; mip < kMipCount; ++mip) {
    for (uint32_t face = 0; face < 6; ++face) {
      PrefilterParams p;
      p.params[0] = RoughnessForMip(mip);
      p.params[1] = float(face);
      p.params[2] = float(std::max(1u, kFaceSize >> mip));
      const size_t at = size_t(stride) * (mip * 6 + face);
      std::memcpy(staging.data() + at, &p, sizeof(p));
    }
  }
  params_->Write(0, {staging.data(), staging.size()});

  auto table = device_->CreateBindingTable(
      {.render_pipeline = pipeline_.get(),
       .entries = {{.slot = kFrameSlot,
                    .kind = BindingKind::UniformBuffer,
                    .buffer = params_.get(),
                    // One table, thirty draws: the offset selects the
                    // (face, mip) rather than thirty tables doing it.
                    .dynamic_offset = true},
                   {.slot = kSourceSlot,
                    .kind = BindingKind::SampledTexture,
                    .texture_view = source_view},
                   {.slot = kSamplerSlot,
                    .kind = BindingKind::Sampler,
                    .sampler = source_sampler}},
       .label = "ibl_prefilter"});
  if (!table) return false;  // CreateBindingTable logged why

  auto encoder = device_->CreateCommandEncoder("ibl_prefilter");
  if (!encoder) return false;
  encoder->Transition(source_cube, ResourceState::ShaderRead);
  encoder->Transition(params_.get(), ResourceState::ShaderRead);

  for (uint32_t mip = 0; mip < kMipCount; ++mip) {
    const uint32_t size = std::max(1u, kFaceSize >> mip);
    for (uint32_t face = 0; face < 6; ++face) {
      // ONE FACE, ONE MIP, as a flat 2D image -- the render-target half of the
      // view pair cube support exists for.
      ITextureView* target =
          texture_->CreateView({.base_mip = mip,
                                .mip_count = 1,
                                .base_layer = face,
                                .layer_count = 1,
                                .dimension = TextureViewDimension::Tex2D,
                                .label = "ibl_prefiltered.face"});
      if (!target) return false;  // CreateView logged why

      encoder->Transition(texture_.get(), ResourceState::RenderTarget);
      IRenderPass* pass = encoder->BeginRenderPass(
          {.color_attachments = {{.view = target,
                                  .load_op = LoadOp::Clear,
                                  .store_op = StoreOp::Store}},
           .label = "ibl_prefilter"});
      if (!pass) {
        spdlog::error("ibl: could not begin the prefilter pass for face {} mip {}",
                      face, mip);
        return false;
      }
      const uint32_t offsets[1] = {uint32_t(stride * (mip * 6 + face))};
      pass->SetPipeline(pipeline_.get());
      pass->SetViewport(0, 0, float(size), float(size));
      pass->SetBindingTable(0, table.get(), offsets);
      pass->Draw(3);  // one fullscreen triangle
      pass->End();
    }
  }
  encoder->Finish();
  device_->Submit(*encoder);
  // Synchronous: the resolve samples this, and a half-built chain is worse than
  // a stall at startup or on an environment change.
  device_->WaitIdle();
  return true;
}

std::unique_ptr<BrdfLut> BrdfLut::Create(IRhiDevice& device,
                                         slang::SlangCompiler& compiler) {
  auto out = std::unique_ptr<BrdfLut>(new BrdfLut());

  auto vs = LoadShader(device, compiler, "brdf_lut", "vs_brdf_lut");
  auto fs = LoadShader(device, compiler, "brdf_lut", "fs_brdf_lut");
  if (!vs || !fs) return nullptr;

  auto pipeline = device.CreateRenderPipeline(
      {.vertex_shader = vs.get(),
       .vertex_entry = "vs_brdf_lut",
       .fragment_shader = fs.get(),
       .fragment_entry = "fs_brdf_lut",
       .color_formats = {Format::RG16Float},
       .cull_mode = CullMode::None,
       .label = "ibl_brdf_lut"});
  if (!pipeline) return nullptr;

  out->texture_ = device.CreateTexture(
      {.width = kSize,
       .height = kSize,
       .format = Format::RG16Float,
       .usage = TextureUsage::Sampled | TextureUsage::RenderTarget |
                TextureUsage::CopySrc,
       .label = "ibl_brdf_lut"});
  if (!out->texture_) return nullptr;
  out->view_ = out->texture_->GetDefaultView();
  if (!out->view_) return nullptr;

  auto encoder = device.CreateCommandEncoder("ibl_brdf_lut");
  if (!encoder) return nullptr;
  encoder->Transition(out->texture_.get(), ResourceState::RenderTarget);
  IRenderPass* pass = encoder->BeginRenderPass(
      {.color_attachments = {{.view = out->view_,
                              .load_op = LoadOp::Clear,
                              .store_op = StoreOp::Store}},
       .label = "ibl_brdf_lut"});
  if (!pass) {
    spdlog::error("ibl: could not begin the BRDF LUT pass");
    return nullptr;
  }
  pass->SetPipeline(pipeline.get());
  pass->SetViewport(0, 0, float(kSize), float(kSize));
  // NO BINDING TABLE: the shader has no bindings at all, every input being
  // derived from the fragment's UV.
  pass->Draw(3);  // one fullscreen triangle
  pass->End();
  encoder->Finish();
  device.Submit(*encoder);
  device.WaitIdle();
  return out;
}

}  // namespace badlands::ibl
