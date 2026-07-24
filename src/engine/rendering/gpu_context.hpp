#pragma once

#include <SDL3/SDL.h>
#if defined(SDL_PLATFORM_MACOS)
#include <SDL3/SDL_metal.h>
#endif

#include <cstdint>
#include <dawn/webgpu_cpp.h>

namespace badlands {

// Owns the WebGPU instance/adapter/device/queue/surface for a single SDL
// window.
//
// Ported from sampo's rendering/gpu_context.{hpp,cpp}, trimmed to the native
// (non-Emscripten) device/surface-init slice Stage 1 needs: sampo's pipeline
// generator, render-state-provider, frame-context and device-capability
// toggles (HDR surface, timestamp queries, shared texture memory, ...) are
// not ported here — they land with later Stage 1 tasks that actually
// consume them.
class GpuContext {
 public:
  GpuContext();
  ~GpuContext();

  // Non-copyable, movable.
  GpuContext(const GpuContext&) = delete;
  GpuContext& operator=(const GpuContext&) = delete;
  GpuContext(GpuContext&&) noexcept;
  GpuContext& operator=(GpuContext&&) noexcept;

  // Creates the WebGPU instance, an SDL-backed surface, and requests an
  // adapter/device compatible with it. Configures the surface at `window`'s
  // current pixel size. Returns false (after logging to stderr) on failure.
  bool Initialize(SDL_Window* window);
  void Shutdown();

  // Reconfigures the surface, e.g. in response to
  // SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED.
  void Configure(uint32_t width, uint32_t height);

  // Acquires the current surface texture's view for this frame. Returns a
  // null TextureView if acquisition failed (surface lost/out-of-date);
  // callers should skip the frame in that case.
  wgpu::TextureView AcquireSurfaceTexture();

  // Presents the surface; call once per frame after submitting the command
  // buffer that renders into the view returned by AcquireSurfaceTexture().
  void Present();

  wgpu::Instance GetInstance() const { return instance_; }
  wgpu::Adapter GetAdapter() const { return adapter_; }
  wgpu::Device GetDevice() const { return device_; }
  wgpu::Queue GetQueue() const { return queue_; }
  wgpu::Surface GetSurface() const { return surface_; }
  wgpu::TextureFormat GetSurfaceFormat() const { return surface_format_; }

  // Whether the device has the `TextureFormatsTier1` feature (Dawn's native
  // name for R8Unorm storage-texture support — the standalone
  // `R8UnormStorage` feature was subsumed into it). Requested opportunistically
  // in Initialize() when the adapter advertises it; never required (Stage 3
  // M6's GTAO compute pass gates its StorageBinding AO texture usage on this).
  bool HasR8UnormStorage() const { return has_r8unorm_storage_; }

  // True when the device enabled TimestampQuery (GPU per-pass timing via
  // GpuTimer). Enabled opportunistically in Initialize(); never required.
  bool HasTimestampQuery() const { return has_timestamp_query_; }

  // True when the surface's CAMetalLayer was successfully tagged for
  // Display-P3 output (linear-EDR P3 on HDR displays, sRGB-encoded P3 on
  // SDR). The renderer keys its P3 resolve mode (tonemap mode 2) on this.
  // False = untagged 8-bit layer = the pre-P3 sRGB behavior (Configure
  // guarantees a float surface never stays untagged — see
  // ResolveSurfaceFormatAfterTagging).
  bool IsP3() const { return output_is_p3_; }

  // Pure post-tagging surface-format decision (unit-tested in
  // resolve_composite_tests): an untagged (nil-colorspace) float CAMetalLayer
  // has no defined transfer — the resolve would emit linear values into a
  // layer nothing reasons about — so a float surface whose P3 tagging failed
  // falls back to BGRA8Unorm (the known-good untagged path). 8-bit formats
  // pass through regardless of tagging, which also terminates Configure's
  // single fallback retry.
  static wgpu::TextureFormat ResolveSurfaceFormatAfterTagging(
      wgpu::TextureFormat chosen, bool tag_ok) {
    if (chosen == wgpu::TextureFormat::RGBA16Float && !tag_ok) {
      return wgpu::TextureFormat::BGRA8Unorm;
    }
    return chosen;
  }

 private:
  static wgpu::Surface CreateSurface(wgpu::Instance instance,
                                     SDL_Window* window,
                                     void** out_metal_layer);
  static wgpu::Adapter RequestAdapter(wgpu::Instance instance,
                                      wgpu::RequestAdapterOptions const* opts);
  static wgpu::Device RequestDevice(wgpu::Instance instance,
                                    wgpu::Adapter adapter,
                                    wgpu::DeviceDescriptor const* desc);
  wgpu::TextureFormat GetPreferredFormat();

  wgpu::Instance instance_;
  wgpu::Adapter adapter_;
  wgpu::Device device_;
  wgpu::Queue queue_;
  wgpu::Surface surface_;
  wgpu::TextureFormat surface_format_ = wgpu::TextureFormat::BGRA8Unorm;
  wgpu::PresentMode present_mode_ = wgpu::PresentMode::Fifo;
  bool has_r8unorm_storage_ = false;
  bool has_timestamp_query_ = false;
  // The CAMetalLayer backing the surface (created in CreateSurface, macOS
  // only) — the app tags its colorspace/EDR properties directly because the
  // pinned Dawn rejects SurfaceColorManagement (see metal_layer_color.hpp).
  void* metal_layer_ = nullptr;
  bool is_hdr_ = false;        // display reported HDR at startup
  bool output_is_p3_ = false;  // layer successfully tagged Display-P3
};

/// Flag set by the uncaptured WebGPU error callback. Callers may poll and
/// reset this to detect Dawn validation errors during development.
extern bool g_gpu_error_flag;

}  // namespace badlands
