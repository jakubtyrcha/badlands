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

}  // namespace badlands::rhi::metal
