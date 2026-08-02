// Inspection-facing names for the two enums a debug panel has to render:
// CommandKindId (the command log's rows) and BuildingKind.
//
// Both are DATA about identity, and both live here rather than in a consumer's
// switch for one reason: a switch with a `default` returns "?" for every value
// added after it was written, and nothing fails until somebody notices the "?"
// on screen. A dense table plus a static_assert turns that into a compile
// error. This is the argument activity_catalog.cpp already makes for
// ActivityName; the command log had the other half of it.
//
// Like activity_catalog.cpp, this TU depends on nothing but badlands_sim.hpp,
// so a lean consumer can link the naming tables without pulling in the registry
// or the map.

#include "badlands_sim.hpp"

#include <array>

namespace badlands {

namespace {

// Indexed by CommandKindId; the static_assert below pins the two in step.
constexpr std::array<const char*, static_cast<size_t>(kCommandKindCount)> kCommandNames{{
    "PlaceBuilding",
    "RecruitHero",
    "DestroyBuilding",
    "MoveTo",
    "EnterBuilding",
    "EnterHome",
    "Buy",
    "Attack",
    "SetBehavior",
    "CollectTax",
    "Deposit",
    "AttackBuilding",
    "Chat",
    "Engage",
    "UseSkill",
    "CancelFocus",
    "FocusSkill",
}};

// Indexed by BuildingKind, same contract.
constexpr std::array<const char*, static_cast<size_t>(BuildingKind::Count)> kBuildingNames{{
    "Castle",
    "FreeCompanyQuarters",
    "HuntersCamp",
    "ThievesDen",
    "Scriptorium",
    "Tavern",
    "Apothecary",
    "Watchtower",
    "House",
    "Sewer",
    "Wall",
}};

// A table entry is only useful if it is actually filled in -- an empty string
// would read as "no name" at the call site and pass the density check.
constexpr bool all_named(const char* const* names, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        if (names[i] == nullptr || names[i][0] == '\0') {
            return false;
        }
    }
    return true;
}
static_assert(all_named(kCommandNames.data(), kCommandNames.size()),
              "every CommandKindId needs a row in kCommandNames");
static_assert(all_named(kBuildingNames.data(), kBuildingNames.size()),
              "every BuildingKind needs a row in kBuildingNames");

}  // namespace

const char* CommandKindName(CommandKindId kind) {
    const int32_t i = static_cast<int32_t>(kind);
    if (i < 0 || i >= kCommandKindCount) {
        return "?";
    }
    return kCommandNames[static_cast<size_t>(i)];
}

const char* BuildingKindName(BuildingKind kind) {
    const int32_t i = static_cast<int32_t>(kind);
    if (i < 0 || i >= static_cast<int32_t>(BuildingKind::Count)) {
        return "?";
    }
    return kBuildingNames[static_cast<size_t>(i)];
}

}  // namespace badlands
