// ActivityHistogram: goal statistics as a fold over snapshot rows.
//
// Note what this file does NOT include: game_state.h, entt, or anything else
// from the sim. It sees exactly what any other observer sees -- a span of
// CharacterState -- which is the whole point. See the class comment in
// badlands_sim.hpp for why counting lives out here rather than inside the tick.

#include "badlands_sim.hpp"

namespace badlands {

void ActivityHistogram::Accumulate(std::span<const CharacterState> rows) {
    for (const CharacterState& row : rows) {
        ++samples_;
        const int32_t id = row.behavior;
        if (id < 0 || id >= kActivityCount) {
            continue;  // no decision yet (or an id from a newer build): counted
                       // as a sample, attributed to nothing
        }
        ++total_[id];
        const int32_t cls = row.hero_class;
        if (cls >= 0 && cls < HERO_CLASS_COUNT) {
            ++per_class_[cls][id];
        }
    }
}

void ActivityHistogram::Reset() { *this = ActivityHistogram{}; }

uint64_t ActivityHistogram::Total(ActivityId id) const {
    const int32_t i = static_cast<int32_t>(id);
    return (i >= 0 && i < kActivityCount) ? total_[i] : 0;
}

uint64_t ActivityHistogram::ForClass(HeroClassId cls, ActivityId id) const {
    const int32_t c = static_cast<int32_t>(cls);
    const int32_t i = static_cast<int32_t>(id);
    if (c < 0 || c >= HERO_CLASS_COUNT || i < 0 || i >= kActivityCount) {
        return 0;
    }
    return per_class_[c][i];
}

}  // namespace badlands
