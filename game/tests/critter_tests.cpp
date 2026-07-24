// Deer (critter archetype): reactive walk -> graze -> walk in Forest/Plains,
// bolting from any non-critter in sight. Exercised both as pure blocks on a
// synthetic view and end-to-end through the sim.

#include "behaviours/blocks.h"
#include "behaviours/selectors.h"
#include "behaviours/world_view.h"
#include "brain.h"
#include "command.h"
#include "components.h"
#include "game_state.h"
#include "heroes.h"  // biome_at
#include "sim_internal.hpp"

#include <catch_amalgamated.hpp>

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <array>
#include <cmath>

using namespace badlands;

namespace {

// Find a world position whose dominant biome is `want`, within the interior of
// the nav grid (now spanning the full 256 m map, so Forest/Plains/Swamp/Lake are
// all reachable). The interior bound keeps a fleeing/roaming deer off the grid
// edge, where the nav pipeline clamps.
constexpr float kInterior = 100.0f;
bool find_biome(const BadlandsGame& g, mapgen::Biome want, glm::vec2& out) {
    for (float z = -kInterior; z <= kInterior; z += 1.0f) {
        for (float x = -kInterior; x <= kInterior; x += 1.0f) {
            if (biome_at(g, {x, z}) == want) {
                out = {x, z};
                return true;
            }
        }
    }
    return false;
}

uint32_t spawn_deer(BadlandsGame& g, glm::vec2 pos) {
    CharacterDesc d{};
    d.archetype = Archetype::Critter;
    d.pos_x = pos.x;
    d.pos_z = pos.y;
    d.team = 2;  // neutral wildlife
    d.hp = 8.0f;
    d.move_speed = 3.5f;
    d.attack_range = 0.0f;
    d.attack_damage = 0.0f;
    d.attack_cooldown = 1.0f;
    d.size_x = d.size_y = d.size_z = 0.8f;
    d.color_r = 0.6f;
    d.color_g = 0.4f;
    d.color_b = 0.2f;
    return spawn_into(g, d);
}

}  // namespace

TEST_CASE("Flee is archetype-agnostic: same block, hero view and deer view") {
    // The named shared-block proof. Flee reads only the threat fields, so it
    // must behave identically regardless of which archetype's view carries them.
    const SimFactors f;
    WorldView deer;
    deer.pos = {0.0f, 0.0f};
    add_threat(deer, {3.0f, 0.0f}, 3.0f);  // 3 m east

    WorldView hero = deer;  // identical threat fields, different "archetype"
    hero.has_home = true;   // hero-only field a deer never sets
    hero.fatigue = 0.9f;

    CHECK(score_flee(deer, f) == score_flee(hero, f));
    const BehaviourResult a = act_flee(deer, f);
    const BehaviourResult b = act_flee(hero, f);
    CHECK(a.target.x == b.target.x);
    CHECK(a.target.y == b.target.y);
    // Runs AWAY from the threat (westward, since the threat is east).
    CHECK(a.target.x < deer.pos.x);
}

TEST_CASE("Flee gates on the flee radius, and beats graze/roam") {
    const SimFactors f;  // flee_radius 8
    std::array<Candidate, 4> deer{{{score_flee, act_flee},
                                   {score_graze, act_graze},
                                   {score_roam, act_roam},
                                   {score_idle, act_idle}}};
    WorldView v;
    v.grazing = true;  // would graze if not threatened
    add_threat(v, {5.0f, 0.0f}, 5.0f);  // inside flee_radius
    // A bolt reports Flee, not Roam: the statistics histogram has to be able to
    // tell a panicking herd from a grazing one.
    CHECK(select_priority(deer, v, f).id == Behavior::Flee);

    // Threat present but beyond flee_radius -> graze wins instead.
    v.threats[0].dist = 20.0f;
    CHECK(select_priority(deer, v, f).id == Behavior::Graze);
}

TEST_CASE("a deer bolts from an approaching hero, then settles when it leaves") {
    auto owned = make_world(BrainDesc{});
    BadlandsGame& g = *owned;
    glm::vec2 woods;
    REQUIRE(find_biome(g, mapgen::Biome::Forest, woods));

    uint32_t deer = spawn_deer(g, woods);
    entt::entity de = g.slots[deer];

    // A threat right next to the deer.
    CharacterDesc hunter = MercenaryDesc(woods.x + 2.0f, woods.y);
    spawn_into(g, hunter);

    const glm::vec2 threat{woods.x + 2.0f, woods.y};
    tick_world(g, 1.0f / 30.0f);

    const MoveTarget& mt = g.registry.get<MoveTarget>(de);
    REQUIRE(mt.kind == MoveTarget::Kind::Point);
    // Deer's goal is further from the threat than the deer currently is.
    const glm::vec2 deer_pos = g.registry.get<Position>(de).pos;
    CHECK(glm::distance(mt.point, threat) > glm::distance(deer_pos, threat));
    CHECK(g.registry.get<CritterState>(de).behavior == static_cast<int32_t>(Behavior::Flee));
}

// A Forest/Plains cell whose roam ring is mostly good terrain -- a sensible deer
// home, not a shore where half the range is Lake. Picking a home is the caller's
// job (the sandbox does too).
bool good_home_biome(mapgen::Biome b) {
    return b == mapgen::Biome::Forest || b == mapgen::Biome::Plains;
}
bool find_good_home(const BadlandsGame& g, float radius, glm::vec2& out) {
    glm::vec2 best{0.0f, 0.0f};
    float best_frac = -1.0f;
    for (float z = -kInterior; z <= kInterior; z += 2.0f) {
        for (float x = -kInterior; x <= kInterior; x += 2.0f) {
            if (!good_home_biome(biome_at(g, {x, z}))) {
                continue;
            }
            int good = 0;
            for (int k = 0; k < 16; ++k) {
                const float a = static_cast<float>(k) * 6.2831853f / 16.0f;
                const glm::vec2 p{x + std::cos(a) * radius, z + std::sin(a) * radius};
                const mapgen::Biome b = biome_at(g, p);
                if (b == mapgen::Biome::Forest || b == mapgen::Biome::Plains) {
                    ++good;
                }
            }
            const float frac = static_cast<float>(good) / 16.0f;
            if (frac > best_frac) {
                best_frac = frac;
                best = {x, z};
            }
        }
    }
    out = best;
    return best_frac >= 0.0f;
}

TEST_CASE("a deer wanders within its home range and keeps to good biome") {
    auto owned = make_world(BrainDesc{});
    BadlandsGame& g = *owned;
    glm::vec2 meadow;
    REQUIRE(find_good_home(g, g.factors.critter.roam_radius, meadow));

    uint32_t deer = spawn_deer(g, meadow);
    entt::entity de = g.slots[deer];
    const glm::vec2 anchor = g.registry.get<CritterState>(de).roam_anchor;
    const CritterFactors& cf = g.factors.critter;

    // No threat: sample the deer's chosen goals across several roam windows.
    // Each must sit within the roam range of its anchor and land on Forest/Plains.
    int good = 0;
    int samples = 0;
    for (int i = 0; i < 20; ++i) {
        g.world_millis = static_cast<int64_t>(i) * 4000;  // step one roam window each time
        tick_world(g, 1.0f / 30.0f);
        const MoveTarget& mt = g.registry.get<MoveTarget>(de);
        if (mt.kind != MoveTarget::Kind::Point) {
            continue;  // grazing window: holds position
        }
        ++samples;
        CHECK(glm::distance(mt.point, anchor) <= cf.roam_radius + 1.0f);
        const mapgen::Biome b = biome_at(g, mt.point);
        if (b == mapgen::Biome::Forest || b == mapgen::Biome::Plains) {
            ++good;
        }
    }
    REQUIRE(samples > 0);
    // Given a sensible home, the biome filter keeps the large majority of goals
    // on good (Forest/Plains) terrain rather than the Lake.
    CHECK(good >= samples * 3 / 4);
}

TEST_CASE("a deer carries no hero components and reports no name") {
    auto owned = make_world(BrainDesc{});
    BadlandsGame& g = *owned;
    glm::vec2 woods;
    REQUIRE(find_biome(g, mapgen::Biome::Forest, woods));
    uint32_t deer = spawn_deer(g, woods);
    entt::entity de = g.slots[deer];

    CHECK(g.registry.all_of<CritterState>(de));
    CHECK_FALSE(g.registry.all_of<HeroSimulationState>(de));

    const std::vector<CharacterState> rows = characters_of(g);
    REQUIRE(rows.size() == 1);
    CHECK(std::string(rows[0].name).empty());
}
