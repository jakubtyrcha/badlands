#include "executables/ai_sandbox/sneak_mode.hpp"

#include <algorithm>
#include <cstdio>

#include <spdlog/spdlog.h>

namespace badlands {

namespace {

float seconds_of(int64_t millis) { return static_cast<float>(millis) / 1000.0f; }

}  // namespace

void observe_sneak(const std::vector<GameEvent>& events, uint32_t hero_slot,
                   SneakProgress& p) {
    if (hero_slot == UINT32_MAX) {
        return;
    }
    for (const GameEvent& ev : events) {
        switch (ev.kind) {
            case GameEventKind::StatusApplied:
                // The status landing ON the robber, and only the first time: a
                // refresh is the same sneak, not a second one.
                if (p.stage == SneakStage::Waiting && ev.target_id == hero_slot &&
                    static_cast<int32_t>(ev.amount) ==
                        static_cast<int32_t>(StatusKind::Sneaking)) {
                    p.stage = SneakStage::Sneaked;
                    p.sneaked_at_millis = ev.at_millis;
                }
                break;
            case GameEventKind::SkillUsed:
                if (ev.actor_id == hero_slot &&
                    static_cast<int32_t>(ev.amount) == static_cast<int32_t>(SkillId::Backstab)) {
                    p.backstabbed = true;
                }
                break;
            case GameEventKind::DamageDealt:
                // The payoff: the first blow the robber lands AFTER it went
                // unseen. Keyed on the actor, so the bandit hitting back --
                // which is the far more common event in this stream -- can
                // never be mistaken for it.
                if (p.stage == SneakStage::Sneaked && ev.actor_id == hero_slot &&
                    ev.target_kind == kEventTargetCharacter) {
                    p.stage = SneakStage::Struck;
                    p.struck_at_millis = ev.at_millis;
                    p.strike_damage = ev.amount;
                }
                break;
            default:
                break;
        }
    }
}

WorldConfig SneakMode::Configure() {
    // The tube, always: it is the longest arena, and the approach is most of
    // what this mode exists to watch. Nothing is sampled here -- a scenario
    // that varied would make a FAILED line ambiguous between "the brain did not
    // do it" and "this arena did not suit it".
    layout_ = build_arena(ArenaShape::Tube);

    WorldConfig cfg;
    cfg.prebuild_colony = false;
    cfg.map = MapKind::FlatPlains;
    cfg.terrain_blocking = true;
    cfg.plops = layout_.plops;
    return cfg;
}

void SneakMode::Stage(Sim& sim) {
    // Shift the robber along the tube's long axis by round, so consecutive
    // rounds close over different distances and therefore draw a different roll
    // stream. Toward the bandit, never past the far wall.
    const int32_t steps = std::max(1, cfg_.separation_steps);
    const float shift =
        static_cast<float>(round_ % static_cast<uint32_t>(steps)) * cfg_.separation_step_m;
    const float start_x = layout_.spawn_a.x + shift;

    hero_slot_ = sim.SpawnCreature(CreatureId::GraveRobber, /*team=*/0, start_x,
                                   layout_.spawn_a.y, cfg_.hero_level);
    sim.SpawnCreature(CreatureId::Bandit, /*team=*/1, layout_.spawn_b.x, layout_.spawn_b.y);
    started_millis_ = sim.World().world_millis;
    progress_ = SneakProgress{};
}

bool SneakMode::Observe(const std::vector<CharacterState>& /*rows*/,
                        const std::vector<GameEvent>& events, int64_t world_millis) {
    observe_sneak(events, hero_slot_, progress_);
    const int64_t elapsed = world_millis - started_millis_;

    // The round ends the moment the blow lands -- there is nothing further to
    // learn from watching the fight play out -- or when the budget runs out.
    const bool done = progress_.stage == SneakStage::Struck;
    if (!done && elapsed < cfg_.max_millis) {
        return false;
    }

    char line[256];
    if (done) {
        std::snprintf(line, sizeof(line),
                      "sneaked at %.1fs, %s at %.1fs for %.1f (OK)",
                      seconds_of(progress_.sneaked_at_millis - started_millis_),
                      progress_.backstabbed ? "STABBED" : "swung",
                      seconds_of(progress_.struck_at_millis - started_millis_),
                      progress_.strike_damage);
    } else if (progress_.stage == SneakStage::Sneaked) {
        std::snprintf(line, sizeof(line), "sneaked at %.1fs but never struck in %.0fs (FAILED)",
                      seconds_of(progress_.sneaked_at_millis - started_millis_),
                      seconds_of(cfg_.max_millis));
    } else {
        std::snprintf(line, sizeof(line), "never sneaked in %.0fs (FAILED)",
                      seconds_of(cfg_.max_millis));
    }
    last_result_ = line;
    spdlog::info("sneak {}: {}", round_, last_result_);
    ++round_;
    return true;
}

std::string SneakMode::Status() const {
    const char* stage = progress_.stage == SneakStage::Struck    ? "struck"
                        : progress_.stage == SneakStage::Sneaked ? "unseen"
                                                                 : "approaching";
    char line[256];
    std::snprintf(line, sizeof(line), "sneak %u: GraveRobber(lvl %d) vs Bandit -- %s\nlast: %s",
                  round_, cfg_.hero_level, stage, last_result_.c_str());
    return line;
}

}  // namespace badlands
