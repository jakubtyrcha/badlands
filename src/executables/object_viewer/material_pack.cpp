#include "executables/object_viewer/material_pack.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "badlands_assets.h"

namespace badlands::object_viewer {

using namespace badlands::rhi;

namespace {

// Decodes one image file into a full mip chain. Returns an invalid CpuTexture
// after logging if the file is missing or will not decode.
//
// `flip_green` inverts the green channel BEFORE the mip chain is built, which
// is how a DirectX-convention normal map becomes a GL-convention one. Doing it
// at load rather than in the shader is the same choice material_library.cpp
// makes, and for the same reason: every uploaded normal map is then one
// convention, so no consumer has to know which pack it came from.
CpuTexture DecodeWithMips(const std::string& path, bool flip_green = false) {
  CpuTexture tex;
  BadlandsImage img = badlands_decode_image(path.c_str());
  if (!img.rgba || img.width == 0 || img.height == 0) {
    spdlog::error("object_viewer: could not decode '{}'", path);
    badlands_image_free(img);
    return tex;
  }
  tex.width = img.width;
  tex.height = img.height;
  const size_t bytes = size_t(img.width) * img.height * 4;
  tex.mips.emplace_back(img.rgba, img.rgba + bytes);
  badlands_image_free(img);
  if (flip_green) {
    // BEFORE mipping: flipping afterwards would have to be done per level, and
    // a level that was missed is a surface lit the wrong way only at distance.
    for (size_t i = 1; i < tex.mips[0].size(); i += 4) {
      tex.mips[0][i] = uint8_t(255 - tex.mips[0][i]);
    }
  }

  // Down to 1x1. Stopping earlier leaves the top mips undefined, and a sampler
  // asked for a level that was never written reads whatever the allocation
  // happened to contain.
  uint32_t w = tex.width, h = tex.height;
  while (w > 1 || h > 1) {
    tex.mips.push_back(DownsampleBox(tex.mips.back(), w, h));
    w = std::max(1u, w / 2);
    h = std::max(1u, h / 2);
  }
  return tex;
}

// Uploads a CpuTexture, one Write per mip. `srgb` picks the format, and it is
// the single decision that separates colour from data.
TexturePtr Upload(IRhiDevice& device, const CpuTexture& tex, bool srgb,
                  const std::string& label) {
  if (!tex.Valid()) return nullptr;
  auto gpu = device.CreateTexture(
      {.width = tex.width,
       .height = tex.height,
       .mip_levels = uint32_t(tex.mips.size()),
       .format = srgb ? Format::RGBA8UnormSrgb : Format::RGBA8Unorm,
       .usage = TextureUsage::Sampled | TextureUsage::CopyDst,
       .label = label});
  if (!gpu) return nullptr;
  for (uint32_t m = 0; m < tex.mips.size(); ++m) {
    gpu->Write(m, 0, tex.mips[m]);
  }
  return gpu;
}

}  // namespace

std::vector<uint8_t> DownsampleBox(const std::vector<uint8_t>& src, uint32_t w,
                                   uint32_t h) {
  const uint32_t dw = std::max(1u, w / 2);
  const uint32_t dh = std::max(1u, h / 2);
  std::vector<uint8_t> dst(size_t(dw) * dh * 4, 0);
  for (uint32_t y = 0; y < dh; ++y) {
    for (uint32_t x = 0; x < dw; ++x) {
      // Clamped, so an odd dimension averages the pixel with itself rather than
      // reading past the row. A non-power-of-two source is rare here but a
      // one-row overread would be silent.
      const uint32_t x0 = std::min(x * 2, w - 1);
      const uint32_t x1 = std::min(x * 2 + 1, w - 1);
      const uint32_t y0 = std::min(y * 2, h - 1);
      const uint32_t y1 = std::min(y * 2 + 1, h - 1);
      for (int c = 0; c < 4; ++c) {
        const uint32_t sum = uint32_t(src[(size_t(y0) * w + x0) * 4 + c]) +
                             uint32_t(src[(size_t(y0) * w + x1) * 4 + c]) +
                             uint32_t(src[(size_t(y1) * w + x0) * 4 + c]) +
                             uint32_t(src[(size_t(y1) * w + x1) * 4 + c]);
        // +2 before /4: truncation alone darkens the chain by half an LSB per
        // level, which compounds into a visibly dark distant surface.
        dst[(size_t(y) * dw + x) * 4 + c] = uint8_t((sum + 2) / 4);
      }
    }
  }
  return dst;
}

void CpuTexture::SampleBilinear(float u, float v, uint32_t mip,
                                float out_rgba[4]) const {
  const uint32_t m = std::min(mip, uint32_t(mips.size() - 1));
  const uint32_t mw = std::max(1u, width >> m);
  const uint32_t mh = std::max(1u, height >> m);
  const auto& data = mips[m];

  // Repeat addressing, then the half-texel shift a GPU applies before
  // filtering. Skipping the shift puts the oracle half a texel off, which shows
  // up as a near-miss on smooth textures and an outright mismatch on sharp ones.
  auto wrap = [](float x, uint32_t n) {
    float f = std::fmod(x, float(n));
    if (f < 0) f += float(n);
    return f;
  };
  const float fx = wrap(u * float(mw) - 0.5f, mw);
  const float fy = wrap(v * float(mh) - 0.5f, mh);
  const uint32_t x0 = uint32_t(fx) % mw;
  const uint32_t y0 = uint32_t(fy) % mh;
  const uint32_t x1 = (x0 + 1) % mw;
  const uint32_t y1 = (y0 + 1) % mh;
  const float tx = fx - std::floor(fx);
  const float ty = fy - std::floor(fy);

  for (int c = 0; c < 4; ++c) {
    auto at = [&](uint32_t x, uint32_t y) {
      return float(data[(size_t(y) * mw + x) * 4 + c]) / 255.0f;
    };
    const float top = at(x0, y0) * (1 - tx) + at(x1, y0) * tx;
    const float bot = at(x0, y1) * (1 - tx) + at(x1, y1) * tx;
    out_rgba[c] = top * (1 - ty) + bot * ty;
  }
}

namespace {

// Builds a CpuTexture from a per-texel function, then mips it exactly as a
// loaded one would -- so a synthetic pack exercises the SAME upload path a real
// one does, rather than a shortcut that could diverge from it.
template <typename Fn>
CpuTexture MakeCpuTexture(uint32_t size, Fn&& texel) {
  CpuTexture tex;
  tex.width = size;
  tex.height = size;
  std::vector<uint8_t> level(size_t(size) * size * 4, 0);
  for (uint32_t y = 0; y < size; ++y) {
    for (uint32_t x = 0; x < size; ++x) {
      texel(x, y, &level[(size_t(y) * size + x) * 4]);
    }
  }
  tex.mips.push_back(std::move(level));
  uint32_t w = size, h = size;
  while (w > 1 || h > 1) {
    tex.mips.push_back(DownsampleBox(tex.mips.back(), w, h));
    w = std::max(1u, w / 2);
    h = std::max(1u, h / 2);
  }
  return tex;
}

CpuTexture ConstantTexture(uint32_t size, uint8_t r, uint8_t g, uint8_t b,
                           uint8_t a = 255) {
  return MakeCpuTexture(size, [=](uint32_t, uint32_t, uint8_t* p) {
    p[0] = r; p[1] = g; p[2] = b; p[3] = a;
  });
}

bool UploadPack(IRhiDevice& device, MaterialPack& pack,
                const std::string& label) {
  pack.gpu_albedo = Upload(device, pack.albedo, true, label + ":albedo");
  pack.gpu_normal = Upload(device, pack.normal, false, label + ":normal");
  pack.gpu_arm = Upload(device, pack.arm, false, label + ":arm");
  pack.gpu_displacement =
      Upload(device, pack.displacement, false, label + ":disp");
  pack.sampler = device.CreateSampler({.mag_filter = FilterMode::Linear,
                                       .min_filter = FilterMode::Linear,
                                       .mip_filter = FilterMode::Linear,
                                       .address_u = AddressMode::Repeat,
                                       .address_v = AddressMode::Repeat,
                                       .max_anisotropy = 16,
                                       .label = label});
  if (!pack.gpu_albedo || !pack.gpu_normal || !pack.gpu_arm ||
      !pack.gpu_displacement || !pack.sampler) {
    spdlog::error("object_viewer: could not upload the synthetic pack '{}'",
                  label);
    return false;
  }
  pack.dir = label;
  return true;
}

}  // namespace

std::unique_ptr<MaterialPack> MakeConstantPack(IRhiDevice& device,
                                               const TestPackValues& v,
                                               uint32_t size) {
  auto pack = std::make_unique<MaterialPack>();
  pack->albedo = ConstantTexture(size, v.albedo[0], v.albedo[1], v.albedo[2]);
  // (128, 128, 255) is the flat tangent-space normal. Over a plane whose
  // geometric normal is +y, the resolved shading normal must therefore be
  // exactly (0, 1, 0) -- which is a closed form, not a measurement.
  pack->normal = ConstantTexture(size, 128, 128, 255);
  pack->arm = ConstantTexture(size, v.ao, v.roughness, v.metallic);
  pack->displacement =
      ConstantTexture(size, v.displacement, v.displacement, v.displacement);
  if (!UploadPack(device, *pack, "test:constant")) return nullptr;
  return pack;
}

std::unique_ptr<MaterialPack> MakeCheckerPack(IRhiDevice& device,
                                              uint32_t size) {
  auto pack = std::make_unique<MaterialPack>();
  // ONE-TEXEL checker, so mip 1 is already flat mid-grey and every level above
  // it stays there. That makes "which mip was sampled" the difference between a
  // high-contrast result and a uniform one, rather than a subtle blur.
  pack->albedo = MakeCpuTexture(size, [](uint32_t x, uint32_t y, uint8_t* p) {
    const uint8_t c = ((x + y) & 1u) ? 255 : 0;
    p[0] = c; p[1] = c; p[2] = c; p[3] = 255;
  });
  pack->normal = ConstantTexture(size, 128, 128, 255);
  pack->arm = ConstantTexture(size, 255, 128, 0);
  pack->displacement = ConstantTexture(size, 0, 0, 0);
  if (!UploadPack(device, *pack, "test:checker")) return nullptr;
  return pack;
}

std::unique_ptr<MaterialPack> LoadMaterialPack(IRhiDevice& device,
                                               const std::string& dir) {
  const std::string manifest_path = dir + "/material.json";
  std::ifstream f(manifest_path);
  if (!f) {
    spdlog::error("object_viewer: no material manifest at '{}'", manifest_path);
    return nullptr;
  }
  nlohmann::json manifest;
  try {
    f >> manifest;
  } catch (const std::exception& e) {
    spdlog::error("object_viewer: '{}' is not valid JSON: {}", manifest_path,
                  e.what());
    return nullptr;
  }

  // The MANIFEST is the source of truth for which file fills each slot, not a
  // filename convention -- the same rule MaterialLibrary follows, and for the
  // same reason: these packs name their maps inconsistently.
  auto path_for = [&](const char* key) -> std::string {
    if (!manifest.contains(key) || !manifest[key].is_string()) return {};
    return dir + "/" + manifest[key].get<std::string>();
  };

  auto pack = std::make_unique<MaterialPack>();
  pack->dir = dir;
  for (const char* key : {"albedo", "normal", "arm"}) {
    const std::string p = path_for(key);
    if (p.empty()) {
      spdlog::error("object_viewer: '{}' names no '{}' map -- a material "
                    "without one is not a material, and a default would hide "
                    "the missing file",
                    manifest_path, key);
      return nullptr;
    }
  }
  // THE MANIFEST'S normal_format IS LOAD-BEARING, not decoration. A pack
  // authored to the other convention lights with its v-axis inverted, which
  // reads as a lighting choice rather than as a bug -- so it is honoured here
  // exactly as material_library.cpp honours it, and an unrecognised value says
  // so rather than being assumed.
  const std::string normal_format =
      manifest.value("normal_format", std::string("dx"));
  const bool is_dx_normal = normal_format == "dx";
  if (!is_dx_normal && normal_format != "gl") {
    spdlog::warn(
        "object_viewer: pack '{}' declares normal_format='{}' (expected 'dx' "
        "or 'gl') -- treating as already GL-convention, so no flip",
        dir, normal_format);
  }
  pack->albedo = DecodeWithMips(path_for("albedo"));
  pack->normal = DecodeWithMips(path_for("normal"), is_dx_normal);
  pack->arm = DecodeWithMips(path_for("arm"));
  if (!pack->albedo.Valid() || !pack->normal.Valid() || !pack->arm.Valid()) {
    return nullptr;  // DecodeWithMips logged which
  }
  // The one optional map. Absent is normal; present-but-broken is not, so a
  // named file that fails to decode still refuses.
  if (const std::string p = path_for("displacement"); !p.empty()) {
    pack->displacement = DecodeWithMips(p);
    if (!pack->displacement.Valid()) return nullptr;
  }

  // sRGB for albedo ONLY. The other three are linear data -- normals, and
  // roughness/metallic/AO -- and decoding them through the sRGB curve is the
  // classic mistake that reads as a lighting choice rather than a bug.
  pack->gpu_albedo = Upload(device, pack->albedo, true, dir + ":albedo");
  pack->gpu_normal = Upload(device, pack->normal, false, dir + ":normal");
  pack->gpu_arm = Upload(device, pack->arm, false, dir + ":arm");
  if (pack->HasDisplacement()) {
    pack->gpu_displacement =
        Upload(device, pack->displacement, false, dir + ":disp");
    if (!pack->gpu_displacement) return nullptr;
  }
  if (!pack->gpu_albedo || !pack->gpu_normal || !pack->gpu_arm) {
    spdlog::error("object_viewer: could not upload the maps of '{}'", dir);
    return nullptr;
  }

  pack->sampler = device.CreateSampler({.mag_filter = FilterMode::Linear,
                                        .min_filter = FilterMode::Linear,
                                        .mip_filter = FilterMode::Linear,
                                        .address_u = AddressMode::Repeat,
                                        .address_v = AddressMode::Repeat,
                                        .max_anisotropy = 16,
                                        .label = "material"});
  if (!pack->sampler) return nullptr;

  spdlog::info("object_viewer: loaded '{}' ({}x{}, {} mips{})", dir,
               pack->albedo.width, pack->albedo.height,
               pack->albedo.mips.size(),
               pack->HasDisplacement() ? ", with displacement" : "");
  return pack;
}

}  // namespace badlands::object_viewer
