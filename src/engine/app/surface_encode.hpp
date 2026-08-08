#pragma once

// Turning a backbuffer readback into the bytes a PNG wants.
//
// EXISTS BECAUSE THE LAYER'S SCREENSHOT ASSUMED AN 8-BIT SURFACE. AppShell
// upgrades the swapchain to RGBA16Float on an HDR display -- that is the whole
// point of the extended-range path -- and the writer fed those half-float bytes
// straight into an RGBA8 encoder, producing a PNG of raw bit patterns covering
// the top-left quarter of the image, at the full claimed dimensions, with no
// warning and an exit code of 0.
//
// A pure function over bytes, so the formats can be checked without a GPU, a
// window, or an HDR display -- which is to say on CI.

#include <cstdint>
#include <span>
#include <vector>

#include "engine/rhi/rhi_types.hpp"

namespace badlands::rhi_app {

// Converts one readback's bytes to tightly packed 8-bit RGBA.
//
// Returns false, after logging the format by name, for anything it cannot
// encode -- rule 3, and the alternative is the garbage PNG this replaces.
//
// EXTENDED-RANGE VALUES SATURATE. A float surface holds linear values above 1
// and 8-bit PNG has nowhere to put them, so the encode clamps and then applies
// the sRGB transfer function. That is a lossy preview by construction, and it
// is what the headless path already documented; `clipped` reports whether any
// channel actually exceeded the range so a caller can say so rather than
// pretending the file is the frame.
bool EncodeSurfaceToRgba8(std::span<const uint8_t> src, rhi::Format format,
                          uint32_t width, uint32_t height,
                          std::vector<uint8_t>& out, bool* clipped = nullptr);

}  // namespace badlands::rhi_app
