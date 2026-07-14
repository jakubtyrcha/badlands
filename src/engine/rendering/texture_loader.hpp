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

#include <dawn/webgpu_cpp.h>

#include "engine/rendering/shader/gpu_pipeline_generator.hpp"

namespace badlands {

// A loaded 2D texture with a full GPU-generated mip chain.
struct LoadedTexture {
  wgpu::Texture texture;  // owns the GPU texture
  wgpu::TextureView view; // 2D view over all mip levels
};

// Decodes the JPEG at `path` (via the `assets` Rust crate), uploads it as
// mip level 0 of an RGBA8Unorm Dawn texture sized to a full mip chain, then
// GPU-generates all lower mips via a render-path box downsample (see
// shaders/compute/mip_generator_render.wesl), and returns a view over all
// mip levels.
//
// `device`/`queue`/`pipeline_gen` must be the app's existing instances (this
// function does not create its own GpuPipelineGenerator or Dawn device).
// Returns a default-constructed LoadedTexture (null members) on failure
// (decode error, pipeline compile failure) after logging.
LoadedTexture LoadTexture2D(wgpu::Device device, wgpu::Queue queue,
                            GpuPipelineGenerator& pipeline_gen,
                            const std::string& path);

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

}  // namespace badlands
