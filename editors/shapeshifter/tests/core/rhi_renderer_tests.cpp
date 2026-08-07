// The editor's RHI renderer, on the Null backend wherever the property allows.
//
// CATCH2, not the doctest the rest of editors/shapeshifter uses. This target
// links badlands_rhi and the render graph, whose own suites are Catch2, and two
// frameworks in one binary is worse than two in one repo. The editor's pure-CPU
// core keeps its doctest suite next door, untouched.

#include <catch_amalgamated.hpp>

#include "engine/rhi/rhi_device.hpp"

using namespace badlands::rhi;

TEST_CASE("shapeshifter: the core can create an RHI device", "[ss-rhi]") {
  // The whole of task 1: proves shapeshifter_core links the RHI and that a
  // device can be made from inside the editor's own target, before anything is
  // built on top of it.
  auto device = CreateDevice({.backend = BackendKind::Null,
                              .enable_validation = true,
                              .label = "shapeshifter_tests"});
  REQUIRE(device != nullptr);
}
