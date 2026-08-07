#include "engine/app/surface_encode.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <spdlog/spdlog.h>

#include "core/half.hpp"

namespace badlands::rhi_app {

using rhi::Format;

namespace {

// The sRGB transfer function. A float surface is LINEAR, and 8-bit PNG viewers
// assume encoded, so skipping this makes every screenshot of the HDR path look
// several stops too dark rather than merely clipped.
uint8_t EncodeChannel(float linear, bool& clipped) {
  if (linear > 1.0f) {
    clipped = true;
    linear = 1.0f;
  }
  if (!(linear > 0.0f)) linear = 0.0f;  // written to catch NaN
  const float encoded = linear <= 0.0031308f
                            ? linear * 12.92f
                            : 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
  return uint8_t(std::clamp(encoded * 255.0f + 0.5f, 0.0f, 255.0f));
}

}  // namespace

bool EncodeSurfaceToRgba8(std::span<const uint8_t> src, Format format,
                          uint32_t width, uint32_t height,
                          std::vector<uint8_t>& out, bool* clipped) {
  const size_t texels = size_t(width) * height;
  bool any_clipped = false;

  // BOUNDS BY SUBTRACTION, not by addition: `texels * bytes` can wrap on a
  // width and height that are individually plausible.
  auto has_room = [&](size_t bytes_per_texel) {
    return texels != 0 && src.size() / bytes_per_texel >= texels;
  };

  switch (format) {
    case Format::RGBA8Unorm:
    case Format::RGBA8UnormSrgb:
    case Format::BGRA8Unorm:
    case Format::BGRA8UnormSrgb: {
      if (!has_room(4)) break;
      out.assign(src.begin(), src.begin() + std::ptrdiff_t(texels * 4));
      // BGRA on the wire (CAMetalLayer takes nothing else), RGBA in a PNG.
      if (format == Format::BGRA8Unorm || format == Format::BGRA8UnormSrgb) {
        for (size_t i = 0; i + 3 < out.size(); i += 4) {
          std::swap(out[i], out[i + 2]);
        }
      }
      if (clipped) *clipped = false;
      return true;
    }
    case Format::RGBA16Float: {
      if (!has_room(8)) break;
      out.assign(texels * 4, 0);
      for (size_t i = 0; i < texels * 4; ++i) {
        uint16_t h = 0;
        std::memcpy(&h, &src[i * 2], 2);
        const float v = core::HalfToFloat(h);
        // ALPHA IS NOT A COLOUR. Running the transfer function over it would
        // lighten every partially transparent texel in the file.
        out[i] = (i % 4 == 3)
                     ? uint8_t(std::clamp(v * 255.0f + 0.5f, 0.0f, 255.0f))
                     : EncodeChannel(v, any_clipped);
      }
      if (clipped) *clipped = any_clipped;
      return true;
    }
    default:
      spdlog::error("rhi_app: cannot write a PNG from surface format {}",
                    rhi::ToString(format));
      return false;
  }

  spdlog::error(
      "rhi_app: readback is {} bytes, too small for {}x{} of format {}",
      src.size(), width, height, rhi::ToString(format));
  return false;
}

}  // namespace badlands::rhi_app
