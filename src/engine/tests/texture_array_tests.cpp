// GPU tests for PackTexturesIntoArray -- the primitive that turns N already-
// mipped 2D textures into one texture_2d_array (layer i = layers[i]) by copying
// every source mip across, which is how the terrain material gets real mipped
// PBR layers without array-aware mip generation.
//
// Covers the happy path (layer identity + ordering + the mip chain survives the
// copy) and the two validation contracts that are easy to get wrong: a null
// layer must be rejected BEFORE anything is dereferenced, and a format mismatch
// must be rejected before Dawn sees an invalid copy.

#include <catch_amalgamated.hpp>

#include <array>
#include <cstdint>
#include <vector>

#include <dawn/webgpu_cpp.h>

#include "core/util/cpu_image.hpp"
#include "engine/rendering/shader/gpu_pipeline_generator.hpp"
#include "engine/rendering/texture_loader.hpp"
#include "engine/rendering/texture_readback.hpp"
#include "engine/rendering/util/find_shader_directory.hpp"
#include "gpu_test_helpers.hpp"

using namespace badlands;

namespace {

struct TestGpu {
  wgpu::Instance instance;
  wgpu::Device device;
  wgpu::Queue queue;
};

TestGpu MakeGpu() {
  TestGpu g;
  g.instance = wgpu::CreateInstance();
  wgpu::Adapter adapter = test::RequestAdapter(g.instance);
  REQUIRE(adapter);
  g.device = test::RequestDevice(adapter, "texture_array_test");
  REQUIRE(g.device);
  g.queue = g.device.GetQueue();
  return g;
}

// A solid-color WxH RGBA8 texture with a full mip chain (via the real loader,
// so the sources match what LoadTexture2D/LoadPack produce -- notably CopySrc).
LoadedTexture SolidMipped(TestGpu& g, GpuPipelineGenerator& gen, uint32_t size,
                          uint8_t r, uint8_t gg, uint8_t b) {
  std::vector<uint8_t> pixels(static_cast<size_t>(size) * size * 4);
  for (size_t i = 0; i < pixels.size(); i += 4) {
    pixels[i + 0] = r;
    pixels[i + 1] = gg;
    pixels[i + 2] = b;
    pixels[i + 3] = 255;
  }
  return UploadTexture2DWithMips(g.device, g.queue, gen, size, size,
                                 pixels.data());
}

// Bare texture with an explicit format (no mips beyond the requested count) --
// for the format-mismatch contract.
wgpu::Texture BareTexture(wgpu::Device device, uint32_t size, uint32_t mips,
                          wgpu::TextureFormat format) {
  wgpu::TextureDescriptor d;
  d.size = {size, size, 1};
  d.format = format;
  d.usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopySrc |
            wgpu::TextureUsage::CopyDst;
  d.mipLevelCount = mips;
  d.sampleCount = 1;
  d.dimension = wgpu::TextureDimension::e2D;
  return device.CreateTexture(&d);
}

}  // namespace

TEST_CASE("PackTexturesIntoArray: layers keep identity, order, and mips") {
  TestGpu g = MakeGpu();
  GpuPipelineGenerator gen(g.device, FindShaderDirectory());

  constexpr uint32_t kSize = 64;  // -> 7 mips
  LoadedTexture red = SolidMipped(g, gen, kSize, 255, 0, 0);
  LoadedTexture blue = SolidMipped(g, gen, kSize, 0, 0, 255);
  REQUIRE(red.texture);
  REQUIRE(blue.texture);
  const uint32_t src_mips = red.texture.GetMipLevelCount();
  REQUIRE(src_mips > 1);  // the copy would be trivial otherwise

  LoadedTexture arr =
      PackTexturesIntoArray(g.device, g.queue, {red.texture, blue.texture});
  REQUIRE(arr.texture);
  REQUIRE(arr.view);

  // Shape: 2 layers, source size, and the full source mip chain carried over
  // (the whole point -- no array-aware mip generation exists).
  CHECK(arr.texture.GetDepthOrArrayLayers() == 2);
  CHECK(arr.texture.GetWidth() == kSize);
  CHECK(arr.texture.GetHeight() == kSize);
  CHECK(arr.texture.GetMipLevelCount() == src_mips);

  // Identity + ordering: layer 0 is red, layer 1 is blue (not swapped, not
  // both the same source).
  CpuImage l0, l1;
  TextureReadback readback(g.instance, g.device, g.queue);
  REQUIRE(readback.ReadTextureMip(arr.texture, /*mip=*/0, /*layer=*/0)
              .AwaitInto(l0));
  REQUIRE(readback.ReadTextureMip(arr.texture, /*mip=*/0, /*layer=*/1)
              .AwaitInto(l1));
  const CpuImage::Color c0 = l0.GetPixel(kSize / 2, kSize / 2);
  const CpuImage::Color c1 = l1.GetPixel(kSize / 2, kSize / 2);
  INFO("layer0 = " << int(c0.r) << "," << int(c0.g) << "," << int(c0.b)
                   << "  layer1 = " << int(c1.r) << "," << int(c1.g) << ","
                   << int(c1.b));
  CHECK(c0.r > 200);
  CHECK(c0.b < 55);
  CHECK(c1.b > 200);
  CHECK(c1.r < 55);
}

TEST_CASE("PackTexturesIntoArray: a null layer is rejected, not dereferenced") {
  // Regression: the null guard used to sit AFTER layers[0].GetWidth(), so a
  // null first layer segfaulted before its own check could fire. A poisoned
  // cache entry (a pack whose load failed) reaches here as exactly this.
  TestGpu g = MakeGpu();
  GpuPipelineGenerator gen(g.device, FindShaderDirectory());
  LoadedTexture valid = SolidMipped(g, gen, 64, 0, 255, 0);
  REQUIRE(valid.texture);

  // Null FIRST is the case that used to crash.
  LoadedTexture a =
      PackTexturesIntoArray(g.device, g.queue, {wgpu::Texture{}, valid.texture});
  CHECK_FALSE(a.texture);
  CHECK_FALSE(a.view);

  // Null in a later slot was always caught; keep it pinned.
  LoadedTexture b =
      PackTexturesIntoArray(g.device, g.queue, {valid.texture, wgpu::Texture{}});
  CHECK_FALSE(b.texture);
  CHECK_FALSE(b.view);

  // Empty input.
  LoadedTexture c = PackTexturesIntoArray(g.device, g.queue, {});
  CHECK_FALSE(c.view);
}

TEST_CASE("PackTexturesIntoArray: mismatched layers are rejected") {
  TestGpu g = MakeGpu();
  GpuPipelineGenerator gen(g.device, FindShaderDirectory());
  LoadedTexture big = SolidMipped(g, gen, 64, 255, 0, 0);
  LoadedTexture small = SolidMipped(g, gen, 32, 0, 0, 255);
  REQUIRE(big.texture);
  REQUIRE(small.texture);

  // Size mismatch: a texture array has one size for every layer.
  LoadedTexture size_mismatch =
      PackTexturesIntoArray(g.device, g.queue, {big.texture, small.texture});
  CHECK_FALSE(size_mismatch.view);

  // Format mismatch: CopyTextureToTexture requires matching formats, so this
  // must be caught by us up front. Before validation existed this returned a
  // non-null view and let Dawn raise an error on the invalid copy.
  wgpu::Texture rgba = BareTexture(g.device, 64, 1, wgpu::TextureFormat::RGBA8Unorm);
  wgpu::Texture bgra = BareTexture(g.device, 64, 1, wgpu::TextureFormat::BGRA8Unorm);
  REQUIRE(rgba);
  REQUIRE(bgra);
  LoadedTexture format_mismatch =
      PackTexturesIntoArray(g.device, g.queue, {rgba, bgra});
  CHECK_FALSE(format_mismatch.texture);
  CHECK_FALSE(format_mismatch.view);
}
