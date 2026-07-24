// Pins the brain wire format v2 (game/src/brain_abi.h): the BL_MAX_ACTIVITIES
// / ActivityId::Count relationship the header itself cannot check (it does
// not include badlands_sim.hpp -- see the header's top comment), the
// documented total size of the per-tick view/suggestion buffers, and
// offsetof for a handful of sentinel fields whose alignment is load-bearing
// (see the header's "LAYOUT RULES" comment for why each pad byte exists). The
// header's own static_asserts catch every OTHER struct size at compile time
// already; this file exists for the one check that needs a C++ enum from
// outside the header, plus a belt-and-suspenders offsetof check on the
// trickiest structs (BlViewWire/BlViewFactors, whose members interleave
// int64_t-bearing sub-structs/arrays with explicit padding).

#include "brain_abi.h"

#include "badlands_sim.hpp"  // badlands::ActivityId

#include <catch_amalgamated.hpp>

#include <cstddef>

using namespace badlands;

TEST_CASE("BL_MAX_ACTIVITIES matches ActivityId::Count", "[brain_abi]") {
    REQUIRE(BL_MAX_ACTIVITIES == static_cast<int32_t>(ActivityId::Count));
}

TEST_CASE("BL_ABI_VERSION is 2", "[brain_abi]") { REQUIRE(BL_ABI_VERSION == 2); }

TEST_CASE("BlViewWire is the documented size", "[brain_abi]") {
    REQUIRE(sizeof(BlViewWire) == 1480);
}

TEST_CASE("BlSuggestionWire is the documented size", "[brain_abi]") {
    REQUIRE(sizeof(BlSuggestionWire) == 40);
}

TEST_CASE("BlViewWire block order: self / suggest / factors / statuses / events / chars",
          "[brain_abi]") {
    // version(4) + _pad(4) precede `self`, which starts with an int64_t and so
    // must land on an 8-byte boundary.
    REQUIRE(offsetof(BlViewWire, self) == 8);
    // sizeof(BlViewSelf) == 88 (a multiple of 8 via its own explicit pad), so
    // `suggest` starts right after with no gap.
    REQUIRE(offsetof(BlViewWire, suggest) == 96);
    // sizeof(BlViewSuggest) == 248 (a multiple of 8 via its own explicit
    // trailing pad), so `factors` lands with no compiler-inserted gap.
    REQUIRE(offsetof(BlViewWire, factors) == 344);
    // sizeof(BlViewFactors) == 88 (a multiple of 8 via its own explicit pad,
    // even though BlViewFactors itself has no int64_t member -- see the
    // header's own comment on this one).
    REQUIRE(offsetof(BlViewWire, status_count) == 432);
    // status_count(4) + _pad2(4) keep `statuses` (BlStatus starts with an
    // int64_t) 8-aligned.
    REQUIRE(offsetof(BlViewWire, statuses) == 440);
    // statuses[8] * 16B = 128B.
    REQUIRE(offsetof(BlViewWire, event_count) == 568);
    // event_count(4) + _pad3(4) keep `events` (BlEvent starts with an
    // int64_t) 8-aligned.
    REQUIRE(offsetof(BlViewWire, events) == 576);
    // events[8] * 32B = 256B.
    REQUIRE(offsetof(BlViewWire, char_count) == 832);
    // char_count(4) + _pad4(4) keep `chars` (BlViewChar starts with an
    // int64_t) 8-aligned.
    REQUIRE(offsetof(BlViewWire, chars) == 840);
}

TEST_CASE("BlViewSuggest / BlViewFactors internal padding lands where documented",
          "[brain_abi]") {
    // threat_count(4) + the explicit _pad(4) precede the BlThreat array.
    REQUIRE(offsetof(BlViewSuggest, threats) == 120);
    // weights[] leads BlViewFactors now (think_min/max_millis are gone).
    REQUIRE(offsetof(BlViewFactors, weights) == 0);
    // weights[14] (56 bytes) precede the scalar tail.
    REQUIRE(offsetof(BlViewFactors, fatigue_seek) == 56);
}

TEST_CASE("BlViewSelf's current-intention summary is grouped per the layout rules",
          "[brain_abi]") {
    // The four int64_t fields (world_millis, think_until_millis, roam_epoch,
    // intention_wake_at) are grouped first; intention_kind joins the other
    // 4-byte fields, with _pad2 rounding the struct to a multiple of 8.
    REQUIRE(offsetof(BlViewSelf, intention_wake_at) == 24);
    REQUIRE(offsetof(BlViewSelf, intention_kind) == 80);
    REQUIRE(sizeof(BlViewSelf) == 88);
}
