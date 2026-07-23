#include "behaviours/deliberation.h"

#include "behaviours/rng.h"

namespace badlands {

bool is_discretionary(int32_t id) {
    if (id < 0 || id >= kActivityCount) {
        return false;  // nothing decided yet -- there is no mind to change
    }
    // Anything that is not an immediate-danger response. A hero deliberates
    // over which errand to run; it does not deliberate over whether to flee.
    return ActivityInfoOf(id).band != ActivityBand::Danger;
}

ThinkDecision deliberate(ActivityId chosen, const WorldView& view, const SimFactors& factors) {
    const int32_t chosen_id = static_cast<int32_t>(chosen);

    // Danger ends deliberation, and cancels one in progress. Checked first so
    // that no combination of the rules below can keep a character standing
    // still while something is bearing down on it.
    if (has_threat(view) || ActivityInfoOf(chosen_id).band == ActivityBand::Danger) {
        return {};
    }

    // A pause already running just continues -- no new decision, nothing logged.
    if (view.now_millis < view.think_until_millis) {
        return {true, 0};
    }

    // Only a genuine change of mind is worth mulling over.
    if (chosen_id == view.current_activity) {
        return {};
    }
    // The pause that just ended must not immediately start another.
    if (view.current_activity == static_cast<int32_t>(ActivityId::Think)) {
        return {};
    }
    // Idle is the ABSENCE of a goal, not a goal: a hero does not stand and think
    // about starting or stopping doing nothing. Treated like "no decision yet".
    if (chosen_id == static_cast<int32_t>(ActivityId::Idle) ||
        view.current_activity == static_cast<int32_t>(ActivityId::Idle)) {
        return {};
    }
    if (!is_discretionary(view.current_activity) || !is_discretionary(chosen_id)) {
        return {};
    }

    uint64_t rng = seed_of(view.slot, view.now_millis);
    const int64_t duration =
        range_i64(rng, factors.hero.think_min_millis, factors.hero.think_max_millis);
    // A zero draw is not a pause: it would log a Think the character leaves in
    // the same tick. This is also what makes think_max_millis = 0 a clean
    // "deliberation off" switch.
    return duration > 0 ? ThinkDecision{true, duration} : ThinkDecision{};
}

}  // namespace badlands
