#include "behaviours/selectors.h"

namespace badlands {

BehaviourResult select_argmax(std::span<const Candidate> candidates, const WorldView& view,
                              const SimFactors& factors) {
    const Candidate* best = nullptr;
    float best_score = 0.0f;  // 0 means "not applicable"; a real winner exceeds it
    for (const Candidate& c : candidates) {
        const float s = c.score(view, factors);
        if (s > best_score) {  // strictly greater -> ties keep the earliest
            best_score = s;
            best = &c;
        }
    }
    return best ? best->act(view, factors) : BehaviourResult{};  // default: Idle in place
}

BehaviourResult select_priority(std::span<const Candidate> candidates, const WorldView& view,
                                const SimFactors& factors) {
    for (const Candidate& c : candidates) {
        if (c.score(view, factors) > 0.0f) {
            return c.act(view, factors);
        }
    }
    return BehaviourResult{};  // nothing applicable -> Idle in place
}

BehaviourResult select_banded(std::span<const ActivityDef> activities,
                              const ActivityWeights& weights, const WorldView& view,
                              const SimFactors& factors) {
    // Bands are tried strictly in order, so the hierarchy is enforced by the
    // CONTROL FLOW rather than by the numbers -- no weight can cross a band.
    for (int32_t b = 0; b < static_cast<int32_t>(ActivityBand::Count); ++b) {
        const auto band = static_cast<ActivityBand>(b);
        const ActivityDef* best = nullptr;
        float best_utility = 0.0f;
        for (const ActivityDef& a : activities) {
            if (a.band != band) {
                continue;
            }
            const float weight = weights.of(a.id);
            if (weight <= 0.0f) {
                continue;  // this actor does not have this activity at all
            }
            const float score = a.score(view, factors);
            if (score <= 0.0f) {
                continue;  // vetoed by its own considerations
            }
            const float utility = weight * score;
            if (utility > best_utility) {  // strictly greater -> ties keep the earliest
                best_utility = utility;
                best = &a;
            }
        }
        if (best != nullptr) {
            return best->act(view, factors);
        }
    }
    return BehaviourResult{};  // nothing applicable anywhere -> Idle in place
}

}  // namespace badlands
