#include "engine/ibl/environment.hpp"

#include <algorithm>
#include <cmath>

#include <glm/gtc/packing.hpp>
#include <spdlog/spdlog.h>

#include "badlands_assets.h"
#include "core/math/spherical_harmonics.hpp"
#include "core/parallel.hpp"

namespace badlands::ibl {

namespace {

constexpr float kPi = 3.14159265358979323846f;

}  // namespace

glm::vec3 EvaluateSky(const SkySettings& sky, glm::vec3 dir) {
  const float y = dir.y;
  glm::vec3 c;
  if (y >= 0.0f) {
    // Horizon -> zenith. The square root pulls the gradient towards the
    // horizon, where a linear ramp puts almost all of its variation overhead
    // and leaves the band a surface actually reflects nearly flat.
    c = glm::mix(sky.horizon, sky.zenith, std::sqrt(std::min(y, 1.0f)));
  } else {
    c = glm::mix(sky.horizon, sky.ground, std::sqrt(std::min(-y, 1.0f)));
  }
  return c * sky.intensity;
}

RadianceFn ProceduralSky(const SkySettings& sky) {
  // BY VALUE. A reference here would dangle the moment the caller's settings
  // went out of scope, and the face evaluation runs on other threads.
  return [sky](glm::vec3 dir) { return EvaluateSky(sky, dir); };
}

std::unique_ptr<EquirectImage> EquirectImage::Load(const std::string& path) {
  BadlandsImageF32 img = badlands_decode_hdr(path.c_str());
  if (!img.rgb || img.width == 0 || img.height == 0) {
    spdlog::error("ibl: could not decode '{}' as a Radiance .hdr", path);
    badlands_image_f32_free(img);
    return nullptr;
  }
  auto out = FromTexels(img.width, img.height, img.rgb);
  badlands_image_f32_free(img);
  if (out) spdlog::info("ibl: loaded '{}' ({}x{})", path, img.width, img.height);
  return out;
}

std::unique_ptr<EquirectImage> EquirectImage::FromTexels(uint32_t width,
                                                         uint32_t height,
                                                         const float* rgb) {
  if (width == 0 || height == 0 || !rgb) {
    spdlog::error("ibl: EquirectImage needs non-zero dimensions and texels");
    return nullptr;
  }
  auto out = std::unique_ptr<EquirectImage>(new EquirectImage());
  out->width_ = width;
  out->height_ = height;
  out->rgb_.assign(rgb, rgb + size_t(width) * height * 3);
  return out;
}

glm::vec3 EquirectImage::Sample(glm::vec3 dir) const {
  if (rgb_.empty()) return glm::vec3(0.0f);

  // Latitude-longitude: u from the azimuth around +Y, v from the polar angle.
  const float u = std::atan2(dir.x, -dir.z) / (2.0f * kPi) + 0.5f;
  const float v = std::acos(std::clamp(dir.y, -1.0f, 1.0f)) / kPi;

  const float fx = u * float(width_) - 0.5f;
  const float fy = v * float(height_) - 0.5f;
  const int x0 = int(std::floor(fx));
  const int y0 = int(std::floor(fy));
  const float tx = fx - float(x0);
  const float ty = fy - float(y0);

  auto texel = [&](int x, int y) {
    // WRAPPED in longitude and CLAMPED in latitude. Wrapping v instead would
    // fetch the far side of the sky one texel past a pole, which shows as a
    // bright ring exactly where a mirror sphere reflects straight up.
    const int wx = ((x % int(width_)) + int(width_)) % int(width_);
    const int wy = std::clamp(y, 0, int(height_) - 1);
    const size_t i = (size_t(wy) * width_ + size_t(wx)) * 3;
    return glm::vec3(rgb_[i], rgb_[i + 1], rgb_[i + 2]);
  };

  return glm::mix(glm::mix(texel(x0, y0), texel(x0 + 1, y0), tx),
                  glm::mix(texel(x0, y0 + 1), texel(x0 + 1, y0 + 1), tx), ty);
}

RadianceFn EquirectRadiance(const EquirectImage& image) {
  return [&image](glm::vec3 dir) { return image.Sample(dir); };
}

glm::vec3 FaceUVToDirection(uint32_t face, float u, float v) {
  // [0,1] -> [-1,1]. Verbatim from CubemapBuilder::FaceUVToDirection; the two
  // must not drift, and the prefilter shader repeats it a third time.
  const float s = u * 2.0f - 1.0f;
  const float t = v * 2.0f - 1.0f;
  switch (face) {
    case 0: return glm::normalize(glm::vec3(1.0f, -t, -s));   // +X
    case 1: return glm::normalize(glm::vec3(-1.0f, -t, s));   // -X
    case 2: return glm::normalize(glm::vec3(s, 1.0f, t));     // +Y
    case 3: return glm::normalize(glm::vec3(s, -1.0f, -t));   // -Y
    case 4: return glm::normalize(glm::vec3(s, -t, 1.0f));    // +Z
    case 5: return glm::normalize(glm::vec3(-s, -t, -1.0f));  // -Z
    default: return glm::vec3(0.0f, 1.0f, 0.0f);
  }
}

std::vector<uint16_t> EvaluateCubeFaces(const RadianceFn& fn,
                                        uint32_t face_size) {
  if (face_size == 0 || !fn) {
    spdlog::error("ibl: EvaluateCubeFaces needs a non-zero size and a function");
    return {};
  }
  const size_t face_stride = size_t(face_size) * face_size * 4;
  std::vector<uint16_t> data(face_stride * 6);

  // One unit of work per ROW of every face, each writing a disjoint slice. A
  // sun drag re-bakes this every frame it moves, and 128^2 x 6 is ~98k
  // evaluations -- enough to be felt on one thread.
  const size_t total_rows = size_t(face_size) * 6;
  ParallelFor(total_rows, [&](size_t r) {
    const uint32_t face = uint32_t(r / face_size);
    const uint32_t py = uint32_t(r % face_size);
    const float v = (float(py) + 0.5f) / float(face_size);
    const size_t row_base =
        face * face_stride + size_t(py) * size_t(face_size) * 4;
    for (uint32_t px = 0; px < face_size; ++px) {
      const float u = (float(px) + 0.5f) / float(face_size);
      const glm::vec3 c = fn(FaceUVToDirection(face, u, v));
      const size_t i = row_base + size_t(px) * 4;
      data[i + 0] = glm::packHalf1x16(c.r);
      data[i + 1] = glm::packHalf1x16(c.g);
      data[i + 2] = glm::packHalf1x16(c.b);
      data[i + 3] = glm::packHalf1x16(1.0f);
    }
  });
  return data;
}

rhi::TexturePtr BuildEnvironmentCube(rhi::IRhiDevice& device,
                                     const RadianceFn& fn,
                                     uint32_t face_size) {
  const std::vector<uint16_t> texels = EvaluateCubeFaces(fn, face_size);
  if (texels.empty()) return nullptr;  // EvaluateCubeFaces logged why

  auto cube = device.CreateTexture(
      {.width = face_size,
       .height = face_size,
       .array_layers = 6,
       .mip_levels = 1,
       .format = rhi::Format::RGBA16Float,
       .usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::CopyDst |
                rhi::TextureUsage::CopySrc,
       .dimension = rhi::TextureDimension::Cube,
       .label = "ibl_environment"});
  if (!cube) return nullptr;  // CreateTexture logged why

  const size_t face_stride = size_t(face_size) * face_size * 4;
  for (uint32_t face = 0; face < 6; ++face) {
    const uint16_t* start = texels.data() + face * face_stride;
    cube->Write(0, face,
                {reinterpret_cast<const uint8_t*>(start),
                 face_stride * sizeof(uint16_t)});
  }
  return cube;
}

void ProjectIrradiance(const RadianceFn& fn, glm::vec4 out_sh[9],
                       int sample_count) {
  if (!fn) {
    spdlog::error("ibl: ProjectIrradiance needs a radiance function");
    return;
  }
  const sh::SHL2 coeffs = sh::ProjectFunctionToSHL2(
      [&fn](glm::vec3 dir) { return fn(dir); }, sample_count);
  for (int i = 0; i < 9; ++i) {
    out_sh[i] = glm::vec4(coeffs[size_t(i)], 0.0f);
  }
}

}  // namespace badlands::ibl
