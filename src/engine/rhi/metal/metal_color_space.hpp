#pragma once

// Tagging a CAMetalLayer with the colour space it presents in.
//
// PORTED, NOT SHARED. src/engine/rendering/metal_layer_color.{hpp,mm} does the
// same thing for the Dawn path and STAYS THERE -- badlands_game still uses it,
// and badlands_rhi deliberately does not link badlands_engine. Two copies of
// fourteen lines is the cheaper of the two wrongs here; the alternative is the
// RHI depending on the renderer it exists to replace.
//
// WHY THE APP OWNS THIS AT ALL. The layer's colorspace and its EDR flag are
// presentation properties of the surface, not of the device or the queue, and
// nothing in the Metal API sets them as a side effect of anything else. If no
// one tags the layer it stays nil-colorspace, which is a defined state for
// 8-bit content and an undefined one for float.

namespace badlands::rhi::metal {

// Tags `ca_metal_layer` (a CAMetalLayer*) and returns whether it took.
//
// `extended_linear` picks between the two P3 variants: extended-linear P3 with
// EDR enabled (values above 1.0 become headroom rather than clipping at SDR
// white), or plain Display P3 with the sRGB transfer curve.
//
// Returns false, without logging, if the layer is nil or CoreGraphics declines
// to make the colour space -- the caller decides what a failure means, because
// on an 8-bit surface it is a cosmetic gamut miss and on a float one it is a
// reason to abandon the format entirely.
bool ConfigureLayerColorSpace(void* ca_metal_layer, bool extended_linear);

}  // namespace badlands::rhi::metal
