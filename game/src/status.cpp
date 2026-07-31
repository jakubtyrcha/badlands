#include "status.h"

#include "game_state.h"  // BadlandsGame, emit_event, slot_for_entity
#include "intention.h"   // abort_current_intention -- a stun ends the running plan
#include "strike.h"      // cancel_strike -- and drops a swing still being wound up

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>

namespace badlands {

namespace {

// Dense, indexed by StatusKind (static_assert-pinned), the same discipline
// ActivityCatalog/SkillDefs use for their own name tables.
constexpr std::array<const char*, static_cast<size_t>(kStatusKindCount)> kStatusNames{{
    "None",
    "Stunned",
    "Cursed",
    "Disengaged",
}};

// Index of `kind` in `s`, or -1.
int32_t index_of(const Statuses& s, StatusKind kind) {
    for (int32_t i = 0; i < s.count && i < kMaxStatuses; ++i) {
        if (s.entries[i].kind == kind) {
            return i;
        }
    }
    return -1;
}

}  // namespace

const char* StatusName(int32_t kind) {
    if (kind < 0 || kind >= kStatusKindCount) {
        return "-";
    }
    return kStatusNames[static_cast<size_t>(kind)];
}

bool has_status(const entt::registry& reg, entt::entity e, StatusKind kind) {
    return remaining_millis_of(reg, e, kind) > 0;
}

int64_t remaining_millis_of(const entt::registry& reg, entt::entity e, StatusKind kind) {
    if (e == entt::null || !reg.valid(e)) {
        return 0;
    }
    const auto* s = reg.try_get<Statuses>(e);
    if (s == nullptr) {
        return 0;
    }
    const int32_t i = index_of(*s, kind);
    return i < 0 ? 0 : s->entries[i].remaining_millis;
}

bool apply_status(BadlandsGame& game, entt::entity e, StatusKind kind, int64_t millis,
                  uint32_t source_slot) {
    if (millis <= 0 || kind == StatusKind::None) {
        return false;  // "for no time at all" is not an affliction (status.h)
    }
    entt::registry& reg = game.registry;
    if (e == entt::null || !reg.valid(e)) {
        return false;
    }
    Statuses& s = reg.get_or_emplace<Statuses>(e);

    if (const int32_t i = index_of(s, kind); i >= 0) {
        // Refresh: keep the longer remaining. A weaker stun landing mid-stun
        // must not cure the stronger one already running.
        if (millis > s.entries[i].remaining_millis) {
            s.entries[i].remaining_millis = millis;
            s.entries[i].source_slot = source_slot;
        }
    } else {
        if (s.count >= kMaxStatuses) {
            spdlog::warn("[status] entity has {} statuses already; '{}' dropped", s.count,
                         StatusName(static_cast<int32_t>(kind)));
            return false;
        }
        s.entries[s.count] = StatusEntry{kind, millis, source_slot};
        ++s.count;
    }

    // Stunned ends whatever the victim was doing. Not a timer concern, but it
    // belongs here rather than in every caller: a stun applied by a skill, a
    // trap, or a future monster ability must all interrupt the same way. The
    // brain re-decides from scratch on its first wake after the stun (it is
    // not consulted during one -- sim.cpp's think dispatch).
    if (kind == StatusKind::Stunned) {
        abort_current_intention(game, slot_for_entity(game, e));
        // And drops a blow still being wound up (game/src/strike.h). This is
        // what makes a stun an INTERRUPT rather than only a debuff: catching
        // an opponent mid-swing costs it the whole attack. A strike already
        // past its resolve is untouched -- that blow was thrown.
        cancel_strike(game, e);
    }

    const glm::vec2 pos =
        reg.all_of<Position>(e) ? reg.get<Position>(e).pos : glm::vec2{0.0f, 0.0f};
    emit_event(game, GameEvent{.kind = GameEventKind::StatusApplied,
                               .actor_id = source_slot,
                               .target_id = slot_for_entity(game, e),
                               .target_kind = kEventTargetCharacter,
                               .amount = static_cast<float>(kind),
                               .x = pos.x,
                               .z = pos.y,
                               .at_millis = game.world_millis});
    return true;
}

void advance_statuses(BadlandsGame& game) {
    for (auto [e, s] : game.registry.view<Statuses>().each()) {
        int32_t live = 0;
        for (int32_t i = 0; i < s.count && i < kMaxStatuses; ++i) {
            s.entries[i].remaining_millis -= kMillisPerTick;
            if (s.entries[i].remaining_millis > 0) {
                // Compact in place: survivors keep their relative order, so
                // the array never depends on which entry expired.
                s.entries[live++] = s.entries[i];
            }
        }
        for (int32_t i = live; i < s.count && i < kMaxStatuses; ++i) {
            s.entries[i] = StatusEntry{};
        }
        s.count = live;
    }
}

}  // namespace badlands
