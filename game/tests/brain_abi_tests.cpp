// Pins the brain wire format (game/src/brain_abi.h): the BL_MAX_ACTIVITIES /
// ActivityId::Count relationship the header itself cannot check (it does not
// include badlands_sim.hpp -- see the header's top comment), the documented
// total size of the per-tick view buffer, and offsetof for a handful of
// sentinel fields whose alignment is load-bearing (see the header's
// "LAYOUT RULES" comment for why each pad byte exists). The header's own
// static_asserts catch every OTHER struct size at compile time already; this
// file exists for the one check that needs a C++ enum from outside the
// header, plus a belt-and-suspenders offsetof check on the trickiest struct
// (BlViewWire, whose members interleave int64_t-bearing sub-structs with
// explicit padding).

#include "brain_abi.h"

#include "badlands_sim.hpp"  // badlands::ActivityId

#include <catch_amalgamated.hpp>

#include <cstddef>

using namespace badlands;

TEST_CASE("BL_MAX_ACTIVITIES matches ActivityId::Count", "[brain_abi]") {
    REQUIRE(BL_MAX_ACTIVITIES == static_cast<int32_t>(ActivityId::Count));
}

TEST_CASE("BlViewWire is the documented size", "[brain_abi]") {
    REQUIRE(sizeof(BlViewWire) == 1080);
}

TEST_CASE("BlViewWire sentinel field offsets", "[brain_abi]") {
    // version(4) + _pad(4) precede `self`, which starts with an int64_t and so
    // must land on an 8-byte boundary.
    REQUIRE(offsetof(BlViewWire, self) == 8);
    // sizeof(BlViewSelf) == 72 (already a multiple of 8), so `suggest` starts
    // right after with no gap.
    REQUIRE(offsetof(BlViewWire, suggest) == 80);
    // sizeof(BlViewSuggest) == 248 (a multiple of 8 only because of its own
    // explicit trailing pad before `threats`), so `factors` -- which starts
    // with an int64_t -- lands 8-aligned with no compiler-inserted gap.
    REQUIRE(offsetof(BlViewWire, factors) == 328);
    // sizeof(BlViewFactors) == 104 (a multiple of 8 only because of its own
    // explicit trailing pad).
    REQUIRE(offsetof(BlViewWire, char_count) == 432);
    // char_count(4) + _pad2(4) keep `chars` (BlViewChar starts with an
    // int64_t) 8-aligned.
    REQUIRE(offsetof(BlViewWire, chars) == 440);
}

TEST_CASE("BlViewSuggest / BlViewFactors internal padding lands where documented",
          "[brain_abi]") {
    // threat_count(4) + the explicit _pad(4) precede the BlThreat array.
    REQUIRE(offsetof(BlViewSuggest, threats) == 120);
    // think_min_millis(8) + think_max_millis(8) precede the weights array.
    REQUIRE(offsetof(BlViewFactors, weights) == 16);
    // weights[14] (56 bytes) follow immediately after.
    REQUIRE(offsetof(BlViewFactors, fatigue_seek) == 72);
}
