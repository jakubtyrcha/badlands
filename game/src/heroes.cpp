#include "heroes.h"

#include "brain.h"
#include "components.h"
#include "entity_memory.h"  // EntityMemory, seed_home_town_memory
#include "game_state.h"
#include "placement.h"
#include "town_brain.h"  // badlands::Behavior (the InsideBuilding::purpose id space)

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace badlands {

namespace {
// Deterministic display name from the spawn slot (a small fixed pool).
std::string hero_name(uint32_t slot) {
    static const char* kNames[] = {
        "Ada",  "Bran", "Cael", "Dara",  "Ell",  "Finn", "Gwen", "Hale",
        "Isa",  "Joss", "Kira", "Lorne", "Mira", "Nils", "Orin", "Petra",
    };
    constexpr uint32_t n = sizeof(kNames) / sizeof(kNames[0]);
    return kNames[slot % n];
}
}  // namespace

namespace {
// Map-local = world + size*0.5 (MapData spans [0, size]; the world is centred).
glm::vec2 to_map_local(const MapData& map, glm::vec2 world_xz) {
    return {world_xz.x + map.size_x_m() * 0.5f, world_xz.y + map.size_z_m() * 0.5f};
}
}  // namespace

mapgen::Biome biome_at(const BadlandsGame& game, glm::vec2 world_xz) {
    if (game.map.empty()) {
        return mapgen::Biome::Plains;
    }
    const glm::vec2 local = to_map_local(game.map, world_xz);
    return game.map.DominantBiomeAt(local.x, local.y);
}

float height_at(const BadlandsGame& game, glm::vec2 world_xz) {
    if (game.map.empty()) {
        return 0.0f;
    }
    const glm::vec2 local = to_map_local(game.map, world_xz);
    return game.map.HeightAt(local.x, local.y);
}

int32_t guild_hero_class(int kind) {
    // The recruit set (BuildingDef::recruits) is the single source of truth; a
    // guild's class is its first recruit slot, -1 for a non-guild building.
    const BuildingDef def = BuildingDefOf(static_cast<BuildingKind>(kind));
    return def.recruit_count > 0 ? static_cast<int32_t>(def.recruits[0]) : -1;
}

// HeroClassName lives with the other inspection-facing identity tables in
// activity_catalog.cpp -- it is pure naming data, and keeping it out of this
// (registry/placement-dependent) TU lets lean consumers like the factors
// manifest loader use it without pulling in the whole sim.

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
    // Fog-of-war: heroes reveal a forward cone as they patrol.
    d.vision_radius = 14.0f;
    d.vision_cone_half_angle_deg = 60.0f;
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

    // Fog-of-war facing + vision. Facing seeds from the desc (or the model
    // forward default when unset); the cone half-cosine is cos(half-angle),
    // clamped so a >=180deg half-angle is a full circle (-1).
    glm::vec2 facing0{desc.facing_x, desc.facing_z};
    if (glm::dot(facing0, facing0) < 1e-12f) {
        facing0 = kCharacterForward;
    }
    reg.emplace<Facing>(e, glm::normalize(facing0));
    const float half_cos =
        (desc.vision_cone_half_angle_deg >= 180.0f)
            ? -1.0f
            : std::cos(glm::radians(desc.vision_cone_half_angle_deg));
    reg.emplace<Vision>(e, desc.vision_radius, half_cos);

    float radius = 0.5f * std::min(desc.size_x, desc.size_z);
    reg.emplace<Agent>(e, radius);
    reg.emplace<MoveTarget>(e);
    reg.emplace<NavPath>(e);

    // --- archetype recipe: ONLY heroes carry hero state -------------------
    // This is what stops a goblin owning fatigue/inventory/a home and running
    // the townsfolk errand loop. Generic combat/movement components above are
    // archetype-independent and every entity gets them.
    BrainKind brain_kind = BrainKind::None;
    switch (desc.archetype) {
        case Archetype::Hero: {
            int32_t hero_class = -1;
            if (home >= 0 && static_cast<size_t>(home) < game.placement.buildings.size()) {
                hero_class = guild_hero_class(game.placement.buildings[home].kind);
            }
            reg.emplace<HeroCharacter>(e, hero_class);
            HeroSimulationState sim{};
            sim.inventory = 0;
            sim.home_building_id = home;
            reg.emplace<HeroSimulationState>(e, sim);
            HeroDisplayState disp{};
            disp.name = hero_name(slot);
            reg.emplace<HeroDisplayState>(e, disp);
            brain_kind = BrainKind::Town;

            // EntityMemory: consumers opt in -- the sole reader today is the
            // wasm hero brain (BrainKind::Town, wasm_brain.cpp's
            // pack_view_wire), so only heroes carry the bounded knowledge
            // sandbox (game/src/entity_memory.h). Deer/goblins/tax collectors
            // get none: update_entity_memory's observer loop is O(heroes x N)
            // rather than O(N^2) as a result. A hero with a home starts
            // already knowing it (and the town's other buildings) --
            // residents know their town.
            EntityMemory mem{};
            if (home >= 0) {
                seed_home_town_memory(game, mem, static_cast<uint32_t>(home));
            }
            reg.emplace<EntityMemory>(e, mem);
            break;
        }
        case Archetype::Townfolk: {
            TaxCollectorState tc{};
            tc.home_building_id = home;  // the Castle it set out from
            reg.emplace<TaxCollectorState>(e, std::move(tc));
            brain_kind = BrainKind::Townfolk;
            break;
        }
        case Archetype::Critter: {
            CritterState cs{};
            cs.roam_anchor = {desc.pos_x, desc.pos_z};  // wander around the spawn spot
            reg.emplace<CritterState>(e, cs);
            brain_kind = BrainKind::Critter;
            break;
        }
        case Archetype::Monster:
            brain_kind = BrainKind::Monster;
            break;
    }

    reg.emplace<Brain>(e, game.brains ? spawn_brain(*game.brains, slot) : nullptr, brain_kind);

    return slot;
}

uint32_t roster_count(const BadlandsGame& game, uint32_t building_id) {
    uint32_t n = 0;
    for (auto [e, hs] : game.registry.view<const HeroSimulationState>().each()) {
        if (hs.home_building_id == static_cast<int32_t>(building_id)) {
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

// The destruction cascade: expel occupants, tombstone + free the grid/nav
// obstacle, and rehome residents. No policy check -- combat (raze) and the
// player destroy action (destroy_building_impl) share this; the policy lives in
// the caller. Returns 0, or -1 for unknown/already-dead.
int64_t raze_building(BadlandsGame& game, uint32_t building_id) {
    auto& bs = game.placement.buildings;
    if (building_id >= bs.size() || !bs[building_id].alive) {
        return -1;  // unknown or already destroyed
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
    for (auto [e, hs] : reg.view<HeroSimulationState>().each()) {
        if (hs.home_building_id == static_cast<int32_t>(building_id)) {
            residents.push_back(e);
        }
    }
    for (entt::entity e : residents) {
        int32_t cls = reg.get<HeroCharacter>(e).hero_class;
        reg.get<HeroSimulationState>(e).home_building_id = -1;  // homeless until a slot is found
        for (uint32_t i = 0; i < bs.size(); ++i) {
            if (!bs[i].alive || guild_hero_class(bs[i].kind) != cls) {
                continue;
            }
            if (roster_count(game, i) >= static_cast<uint32_t>(kGuildRosterCap)) {
                continue;
            }
            reg.get<HeroSimulationState>(e).home_building_id = static_cast<int32_t>(i);
            break;
        }
    }
    return 0;
}

// Player destroy action: only the 7 user-buildable kinds may be razed this way.
int64_t destroy_building_impl(BadlandsGame& game, uint32_t building_id) {
    auto& bs = game.placement.buildings;
    if (building_id >= bs.size() || !bs[building_id].alive) {
        return -1;
    }
    if (!BuildingDefOf(static_cast<BuildingKind>(bs[building_id].kind)).user_destructible) {
        return -2;  // Castle/House/Sewer are not player-destructible
    }
    return raze_building(game, building_id);
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
    // Entering satisfies NOTHING on its own -- it starts the hero refilling
    // (advance_needs) at a rate, and it leaves when full. Zeroing the need at
    // the door would make an 8-hour evening at the tavern indistinguishable
    // from touching the doorframe.
    const int32_t purpose = kind == static_cast<int>(BuildingKind::Tavern)
                                ? static_cast<int32_t>(Behavior::VisitTavern)
                                : static_cast<int32_t>(Behavior::GoHome);
    game.registry.emplace_or_replace<InsideBuilding>(e, static_cast<int32_t>(bid), purpose);
    return true;
}

// Reachable from CommandKind::EnterHome, which any noiser script can enqueue
// against any archetype (intent_enter_home has no archetype guard of its own
// -- see brain.cpp) -- so `e` is not guaranteed to be a hero here either.
bool hero_enter_home(BadlandsGame& game, entt::entity e) {
    const auto* sim = game.registry.try_get<HeroSimulationState>(e);
    if (sim == nullptr) {
        return false;  // non-hero: no home to enter
    }
    int32_t home = sim->home_building_id;
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
    game.registry.emplace_or_replace<InsideBuilding>(
        e, home, static_cast<int32_t>(Behavior::GoHome));
    // Sleep refills fatigue over time (advance_needs), it does not clear it at
    // the door. Emptying the pack is instantaneous and unrelated.
    game.registry.get<HeroSimulationState>(e).inventory = 0;
    return true;
}

// Same non-hero-reachability note as hero_enter_home above (CommandKind::Buy).
bool hero_buy(BadlandsGame& game, entt::entity e) {
    if (!game.registry.all_of<HeroSimulationState>(e)) {
        return false;  // non-hero: nothing to buy for
    }
    uint32_t bid = 0;
    if (!at_building_of_kind(game, e, static_cast<int32_t>(BuildingKind::Apothecary), bid)) {
        return false;
    }
    game.registry.get<HeroSimulationState>(e).inventory = kInventoryCap;
    return true;
}

bool should_leave_building(const BadlandsGame& game, entt::entity e,
                           const InsideBuilding& inside) {
    const auto* sim = game.registry.try_get<HeroSimulationState>(e);
    if (sim == nullptr) {
        return true;  // not a hero: nothing to be in there for
    }
    // The building went away underneath them (razed while they slept).
    const auto& bs = game.placement.buildings;
    if (inside.building_id < 0 || static_cast<size_t>(inside.building_id) >= bs.size() ||
        !bs[inside.building_id].alive) {
        return true;
    }
    // Done when the reserve it came for is full. `>= 1` rather than a timer, so
    // the stay lasts exactly as long as the hero needed it to.
    //
    // Everything else that should end a stay goes here: home under attack, a
    // threat at the door, being summoned, dawn.
    if (inside.purpose == static_cast<int32_t>(Behavior::VisitTavern)) {
        return sim->content >= 1.0f;
    }
    return sim->fatigue >= 1.0f;
}

void advance_inside(BadlandsGame& game) {
    entt::registry& reg = game.registry;
    std::vector<entt::entity> exit;
    for (auto [e, ib] : reg.view<InsideBuilding>().each()) {
        if (should_leave_building(game, e, ib)) {
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

void advance_chats(BadlandsGame& game, float dt) {
    entt::registry& reg = game.registry;
    const HeroFactors& hf = game.factors.hero;

    // Collect first, mutate after: dissolving a session touches the partner
    // too, and removing components mid-view is not safe.
    std::vector<entt::entity> ending;
    for (auto [e, chat] : reg.view<ChattingState>().each()) {
        chat.remaining -= dt;
        const entt::entity partner =
            entity_for_slot(game, static_cast<int32_t>(chat.partner_slot));

        bool over = chat.remaining <= 0.0f;
        // The partner left -- died, went inside, or was never valid.
        if (!over && (partner == entt::null || reg.all_of<InsideBuilding>(partner) ||
                      !reg.all_of<ChattingState>(partner))) {
            over = true;
        }
        // They drifted apart. Generous compared to the radius that started it,
        // so a nudge from unit separation does not end the conversation.
        if (!over && glm::distance(reg.get<Position>(e).pos, reg.get<Position>(partner).pos) >
                         hf.chat_radius * 2.0f) {
            over = true;
        }
        // Something hostile turned up: company is the first thing to go.
        if (!over && nearest_enemy(game, e) != entt::null) {
            over = true;
        }
        if (over) {
            ending.push_back(e);
        }
    }

    for (entt::entity e : ending) {
        if (!reg.valid(e) || !reg.all_of<ChattingState>(e)) {
            continue;  // already dissolved as somebody else's partner
        }
        const entt::entity partner =
            entity_for_slot(game, static_cast<int32_t>(reg.get<ChattingState>(e).partner_slot));
        reg.remove<ChattingState>(e);
        // Both sides always leave together: a one-sided conversation would let
        // the abandoned hero stand there talking to nobody.
        if (partner != entt::null && reg.all_of<ChattingState>(partner)) {
            reg.remove<ChattingState>(partner);
        }
    }
}

}  // namespace badlands
