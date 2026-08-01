#include "executables/ai_sandbox/teleport_mode.hpp"

#include <cmath>
#include <cstdio>

#include <spdlog/spdlog.h>

namespace badlands {

namespace {

// Ticks -> seconds for the log line. The sim counts in ticks; a human reads
// seconds (badlands_sim.hpp).
float seconds_of(int64_t ticks) { return seconds_of_ticks(ticks); }

const CharacterState* row_for(const std::vector<CharacterState>& rows, uint32_t slot) {
    for (const CharacterState& r : rows) {
        if (r.id == slot) {
            return &r;
        }
    }
    return nullptr;
}

}  // namespace

void observe_teleport(const std::vector<GameEvent>& events, uint32_t hero_slot,
                      TeleportProgress& p) {
    if (hero_slot == UINT32_MAX || p.blinked) {
        return;
    }
    for (const GameEvent& ev : events) {
        // Keyed on the SKILL, not merely on the caster: a level-8 apprentice
        // also knows Curse and Calcify, and either firing would otherwise read
        // as a successful round.
        if (ev.kind == GameEventKind::SkillUsed && ev.actor_id == hero_slot &&
            static_cast<int32_t>(ev.amount) == static_cast<int32_t>(SkillId::Teleport)) {
            p.blinked = true;
            p.blinked_at_ticks = ev.at_ticks;
            return;
        }
    }
}

WorldConfig TeleportMode::Configure() {
    // The tube: a 30 m blink needs somewhere to land, and this is the longest
    // arena. Fixed rather than sampled -- a scenario that varied would make a
    // FAILED line ambiguous between "the brain did not do it" and "this arena
    // had nowhere to go".
    layout_ = build_arena(ArenaShape::Tube);

    WorldConfig cfg;
    cfg.prebuild_colony = false;
    cfg.map = MapKind::FlatPlains;
    // Load-bearing here, unlike in the other modes: with terrain blocking off
    // no navmesh is built at all, and the nav window the brain picks its
    // destination from would be empty.
    cfg.terrain_blocking = true;
    cfg.plops = layout_.plops;
    return cfg;
}

void TeleportMode::Stage(Sim& sim) {
    // Against one wall, with the dummy between it and the open arena, so the
    // only way to gain distance is to blink PAST the thing rather than shuffle
    // away from it.
    start_pos_ = layout_.spawn_a;
    hero_slot_ = sim.SpawnCreature(CreatureId::Apprentice, /*team=*/0, start_pos_.x,
                                   start_pos_.y, cfg_.hero_level);
    sim.SpawnCreature(CreatureId::TrainingDummy, /*team=*/1, start_pos_.x + cfg_.start_gap_m,
                      start_pos_.y);
    started_ticks_ = sim.World().world_ticks;
    progress_ = TeleportProgress{};
}

bool TeleportMode::Observe(const std::vector<CharacterState>& rows,
                           const std::vector<GameEvent>& events, int64_t world_ticks) {
    observe_teleport(events, hero_slot_, progress_);
    const int64_t elapsed = world_ticks - started_ticks_;
    if (!progress_.blinked && elapsed < cfg_.max_ticks) {
        return false;
    }

    char line[256];
    if (progress_.blinked) {
        // How FAR, measured off the snapshot rather than trusted from the cast:
        // a blink that reported success but moved nobody is exactly the failure
        // this mode exists to catch.
        float moved = 0.0f;
        if (const CharacterState* r = row_for(rows, hero_slot_); r != nullptr) {
            moved = std::hypot(r->pos_x - start_pos_.x, r->pos_z - start_pos_.y);
        }
        std::snprintf(line, sizeof(line), "blinked %.1f m at %.1fs (%s)", moved,
                      seconds_of(progress_.blinked_at_ticks - started_ticks_),
                      moved > 1.0f ? "OK" : "FAILED: cast but did not move");
    } else {
        std::snprintf(line, sizeof(line), "never blinked in %.0fs (FAILED)",
                      seconds_of(cfg_.max_ticks));
    }
    last_result_ = line;
    spdlog::info("teleport {}: {}", round_, last_result_);
    ++round_;
    return true;
}

std::string TeleportMode::Status() const {
    char line[256];
    std::snprintf(line, sizeof(line),
                  "teleport %u: Apprentice(lvl %d) vs TrainingDummy(threat 20) -- %s\nlast: %s",
                  round_, cfg_.hero_level, progress_.blinked ? "gone" : "cornered",
                  last_result_.c_str());
    return line;
}

}  // namespace badlands
