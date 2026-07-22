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

}  // namespace badlands
