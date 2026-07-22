#include "economy.h"

#include "badlands_sim.hpp"
#include "components.h"
#include "game_state.h"
#include "heroes.h"  // spawn_entity
#include "placement.h"

#include <entt/entt.hpp>
#include <glm/glm.hpp>

namespace badlands {

namespace {

// True on the tick that world_millis crosses a `period` boundary (the same
// crossing test day_count/midnight uses). period must be > 0.
bool crossed(int64_t world_millis, int64_t period) {
    if (period <= 0) {
        return false;
    }
    return (world_millis / period) != ((world_millis - kMillisPerTick) / period);
}

uint32_t count_alive_tax_collectors(const BadlandsGame& game) {
    uint32_t n = 0;
    for (auto e : game.registry.view<const TaxCollectorState>()) {
        (void)e;
        ++n;
    }
    return n;
}

void spawn_tax_collector(BadlandsGame& game, uint32_t castle_id, glm::vec2 door) {
    CharacterDesc d{};
    d.archetype = Archetype::Townfolk;
    d.pos_x = door.x;
    d.pos_z = door.y;
    d.team = 0;  // the player's own
    d.hp = 12.0f;
    d.move_speed = game.factors.townfolk.move_speed;
    d.attack_range = 0.0f;
    d.attack_damage = 0.0f;
    d.attack_cooldown = 1.0f;
    d.size_x = d.size_z = 0.7f;
    d.size_y = 1.6f;
    d.color_r = 0.85f;  // taxman gold, distinct from heroes/deer
    d.color_g = 0.72f;
    d.color_b = 0.15f;
    // The recipe sets TaxCollectorState.home_building_id from `home`.
    spawn_entity(game, d, static_cast<int32_t>(castle_id));
}

}  // namespace

void advance_economy(BadlandsGame& game) {
    if (!crossed(game.world_millis, kMillisPerDay)) {
        return;  // not midnight
    }
    const uint32_t income = game.factors.townfolk.house_income_per_day;
    for (PlacedBuilding& b : game.placement.buildings) {
        if (b.alive && b.kind == static_cast<int32_t>(BuildingKind::House)) {
            b.taxable_income += income;
        }
    }
}

void run_spawners(BadlandsGame& game) {
    const TownfolkFactors& tf = game.factors.townfolk;
    if (!crossed(game.world_millis, tf.spawn_interval_millis)) {
        return;
    }
    if (count_alive_tax_collectors(game) >= static_cast<uint32_t>(tf.max_alive)) {
        return;  // at the cap
    }
    // One collector, from the lowest-id alive Castle (deterministic).
    const auto& bs = game.placement.buildings;
    for (uint32_t i = 0; i < bs.size(); ++i) {
        if (!bs[i].alive || bs[i].kind != static_cast<int32_t>(BuildingKind::Castle)) {
            continue;
        }
        glm::vec2 door;
        if (building_approach_tile(game.placement, bs[i], door)) {
            spawn_tax_collector(game, i, door);
        }
        break;  // one per interval
    }
}

}  // namespace badlands
