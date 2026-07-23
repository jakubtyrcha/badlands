// The determinism contract: state_after = f(seed, initial config, command log,
// N ticks). These are the architecture's load-bearing tests -- everything else
// (needs, brains, the day/night loop) is policy layered on top, and every future
// system inherits the guarantee only if these keep passing.
//
// Two properties, tested separately:
//   1. Reproducible  -- the same inputs twice produce the same state AND the
//                       same command log (nothing depends on real time, address
//                       order, or unseeded randomness).
//   2. Replayable    -- feeding a recorded log back into a fresh sim, with the
//                       brains switched off, reproduces the state exactly. This
//                       is what makes the log a TRACE and not just a debug list.

#include "sim_internal.hpp"
#include "command.h"
#include "components.h"
#include "game_state.h"
#include "vision.h"

#include <catch_amalgamated.hpp>

#include <string>
#include <vector>

using namespace badlands;

namespace {

constexpr int kRunTicks = 400;  // long enough for needs to cross thresholds

// Seeds a town + roster through the player command path, exactly as the
// ai_sandbox does, so the seed itself is part of the recorded log.
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
            Action recruit{ActionKind::RecruitHero, static_cast<uint32_t>(id),
                               0.0f, 0.0f, 0, 0};
            REQUIRE(dispatch_into(*g, recruit) >= 0);
        }
    }
}

// The observable state, read only through the Sim snapshot API.
struct Snapshot {
    std::vector<CharacterState> characters;
    WorldState world{};
    uint64_t ticks = 0;
};

Snapshot snapshot(BadlandsGame* g) {
    Snapshot s;
    s.characters = characters_of(*g);
    s.world = world_of(*g);
    s.ticks = stats_of(*g).ticks;
    return s;
}

void require_same(const Snapshot& a, const Snapshot& b) {
    REQUIRE(a.characters.size() == b.characters.size());
    REQUIRE(a.ticks == b.ticks);
    CHECK(a.world.world_millis == b.world.world_millis);
    CHECK(a.world.gold == b.world.gold);
    for (size_t i = 0; i < a.characters.size(); ++i) {
        const CharacterState& x = a.characters[i];
        const CharacterState& y = b.characters[i];
        INFO("character row " << i << " (" << x.name << ")");
        CHECK(x.id == y.id);
        CHECK(x.pos_x == y.pos_x);  // bit-exact: no tolerance on a determinism test
        CHECK(x.pos_z == y.pos_z);
        CHECK(x.hp == y.hp);
        CHECK(x.fatigue == y.fatigue);
        CHECK(x.boredom == y.boredom);
        CHECK(x.behavior == y.behavior);
        CHECK(x.inside_building_id == y.inside_building_id);
        CHECK(std::string(x.name) == std::string(y.name));
    }
}

bool same_command(const Command& a, const Command& b) {
    return a.kind == b.kind && a.actor == b.actor && a.target_id == b.target_id &&
           a.point == b.point && a.param_a == b.param_a && a.param_b == b.param_b &&
           a.at_millis == b.at_millis;
}

}  // namespace

TEST_CASE("the same inputs produce the same state and the same command log") {
    auto a_owned = make_world(nullptr);
    BadlandsGame* a = a_owned.get();
    auto b_owned = make_world(nullptr);
    BadlandsGame* b = b_owned.get();
    seed_town(a);
    seed_town(b);
    for (int i = 0; i < kRunTicks; ++i) {
        tick_world(*a, 1.0f / 30.0f);
        tick_world(*b, 1.0f / 30.0f);
    }

    require_same(snapshot(a), snapshot(b));

    // The trace is reproducible too -- not just the state it produced.
    REQUIRE(a->command_log.size() == b->command_log.size());
    REQUIRE(!a->command_log.empty());
    for (size_t i = 0; i < a->command_log.size(); ++i) {
        INFO("command " << i);
        CHECK(same_command(a->command_log[i], b->command_log[i]));
    }

        }

TEST_CASE("a recorded command log replays into a fresh sim exactly") {
    auto live_owned = make_world(nullptr);
    BadlandsGame* live = live_owned.get();
    seed_town(live);
    for (int i = 0; i < kRunTicks; ++i) {
        tick_world(*live, 1.0f / 30.0f);
    }
    const std::vector<Command> log = live->command_log;
    REQUIRE(!log.empty());

    // Fresh sim, brains OFF: every decision comes from the log. Note there is no
    // seed_town call -- the seed is IN the log, so the world is rebuilt from the
    // trace alone.
    auto replay_owned = make_world(nullptr);
    BadlandsGame* replay = replay_owned.get();
    replay->replay_log = &log;
    for (int i = 0; i < kRunTicks; ++i) {
        tick_world(*replay, 1.0f / 30.0f);
    }

    CHECK(replay->replay_cursor == log.size());  // the whole trace was consumed
    require_same(snapshot(live), snapshot(replay));

        }

TEST_CASE("a run with fog of war and explorers replays exactly") {
    // Exploration is the most replay-fragile thing the AI does: its decisions
    // depend on a seeded appetite draw AND on a query over the fog-of-war grid,
    // which is itself a product of where everyone walked. If either leaked a
    // dependency on anything outside (initial config, seed, command log), this
    // is where it would show.
    //
    // The vision grid is INITIAL CONFIG, so both worlds configure it the same
    // way -- exactly as a real replay would have to.
    auto seed_world = [](BadlandsGame* g) {
        configure_vision(g->vision, -128.0f, -128.0f, 256.0f, 256.0f, 1.0f);
        Action camp{ActionKind::PlaceBuilding, 0, -14.0f, 44.0f,
                    static_cast<int32_t>(BuildingKind::HuntersCamp), 0};
        const int64_t id = dispatch_into(*g, camp);
        REQUIRE(id >= 0);
        for (int i = 0; i < 3; ++i) {
            Action hire{ActionKind::RecruitHero, static_cast<uint32_t>(id), 0.0f, 0.0f, 0, 0};
            REQUIRE(dispatch_into(*g, hire) >= 0);
        }
    };

    auto live_owned = make_world(nullptr);
    BadlandsGame* live = live_owned.get();
    seed_world(live);
    for (int i = 0; i < kRunTicks; ++i) {
        tick_world(*live, 1.0f / 30.0f);
    }
    const std::vector<Command> log = live->command_log;

    // The hunters must actually have explored, or this proves nothing.
    bool explored = false;
    for (const Command& c : log) {
        explored = explored || (c.kind == CommandKind::SetBehavior &&
                                c.param_a == static_cast<int32_t>(ActivityId::Explore));
    }
    REQUIRE(explored);

    // Same inputs again -> same trace, bit for bit.
    auto twin_owned = make_world(nullptr);
    BadlandsGame* twin = twin_owned.get();
    seed_world(twin);
    for (int i = 0; i < kRunTicks; ++i) {
        tick_world(*twin, 1.0f / 30.0f);
    }
    REQUIRE(twin->command_log.size() == log.size());
    for (size_t i = 0; i < log.size(); ++i) {
        INFO("command " << i);
        CHECK(same_command(twin->command_log[i], log[i]));
    }
    require_same(snapshot(live), snapshot(twin));

    // And the recorded trace, replayed with the brains off, rebuilds the world.
    auto replay_owned = make_world(nullptr);
    BadlandsGame* replay = replay_owned.get();
    configure_vision(replay->vision, -128.0f, -128.0f, 256.0f, 256.0f, 1.0f);
    replay->replay_log = &log;
    for (int i = 0; i < kRunTicks; ++i) {
        tick_world(*replay, 1.0f / 30.0f);
    }
    CHECK(replay->replay_cursor == log.size());
    require_same(snapshot(live), snapshot(replay));
}

TEST_CASE("replayed commands re-log identically (the trace round-trips)") {
    auto live_owned = make_world(nullptr);
    BadlandsGame* live = live_owned.get();
    seed_town(live);
    for (int i = 0; i < 120; ++i) {
        tick_world(*live, 1.0f / 30.0f);
    }
    const std::vector<Command> log = live->command_log;

    auto replay_owned = make_world(nullptr);
    BadlandsGame* replay = replay_owned.get();
    replay->replay_log = &log;
    for (int i = 0; i < 120; ++i) {
        tick_world(*replay, 1.0f / 30.0f);
    }

    REQUIRE(replay->command_log.size() == log.size());
    for (size_t i = 0; i < log.size(); ++i) {
        INFO("command " << i);
        CHECK(same_command(replay->command_log[i], log[i]));
    }

        }
