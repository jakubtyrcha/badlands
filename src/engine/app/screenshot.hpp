#pragma once

// Task S2.A1: offscreen screenshot readback, extracted from Stage 1's
// src/main.cpp (RenderScreenshot) and generalized to render an AppView
// instead of a fixed scene/camera/registry tuple.

#include <cstdint>
#include <string>

#include <dawn/webgpu_cpp.h>

#include "engine/rendering/scene_renderer.hpp"  // ShadowDebugMode (Off default)

namespace badlands {

class GpuContext;
class GpuPipelineGenerator;
class AppView;
enum class GBufferDebugMode;

// Renders `view` once into an offscreen RGBA8Unorm target (instead of the
// SDL surface) sized `width`x`height`, reads it back, and writes it to
// `path` as a PNG via the `assets` crate. Syncs `view`'s scene (via
// `view.Update(0, ...)`) immediately before rendering, with the offscreen
// renderer's G-buffer debug mode set to `debug_mode` and its directional-
// shadow debug mode set to `shadow_debug_mode` (Task T3, default Off so
// non-shadow call sites need no change) — both forwarded so a capture honors
// the live debug visualization. `time_of_day` (t01 in [0,1)) is applied via
// view.SeekToTimeOfDay before the render so a headless capture picks a
// deterministic time in a day/night cycle (ignored by views without one).
// Builds a throwaway SceneRenderer (one-shot use); the F2 recorder uses
// ScreenshotRecorder's persistent renderer instead. Blocks until the GPU work
// + readback complete. Returns false (after logging) on failure.
// `grading` forwards the live renderer's color-grading config into the
// throwaway capture renderer (same pattern as debug_mode /
// shadow_debug_mode) — grading is part of the game's image. Defaults to a
// default-constructed config (grading disabled).
bool SaveScreenshot(GpuContext& gpu, GpuPipelineGenerator& pipeline_gen,
                    AppView& view, uint32_t width, uint32_t height,
                    const std::string& path, GBufferDebugMode debug_mode,
                    ShadowDebugMode shadow_debug_mode = ShadowDebugMode::Off,
                    float time_of_day = 0.5f,
                    const ColorGradingConfig& grading = {});

// Creates the offscreen capture target: a `width`x`height` RGBA8Unorm texture
// with RenderAttachment|CopySrc usage. RGBA8Unorm (non-sRGB) matches the
// window surface's BGRA8Unorm so the tonemap shader's manual linear->srgb
// encode isn't applied a second time by the hardware on store.
wgpu::Texture CreateScreenshotTarget(wgpu::Device device, uint32_t width,
                                     uint32_t height);

// Reads an already-rendered RGBA8Unorm CopySrc `texture` (size `width`x
// `height`) back to CPU (via TextureReadback -> CpuImage) and writes it to
// `path` as a PNG via the `assets` crate. Blocks until the copy + buffer map
// complete. Shared by SaveScreenshot and ScreenshotRecorder. `instance` is
// used to pump Dawn's event loop while awaiting the readback. Returns false
// (after logging) on failure.
bool WriteTextureToPng(wgpu::Instance instance, wgpu::Device device,
                       wgpu::Queue queue, const wgpu::Texture& texture,
                       uint32_t width, uint32_t height,
                       const std::string& path);

}  // namespace badlands
