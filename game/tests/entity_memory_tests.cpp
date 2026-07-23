// EntityMemory (game/src/entity_memory.h): the host-owned per-character
// knowledge sandbox. Pure derived state driven by tick_world -- these tests
// exercise the update pass (visibility/TTL/eviction), spawn seeding, and the
// determinism contract, entirely through the internal BadlandsGame fixture
// style shared with heroes_tests.cpp / vision_tests.cpp / determinism_tests.cpp.

#include "components.h"
#include "entity_memory.h"
#include "game_state.h"
#include "sim_internal.hpp"  // make_world / spawn_into / dispatch_into / tick_world

#include <catch_amalgamated.hpp>
#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <algorithm>
#include <vector>

using namespace badlands;

namespace {

// A minimal, deliberately immobile character (move_speed left at its
// zero-initialized default): archetype/team/hp are all these tests need, and
// zero move_speed means it cannot drift from its spawn position regardless
// of whatever its archetype's own brain decides to do, keeping every test's
// geometry exact across the tick(s) it runs.
CharacterDesc scout(float x, float z, float vision_radius) {
    CharacterDesc d{};
    d.archetype = Archetype::Monster;
    d.pos_x = x;
    d.pos_z = z;
    d.team = 0;
    d.hp = 10.0f;
    d.size_x = d.size_y = d.size_z = 1.0f;
    d.vision_radius = vision_radius;
    d.vision_cone_half_angle_deg = 180.0f;  // irrelevant: v1 memory is radius-only
    return d;
}

uint32_t place_at(BadlandsGame* g, BuildingKind kind, float x, float z) {
    Action a{ActionKind::PlaceBuilding, 0, x, z, static_cast<int32_t>(kind), 0};
    int64_t r = dispatch_into(*g, a);
    return (r < 0) ? UINT32_MAX : static_cast<uint32_t>(r);
}

uint32_t recruit_at(BadlandsGame* g, uint32_t bid) {
    Action a{ActionKind::RecruitHero, bid, 0.0f, 0.0f, 0, 0};
    int64_t r = dispatch_into(*g, a);
    return (r < 0) ? UINT32_MAX : static_cast<uint32_t>(r);
}

int64_t destroy_at(BadlandsGame* g, uint32_t bid) {
    Action a{ActionKind::DestroyBuilding, bid, 0.0f, 0.0f, 0, 0};
    return dispatch_into(*g, a);
}

const MemoryChar* find_char(const EntityMemory& mem, uint32_t slot) {
    for (int32_t i = 0; i < mem.char_count; ++i) {
        if (mem.chars[i].slot == slot) {
            return &mem.chars[i];
        }
    }
    return nullptr;
}

const MemoryChar* find_char(BadlandsGame& g, entt::entity observer, uint32_t slot) {
    return find_char(g.registry.get<EntityMemory>(observer), slot);
}

const MemoryBuilding* find_building(const EntityMemory& mem, uint32_t id) {
    for (int32_t i = 0; i < mem.building_count; ++i) {
        if (mem.buildings[i].id == id) {
            return &mem.buildings[i];
        }
    }
    return nullptr;
}

// Seeds a small town + a 3-hero roster through the player command path
// (mirrors determinism_tests.cpp's seed_town, duplicated per this file's own
// fixture-independence convention).
void seed_town(BadlandsGame* g) {
    struct Seed {
        int kind;
        float x, z;
    };
    const Seed kSeed[] = {
        {static_cast<int32_t>(BuildingKind::FreeCompanyQuarters), -14.0f, -8.0f},
        {static_cast<int32_t>(BuildingKind::Tavern), 14.0f, -8.0f},
        {static_cast<int32_t>(BuildingKind::Apothecary), 14.0f, 8.0f},
    };
    for (const Seed& s : kSeed) {
        Action place{ActionKind::PlaceBuilding, 0, s.x, s.z, s.kind, 0};
        int64_t id = dispatch_into(*g, place);
        REQUIRE(id >= 0);
        if (s.kind != static_cast<int32_t>(BuildingKind::FreeCompanyQuarters)) {
            continue;
        }
        for (int i = 0; i < 3; ++i) {
            Action recruit{ActionKind::RecruitHero, static_cast<uint32_t>(id), 0.0f, 0.0f, 0, 0};
            REQUIRE(dispatch_into(*g, recruit) >= 0);
        }
    }
}

std::vector<MemoryChar> sorted_chars(const EntityMemory& mem) {
    std::vector<MemoryChar> v(mem.chars, mem.chars + mem.char_count);
    std::sort(v.begin(), v.end(),
              [](const MemoryChar& a, const MemoryChar& b) { return a.slot < b.slot; });
    return v;
}

std::vector<MemoryBuilding> sorted_buildings(const EntityMemory& mem) {
    std::vector<MemoryBuilding> v(mem.buildings, mem.buildings + mem.building_count);
    std::sort(v.begin(), v.end(),
              [](const MemoryBuilding& a, const MemoryBuilding& b) { return a.id < b.id; });
    return v;
}

}  // namespace

TEST_CASE("two characters within vision radius remember each other after one tick") {
    auto g = make_world(nullptr);
    uint32_t a = spawn_into(*g, scout(0.0f, 0.0f, 14.0f));
    uint32_t b = spawn_into(*g, scout(5.0f, 0.0f, 14.0f));
    tick_world(*g, 1.0f / 30.0f);

    const MemoryChar* a_sees_b = find_char(*g, g->slots[a], b);
    REQUIRE(a_sees_b != nullptr);
    CHECK(a_sees_b->visible_now);
    CHECK(a_sees_b->archetype == static_cast<int32_t>(Archetype::Monster));
    CHECK(a_sees_b->team == 0);
    CHECK(a_sees_b->last_pos == glm::vec2(5.0f, 0.0f));
    CHECK(a_sees_b->last_hp == 10.0f);
    CHECK(a_sees_b->last_seen_millis == g->world_millis);

    const MemoryChar* b_sees_a = find_char(*g, g->slots[b], a);
    REQUIRE(b_sees_a != nullptr);
    CHECK(b_sees_a->visible_now);
    CHECK(b_sees_a->last_pos == glm::vec2(0.0f, 0.0f));
}

TEST_CASE("characters outside vision radius are not remembered") {
    auto g = make_world(nullptr);
    uint32_t a = spawn_into(*g, scout(0.0f, 0.0f, 10.0f));
    spawn_into(*g, scout(100.0f, 100.0f, 10.0f));
    tick_world(*g, 1.0f / 30.0f);

    const EntityMemory& mem = g->registry.get<EntityMemory>(g->slots[a]);
    CHECK(mem.char_count == 0);
}

TEST_CASE("a missing or zero-radius Vision means seeing nothing this tick") {
    auto g = make_world(nullptr);
    uint32_t a = spawn_into(*g, scout(0.0f, 0.0f, 0.0f));  // radius 0
    spawn_into(*g, scout(1.0f, 0.0f, 14.0f));
    tick_world(*g, 1.0f / 30.0f);
    CHECK(g->registry.get<EntityMemory>(g->slots[a]).char_count == 0);
}

TEST_CASE("persistence: a teleported target is remembered stale, then forgotten past the TTL") {
    auto g = make_world(nullptr);
    uint32_t a = spawn_into(*g, scout(0.0f, 0.0f, 20.0f));
    uint32_t b = spawn_into(*g, scout(5.0f, 0.0f, 0.0f));
    tick_world(*g, 1.0f / 30.0f);  // A sees B

    entt::entity ae = g->slots[a];
    entt::entity be = g->slots[b];
    const glm::vec2 b_seen_pos = g->registry.get<Position>(be).pos;
    const int64_t seen_at = g->world_millis;

    const MemoryChar* rec = find_char(*g, ae, b);
    REQUIRE(rec != nullptr);
    CHECK(rec->visible_now);
    CHECK(rec->last_pos == b_seen_pos);

    // Teleport B far out of A's radius directly via the registry (a test).
    g->registry.get<Position>(be).pos = glm::vec2(500.0f, 500.0f);
    tick_world(*g, 1.0f / 30.0f);

    rec = find_char(*g, ae, b);
    REQUIRE(rec != nullptr);
    CHECK_FALSE(rec->visible_now);
    CHECK(rec->last_pos == b_seen_pos);       // stale: the OLD position
    CHECK(rec->last_seen_millis == seen_at);  // not refreshed

    // Advance sim time past memory_ttl_millis: the entry is forgotten.
    const int64_t ttl = g->factors.hero.memory_ttl_millis;
    while (g->world_millis - seen_at <= ttl) {
        tick_world(*g, 1.0f / 30.0f);
    }
    CHECK(find_char(*g, ae, b) == nullptr);
}

TEST_CASE("capacity: a 17th simultaneous sighting is dropped, not an incumbent") {
    auto g = make_world(nullptr);
    uint32_t obs = spawn_into(*g, scout(0.0f, 0.0f, 100.0f));
    std::vector<uint32_t> targets;
    for (int i = 0; i < 17; ++i) {
        targets.push_back(spawn_into(*g, scout(static_cast<float>(i + 1), 0.0f, 0.0f)));
    }
    tick_world(*g, 1.0f / 30.0f);

    entt::entity oe = g->slots[obs];
    const EntityMemory& mem = g->registry.get<EntityMemory>(oe);
    CHECK(mem.char_count == BL_MAX_CHARS);
    for (int i = 0; i < 16; ++i) {
        CHECK(find_char(mem, targets[i]) != nullptr);
    }
    CHECK(find_char(mem, targets[16]) == nullptr);  // the largest slot: dropped

}

TEST_CASE("a recruited hero starts knowing its home and the castle") {
    auto owned = make_world(nullptr);  // prebuilds the Castle (id 0) at kCastleSpawn
    BadlandsGame* g = owned.get();
    uint32_t guild = place_at(g, BuildingKind::FreeCompanyQuarters, -30.0f, 30.0f);
    REQUIRE(guild != UINT32_MAX);
    uint32_t hid = recruit_at(g, guild);
    REQUIRE(hid != UINT32_MAX);

    const EntityMemory& mem = g->registry.get<EntityMemory>(g->slots[hid]);
    const MemoryBuilding* home = find_building(mem, guild);
    REQUIRE(home != nullptr);
    CHECK(home->is_home);
    CHECK(home->alive);

    const MemoryBuilding* castle = find_building(mem, 0);
    REQUIRE(castle != nullptr);
    CHECK_FALSE(castle->is_home);
    CHECK(castle->alive);
}

TEST_CASE("a homeless spawn starts with an empty EntityMemory") {
    auto g = make_world(nullptr);
    uint32_t goblin = spawn_into(*g, GoblinDesc(10.0f, 10.0f));
    const EntityMemory& mem = g->registry.get<EntityMemory>(g->slots[goblin]);
    CHECK(mem.char_count == 0);
    CHECK(mem.building_count == 0);
}

TEST_CASE("building sighting: recorded when seen, alive flips false when destroyed in sight") {
    auto owned = make_world(nullptr);
    BadlandsGame* g = owned.get();
    uint32_t obs = spawn_into(*g, scout(-40.0f, -40.0f, 12.0f));  // far from the castle (0,54)
    uint32_t bid = place_at(g, BuildingKind::Tavern, -40.0f, -35.0f);  // ~5 units away
    REQUIRE(bid != UINT32_MAX);

    tick_world(*g, 1.0f / 30.0f);
    entt::entity oe = g->slots[obs];
    const MemoryBuilding* rec = find_building(g->registry.get<EntityMemory>(oe), bid);
    REQUIRE(rec != nullptr);
    CHECK(rec->alive);
    CHECK(rec->kind == static_cast<int32_t>(BuildingKind::Tavern));

    REQUIRE(destroy_at(g, bid) == 0);
    tick_world(*g, 1.0f / 30.0f);
    rec = find_building(g->registry.get<EntityMemory>(oe), bid);
    REQUIRE(rec != nullptr);
    CHECK_FALSE(rec->alive);
}

TEST_CASE("building sighting: destroyed out of sight keeps the memory's stale alive=true") {
    auto owned = make_world(nullptr);
    BadlandsGame* g = owned.get();
    uint32_t obs = spawn_into(*g, scout(-40.0f, -40.0f, 12.0f));
    uint32_t bid = place_at(g, BuildingKind::Tavern, -40.0f, -35.0f);
    REQUIRE(bid != UINT32_MAX);
    tick_world(*g, 1.0f / 30.0f);
    entt::entity oe = g->slots[obs];
    REQUIRE(find_building(g->registry.get<EntityMemory>(oe), bid) != nullptr);

    // Move the observer far away, out of sight, THEN destroy the building.
    g->registry.get<Position>(oe).pos = glm::vec2(500.0f, 500.0f);
    tick_world(*g, 1.0f / 30.0f);
    REQUIRE(destroy_at(g, bid) == 0);
    tick_world(*g, 1.0f / 30.0f);

    const MemoryBuilding* rec = find_building(g->registry.get<EntityMemory>(oe), bid);
    REQUIRE(rec != nullptr);
    CHECK(rec->alive);  // stale: last seen before the destruction

}

TEST_CASE("EntityMemory is deterministic: two identical runs match after sorting") {
    auto a_owned = make_world(nullptr);
    auto b_owned = make_world(nullptr);
    BadlandsGame* a = a_owned.get();
    BadlandsGame* b = b_owned.get();
    seed_town(a);
    seed_town(b);
    for (int i = 0; i < 50; ++i) {
        tick_world(*a, 1.0f / 30.0f);
        tick_world(*b, 1.0f / 30.0f);
    }

    REQUIRE(a->slots.size() == b->slots.size());
    for (uint32_t slot = 0; slot < a->slots.size(); ++slot) {
        entt::entity ea = a->slots[slot];
        entt::entity eb = b->slots[slot];
        REQUIRE(a->registry.valid(ea) == b->registry.valid(eb));
        if (!a->registry.valid(ea)) {
            continue;
        }
        const EntityMemory& ma = a->registry.get<EntityMemory>(ea);
        const EntityMemory& mb = b->registry.get<EntityMemory>(eb);

        auto ca = sorted_chars(ma);
        auto cb = sorted_chars(mb);
        REQUIRE(ca.size() == cb.size());
        for (size_t i = 0; i < ca.size(); ++i) {
            INFO("slot " << slot << " char row " << i);
            CHECK(ca[i].slot == cb[i].slot);
            CHECK(ca[i].archetype == cb[i].archetype);
            CHECK(ca[i].team == cb[i].team);
            CHECK(ca[i].last_pos == cb[i].last_pos);
            CHECK(ca[i].last_hp == cb[i].last_hp);
            CHECK(ca[i].visible_now == cb[i].visible_now);
            CHECK(ca[i].last_seen_millis == cb[i].last_seen_millis);
        }

        auto bda = sorted_buildings(ma);
        auto bdb = sorted_buildings(mb);
        REQUIRE(bda.size() == bdb.size());
        for (size_t i = 0; i < bda.size(); ++i) {
            INFO("slot " << slot << " building row " << i);
            CHECK(bda[i].id == bdb[i].id);
            CHECK(bda[i].kind == bdb[i].kind);
            CHECK(bda[i].door == bdb[i].door);
            CHECK(bda[i].alive == bdb[i].alive);
            CHECK(bda[i].is_home == bdb[i].is_home);
            CHECK(bda[i].last_seen_millis == bdb[i].last_seen_millis);
        }
    }
}

TEST_CASE("a smaller memory_ttl_millis factor forgets a stale sighting sooner") {
    auto g = make_world(nullptr);
    // Sim::SetFactors (sim.cpp) is exactly `game.factors = f`; mutating the
    // field directly here is the same operation, consistent with this
    // suite's BadlandsGame*-fixture style (make_world/tick_world, not the
    // Sim wrapper).
    g->factors.hero.memory_ttl_millis = 200;  // ~6 ticks, instead of the 10s default
    uint32_t a = spawn_into(*g, scout(0.0f, 0.0f, 20.0f));
    uint32_t b = spawn_into(*g, scout(5.0f, 0.0f, 0.0f));
    tick_world(*g, 1.0f / 30.0f);  // A sees B

    entt::entity ae = g->slots[a];
    REQUIRE(find_char(*g, ae, b) != nullptr);

    // Move B out of range, then advance a bounded number of ticks: the
    // shrunk TTL must forget it well within the ticks the 10s default would
    // have taken (~300).
    g->registry.get<Position>(g->slots[b]).pos = glm::vec2(500.0f, 500.0f);
    int ticks = 0;
    while (find_char(*g, ae, b) != nullptr && ticks < 30) {
        tick_world(*g, 1.0f / 30.0f);
        ++ticks;
    }
    CHECK(find_char(*g, ae, b) == nullptr);
    CHECK(ticks < 30);
}
