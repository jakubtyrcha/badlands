#include "skill_focus.h"

#include "game_state.h"  // BadlandsGame, slot_for_entity, emit_event
#include "skill_cast.h"  // validate_cast, run_cast

#include <spdlog/spdlog.h>

#include <cmath>
#include <vector>

namespace badlands {

namespace {

int64_t millis_of(float seconds) {
    return static_cast<int64_t>(std::lround(seconds * 1000.0f));
}

void emit_cancelled(BadlandsGame& game, entt::entity e, const SkillFocus& f) {
    const glm::vec2 pos = game.registry.all_of<Position>(e)
                              ? game.registry.get<Position>(e).pos
                              : glm::vec2{0.0f, 0.0f};
    emit_event(game, GameEvent{.kind = GameEventKind::FocusCancelled,
                               .actor_id = slot_for_entity(game, e),
                               .target_id = f.target_slot,
                               .target_kind = kEventTargetCharacter,
                               .amount = static_cast<float>(f.id),
                               .x = pos.x,
                               .z = pos.y,
                               .at_millis = game.world_millis});
}

}  // namespace

bool begin_focus(BadlandsGame& game, entt::entity e, int32_t skill_index,
                 uint32_t target_slot) {
    entt::registry& reg = game.registry;
    if (e == entt::null || !reg.valid(e)) {
        return false;
    }
    if (reg.all_of<SkillFocus>(e)) {
        return false;  // one commitment at a time, as with a strike
    }
    const auto* skills = reg.try_get<Skills>(e);
    if (skills == nullptr || skill_index < 0 || skill_index >= skills->count ||
        skill_index >= kMaxSkills) {
        return false;
    }
    const SkillId id = skills->ids[skill_index];
    const SkillSpec& spec = game.skills.specs[static_cast<size_t>(id)];
    const int64_t duration = millis_of(spec.intention_duration_seconds);
    if (duration <= 0) {
        // An "instant focus" is a contradiction. Running it as an ordinary cast
        // instead would make the trigger meaningless -- the whole difference
        // between the channels is that this one costs time.
        spdlog::warn("[focus] {} declares no intention_duration; focus refused",
                     SkillName(static_cast<int32_t>(id)));
        return false;
    }
    reg.emplace<SkillFocus>(e, SkillFocus{.resolve_at_millis = game.world_millis + duration,
                                          .id = id,
                                          .skill_index = skill_index,
                                          .target_slot = target_slot});
    return true;
}

bool focusing(const entt::registry& reg, entt::entity e) {
    return e != entt::null && reg.valid(e) && reg.all_of<SkillFocus>(e);
}

bool cancel_focus(BadlandsGame& game, entt::entity e) {
    if (!focusing(game.registry, e)) {
        return false;
    }
    const SkillFocus f = game.registry.get<SkillFocus>(e);
    game.registry.erase<SkillFocus>(e);
    // No cooldown was ever stamped -- run_cast does that, and it never ran --
    // so an interrupted focus costs the seconds and nothing else.
    emit_cancelled(game, e, f);
    return true;
}

void advance_focus(BadlandsGame& game) {
    // Collected first: run_cast mutates the registry (damage, statuses, events),
    // and a view iterated while its own pool is written to is not safe.
    std::vector<entt::entity> due;
    for (auto [e, f] : game.registry.view<const SkillFocus>().each()) {
        if (game.world_millis >= f.resolve_at_millis) {
            due.push_back(e);
        }
    }
    for (entt::entity e : due) {
        if (!game.registry.valid(e) || !game.registry.all_of<SkillFocus>(e)) {
            continue;  // died, or an earlier resolution in this sweep cancelled it
        }
        const SkillFocus f = game.registry.get<SkillFocus>(e);
        const uint32_t caster_slot = slot_for_entity(game, e);
        // THE re-validation: nothing was captured when the focus began, so
        // everything is checked now. A target that died, moved out of range, or
        // went unseeable in the meantime gets no shot at all.
        CastPlan plan;
        if (!validate_cast(game, caster_slot, f.skill_index, f.target_slot, plan,
                           SkillTrigger::Intention)) {
            game.registry.erase<SkillFocus>(e);
            emit_cancelled(game, e, f);
            continue;
        }
        // Dropped BEFORE the cast: run_cast can kill its target, which drives
        // the death sweep, and leaving a resolved focus behind for that to walk
        // over would be a component describing something already finished.
        game.registry.erase<SkillFocus>(e);
        run_cast(game, caster_slot, f.skill_index, plan);
    }
}

}  // namespace badlands
