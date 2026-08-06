#include "game/visual/impostor_baker.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <glm/gtc/matrix_transform.hpp>
#include <spdlog/spdlog.h>

#include "core/util/cpu_image.hpp"
#include "engine/rendering/shader/gpu_pipeline_generator.hpp"
#include "engine/rendering/texture_readback.hpp"
#include "game/visual/alpha_coverage.hpp"
#include "game/visual/octahedral.hpp"

namespace badlands {

namespace {

// Per-view uniforms for impostor_bake.wesl. std140-compatible by construction
// (mat4 then two vec4s), so no padding is needed.
struct BakeUniforms {
  glm::mat4 mvp{1.0f};
  glm::vec4 tint{1.0f};
  glm::vec4 params{0.0f};    // x = translucency, y = alpha cutoff, z = AO
  glm::vec4 view_dir{0.0f};  // xyz = tree-local direction toward the eye
};

wgpu::Buffer MakeUniform(wgpu::Device device, const BakeUniforms& u) {
  wgpu::BufferDescriptor desc;
  desc.size = sizeof(BakeUniforms);
  desc.usage = wgpu::BufferUsage::Uniform;
  desc.mappedAtCreation = true;
  wgpu::Buffer b = device.CreateBuffer(&desc);
  if (!b) return nullptr;
  std::memcpy(b.GetMappedRange(0, sizeof(BakeUniforms)), &u,
              sizeof(BakeUniforms));
  b.Unmap();
  return b;
}

wgpu::Buffer MakeVertexBuffer(wgpu::Device device,
                              const std::vector<float>& data) {
  if (data.empty()) return nullptr;
  wgpu::BufferDescriptor desc;
  desc.size = data.size() * sizeof(float);
  desc.usage = wgpu::BufferUsage::Vertex;
  desc.mappedAtCreation = true;
  wgpu::Buffer b = device.CreateBuffer(&desc);
  if (!b) return nullptr;
  std::memcpy(b.GetMappedRange(0, desc.size), data.data(), desc.size);
  b.Unmap();
  return b;
}

wgpu::Buffer MakeIndexBuffer(wgpu::Device device,
                             const std::vector<uint32_t>& data) {
  if (data.empty()) return nullptr;
  wgpu::BufferDescriptor desc;
  desc.size = data.size() * sizeof(uint32_t);
  desc.usage = wgpu::BufferUsage::Index;
  desc.mappedAtCreation = true;
  wgpu::Buffer b = device.CreateBuffer(&desc);
  if (!b) return nullptr;
  std::memcpy(b.GetMappedRange(0, desc.size), data.data(), desc.size);
  b.Unmap();
  return b;
}

wgpu::Texture MakeRgbaTexture(wgpu::Device device, wgpu::Queue queue,
                              uint32_t size, const uint8_t* rgba) {
  wgpu::TextureDescriptor desc;
  desc.dimension = wgpu::TextureDimension::e2D;
  desc.size = {size, size, 1};
  desc.format = wgpu::TextureFormat::RGBA8Unorm;
  desc.mipLevelCount = 1;
  desc.usage =
      wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;
  wgpu::Texture t = device.CreateTexture(&desc);
  if (!t) return nullptr;

  wgpu::TexelCopyTextureInfo dst;
  dst.texture = t;
  wgpu::TexelCopyBufferLayout layout;
  layout.bytesPerRow = size * 4;
  layout.rowsPerImage = size;
  wgpu::Extent3D extent = {size, size, 1};
  queue.WriteTexture(&dst, rgba, static_cast<size_t>(size) * size * 4, &layout,
                     &extent);
  return t;
}

// IEEE half -> float. Needed because the thickness target must be R16Float --
// the accumulation is SIGNED (a closed solid contributes exit minus enter), so
// an 8-bit unorm target would clamp every negative term to zero and destroy the
// subtraction, while float32 blending is an optional WebGPU feature. R16Float
// is core-blendable and copyable, but CpuImage's readback does not decode it.
float HalfToFloat(uint16_t h) {
  const uint32_t sign = static_cast<uint32_t>(h & 0x8000u) << 16;
  const uint32_t exp = (h >> 10) & 0x1Fu;
  const uint32_t mant = h & 0x3FFu;
  uint32_t bits = 0;
  if (exp == 0) {
    if (mant != 0) {  // subnormal: renormalize
      uint32_t e = 127 - 15 + 1, mm = mant;
      while ((mm & 0x400u) == 0) { mm <<= 1; --e; }
      mm &= 0x3FFu;
      bits = sign | (e << 23) | (mm << 13);
    } else {
      bits = sign;  // +-0
    }
  } else if (exp == 31) {
    bits = sign | 0x7F800000u | (mant << 13);  // inf / NaN
  } else {
    bits = sign | ((exp - 15 + 127) << 23) | (mant << 13);
  }
  float out;
  std::memcpy(&out, &bits, sizeof(out));
  return out;
}

// Blocking readback of an R16Float texture into floats. The shipping
// TextureReadback only decodes 8-bit and 32-bit-float formats.
std::vector<float> ReadR16FloatSync(wgpu::Instance instance, wgpu::Device device,
                                    wgpu::Queue queue, wgpu::Texture texture,
                                    uint32_t size) {
  const uint32_t bytes_per_row = size * 2;  // 512 * 2 = 1024, already aligned
  wgpu::BufferDescriptor bdesc;
  bdesc.size = static_cast<uint64_t>(bytes_per_row) * size;
  bdesc.usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead;
  wgpu::Buffer staging = device.CreateBuffer(&bdesc);
  if (!staging) return {};

  wgpu::CommandEncoder encoder = device.CreateCommandEncoder();
  wgpu::TexelCopyTextureInfo src;
  src.texture = texture;
  wgpu::TexelCopyBufferInfo dst;
  dst.buffer = staging;
  dst.layout.bytesPerRow = bytes_per_row;
  dst.layout.rowsPerImage = size;
  wgpu::Extent3D extent = {size, size, 1};
  encoder.CopyTextureToBuffer(&src, &dst, &extent);
  wgpu::CommandBuffer cmd = encoder.Finish();
  queue.Submit(1, &cmd);

  bool done = false, ok = false;
  staging.MapAsync(wgpu::MapMode::Read, 0, bdesc.size,
                   wgpu::CallbackMode::AllowProcessEvents,
                   [&](wgpu::MapAsyncStatus status, wgpu::StringView) {
                     ok = status == wgpu::MapAsyncStatus::Success;
                     done = true;
                   });
  while (!done) {
    device.Tick();
    instance.ProcessEvents();
  }
  if (!ok) return {};

  const uint16_t* halves =
      static_cast<const uint16_t*>(staging.GetConstMappedRange(0, bdesc.size));
  std::vector<float> out(static_cast<size_t>(size) * size);
  for (size_t i = 0; i < out.size(); ++i) out[i] = HalfToFloat(halves[i]);
  staging.Unmap();
  return out;
}

// One mip level of one atlas layer, as a flat RGBA8 buffer.
struct Level {
  uint32_t size = 0;
  std::vector<uint8_t> rgba;
};

// Box-downsamples the albedo and surface layers by 2 together, weighting both
// by COVERAGE.
//
// The weighting is the whole point, and an unweighted filter is subtly ruinous.
// A covered texel holds (albedo, 1); a cleared one holds (0, 0). Averaging all
// four channels blends real albedo toward the clear BLACK at every silhouette
// edge, and PreserveTileCoverage then lifts those half-dark texels back over
// the cutoff so they render. On a ~20% coverage tile that compounds down the
// chain until most surviving texels carry a fraction of the true colour -- the
// tree fades toward black exactly where the impostor takes over. leaf_texture
// .cpp dodges this by rewriting flat RGB per level; here the colour varies, so
// it has to be weighted instead.
//
// The SURFACE map is weighted by the ALBEDO's coverage, not by its own alpha
// (which is AO, not coverage). Its encoded normal has the same problem in a
// worse form: diluting toward the (0.5, 0.5) clear pulls every edge normal
// toward local +Z.
//
// The filter is TILE-SAFE with no special casing: tiles are power-of-two sized
// and power-of-two aligned, so a 2x2 output texel always reads two input texels
// from the same tile. That holds while the tile stays even, which is why the
// chain stops at 4x4.
void DownsamplePair(const Level& src_albedo, const Level& src_surface,
                    Level& out_albedo, Level& out_surface) {
  const uint32_t n = std::max(1u, src_albedo.size / 2);
  out_albedo.size = n;
  out_surface.size = n;
  out_albedo.rgba.assign(static_cast<size_t>(n) * n * 4, 0);
  out_surface.rgba.assign(static_cast<size_t>(n) * n * 4, 0);

  for (uint32_t y = 0; y < n; ++y) {
    for (uint32_t x = 0; x < n; ++x) {
      uint32_t weight = 0;             // sum of coverage
      uint32_t alb[3] = {0, 0, 0};     // coverage-weighted colour
      uint32_t surf[4] = {0, 0, 0, 0};
      for (uint32_t dy = 0; dy < 2; ++dy) {
        for (uint32_t dx = 0; dx < 2; ++dx) {
          const uint32_t sx = std::min(2 * x + dx, src_albedo.size - 1);
          const uint32_t sy = std::min(2 * y + dy, src_albedo.size - 1);
          const size_t si =
              (static_cast<size_t>(sy) * src_albedo.size + sx) * 4;
          const uint32_t a = src_albedo.rgba[si + 3];
          weight += a;
          for (int c = 0; c < 3; ++c) alb[c] += src_albedo.rgba[si + c] * a;
          for (int c = 0; c < 4; ++c) surf[c] += src_surface.rgba[si + c] * a;
        }
      }

      const size_t oi = (static_cast<size_t>(y) * n + x) * 4;
      // Coverage itself is the plain box average -- that is what makes a
      // silhouette soften rather than stay binary, and what the coverage fit
      // then rescales.
      out_albedo.rgba[oi + 3] = static_cast<uint8_t>(weight / 4);
      if (weight == 0) {
        // Nothing covered: leave the clear value rather than dividing by zero.
        out_surface.rgba[oi + 0] = 128;
        out_surface.rgba[oi + 1] = 128;
        continue;
      }
      for (int c = 0; c < 3; ++c) {
        out_albedo.rgba[oi + c] = static_cast<uint8_t>(alb[c] / weight);
      }
      for (int c = 0; c < 4; ++c) {
        out_surface.rgba[oi + c] = static_cast<uint8_t>(surf[c] / weight);
      }
    }
  }
}

// Copies one tile's texels out of a layer level, tightly packed.
std::vector<uint8_t> ExtractTile(const Level& lvl, int i, int j, uint32_t mip) {
  const ImpostorTileRect t = ImpostorTilePixels(i, j, mip);
  std::vector<uint8_t> out(static_cast<size_t>(t.size) * t.size * 4);
  for (uint32_t y = 0; y < t.size; ++y) {
    const size_t src = (static_cast<size_t>(t.y + y) * lvl.size + t.x) * 4;
    std::memcpy(&out[static_cast<size_t>(y) * t.size * 4], &lvl.rgba[src],
                static_cast<size_t>(t.size) * 4);
  }
  return out;
}

// Rescales each TILE's alpha so its own coverage matches its own mip-0
// coverage.
//
// Per tile, not per layer, and the difference is not academic: a near-overhead
// view covers a few percent of its tile while a side view covers ten times
// that, so one scale fitted over the whole layer is dominated by the dense
// tiles and leaves the sparse ones to dissolve -- which is exactly the view a
// top-down camera spends most of its time looking at.
void PreserveTileCoverage(Level& lvl, uint32_t mip, uint8_t cutoff_byte,
                          const std::vector<float>& base_coverage) {
  const ImpostorTileRect t = ImpostorTilePixels(0, 0, mip);
  const size_t texels = static_cast<size_t>(t.size) * t.size;

  for (int j = 0; j < kImpostorViewsPerAxis; ++j) {
    for (int i = 0; i < kImpostorViewsPerAxis; ++i) {
      std::vector<uint8_t> tile = ExtractTile(lvl, i, j, mip);
      const float target =
          base_coverage[static_cast<size_t>(ImpostorViewIndex(i, j))];
      const float s =
          FitAlphaCoverageScale(tile, texels, cutoff_byte, target);
      ApplyAlphaScale(tile, texels, s);

      const ImpostorTileRect r = ImpostorTilePixels(i, j, mip);
      for (uint32_t y = 0; y < r.size; ++y) {
        const size_t dst = (static_cast<size_t>(r.y + y) * lvl.size + r.x) * 4;
        std::memcpy(&lvl.rgba[dst], &tile[static_cast<size_t>(y) * r.size * 4],
                    static_cast<size_t>(r.size) * 4);
      }
    }
  }
}

// Reads one array layer's mip 0 back into a tightly-packed RGBA8 buffer. The
// readback comes back with a 256-byte-aligned row pitch, which the mip
// arithmetic must not see.
bool ReadLayer(TextureReadback& readback, wgpu::Texture texture, uint32_t layer,
               Level& out) {
  CpuImage img = readback.ReadTextureMip(texture, 0, layer).Await();
  if (img.GetWidth() != kImpostorLayerPx ||
      img.GetHeight() != kImpostorLayerPx) {
    return false;
  }
  out.size = kImpostorLayerPx;
  out.rgba.assign(static_cast<size_t>(kImpostorLayerPx) * kImpostorLayerPx * 4,
                  0);
  for (uint32_t y = 0; y < kImpostorLayerPx; ++y) {
    for (uint32_t x = 0; x < kImpostorLayerPx; ++x) {
      const CpuImage::Color c = img.GetPixel(x, y);
      const size_t i = (static_cast<size_t>(y) * kImpostorLayerPx + x) * 4;
      out.rgba[i + 0] = c.r;
      out.rgba[i + 1] = c.g;
      out.rgba[i + 2] = c.b;
      out.rgba[i + 3] = c.a;
    }
  }
  return true;
}

void WriteLayerMip(wgpu::Queue queue, wgpu::Texture texture, uint32_t layer,
                   uint32_t mip, const Level& level) {
  wgpu::TexelCopyTextureInfo dst;
  dst.texture = texture;
  dst.mipLevel = mip;
  dst.origin = {0, 0, layer};
  wgpu::TexelCopyBufferLayout layout;
  // queue.WriteTexture takes CPU data, so the 256-byte bytesPerRow rule that
  // applies to buffer copies does not apply here -- a tightly packed row is
  // legal, which matters from mip 4 down where 4*width < 256.
  layout.bytesPerRow = level.size * 4;
  layout.rowsPerImage = level.size;
  wgpu::Extent3D extent = {level.size, level.size, 1};
  queue.WriteTexture(&dst, level.rgba.data(), level.rgba.size(), &layout,
                     &extent);
}

// The bake's eye basis for a view direction. Shared in spirit with the runtime
// quad, which must reconstruct the same one -- a mismatch rotates every
// impostor about its own axis.
glm::mat4 BakeView(glm::vec3 dir, glm::vec3 center, float radius) {
  // Views top out around 72 degrees of elevation, so +Y is never near-parallel
  // to `dir`; the guard is here for a future grid that reaches higher rather
  // than for the current one.
  const glm::vec3 up = std::abs(dir.y) > 0.99f ? glm::vec3(0.0f, 0.0f, 1.0f)
                                               : glm::vec3(0.0f, 1.0f, 0.0f);
  return glm::lookAt(center + dir * radius, center, up);
}

}  // namespace

ImpostorBakeResult BakeImpostorAtlas(wgpu::Device device, wgpu::Queue queue,
                                     GpuPipelineGenerator& pipeline_gen,
                                     std::span<const InstancedLodModel> models) {
  ImpostorBakeResult result;
  const wgpu::Instance instance = device.GetAdapter().GetInstance();
  if (models.empty()) {
    spdlog::error("BakeImpostorAtlas: no models");
    return result;
  }

  result.atlas =
      CreateImpostorAtlas(device, static_cast<uint32_t>(models.size()));
  if (!result.atlas.valid()) return result;

  RenderPipelineDeclaration decl;
  decl.shader_path = "game/impostor_bake.wesl";
  decl.vertex_layout = VertexLayout::kTexturedMesh;
  // Two-sided: leaf cards are flat quads seen from either face, and half the
  // crown would vanish under back-face culling.
  decl.cull_mode = wgpu::CullMode::None;
  decl.depth_format = wgpu::TextureFormat::Depth32Float;
  decl.depth_write = true;
  // Conventional depth, not the engine's reversed-Z -- this pass shares its
  // depth with nothing (see impostor_bake.wesl).
  decl.depth_compare = wgpu::CompareFunction::Less;

  const RenderTargetFormats formats = {wgpu::TextureFormat::RGBA8Unorm,
                                       wgpu::TextureFormat::RGBA8Unorm};
  std::shared_ptr<const CompiledPipeline> pipeline =
      pipeline_gen.GetPipeline(decl, formats);
  if (!pipeline || !pipeline->pipeline) {
    spdlog::error("BakeImpostorAtlas: impostor_bake pipeline failed to build");
    return result;
  }

  // Thickness: additive, and NO depth state at all -- every layer of the crown
  // has to contribute, so an early-Z reject would truncate the sum to the
  // front-most surface.
  RenderPipelineDeclaration thick_decl;
  thick_decl.shader_path = "game/impostor_thickness.wesl";
  thick_decl.vertex_layout = VertexLayout::kTexturedMesh;
  thick_decl.cull_mode = wgpu::CullMode::None;
  wgpu::BlendState additive;
  additive.color.srcFactor = wgpu::BlendFactor::One;
  additive.color.dstFactor = wgpu::BlendFactor::One;
  additive.color.operation = wgpu::BlendOperation::Add;
  additive.alpha.srcFactor = wgpu::BlendFactor::One;
  additive.alpha.dstFactor = wgpu::BlendFactor::One;
  additive.alpha.operation = wgpu::BlendOperation::Add;
  thick_decl.custom_blend = additive;
  std::shared_ptr<const CompiledPipeline> thick_pipeline =
      pipeline_gen.GetPipeline(
          thick_decl, RenderTargetFormats{wgpu::TextureFormat::R16Float});
  if (!thick_pipeline || !thick_pipeline->pipeline) {
    spdlog::error("BakeImpostorAtlas: impostor_thickness pipeline failed");
    return result;
  }

  // R16Float because additive blending on a float target is only guaranteed in
  // core WebGPU at 16 bits; float32 blending sits behind an optional feature.
  wgpu::TextureDescriptor thick_desc;
  thick_desc.dimension = wgpu::TextureDimension::e2D;
  thick_desc.size = {kImpostorLayerPx, kImpostorLayerPx, 1};
  thick_desc.format = wgpu::TextureFormat::R16Float;
  thick_desc.usage =
      wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::CopySrc;
  wgpu::Texture thickness = device.CreateTexture(&thick_desc);
  if (!thickness) {
    spdlog::error("BakeImpostorAtlas: thickness target creation failed");
    return result;
  }

  // Depth at layer resolution; mip 0 is the only level rendered, the rest are
  // filtered down from it.
  wgpu::TextureDescriptor depth_desc;
  depth_desc.dimension = wgpu::TextureDimension::e2D;
  depth_desc.size = {kImpostorLayerPx, kImpostorLayerPx, 1};
  depth_desc.format = wgpu::TextureFormat::Depth32Float;
  depth_desc.usage = wgpu::TextureUsage::RenderAttachment;
  wgpu::Texture depth = device.CreateTexture(&depth_desc);

  // The fallback albedo for a submesh that binds none (solid-colour bark, a
  // voxel crown): an opaque white 1x1, so `tint` alone becomes the albedo.
  const uint8_t white[4] = {255, 255, 255, 255};
  wgpu::Texture white_tex = MakeRgbaTexture(device, queue, 1, white);
  wgpu::TextureView white_view = white_tex ? white_tex.CreateView() : nullptr;

  wgpu::SamplerDescriptor samp_desc;
  samp_desc.magFilter = wgpu::FilterMode::Linear;
  samp_desc.minFilter = wgpu::FilterMode::Linear;
  wgpu::Sampler sampler = device.CreateSampler(&samp_desc);

  if (!depth || !white_view || !sampler) {
    spdlog::error("BakeImpostorAtlas: failed to create bake resources");
    return result;
  }

  result.placement.resize(models.size());
  TextureReadback readback(instance, device, queue);
  size_t total_draws = 0;

  for (size_t m = 0; m < models.size(); ++m) {
    const InstancedLodModel& model = models[m];

    // Resolve the spec's (lod, submesh) references into drawables, dropping
    // any that turned out empty.
    //
    // Range-checked HERE rather than relying on ValidateLodModel: that runs
    // inside BuildInstancedLodField, and every real caller bakes BEFORE
    // building the field (the field needs the atlas this produces), so an
    // out-of-range index would read out of bounds here with nothing having
    // checked it.
    struct Drawable {
      const ImpostorBakeSubmesh* spec = nullptr;
      const TexturedMeshResult* mesh = nullptr;
      wgpu::Buffer vertex_buffer;
      wgpu::Buffer index_buffer;
      uint32_t index_count = 0;
    };
    std::vector<Drawable> drawables;

    // One frame for all 16 views: centre and radius of the silhouette's
    // bounding sphere, in native units, over exactly what gets baked.
    Aabb bounds = Aabb::Empty();

    for (const ImpostorBakeSubmesh& sub : model.impostor.submeshes) {
      if (sub.lod >= model.levels.size() ||
          sub.submesh >= model.levels[sub.lod].size()) {
        spdlog::error(
            "BakeImpostorAtlas: model {} bakes (lod {}, submesh {}), outside "
            "its {} levels",
            m, sub.lod, sub.submesh, model.levels.size());
        return result;
      }
      const TexturedMeshResult& mesh = model.levels[sub.lod][sub.submesh];
      if (mesh.mesh.vertex_count == 0 || mesh.mesh.indices.empty()) continue;
      bounds = bounds.Union(mesh.local_bounds);

      Drawable d;
      d.spec = &sub;
      d.mesh = &mesh;
      d.vertex_buffer = MakeVertexBuffer(device, mesh.mesh.vertices);
      d.index_buffer = MakeIndexBuffer(device, mesh.mesh.indices);
      d.index_count = static_cast<uint32_t>(mesh.mesh.indices.size());
      if (!d.vertex_buffer || !d.index_buffer) {
        spdlog::error("BakeImpostorAtlas: buffer creation failed at model {}",
                      m);
        return result;
      }
      drawables.push_back(std::move(d));
    }

    if (drawables.empty()) {
      spdlog::error(
          "BakeImpostorAtlas: model {} named {} bake submesh(es) but none has "
          "geometry -- its atlas layer would render as a hole with nothing in "
          "the log",
          m, model.impostor.submeshes.size());
      return result;
    }

    const glm::vec3 center = bounds.Center();
    const float radius =
        std::max(0.5f * glm::length(bounds.max - bounds.min), 1e-3f);
    result.placement[m] = ImpostorPlacement{center, radius};

    // One bind group per (view, drawable): they differ in tint, brightness and
    // texture, and all three live in the same group.
    std::vector<wgpu::Buffer> uniforms;
    std::vector<wgpu::BindGroup> bind_groups;
    const size_t groups_per_view = drawables.size();
    uniforms.reserve(static_cast<size_t>(kImpostorViewCount) * groups_per_view);
    bind_groups.reserve(static_cast<size_t>(kImpostorViewCount) *
                        groups_per_view);

    // The thickness pass needs only the MVP, so it gets its own (much smaller)
    // uniform per view.
    std::vector<wgpu::Buffer> thick_uniforms;
    std::vector<wgpu::BindGroup> thick_bind_groups;
    thick_uniforms.reserve(kImpostorViewCount);
    thick_bind_groups.reserve(kImpostorViewCount);

    const glm::mat4 proj =
        glm::ortho(-radius, radius, -radius, radius, 0.0f, 2.0f * radius);

    for (int j = 0; j < kImpostorViewsPerAxis; ++j) {
      for (int i = 0; i < kImpostorViewsPerAxis; ++i) {
        const glm::vec3 view_dir = ImpostorViewDirection(i, j);
        const glm::mat4 mvp = proj * BakeView(view_dir, center, radius);

        if (!model.impostor.opaque) {
          wgpu::BufferDescriptor tdesc;
          tdesc.size = sizeof(glm::mat4);
          tdesc.usage = wgpu::BufferUsage::Uniform;
          tdesc.mappedAtCreation = true;
          wgpu::Buffer tb = device.CreateBuffer(&tdesc);
          std::memcpy(tb.GetMappedRange(0, sizeof(glm::mat4)), &mvp,
                      sizeof(glm::mat4));
          tb.Unmap();
          std::vector<wgpu::BindGroupEntry> tentries(1);
          tentries[0].binding = 0;
          tentries[0].buffer = tb;
          tentries[0].size = sizeof(glm::mat4);
          thick_bind_groups.push_back(
              CreateBindGroup(device, *thick_pipeline, 0, tentries));
          thick_uniforms.push_back(std::move(tb));
        }

        for (const Drawable& d : drawables) {
          BakeUniforms u;
          u.mvp = mvp;
          u.tint = glm::vec4(d.spec->tint, 1.0f);
          // x = translucency, y = alpha cutoff, z = AO, w = voxel-brightness
          // mix. Cutoff stays 0 for every case this bakes: bark, a solid voxel
          // crown and a prop's mesh are all opaque, so mip 0's alpha is a hard
          // silhouette mask and there is nothing to cut.
          u.params = glm::vec4(model.impostor.transmission_strength, 0.0f, 1.0f,
                               d.spec->voxel_brightness);
          u.view_dir = glm::vec4(view_dir, 0.0f);

          wgpu::Buffer ub = MakeUniform(device, u);
          std::vector<wgpu::BindGroupEntry> entries(3);
          entries[0].binding = 0;
          entries[0].buffer = ub;
          entries[0].size = sizeof(BakeUniforms);
          entries[1].binding = 1;
          entries[1].textureView = d.spec->albedo ? d.spec->albedo : white_view;
          entries[2].binding = 2;
          entries[2].sampler = sampler;
          bind_groups.push_back(CreateBindGroup(device, *pipeline, 0, entries));
          uniforms.push_back(std::move(ub));
        }
      }
    }

    // One pass for the whole layer's mip 0: clear once, then draw each view
    // into its own viewport.
    wgpu::TextureViewDescriptor tv;
    tv.dimension = wgpu::TextureViewDimension::e2D;
    tv.baseMipLevel = 0;
    tv.mipLevelCount = 1;
    tv.baseArrayLayer = static_cast<uint32_t>(m);
    tv.arrayLayerCount = 1;

    wgpu::RenderPassColorAttachment color[2] = {};
    color[0].view = result.atlas.albedo.CreateView(&tv);
    color[0].loadOp = wgpu::LoadOp::Clear;
    color[0].storeOp = wgpu::StoreOp::Store;
    color[0].clearValue = {0.0, 0.0, 0.0, 0.0};
    color[1].view = result.atlas.surface.CreateView(&tv);
    color[1].loadOp = wgpu::LoadOp::Clear;
    color[1].storeOp = wgpu::StoreOp::Store;
    color[1].clearValue = {0.5, 0.5, 0.0, 0.0};

    wgpu::RenderPassDepthStencilAttachment ds;
    ds.view = depth.CreateView();
    ds.depthLoadOp = wgpu::LoadOp::Clear;
    ds.depthStoreOp = wgpu::StoreOp::Store;
    ds.depthClearValue = 1.0f;

    wgpu::RenderPassDescriptor pass_desc;
    pass_desc.colorAttachmentCount = 2;
    pass_desc.colorAttachments = color;
    pass_desc.depthStencilAttachment = &ds;

    wgpu::CommandEncoder encoder = device.CreateCommandEncoder();
    wgpu::RenderPassEncoder pass = encoder.BeginRenderPass(&pass_desc);
    pass.SetPipeline(pipeline->pipeline);

    size_t bg = 0;
    for (int j = 0; j < kImpostorViewsPerAxis; ++j) {
      for (int i = 0; i < kImpostorViewsPerAxis; ++i) {
        const ImpostorTileRect tile = ImpostorTilePixels(i, j, 0);
        pass.SetViewport(static_cast<float>(tile.x), static_cast<float>(tile.y),
                         static_cast<float>(tile.size),
                         static_cast<float>(tile.size), 0.0f, 1.0f);

        for (const Drawable& d : drawables) {
          pass.SetBindGroup(0, bind_groups[bg++]);
          pass.SetVertexBuffer(0, d.vertex_buffer);
          pass.SetIndexBuffer(d.index_buffer, wgpu::IndexFormat::Uint32);
          pass.DrawIndexed(d.index_count);
          ++total_draws;
        }
      }
    }
    pass.End();

    // --- Thickness pass: same views, same viewports, additive, no depth. ---
    //
    // Skipped entirely for an opaque model. It costs a full render per view
    // plus an R16Float readback to produce a channel the runtime then
    // multiplies by transmission_strength -- zero, for anything opaque. The
    // surface map's alpha stays at its clear value, which is what "no
    // transmitted term" means.
    if (!model.impostor.opaque) {
      wgpu::RenderPassColorAttachment thick_color = {};
      thick_color.view = thickness.CreateView();
      thick_color.loadOp = wgpu::LoadOp::Clear;
      thick_color.storeOp = wgpu::StoreOp::Store;
      thick_color.clearValue = {0.0, 0.0, 0.0, 0.0};

      wgpu::RenderPassDescriptor thick_pass_desc;
      thick_pass_desc.colorAttachmentCount = 1;
      thick_pass_desc.colorAttachments = &thick_color;

      wgpu::RenderPassEncoder tpass = encoder.BeginRenderPass(&thick_pass_desc);
      tpass.SetPipeline(thick_pipeline->pipeline);
      size_t tbg = 0;
      for (int j = 0; j < kImpostorViewsPerAxis; ++j) {
        for (int i = 0; i < kImpostorViewsPerAxis; ++i) {
          const ImpostorTileRect tile = ImpostorTilePixels(i, j, 0);
          tpass.SetViewport(
              static_cast<float>(tile.x), static_cast<float>(tile.y),
              static_cast<float>(tile.size), static_cast<float>(tile.size),
              0.0f, 1.0f);
          tpass.SetBindGroup(0, thick_bind_groups[tbg++]);
          // Every baked submesh occludes, so every one contributes optical path.
          for (const Drawable& d : drawables) {
            tpass.SetVertexBuffer(0, d.vertex_buffer);
            tpass.SetIndexBuffer(d.index_buffer, wgpu::IndexFormat::Uint32);
            tpass.DrawIndexed(d.index_count);
          }
        }
      }
      tpass.End();
    }

    wgpu::CommandBuffer cmd = encoder.Finish();
    queue.Submit(1, &cmd);

    // Mips: read mip 0 back, filter on the CPU, upload the rest. The alpha
    // rescale is why this is not a GPU box filter -- coverage preservation
    // needs a search over scales (see alpha_coverage.hpp), and without it
    // cutout foliage thins and eventually dissolves with distance.
    Level albedo0, surface0;
    if (!ReadLayer(readback, result.atlas.albedo, static_cast<uint32_t>(m),
                   albedo0) ||
        !ReadLayer(readback, result.atlas.surface, static_cast<uint32_t>(m),
                   surface0)) {
      spdlog::error("BakeImpostorAtlas: mip-0 readback failed at model {}", m);
      return result;
    }

    // Fold the accumulated thickness into the surface map's alpha.
    //
    // abs(): the sign of the sum depends on the meshes' winding, which
    // impostor_thickness.wesl deliberately does not try to interpret (a voxel
    // tet has ONE shared normal for all four faces, so a normal-vs-view test
    // cannot identify a back face). A globally inverted winding only flips the
    // total, which this removes.
    //
    // The sum is already in normalized depth units -- the ortho spans exactly
    // 2 * radius -- so it needs no scaling, only a clamp for the pathological
    // case of a mesh that is not closed.
    if (!model.impostor.opaque) {
      const std::vector<float> tvals = ReadR16FloatSync(
          instance, device, queue, thickness, kImpostorLayerPx);
      if (tvals.size() != static_cast<size_t>(kImpostorLayerPx) *
                              kImpostorLayerPx) {
        spdlog::error("BakeImpostorAtlas: thickness readback failed at model {}",
                      m);
        return result;
      }
      std::vector<float> covered;
      covered.reserve(tvals.size());
      for (size_t i = 0; i < tvals.size(); ++i) {
        if (albedo0.rgba[i * 4 + 3] < 128) continue;
        covered.push_back(std::abs(tvals[i]));
      }
      float scale = 1.0f;
      if (!covered.empty()) {
        std::vector<float> sorted = covered;
        const size_t p99 = std::min(sorted.size() - 1, sorted.size() * 99 / 100);
        std::nth_element(sorted.begin(), sorted.begin() + p99, sorted.end());
        scale = 1.0f / std::max(sorted[p99], 1e-4f);
      }
      for (size_t i = 0; i < tvals.size(); ++i) {
        const float t = std::clamp(std::abs(tvals[i]) * scale, 0.0f, 1.0f);
        surface0.rgba[i * 4 + 3] =
            static_cast<uint8_t>(std::lround(t * 255.0f));
      }
      WriteLayerMip(queue, result.atlas.surface, static_cast<uint32_t>(m), 0,
                    surface0);
    }

    // Every submesh this bakes is opaque -- bark, a solid voxel crown, a prop's
    // mesh -- so mip 0's alpha is a hard 0/1 SILHOUETTE mask rather than a leaf
    // cutout. Half-coverage is therefore the right threshold to preserve down
    // the chain; a tree's own leaves.alpha_cutoff described its CARDS, which
    // take no part in the bake.
    constexpr uint8_t cutoff_byte = kImpostorAlphaCutoffByte;

    // Each tile's own mip-0 coverage is the target every coarser level of that
    // tile is fitted back to.
    std::vector<float> base_coverage(kImpostorViewCount, 0.0f);
    {
      const ImpostorTileRect t0 = ImpostorTilePixels(0, 0, 0);
      const size_t texels = static_cast<size_t>(t0.size) * t0.size;
      for (int j = 0; j < kImpostorViewsPerAxis; ++j) {
        for (int i = 0; i < kImpostorViewsPerAxis; ++i) {
          const std::vector<uint8_t> tile = ExtractTile(albedo0, i, j, 0);
          base_coverage[static_cast<size_t>(ImpostorViewIndex(i, j))] =
              AlphaCoverage(tile, texels, cutoff_byte);
        }
      }
    }

    Level a = albedo0, s = surface0;
    for (uint32_t mip = 1; mip < kImpostorMipLevels; ++mip) {
      Level next_a, next_s;
      DownsamplePair(a, s, next_a, next_s);
      a = std::move(next_a);
      s = std::move(next_s);
      // Only the coverage map is rescaled: `surface`'s alpha is baked AO, not
      // a cutout, and forcing it to hold a coverage fraction would corrupt it.
      PreserveTileCoverage(a, mip, cutoff_byte, base_coverage);
      WriteLayerMip(queue, result.atlas.albedo, static_cast<uint32_t>(m), mip, a);
      WriteLayerMip(queue, result.atlas.surface, static_cast<uint32_t>(m), mip, s);
    }
  }

  result.ok = true;
  spdlog::info("impostor bake: {} models x {} views, {} draws", models.size(),
               kImpostorViewCount, total_draws);
  return result;
}

}  // namespace badlands
