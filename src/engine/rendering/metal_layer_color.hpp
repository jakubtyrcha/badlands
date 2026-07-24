#pragma once

// CAMetalLayer color-space tagging (macOS only). The pinned Dawn rejects the
// standard wgpu::SurfaceColorManagement chained struct outright
// (dawn/native/Surface.cpp: "SurfaceColorManagement unsupported."), and its
// Metal swapchain (SwapChainMTL.mm) never touches the layer's colorspace or
// EDR flag — so the app, which created the CAMetalLayer and handed it to Dawn
// (GpuContext::CreateSurface), owns these properties and sets them directly.
//
// Defined in metal_layer_color.mm (ObjC++), compiled only on APPLE — call
// sites must be guarded with SDL_PLATFORM_MACOS; there is no stub elsewhere.

namespace badlands {

// Tags `ca_metal_layer` (a CAMetalLayer*) for Display-P3 output:
//   hdr_extended=true  -> extended linear Display P3 + EDR content flag
//                         (float surface; values >1.0 reach the compositor)
//   hdr_extended=false -> Display P3 (sRGB-curve-encoded, 8-bit SDR surface)
// Returns false if the layer or color space could not be set.
bool ConfigureMetalLayerColorSpace(void* ca_metal_layer, bool hdr_extended);

}  // namespace badlands
