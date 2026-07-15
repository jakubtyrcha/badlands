#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <dawn/webgpu_cpp.h>

namespace badlands {

// Bytes per pixel for a supported texture format (0 if unsupported).
size_t GetBytesPerPixel(wgpu::TextureFormat format);

// True for float formats (Depth32Float, R32Float).
bool IsFloatFormat(wgpu::TextureFormat format);

// CPU-side image with pixel access and PNG I/O (via the Rust `assets` crate).
// Supports 8-bit color formats (RGBA8/BGRA8/R8) and float formats
// (Depth32Float, R32Float). Ported from sampo::CpuImage, trimmed: image I/O
// routes through the `assets` crate (badlands_write_png/badlands_decode_image)
// instead of stb, and the UltraHDR/Oklab/fp16 machinery is dropped.
class CpuImage {
 public:
  struct Color {
    uint8_t r, g, b, a;
  };

  struct ColorF32 {
    float r, g, b, a;
  };

  CpuImage() = default;
  CpuImage(uint32_t width, uint32_t height, wgpu::TextureFormat format);

  // Load an image file (PNG/JPEG, format auto-detected) as RGBA8Unorm.
  static std::optional<CpuImage> LoadFromFile(const std::string& path);

  uint32_t GetWidth() const { return width_; }
  uint32_t GetHeight() const { return height_; }
  wgpu::TextureFormat GetFormat() const { return format_; }

  // Pixel access (format-aware, handles BGRA <-> RGBA conversion).
  // For float formats, returns a grayscale visualization.
  Color GetPixel(uint32_t x, uint32_t y) const;
  void SetPixel(uint32_t x, uint32_t y, Color color);

  // Float pixel access; for 8-bit formats converts to/from normalized [0, 1].
  ColorF32 GetPixelF32(uint32_t x, uint32_t y) const;
  void SetPixelF32(uint32_t x, uint32_t y, ColorF32 color);

  // Depth/float access (for Depth32Float, R32Float formats).
  float GetDepth(uint32_t x, uint32_t y) const;
  void SetDepth(uint32_t x, uint32_t y, float value);

  // Generic first-channel float access (R32Float, Depth32Float).
  float GetFloat(uint32_t x, uint32_t y) const;

  // Raw data access for readback.
  uint8_t* GetData() { return data_.data(); }
  const uint8_t* GetData() const { return data_.data(); }
  size_t GetRowPitch() const { return row_pitch_; }
  size_t GetDataSize() const { return data_.size(); }
  size_t GetBytesPerPixel() const { return bytes_per_pixel_; }

  // PNG export (always converts to RGBA; float formats visualized as grayscale).
  // Writes via the Rust `assets` crate (badlands_write_png).
  bool WritePng(const std::string& path) const;

  // Resize internal buffer (used during readback with a padded row pitch).
  void Resize(uint32_t width, uint32_t height, size_t row_pitch);
  void Resize(uint32_t width, uint32_t height, size_t row_pitch,
              wgpu::TextureFormat format);

 private:
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  size_t row_pitch_ = 0;
  size_t bytes_per_pixel_ = 0;
  wgpu::TextureFormat format_ = wgpu::TextureFormat::Undefined;
  std::vector<uint8_t> data_;
};

}  // namespace badlands
