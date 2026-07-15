#include "core/util/cpu_image.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "badlands_assets.h"  // badlands_write_png / badlands_decode_image

namespace badlands {

size_t GetBytesPerPixel(wgpu::TextureFormat format) {
  switch (format) {
    case wgpu::TextureFormat::Depth32Float:
    case wgpu::TextureFormat::R32Float:
      return 4;
    case wgpu::TextureFormat::BGRA8Unorm:
    case wgpu::TextureFormat::BGRA8UnormSrgb:
    case wgpu::TextureFormat::RGBA8Unorm:
    case wgpu::TextureFormat::RGBA8UnormSrgb:
      return 4;
    case wgpu::TextureFormat::R8Unorm:
      return 1;
    default:
      return 0;  // Unsupported format
  }
}

bool IsFloatFormat(wgpu::TextureFormat format) {
  switch (format) {
    case wgpu::TextureFormat::Depth32Float:
    case wgpu::TextureFormat::R32Float:
      return true;
    default:
      return false;
  }
}

CpuImage::CpuImage(uint32_t width, uint32_t height, wgpu::TextureFormat format)
    : width_(width), height_(height), format_(format) {
  bytes_per_pixel_ = ::badlands::GetBytesPerPixel(format);
  if (bytes_per_pixel_ == 0) {
    bytes_per_pixel_ = 4;  // Fallback for unknown formats
  }
  row_pitch_ = static_cast<size_t>(width) * bytes_per_pixel_;
  data_.resize(row_pitch_ * height);
}

void CpuImage::Resize(uint32_t width, uint32_t height, size_t row_pitch) {
  width_ = width;
  height_ = height;
  row_pitch_ = row_pitch;
  bytes_per_pixel_ = ::badlands::GetBytesPerPixel(format_);
  if (bytes_per_pixel_ == 0) {
    bytes_per_pixel_ = 4;
  }
  data_.resize(row_pitch * height);
}

void CpuImage::Resize(uint32_t width, uint32_t height, size_t row_pitch,
                      wgpu::TextureFormat format) {
  format_ = format;
  Resize(width, height, row_pitch);
}

CpuImage::Color CpuImage::GetPixel(uint32_t x, uint32_t y) const {
  if (x >= width_ || y >= height_) {
    return {0, 0, 0, 0};
  }

  const uint8_t* pixel = data_.data() + y * row_pitch_ + x * bytes_per_pixel_;

  // Float formats: visualize as grayscale.
  if (format_ == wgpu::TextureFormat::Depth32Float ||
      format_ == wgpu::TextureFormat::R32Float) {
    float value;
    std::memcpy(&value, pixel, sizeof(float));
    uint8_t gray =
        static_cast<uint8_t>(std::fmin(std::fmax(value, 0.0f), 1.0f) * 255.0f);
    return {gray, gray, gray, 255};
  }

  // Single-channel: replicate to grayscale (avoids reading past a 1-byte pixel).
  if (format_ == wgpu::TextureFormat::R8Unorm) {
    return {pixel[0], pixel[0], pixel[0], 255};
  }

  // BGRA -> RGBA
  if (format_ == wgpu::TextureFormat::BGRA8Unorm ||
      format_ == wgpu::TextureFormat::BGRA8UnormSrgb) {
    return {pixel[2], pixel[1], pixel[0], pixel[3]};
  }

  // RGBA or other 4-byte formats: return as-is.
  return {pixel[0], pixel[1], pixel[2], pixel[3]};
}

void CpuImage::SetPixel(uint32_t x, uint32_t y, Color color) {
  if (x >= width_ || y >= height_) {
    return;
  }

  uint8_t* pixel = data_.data() + y * row_pitch_ + x * bytes_per_pixel_;

  if (format_ == wgpu::TextureFormat::BGRA8Unorm ||
      format_ == wgpu::TextureFormat::BGRA8UnormSrgb) {
    // RGBA -> BGRA
    pixel[0] = color.b;
    pixel[1] = color.g;
    pixel[2] = color.r;
    pixel[3] = color.a;
  } else if (format_ == wgpu::TextureFormat::R8Unorm) {
    pixel[0] = color.r;
  } else {
    pixel[0] = color.r;
    pixel[1] = color.g;
    pixel[2] = color.b;
    pixel[3] = color.a;
  }
}

CpuImage::ColorF32 CpuImage::GetPixelF32(uint32_t x, uint32_t y) const {
  Color c = GetPixel(x, y);
  return {c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, c.a / 255.0f};
}

void CpuImage::SetPixelF32(uint32_t x, uint32_t y, ColorF32 color) {
  SetPixel(x, y,
           {static_cast<uint8_t>(std::clamp(color.r, 0.0f, 1.0f) * 255.0f),
            static_cast<uint8_t>(std::clamp(color.g, 0.0f, 1.0f) * 255.0f),
            static_cast<uint8_t>(std::clamp(color.b, 0.0f, 1.0f) * 255.0f),
            static_cast<uint8_t>(std::clamp(color.a, 0.0f, 1.0f) * 255.0f)});
}

float CpuImage::GetDepth(uint32_t x, uint32_t y) const {
  if (x >= width_ || y >= height_) {
    return 0.0f;
  }

  if (format_ != wgpu::TextureFormat::Depth32Float &&
      format_ != wgpu::TextureFormat::R32Float) {
    return 0.0f;
  }

  const uint8_t* pixel = data_.data() + y * row_pitch_ + x * bytes_per_pixel_;
  float value;
  std::memcpy(&value, pixel, sizeof(float));  // First float (R channel)
  return value;
}

void CpuImage::SetDepth(uint32_t x, uint32_t y, float value) {
  if (x >= width_ || y >= height_) {
    return;
  }

  if (format_ != wgpu::TextureFormat::Depth32Float &&
      format_ != wgpu::TextureFormat::R32Float) {
    return;
  }

  uint8_t* pixel = data_.data() + y * row_pitch_ + x * bytes_per_pixel_;
  std::memcpy(pixel, &value, sizeof(float));
}

float CpuImage::GetFloat(uint32_t x, uint32_t y) const {
  if (x >= width_ || y >= height_) {
    return 0.0f;
  }

  const uint8_t* pixel = data_.data() + y * row_pitch_ + x * bytes_per_pixel_;

  switch (format_) {
    case wgpu::TextureFormat::R32Float:
    case wgpu::TextureFormat::Depth32Float: {
      float value;
      std::memcpy(&value, pixel, sizeof(float));
      return value;
    }
    default:
      return 0.0f;
  }
}

bool CpuImage::WritePng(const std::string& path) const {
  if (width_ == 0 || height_ == 0) {
    return false;
  }

  // Convert to tightly-packed RGBA8 (badlands_write_png wants no row padding).
  std::vector<uint8_t> rgba_data(static_cast<size_t>(width_) * height_ * 4);
  for (uint32_t y = 0; y < height_; ++y) {
    for (uint32_t x = 0; x < width_; ++x) {
      Color c = GetPixel(x, y);
      size_t dst = (static_cast<size_t>(y) * width_ + x) * 4;
      rgba_data[dst + 0] = c.r;
      rgba_data[dst + 1] = c.g;
      rgba_data[dst + 2] = c.b;
      rgba_data[dst + 3] = 255;  // Force fully opaque for screenshots
    }
  }

  // The C ABI has no success/failure return (errors log to stderr); report
  // success when the input was well-formed.
  badlands_write_png(path.c_str(), rgba_data.data(), width_, height_);
  return true;
}

std::optional<CpuImage> CpuImage::LoadFromFile(const std::string& path) {
  BadlandsImage decoded = badlands_decode_image(path.c_str());
  if (decoded.rgba == nullptr || decoded.width == 0 || decoded.height == 0) {
    badlands_image_free(decoded);  // safe on a NULL/failure result
    return std::nullopt;
  }

  CpuImage img(decoded.width, decoded.height, wgpu::TextureFormat::RGBA8Unorm);
  std::memcpy(img.data_.data(), decoded.rgba,
              static_cast<size_t>(decoded.width) * decoded.height * 4);
  badlands_image_free(decoded);
  return img;
}

}  // namespace badlands
