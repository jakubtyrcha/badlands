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

TEST_CASE("BL_ABI_VERSION is 5", "[brain_abi]") { REQUIRE(BL_ABI_VERSION == 5); }

TEST_CASE("BL_MAX_ATTACKS matches badlands::kMaxAttacks", "[brain_abi]") {
    REQUIRE(BL_MAX_ATTACKS == kMaxAttacks);
}

TEST_CASE("BL_MAX_SKILLS matches badlands::kMaxSkills", "[brain_abi]") {
    REQUIRE(BL_MAX_SKILLS == kMaxSkills);
}

TEST_CASE("BlViewSkill is the documented size", "[brain_abi]") {
    REQUIRE(sizeof(BlViewSkill) == 24);
}

TEST_CASE("BlViewWire is the documented size", "[brain_abi]") {
    REQUIRE(sizeof(BlViewWire) == 1888);
}

TEST_CASE("BlViewAttack is the documented size", "[brain_abi]") {
    REQUIRE(sizeof(BlViewAttack) == 24);
}

TEST_CASE("BlSuggestionWire is the documented size", "[brain_abi]") {
    REQUIRE(sizeof(BlSuggestionWire) == 40);
}

TEST_CASE("BlViewWire block order: self / suggest / factors / statuses / attacks / skills / "
          "events / chars",
          "[brain_abi]") {
    // version(4) + _pad(4) precede `self`, which starts with an int64_t and so
    // must land on an 8-byte boundary.
    REQUIRE(offsetof(BlViewWire, self) == 8);
    // sizeof(BlViewSelf) == 88 (a multiple of 8 via its own explicit pad), so
    // `suggest` starts right after with no gap.
    REQUIRE(offsetof(BlViewWire, suggest) == 96);
    // sizeof(BlViewSuggest) == 376 (a multiple of 8 via its own explicit
    // trailing pad), so `factors` lands with no compiler-inserted gap.
    REQUIRE(offsetof(BlViewWire, factors) == 472);
    // sizeof(BlViewFactors) == 88 (a multiple of 8 via its own explicit pad,
    // even though BlViewFactors itself has no int64_t member -- see the
    // header's own comment on this one).
    REQUIRE(offsetof(BlViewWire, status_count) == 560);
    // status_count(4) + _pad2(4) keep `statuses` (BlStatus starts with an
    // int64_t) 8-aligned.
    REQUIRE(offsetof(BlViewWire, statuses) == 568);
    // statuses[8] * 16B = 128B.
    REQUIRE(offsetof(BlViewWire, attack_count) == 696);
    // attack_count(4) + _pad3(4) precede the BlViewAttack array (v3).
    REQUIRE(offsetof(BlViewWire, attacks) == 704);
    // attacks[3] * 24B = 72B, then the v4 skills block.
    REQUIRE(offsetof(BlViewWire, skill_count) == 776);
    // skill_count(4) + _pad6(4) precede the BlViewSkill array.
    REQUIRE(offsetof(BlViewWire, skills) == 784);
    // skills[8] * 24B = 192B.
    REQUIRE(offsetof(BlViewWire, event_count) == 976);
    // event_count(4) + _pad4(4) keep `events` (BlEvent starts with an
    // int64_t) 8-aligned.
    REQUIRE(offsetof(BlViewWire, events) == 984);
    // events[8] * 32B = 256B.
    REQUIRE(offsetof(BlViewWire, char_count) == 1240);
    // char_count(4) + _pad5(4) keep `chars` (BlViewChar starts with an
    // int64_t) 8-aligned.
    REQUIRE(offsetof(BlViewWire, chars) == 1248);
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
