// See the header for why this is a port of src/engine/rendering/metal_layer_color.mm
// rather than a shared dependency on it.
#include "engine/rhi/metal/metal_color_space.hpp"

#import <QuartzCore/CAMetalLayer.h>

#include <CoreGraphics/CGColorSpace.h>

namespace badlands::rhi::metal {

bool ConfigureLayerColorSpace(void* ca_metal_layer, bool extended_linear) {
  // __bridge, not static_cast: this file is ARC (the whole Metal backend is),
  // and the cast must be the non-transferring one -- the caller still owns the
  // layer. The Dawn copy of this uses a plain static_cast only because
  // badlands_engine is MRR.
  CAMetalLayer* layer = (__bridge CAMetalLayer*)ca_metal_layer;
  if (layer == nil) return false;

  CGColorSpaceRef color_space = CGColorSpaceCreateWithName(
      extended_linear ? kCGColorSpaceExtendedLinearDisplayP3
                      : kCGColorSpaceDisplayP3);
  if (color_space == nullptr) return false;

  layer.colorspace = color_space;
  CGColorSpaceRelease(color_space);
  // Both directions matter. Leaving this YES on an 8-bit surface asks the
  // compositor for headroom the format cannot carry, so it is set from the
  // flag rather than only ever raised.
  layer.wantsExtendedDynamicRangeContent = extended_linear ? YES : NO;
  return true;
}

}  // namespace badlands::rhi::metal
