#pragma once

// New (not a direct sampo port): sampo generates GPU mip chains through a
// general-purpose ProcessingGraph/ResizeNode/MipmapGenerationNode DAG
// (~1550 lines under sampo's src/image_processing/**, intentionally not
// ported — see scene_renderer.cpp's file-level comment). Task E1's plan
// authorizes trimming that machinery to exactly what mip generation needs:
// this file drives the per-level render-path box downsample directly
// against badlands' GpuPipelineGenerator (Task C2), using the two ported
// WESL kernels under shaders/compute/ (mip_generator_render.wesl is the
// render-path kernel this loader wires up; mip_generator.wesl is the
// compute-path kernel, copied verbatim but landed present-but-unused in
// Stage 1 — RGBA8Unorm is filterable+renderable, so only the render path
// runs, same as normalmapped.wesl being inert).
#include <cstdint>
#include <string>
#include <vector>

#include <dawn/webgpu_cpp.h>

#include "badlands_assets.h"  // BadlandsImage / badlands_image_free (ImageGuard)
#include "engine/rendering/shader/gpu_pipeline_generator.hpp"

namespace badlands {

// RAII guard: frees a BadlandsImage's malloc'd pixel buffer on every exit path
// (badlands_image_free is safe to call on a failure/all-NULL result too).
// Shared by texture_loader.cpp and material_library.cpp (which decode JPEGs
// through the same `assets` C ABI).
struct ImageGuard {
  BadlandsImage image;
  ~ImageGuard() { badlands_image_free(image); }
};

// A loaded 2D texture with a full GPU-generated mip chain.
struct LoadedTexture {
  wgpu::Texture texture;  // owns the GPU texture
  wgpu::TextureView view; // 2D view over all mip levels
};

// Creates a 1x1 solid-color RGBA8Unorm texture and returns a view over it
// (the returned view keeps the underlying single-mip texture alive). Used for
// procedural floor/capsule albedo + roughness slots that need no JPEG decode.
// No mip chain (1x1 has none), so any sampler's mipmapFilter is irrelevant.
wgpu::TextureView CreateSolidColorTexture(wgpu::Device device, wgpu::Queue queue,
                                          uint8_t r, uint8_t g, uint8_t b,
                                          uint8_t a = 255);

// Creates an N-layer 1x1 solid-color RGBA8Unorm texture ARRAY (one color per
// layer) and returns a 2D-array (e2DArray) view over all layers. `colors` is
// `layer_count * 4` bytes (one RGBA per layer). The returned view keeps the
// underlying texture alive. No mip chain (1x1). Used for terrain-blend layer
// arrays — e.g. the red/green/blue debug materials, or the biome palette.
// Modeled on standard_material_factory.cpp's CreateSolidColorCubemap1x1 (a
// cubemap is just a 6-layer array).
wgpu::TextureView CreateSolidColorArray(wgpu::Device device, wgpu::Queue queue,
                                        const uint8_t* colors,
                                        uint32_t layer_count);

// Decodes the JPEG/PNG at `path` (via the `assets` Rust crate), uploads it as
// mip level 0 of an RGBA8Unorm Dawn texture sized to a full mip chain, then
// GPU-generates all lower mips via a render-path box downsample (see
// shaders/compute/mip_generator_render.wesl), and returns a view over all
// mip levels.
//
// `flip_green_dx`: when true, the decoded RGBA buffer's green channel is
// CPU-flipped (`g = 255 - g`) before upload -- converts a DirectX-convention
// normal map (green = -Y) to OpenGL convention (green = +Y) so the mip chain
// is generated from, and every mip samples, already-GL-convention data.
// Irrelevant for non-normal textures (albedo, ARM); leave it false.
//
// `device`/`queue`/`pipeline_gen` must be the app's existing instances (this
// function does not create its own GpuPipelineGenerator or Dawn device).
// Returns a default-constructed LoadedTexture (null members) on failure
// (decode error, pipeline compile failure) after logging.
LoadedTexture LoadTexture2D(wgpu::Device device, wgpu::Queue queue,
                            GpuPipelineGenerator& pipeline_gen,
                            const std::string& path,
                            bool flip_green_dx = false);

// Uploads `width`x`height` tightly-packed RGBA8 pixels (`rgba`, already in
// memory -- no JPEG decode) as mip level 0 of a new RGBA8Unorm Dawn texture
// sized to a full mip chain, GPU-generates all lower mips (same render-path
// box downsample as LoadTexture2D), and returns a view over all mip levels.
//
// This is LoadTexture2D's upload+mipgen tail, factored out so callers that
// already have decoded/derived pixels in memory (e.g. MaterialLibrary's
// repacked-roughness texture, built by copying a decoded JPEG's G channel
// into R) can reuse it without a redundant decode. LoadTexture2D itself
// decodes then delegates here.
//
// Returns a default-constructed LoadedTexture (null members) on failure
// (texture creation or pipeline compile failure) after logging.
LoadedTexture UploadTexture2DWithMips(wgpu::Device device, wgpu::Queue queue,
                                      GpuPipelineGenerator& pipeline_gen,
                                      uint32_t width, uint32_t height,
                                      const uint8_t* rgba);

// Packs N already-mipped 2D textures into one RGBA8Unorm texture ARRAY (layer i
// = layers[i]) and returns an e2DArray view over all layers + mip levels.
//
// Every source mip is copied straight across (CopyTextureToTexture), so the mip
// chains the sources already carry are reused as-is -- this deliberately avoids
// needing array-aware mip generation (the render-path kernel
// shaders/compute/mip_generator_render.wesl hardcodes `out.layer = 0` and so
// cannot write layers > 0). Sources must therefore have been created with
// CopySrc -- UploadTexture2DWithMips does that.
//
// All layers must share identical width/height/mipLevelCount; a mismatch is an
// error (returns a null LoadedTexture after logging), because a texture array
// has one size for every layer. `layers` must be non-empty.
LoadedTexture PackTexturesIntoArray(wgpu::Device device, wgpu::Queue queue,
                                    const std::vector<wgpu::Texture>& layers);

}  // namespace badlands
