#pragma once

// Task S2.A1: offscreen screenshot readback, extracted from Stage 1's
// src/main.cpp (RenderScreenshot) and generalized to render an AppView
// instead of a fixed scene/camera/registry tuple.

#include <cstdint>
#include <string>

#include <dawn/webgpu_cpp.h>

namespace badlands {

class GpuContext;
class GpuPipelineGenerator;
class AppView;
enum class GBufferDebugMode;
enum class ShadowDebugMode : uint32_t;

// Renders `view` once into an offscreen RGBA8Unorm target (instead of the
// SDL surface) sized `width`x`height`, reads it back, and writes it to
// `path` as a PNG via the `assets` crate. Syncs `view`'s scene (via
// `view.Update(0, ...)`) immediately before rendering, with the offscreen
// renderer's G-buffer debug mode set to `debug_mode` and its directional-
// shadow debug mode set to `shadow_debug_mode` (Task T3) — both forwarded so
// a capture honors the live debug visualization. Builds a throwaway
// SceneRenderer (one-shot use); the F2 recorder uses ScreenshotRecorder's
// persistent renderer instead. Blocks until the GPU work + readback complete.
// Returns false (after logging) on failure.
bool SaveScreenshot(GpuContext& gpu, GpuPipelineGenerator& pipeline_gen,
                    AppView& view, uint32_t width, uint32_t height,
                    const std::string& path, GBufferDebugMode debug_mode,
                    ShadowDebugMode shadow_debug_mode);

// Creates the offscreen capture target: a `width`x`height` RGBA8Unorm texture
// with RenderAttachment|CopySrc usage. RGBA8Unorm (non-sRGB) matches the
// window surface's BGRA8Unorm so the tonemap shader's manual linear->srgb
// encode isn't applied a second time by the hardware on store.
wgpu::Texture CreateScreenshotTarget(wgpu::Device device, uint32_t width,
                                     uint32_t height);

// Reads an already-rendered RGBA8Unorm CopySrc `texture` (size `width`x
// `height`) back to CPU and writes it to `path` as a PNG via the `assets`
// crate. Blocks until the copy + buffer map complete. Shared by
// SaveScreenshot and ScreenshotRecorder. Returns false (after logging) on
// failure.
bool WriteTextureToPng(wgpu::Device device, wgpu::Queue queue,
                       const wgpu::Texture& texture, uint32_t width,
                       uint32_t height, const std::string& path);

}  // namespace badlands
