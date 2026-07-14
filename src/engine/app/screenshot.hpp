#pragma once

// Task S2.A1: offscreen screenshot readback, extracted from Stage 1's
// src/main.cpp (RenderScreenshot) and generalized to render an AppView
// instead of a fixed scene/camera/registry tuple.

#include <cstdint>
#include <string>

namespace badlands {

class GpuContext;
class GpuPipelineGenerator;
class AppView;

// Renders `view` once into an offscreen RGBA8Unorm target (instead of the
// SDL surface) sized `width`x`height`, reads it back, and writes it to
// `path` as a PNG via the `assets` crate. Syncs `view`'s scene (via
// `view.Update(0, ...)`) immediately before rendering. Blocks until the GPU
// work + readback complete. Returns false (after logging) on failure.
bool SaveScreenshot(GpuContext& gpu, GpuPipelineGenerator& pipeline_gen,
                    AppView& view, uint32_t width, uint32_t height,
                    const std::string& path);

}  // namespace badlands
