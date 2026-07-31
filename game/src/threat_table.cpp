#include "threat_table.h"

#include "components.h"  // CreatureKind, HeroSimulationState

#include <algorithm>

namespace badlands {

namespace {

// The calibration posts. Heroes are anchored PER LEVEL (one point each today,
// at level 1 -- the design doc sketches level-15 and level-20 keyframes that
// slot in here without touching a call site); monsters carry a single
// level-independent anchor.
//
// Set by the user 2026-07-31. These SUPERSEDE the design document's own
// power-curve keyframes where the two differ -- notably the Hunter and Grave
// Robber, which the curves had sharing one line at 2 and these split into
// 1.5 and 1.0.
constexpr ThreatAnchor kMercenary[]{{1, 2.5f}};
constexpr ThreatAnchor kHunter[]{{1, 1.5f}};
constexpr ThreatAnchor kGraveRobber[]{{1, 1.0f}};
constexpr ThreatAnchor kApprentice[]{{1, 0.75f}};
constexpr ThreatAnchor kRat[]{{1, 0.25f}};
constexpr ThreatAnchor kGoblin[]{{1, 1.0f}};
constexpr ThreatAnchor kDeer[]{{1, 0.0f}};  // harmless: no attacks at all
constexpr ThreatAnchor kBandit[]{{1, 2.0f}};
constexpr ThreatAnchor kBanditArcher[]{{1, 2.0f}};
constexpr ThreatAnchor kBanditLeader[]{{1, 5.0f}};
constexpr ThreatAnchor kMudGolem[]{{1, 6.0f}};

struct CreatureAnchors {
    const ThreatAnchor* anchors;
    int32_t count;
};

// Dense, indexed by CreatureId -- the same discipline kNames uses
// (creature_catalog.cpp). A new creature that forgets a row here fails the
// static_assert below, not a duel.
constexpr CreatureAnchors kTable[kCreatureCount] = {
    {kMercenary, 1}, {kHunter, 1},       {kGraveRobber, 1},  {kApprentice, 1},
    {kRat, 1},       {kGoblin, 1},       {kDeer, 1},         {kBandit, 1},
    {kBanditArcher, 1}, {kBanditLeader, 1}, {kMudGolem, 1},
};
static_assert(std::size(kTable) == static_cast<size_t>(kCreatureCount),
              "every CreatureId needs a threat anchor row");

}  // namespace

float interpolate_anchors(const ThreatAnchor* anchors, int32_t count, int32_t level) {
    if (anchors == nullptr || count <= 0) {
        return 0.0f;
    }
    if (level <= anchors[0].level) {
        return anchors[0].threat;  // flat below the first
    }
    for (int32_t i = 1; i < count; ++i) {
        if (level <= anchors[i].level) {
            const ThreatAnchor& lo = anchors[i - 1];
            const ThreatAnchor& hi = anchors[i];
            const int32_t span = hi.level - lo.level;
            if (span <= 0) {
                return hi.threat;  // degenerate/duplicate anchor: no divide by 0
            }
            const float t = static_cast<float>(level - lo.level) / static_cast<float>(span);
            return lo.threat + t * (hi.threat - lo.threat);
        }
    }
    return anchors[count - 1].threat;  // flat above the last, never extrapolated
}

float threat_target(CreatureId creature, int32_t level) {
    const int i = static_cast<int>(creature);
    if (i < 0 || i >= kCreatureCount) {
        return 0.0f;
    }
    return interpolate_anchors(kTable[i].anchors, kTable[i].count, level);
}

float threat_of(const entt::registry& reg, entt::entity e) {
    if (e == entt::null || !reg.valid(e)) {
        return 0.0f;
    }
    const auto* kind = reg.try_get<CreatureKind>(e);
    if (kind == nullptr) {
        return 0.0f;
    }
    const auto* hero = reg.try_get<HeroSimulationState>(e);
    return threat_target(kind->id, hero != nullptr ? hero->level : 1);
}

}  // namespace badlands
