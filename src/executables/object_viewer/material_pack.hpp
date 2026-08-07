#pragma once

// A PBR material pack, loaded onto the RHI.
//
// WHY NOT MaterialLibrary. src/engine/rendering/material_library.hpp takes a
// wgpu::Device and produces a Dawn bind group; there is no seam in it that
// could serve the RHI. What IS reused is everything below the GPU: the
// material.json manifest format, the pack layout on disk, and
// badlands_decode_image from the assets crate.
//
// KEPT LOCAL TO object_viewer because it has exactly one consumer. The
// promotion point is the second one -- moving it into src/engine/ now would be
// generality bought on speculation.
//
// MIPS ARE GENERATED ON THE CPU. The RHI has no blit and no generate-mips, and
// a plane at a grazing angle without a mip chain aliases into noise -- which
// would read as a broken resolve rather than as a missing filter.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "engine/rhi/rhi_device.hpp"

namespace badlands::object_viewer {

// One decoded texture, still on the CPU, with its whole mip chain. Kept around
// after upload because the headless assertions sample it as their ORACLE:
// "the albedo view shows the source texel at this UV" is only a claim if the
// source texel is available to compare against.
struct CpuTexture {
  uint32_t width = 0, height = 0;
  // mips[0] is the full-resolution level; each is RGBA8, tightly packed.
  std::vector<std::vector<uint8_t>> mips;

  bool Valid() const { return width > 0 && height > 0 && !mips.empty(); }

  // Bilinear sample of `mip` at wrapped UV, returned as 0..1 per channel.
  // Mirrors what a Repeat sampler with linear filtering does, so an oracle can
  // predict a GPU fetch rather than approximate it.
  void SampleBilinear(float u, float v, uint32_t mip, float out_rgba[4]) const;
};

// A pack: the four maps a material.json can name.
//
// `displacement` may be absent -- not every pack ships one -- and that is the
// one field allowed to be. The other three are required, because a material
// without albedo, normals or ARM is not a material and substituting a default
// would hide the missing file (rule 2).
struct MaterialPack {
  CpuTexture albedo, normal, arm, displacement;
  rhi::TexturePtr gpu_albedo, gpu_normal, gpu_arm, gpu_displacement;
  rhi::SamplerPtr sampler;
  std::string dir;

  bool HasDisplacement() const { return displacement.Valid(); }
};

// Loads `<dir>/material.json` and everything it names, generates mips, and
// uploads. Returns null after logging on any failure -- a missing map is a
// refusal, not a default.
//
// ALBEDO IS UPLOADED AS RGBA8UnormSrgb so the hardware decodes it on fetch and
// the shader consumes linear; the other three are linear DATA and are uploaded
// as RGBA8Unorm. Getting that backwards is the classic mistake and it looks
// like a lighting choice.
std::unique_ptr<MaterialPack> LoadMaterialPack(rhi::IRhiDevice& device,
                                               const std::string& dir);

// Box-filters `src` (RGBA8, w x h) down to the next mip level. Exposed for its
// own test: a mip chain that halves incorrectly is invisible until something is
// viewed at a distance.
std::vector<uint8_t> DownsampleBox(const std::vector<uint8_t>& src,
                                   uint32_t w, uint32_t h);

// --- Synthetic packs, for assertions ---------------------------------------
//
// THE HEADLESS ORACLES USE THESE, NOT A SHIPPED PACK. Two reasons, and the
// second is the one that matters:
//
//   * A test that asserts on assets/materials/<something> is asserting on a
//     data file, which makes re-exporting a texture a test failure.
//   * CONSTANT textures make the mip level irrelevant. Every level holds the
//     same value, so "the roughness view shows 0.35" is exact no matter which
//     mip the GPU picked -- which takes mip prediction out of nine assertions
//     and leaves it to the one test that is actually about it.
struct TestPackValues {
  // sRGB-encoded, because that is how an albedo map is authored.
  uint8_t albedo[3] = {128, 64, 192};
  uint8_t ao = 153;           // ARM red
  uint8_t roughness = 89;     // ARM green
  uint8_t metallic = 204;     // ARM blue
  uint8_t displacement = 107;
};

// A pack of uniform textures with exactly those values, uploaded like any
// other. `size` is square and must be a power of two so the mip chain is exact.
std::unique_ptr<MaterialPack> MakeConstantPack(rhi::IRhiDevice& device,
                                               const TestPackValues& values,
                                               uint32_t size = 64);

// A pack whose albedo is a 1-texel checkerboard and whose other maps are
// constant. Its point is the OPPOSITE of the constant pack's: mip level changes
// the result, from full contrast at mip 0 to flat grey at the top, so a resolve
// that ignored the analytic gradients and sampled mip 0 everywhere is
// distinguishable from one that did not.
std::unique_ptr<MaterialPack> MakeCheckerPack(rhi::IRhiDevice& device,
                                              uint32_t size = 64);

}  // namespace badlands::object_viewer
