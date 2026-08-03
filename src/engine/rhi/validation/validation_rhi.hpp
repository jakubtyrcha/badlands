#pragma once

// The validation decorator: wraps any IRhiDevice and checks the calls made
// through it.
//
// This is the replacement for the Dawn validation the port gives up, and it is
// why the RHI is a virtual interface at all (D5) -- a decorator can sit in
// front of any backend without either side knowing.
//
// Two jobs, and the second is the one that pays for the first:
//
//   1. Ordinary misuse -- unbound slots, use after Destroy, commands after
//      End, attachment format/usage mismatches, draws with no pipeline.
//   2. RESOURCE-STATE INTENT. Metal tracks hazards itself, so a missing
//      transition still renders correctly and the Metal backend can never
//      reveal it. This layer checks the intent as bookkeeping over the command
//      stream, with no GPU involved, so the check runs in the fast Null suite
//      on any machine. DX12 may still arrive with barrier bugs; the point is
//      that the assertions pinning them down already exist by then.
//
// Violations accumulate rather than throw: probe C found all 14 of the
// engine's existing Dawn validation sites simply ask "did anything go wrong?",
// and accumulating lets one run surface every problem instead of the first.

#include <memory>
#include <string>

#include "engine/rhi/rhi_device.hpp"

namespace badlands::rhi::validation {

// Wraps `inner`, taking ownership. Returns `inner` unchanged if it is null.
std::unique_ptr<IRhiDevice> MakeValidationDevice(
    std::unique_ptr<IRhiDevice> inner);

}  // namespace badlands::rhi::validation
