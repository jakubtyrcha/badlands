#include "engine/rendering/cubemap_builder.hpp"

#include <cstdint>
#include <vector>

#include <glm/gtc/packing.hpp>
#include <spdlog/spdlog.h>

#include "core/parallel.hpp"

namespace badlands {

glm::vec3 CubemapBuilder::FaceUVToDirection(uint32_t face, float u, float v) {
  // [0,1] -> [-1,1]
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

bool CubemapBuilder::Build(wgpu::Device device, wgpu::Queue queue,
                           uint32_t face_size, const RadianceFn& fn) {
  if (face_size == 0 || !fn) {
    spdlog::error("CubemapBuilder::Build: invalid face_size/fn");
    return false;
  }
  face_size_ = face_size;

  wgpu::TextureDescriptor tex_desc;
  tex_desc.size = {face_size, face_size, 6};
  tex_desc.format = wgpu::TextureFormat::RGBA16Float;
  tex_desc.usage =
      wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;
  tex_desc.dimension = wgpu::TextureDimension::e2D;
  tex_desc.mipLevelCount = 1;
  tex_desc.sampleCount = 1;
  texture_ = device.CreateTexture(&tex_desc);
  if (!texture_) {
    spdlog::error("CubemapBuilder::Build: failed to create cube texture");
    return false;
  }

  // 4 half-float channels per texel; one contiguous buffer for all 6 faces so
  // the per-texel radiance evaluation can be parallelized without contention.
  const uint32_t texels = face_size * face_size;
  const size_t face_stride = static_cast<size_t>(texels) * 4;  // uint16 per face
  std::vector<uint16_t> data(face_stride * 6);

  // Evaluate `fn` per texel across the global thread pool, parallelized over
  // all 6 faces' rows (6 * face_size units). Each row writes a disjoint slice,
  // so no synchronization is needed; RadianceFn must be thread-safe (pure).
  const size_t total_rows = static_cast<size_t>(face_size) * 6;
  ParallelFor(total_rows, [&](size_t r) {
    const uint32_t face = static_cast<uint32_t>(r / face_size);
    const uint32_t py = static_cast<uint32_t>(r % face_size);
    const float v = (static_cast<float>(py) + 0.5f) / face_size;
    const size_t row_base = face * face_stride + static_cast<size_t>(py) * face_size * 4;
    for (uint32_t px = 0; px < face_size; ++px) {
      const float u = (static_cast<float>(px) + 0.5f) / face_size;
      const glm::vec3 dir = FaceUVToDirection(face, u, v);
      const glm::vec4 c = fn(dir);
      const size_t i = row_base + static_cast<size_t>(px) * 4;
      data[i + 0] = glm::packHalf1x16(c.r);
      data[i + 1] = glm::packHalf1x16(c.g);
      data[i + 2] = glm::packHalf1x16(c.b);
      data[i + 3] = glm::packHalf1x16(c.a);
    }
  });

  // GPU upload stays serial (the Dawn queue is used single-threaded here).
  for (uint32_t face = 0; face < 6; ++face) {
    wgpu::TexelCopyTextureInfo dst;
    dst.texture = texture_;
    dst.mipLevel = 0;
    dst.origin = {0, 0, face};  // array layer = face

    wgpu::TexelCopyBufferLayout layout;
    layout.bytesPerRow = face_size * 4 * sizeof(uint16_t);
    layout.rowsPerImage = face_size;

    wgpu::Extent3D extent = {face_size, face_size, 1};
    queue.WriteTexture(&dst, data.data() + face * face_stride,
                       face_stride * sizeof(uint16_t), &layout, &extent);
  }

  wgpu::TextureViewDescriptor view_desc;
  view_desc.format = wgpu::TextureFormat::RGBA16Float;
  view_desc.dimension = wgpu::TextureViewDimension::Cube;
  view_desc.baseMipLevel = 0;
  view_desc.mipLevelCount = 1;
  view_desc.baseArrayLayer = 0;
  view_desc.arrayLayerCount = 6;
  view_ = texture_.CreateView(&view_desc);

  return true;
}

}  // namespace badlands
