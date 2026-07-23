#include "behaviours/perception.h"

#include "brain.h"  // BrainKind (what "my kind" means for NotMyKind)
#include "components.h"
#include "game_state.h"

namespace badlands {

namespace {

bool counts_as_threat(const BadlandsGame& game, entt::entity self, entt::entity other,
                      ThreatPolicy policy) {
    switch (policy) {
        case ThreatPolicy::HostileTeam:
            return game.registry.get<Team>(other).id != game.registry.get<Team>(self).id;
        case ThreatPolicy::NotMyKind: {
            const auto* mine = game.registry.try_get<Brain>(self);
            const auto* theirs = game.registry.try_get<Brain>(other);
            // Cannot tell the kinds apart -> treat as foreign, the cautious
            // answer for the prey animal this policy exists to serve.
            if (mine == nullptr || theirs == nullptr) {
                return true;
            }
            return theirs->kind != mine->kind;
        }
    }
    return false;
}

}  // namespace

int32_t collect_threats(const BadlandsGame& game, entt::entity self, glm::vec2 pos, float radius,
                        ThreatPolicy policy, WorldView& out) {
    out.threat_count = 0;
    if (radius <= 0.0f) {
        return 0;
    }
    // Iterate by SLOT, not by registry view: the slot is the name a brain would
    // use in a command (entity handles never cross the AI boundary), and slot
    // order makes the scan deterministic without depending on how EnTT happens
    // to lay out its pools.
    for (uint32_t slot = 0; slot < game.slots.size(); ++slot) {
        const entt::entity e = game.slots[slot];
        if (e == self || !game.registry.valid(e)) {
            continue;
        }
        // Hidden and dead entities are not threats: one you cannot see, the
        // other cannot act.
        if (game.registry.all_of<InsideBuilding>(e)) {
            continue;
        }
        const auto* health = game.registry.try_get<Health>(e);
        if (health != nullptr && health->hp <= 0.0f) {
            continue;
        }
        if (!counts_as_threat(game, self, e, policy)) {
            continue;
        }
        const float d = glm::distance(game.registry.get<Position>(e).pos, pos);
        if (d > radius) {
            continue;
        }
        add_threat(out, game.registry.get<Position>(e).pos, d, slot);
    }
    return out.threat_count;
}

}  // namespace badlands
