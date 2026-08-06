#pragma once

// The Metal backend. Implementation is Objective-C++ (metal_rhi.mm), compiled
// with ARC; nothing Objective-C leaks through this header.

#include <cstdint>
#include <memory>
#include <string>

#include "engine/rhi/rhi_device.hpp"

namespace badlands::rhi::metal {

// Returns null (after logging) if no Metal device is available.
std::unique_ptr<IRhiDevice> CreateMetalDevice(const std::string& label = {},
                                              uint32_t frames_in_flight = 3);

// TEST HOOK. Creates a buffer inside a frame, Destroy()s it, and reports
// whether its MTLBuffer was (a) still alive while the frame was in flight and
// (b) genuinely released once the frame retired. Returns false, after logging
// which half failed, otherwise.
//
// Lives here because the check needs a `__weak` reference to an Objective-C
// object, and nothing Objective-C may cross this header. It exists at all
// because ASan cannot see this class of bug: an object whose retain count
// never reaches zero is not freed-then-used, it is simply never freed, and
// every test still passes. Deferring a handle and stranding it look identical
// from the outside.
bool WeakHandleClearedAfterRetire(IRhiDevice& device);

}  // namespace badlands::rhi::metal
