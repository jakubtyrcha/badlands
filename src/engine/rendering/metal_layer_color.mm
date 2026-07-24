// See the header for why the app (not Dawn) owns the CAMetalLayer's
// colorspace/EDR properties on the pinned Dawn revision.
#include "engine/rendering/metal_layer_color.hpp"

#import <QuartzCore/CAMetalLayer.h>

#include <CoreGraphics/CGColorSpace.h>

namespace badlands {

bool ConfigureMetalLayerColorSpace(void* ca_metal_layer, bool hdr_extended) {
  auto* layer = static_cast<CAMetalLayer*>(ca_metal_layer);
  if (layer == nil) return false;

  CGColorSpaceRef color_space = CGColorSpaceCreateWithName(
      hdr_extended ? kCGColorSpaceExtendedLinearDisplayP3
                   : kCGColorSpaceDisplayP3);
  if (color_space == nullptr) return false;

  layer.colorspace = color_space;
  CGColorSpaceRelease(color_space);
  layer.wantsExtendedDynamicRangeContent = hdr_extended ? YES : NO;
  return true;
}

}  // namespace badlands
