#include "critter_brain.h"

#include "badlands_sim.hpp"
#include "behaviours/blocks.h"
#include "behaviours/perception.h"
#include "behaviours/selectors.h"
#include "behaviours/world_view.h"
#include "brain.h"
#include "command.h"
#include "components.h"
#include "game_state.h"
#include "heroes.h"  // biome_at

#include <array>
#include <limits>

#include <entt/entt.hpp>
#include <glm/glm.hpp>

namespace badlands {

namespace {

// Deer re-draw their wander goal (and flip walk<->graze) on this window.
constexpr int64_t kRoamLeaseMillis = 4000;
// How many biome-filtered candidates to try before accepting whatever we drew.
constexpr int kRoamBiomeTries = 6;

bool good_biome(mapgen::Biome b) {
    return b == mapgen::Biome::Forest || b == mapgen::Biome::Plains;
}

// A wander goal biased toward Forest/Plains: draw the deterministic ring point,
// and if it lands on Lake/Swamp try the next epoch offset a few times. Map
// access lives here (perception); the shared Roam block just walks to the result.
glm::vec2 biome_roam_goal(const BadlandsGame& game, uint32_t slot, int64_t epoch,
                          glm::vec2 anchor, float radius) {
    glm::vec2 candidate = roam_point(slot, epoch, anchor, radius);
    for (int i = 1; i < kRoamBiomeTries; ++i) {
        if (good_biome(biome_at(game, candidate))) {
            return candidate;
        }
        candidate = roam_point(slot, epoch + i, anchor, radius);  // deterministic retry
    }
    return candidate;  // gave up: stay put-ish rather than loop forever
}

WorldView observe_critter(const BadlandsGame& game, uint32_t slot, entt::entity e) {
    const auto& st = game.registry.get<CritterState>(e);
    const CritterFactors& cf = game.factors.critter;
    WorldView v;
    v.slot = slot;
    v.pos = game.registry.get<Position>(e).pos;
    v.roam_epoch = game.world_millis / kRoamLeaseMillis;

    // Graze during the tail fraction of each roam window (walk -> graze -> walk).
    const int64_t phase = game.world_millis % kRoamLeaseMillis;
    const int64_t graze_start =
        static_cast<int64_t>((1.0f - cf.graze_fraction) * static_cast<float>(kRoamLeaseMillis));
    v.grazing = phase >= graze_start;

    // Deer bolt from anything that is not another deer -- heroes, tax
    // collectors and rats alike. Same shared helper a hero uses for hostiles;
    // only the policy differs.
    collect_threats(game, e, v.pos, cf.sight_radius, ThreatPolicy::NotMyKind, v);
    v.roam_goal = biome_roam_goal(game, slot, v.roam_epoch, st.roam_anchor, cf.roam_radius);
    return v;
}

// A deer's activity table. Note what is NOT here: no critter-specific selector,
// no critter-specific scoring rule. It runs the same select_banded over the
// same shared blocks as a hero -- only this table and CritterFactors::weights
// differ. Fleeing sits in the Danger band, so no graze/roam weight can ever
// out-argue a threat, which is the band hierarchy doing its job for a second
// archetype without a line of new logic.
constexpr std::array<ActivityDef, 4> kCritterActivities{{
    {ActivityId::Flee, ActivityBand::Danger, score_flee, act_flee},
    {ActivityId::Graze, ActivityBand::Normal, score_graze, act_graze},
    {ActivityId::Roam, ActivityBand::Normal, score_roam, act_roam},
    {ActivityId::Idle, ActivityBand::Normal, score_idle, act_idle},
}};

}  // namespace

void critter_think(BadlandsGame& game, uint32_t slot) {
    entt::entity e = entity_for_slot(game, static_cast<int32_t>(slot));
    if (e == entt::null || !game.registry.all_of<CritterState>(e)) {
        return;
    }
    const WorldView view = observe_critter(game, slot, e);
    const BehaviourResult r =
        select_banded(kCritterActivities, game.factors.critter.weights, view, game.factors);

    // Both the chosen behaviour (inspection) and the walk goal (the decision)
    // go through the command layer, edge-triggered, like every other mutation.
    enqueue_set_behavior(game, slot, static_cast<int32_t>(r.id));
    enqueue_move_to(game, slot, r.target);
}

}  // namespace badlands
