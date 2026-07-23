#include "critter_brain.h"

#include "badlands_sim.hpp"
#include "behaviours/blocks.h"
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

// Nearest non-critter entity within `max_dist` -- what a deer flees. "Critter"
// is the Brain kind, so deer ignore each other and bolt from heroes / townfolk /
// monsters alike. Perception only: this reads the registry so blocks never do.
bool nearest_threat(const BadlandsGame& game, entt::entity self, glm::vec2 pos, float max_dist,
                    glm::vec2& out_pos, float& out_dist) {
    float best = max_dist;
    bool found = false;
    for (auto [e, p, brain] : game.registry.view<const Position, const Brain>().each()) {
        if (e == self || brain.kind == BrainKind::Critter) {
            continue;
        }
        const float d = glm::distance(p.pos, pos);
        if (d <= best) {
            best = d;
            out_pos = p.pos;
            found = true;
        }
    }
    out_dist = found ? best : 0.0f;
    return found;
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

    v.has_threat =
        nearest_threat(game, e, v.pos, cf.sight_radius, v.threat_pos, v.threat_dist);
    v.roam_goal = biome_roam_goal(game, slot, v.roam_epoch, st.roam_anchor, cf.roam_radius);
    return v;
}

// Priority order: bolt first, then graze in place, then wander, else idle.
constexpr std::array<Candidate, 4> kCritterBlocks{{
    {score_flee, act_flee},
    {score_graze, act_graze},
    {score_roam, act_roam},
    {score_idle, act_idle},
}};

}  // namespace

void critter_think(BadlandsGame& game, uint32_t slot) {
    entt::entity e = entity_for_slot(game, static_cast<int32_t>(slot));
    if (e == entt::null || !game.registry.all_of<CritterState>(e)) {
        return;
    }
    const WorldView view = observe_critter(game, slot, e);
    const BehaviourResult r = select_priority(kCritterBlocks, view, game.factors);

    // Both the chosen behaviour (inspection) and the walk goal (the decision)
    // go through the command layer, edge-triggered, like every other mutation.
    enqueue_set_behavior(game, slot, static_cast<int32_t>(r.id));
    enqueue_move_to(game, slot, r.target);
}

}  // namespace badlands
