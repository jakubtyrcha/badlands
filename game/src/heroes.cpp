#include "heroes.h"

#include "brain.h"
#include "components.h"
#include "game_state.h"
#include "placement.h"

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

namespace badlands {

int32_t guild_hero_class(int kind) {
    switch (static_cast<BuildingKind>(kind)) {
        case BuildingKind::FreeCompanyQuarters:
            return HERO_MERCENARY;
        case BuildingKind::HuntersCamp:
            return HERO_HUNTER;
        case BuildingKind::ThievesDen:
            return HERO_GRAVE_ROBBER;
        case BuildingKind::Scriptorium:
            return HERO_APPRENTICE;
        default:
            return -1;
    }
}

CharacterDesc hero_desc(int32_t hero_class, float x, float z) {
    // Baseline stats shared by every class; color is the only distinguishing
    // field (the panel derives the class name from the home guild's kind).
    constexpr glm::vec3 kColors[HERO_CLASS_COUNT] = {
        {0.35f, 0.45f, 0.80f},  // Mercenary    - blue
        {0.30f, 0.70f, 0.35f},  // Hunter       - green
        {0.60f, 0.45f, 0.75f},  // Grave Robber - violet
        {0.45f, 0.78f, 0.85f},  // Apprentice   - cyan
    };
    static_assert(sizeof(kColors) / sizeof(kColors[0]) == HERO_CLASS_COUNT, "hero color table");
    int idx = (hero_class >= 0 && hero_class < HERO_CLASS_COUNT) ? hero_class : HERO_MERCENARY;
    glm::vec3 c = kColors[idx];

    CharacterDesc d{};
    d.pos_x = x;
    d.pos_z = z;
    d.team = 0;
    d.hp = 25.0f;
    d.move_speed = 2.5f;
    d.attack_range = 1.3f;
    d.attack_damage = 3.0f;
    d.attack_cooldown = 1.0f;
    d.size_x = 0.9f;
    d.size_y = 1.8f;  // hero capsule height in meters
    d.size_z = 0.9f;
    d.color_r = c.x;
    d.color_g = c.y;
    d.color_b = c.z;
    return d;
}

uint32_t spawn_entity(BadlandsGame& game, const CharacterDesc& desc, int32_t home) {
    entt::registry& reg = game.registry;
    entt::entity e = reg.create();
    uint32_t slot = static_cast<uint32_t>(game.slots.size());
    game.slots.push_back(e);

    reg.emplace<Position>(e, glm::vec2{desc.pos_x, desc.pos_z});
    reg.emplace<Team>(e, desc.team);
    reg.emplace<Health>(e, desc.hp, desc.hp);
    reg.emplace<Stats>(e, desc.move_speed, desc.attack_range, desc.attack_damage,
                       desc.attack_cooldown);
    reg.emplace<CooldownTimer>(e, 0.0f);
    reg.emplace<RenderShape>(e, glm::vec3{desc.size_x, desc.size_y, desc.size_z},
                             glm::vec3{desc.color_r, desc.color_g, desc.color_b});
    reg.emplace<Intent>(e, 0, glm::vec2{0.0f, 0.0f});

    float radius = 0.5f * std::min(desc.size_x, desc.size_z);
    reg.emplace<Agent>(e, radius);
    reg.emplace<MoveTarget>(e);
    reg.emplace<NavPath>(e);

    // Hero state (all entities carry these; a plain spawn has class/home -1).
    int32_t hero_class = -1;
    if (home >= 0 && static_cast<size_t>(home) < game.placement.buildings.size()) {
        hero_class = guild_hero_class(game.placement.buildings[home].kind);
    }
    reg.emplace<HeroClass>(e, hero_class);
    reg.emplace<Home>(e, home);
    reg.emplace<Inventory>(e, 0);

    reg.emplace<Brain>(e, game.brains ? spawn_brain(*game.brains, slot) : nullptr);
    return slot;
}

uint32_t roster_count(const BadlandsGame& game, uint32_t building_id) {
    uint32_t n = 0;
    for (auto [e, home] : game.registry.view<const Home>().each()) {
        if (home.building_id == static_cast<int32_t>(building_id)) {
            ++n;
        }
    }
    return n;
}

uint32_t recruit(BadlandsGame& game, uint32_t building_id) {
    auto& bs = game.placement.buildings;
    if (building_id >= bs.size() || !bs[building_id].alive) {
        return std::numeric_limits<uint32_t>::max();
    }
    int32_t cls = guild_hero_class(bs[building_id].kind);
    if (cls < 0) {
        return std::numeric_limits<uint32_t>::max();  // not a guild
    }
    if (roster_count(game, building_id) >= static_cast<uint32_t>(kGuildRosterCap)) {
        return std::numeric_limits<uint32_t>::max();  // roster full
    }
    glm::vec2 tile;
    if (!building_approach_tile(game.placement, bs[building_id], tile)) {
        return std::numeric_limits<uint32_t>::max();  // no free approach tile
    }
    CharacterDesc d = hero_desc(cls, tile.x, tile.y);
    return spawn_entity(game, d, static_cast<int32_t>(building_id));
}

int64_t destroy_building_impl(BadlandsGame& game, uint32_t building_id) {
    auto& bs = game.placement.buildings;
    if (building_id >= bs.size() || !bs[building_id].alive) {
        return -1;  // unknown or already destroyed
    }
    if (!BuildingDefOf(static_cast<BuildingKind>(bs[building_id].kind)).user_destructible) {
        return -2;  // Castle/House/Sewer are not player-destructible
    }
    entt::registry& reg = game.registry;

    // Where expelled/rehomed heroes reappear.
    glm::vec2 tile;
    glm::vec2 reappear = building_approach_tile(game.placement, bs[building_id], tile)
                             ? tile
                             : building_entrance(bs[building_id]);

    // Expel heroes hidden inside the doomed building.
    std::vector<entt::entity> inside;
    for (auto [e, ib] : reg.view<InsideBuilding>().each()) {
        if (ib.building_id == static_cast<int32_t>(building_id)) {
            inside.push_back(e);
        }
    }
    for (entt::entity e : inside) {
        reg.get<Position>(e).pos = reappear;
        reg.remove<InsideBuilding>(e);
    }

    // Tombstone + free the footprint/obstacle before reassigning, so the doomed
    // building is not itself a reassignment candidate.
    bs[building_id].alive = false;
    ++game.placement.nav_epoch;
    rebuild_occupancy(game.placement);
    notify_obstacle_removed(game, building_id);

    // Reassign residents to the lowest-id alive same-class guild with room, else
    // homeless. Sequential so capacity is honored as homes fill.
    std::vector<entt::entity> residents;
    for (auto [e, home] : reg.view<Home>().each()) {
        if (home.building_id == static_cast<int32_t>(building_id)) {
            residents.push_back(e);
        }
    }
    for (entt::entity e : residents) {
        int32_t cls = reg.get<HeroClass>(e).value;
        reg.get<Home>(e).building_id = -1;  // homeless until a slot is found
        for (uint32_t i = 0; i < bs.size(); ++i) {
            if (!bs[i].alive || guild_hero_class(bs[i].kind) != cls) {
                continue;
            }
            if (roster_count(game, i) >= static_cast<uint32_t>(kGuildRosterCap)) {
                continue;
            }
            reg.get<Home>(e).building_id = static_cast<int32_t>(i);
            break;
        }
    }
    return 0;
}

namespace {

// True if `e` stands within kEntranceRadius of the approach tile of the nearest
// alive building of `kind`; writes that building id to `out`.
bool at_building_of_kind(BadlandsGame& game, entt::entity e, int kind, uint32_t& out) {
    glm::vec2 p = game.registry.get<Position>(e).pos;
    uint32_t bid = nearest_building_of(game.placement, kind, p);
    if (bid == std::numeric_limits<uint32_t>::max()) {
        return false;
    }
    glm::vec2 tile;
    if (!building_approach_tile(game.placement, game.placement.buildings[bid], tile)) {
        return false;
    }
    if (glm::distance(p, tile) > kEntranceRadius) {
        return false;
    }
    out = bid;
    return true;
}

}  // namespace

bool hero_enter(BadlandsGame& game, entt::entity e, int kind) {
    uint32_t bid = 0;
    if (!at_building_of_kind(game, e, kind, bid)) {
        return false;
    }
    game.registry.emplace_or_replace<InsideBuilding>(e, static_cast<int32_t>(bid),
                                                     kInsideDurationSeconds);
    return true;
}

bool hero_enter_home(BadlandsGame& game, entt::entity e) {
    int32_t home = game.registry.get<Home>(e).building_id;
    auto& bs = game.placement.buildings;
    if (home < 0 || static_cast<size_t>(home) >= bs.size() || !bs[home].alive) {
        return false;
    }
    glm::vec2 p = game.registry.get<Position>(e).pos;
    glm::vec2 tile;
    if (!building_approach_tile(game.placement, bs[home], tile) ||
        glm::distance(p, tile) > kEntranceRadius) {
        return false;
    }
    game.registry.emplace_or_replace<InsideBuilding>(e, home, kInsideDurationSeconds);
    game.registry.get<Inventory>(e).count = 0;  // resting empties inventory (closes the loop)
    return true;
}

bool hero_buy(BadlandsGame& game, entt::entity e) {
    uint32_t bid = 0;
    if (!at_building_of_kind(game, e, static_cast<int32_t>(BuildingKind::Apothecary), bid)) {
        return false;
    }
    game.registry.get<Inventory>(e).count = kInventoryCap;
    return true;
}

void advance_inside_timers(BadlandsGame& game, float dt) {
    entt::registry& reg = game.registry;
    std::vector<entt::entity> exit;
    for (auto [e, ib] : reg.view<InsideBuilding>().each()) {
        ib.timer -= dt;
        if (ib.timer <= 0.0f) {
            exit.push_back(e);
        }
    }
    auto& bs = game.placement.buildings;
    for (entt::entity e : exit) {
        int32_t bid = reg.get<InsideBuilding>(e).building_id;
        glm::vec2 tile;
        if (bid >= 0 && static_cast<size_t>(bid) < bs.size() &&
            building_approach_tile(game.placement, bs[bid], tile)) {
            reg.get<Position>(e).pos = tile;
        }
        reg.remove<InsideBuilding>(e);
    }
}

}  // namespace badlands
