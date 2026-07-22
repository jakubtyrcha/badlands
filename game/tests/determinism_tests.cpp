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

#include "badlands_game.h"
#include "command.h"
#include "components.h"
#include "game_state.h"

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
        {GAME_BUILDING_FREE_COMPANY_QUARTERS, -14.0f, -8.0f},
        {GAME_BUILDING_TAVERN, 14.0f, -8.0f},
        {GAME_BUILDING_APOTHECARY, 14.0f, 8.0f},
    };
    for (const Seed& s : kSeed) {
        GameAction place{GAME_ACTION_PLACE_BUILDING, 0, s.x, s.z, s.kind, 0};
        int64_t id = game_dispatch(g, &place);
        REQUIRE(id >= 0);
        if (s.kind != GAME_BUILDING_FREE_COMPANY_QUARTERS) {
            continue;
        }
        for (int i = 0; i < 3; ++i) {
            GameAction recruit{GAME_ACTION_RECRUIT_HERO, static_cast<uint32_t>(id),
                               0.0f, 0.0f, 0, 0};
            REQUIRE(game_dispatch(g, &recruit) >= 0);
        }
    }
}

// The observable state, read only through the C ABI (what any observer sees).
struct Snapshot {
    std::vector<GameCharacterState> characters;
    GameWorldState world{};
    uint64_t ticks = 0;
};

Snapshot snapshot(BadlandsGame* g) {
    Snapshot s;
    s.characters.resize(64);
    uint32_t n = game_state(g, s.characters.data(), 64);
    REQUIRE(n <= 64);
    s.characters.resize(n);
    game_world(g, &s.world);
    GameStats stats{};
    game_stats(g, &stats);
    s.ticks = stats.ticks;
    return s;
}

void require_same(const Snapshot& a, const Snapshot& b) {
    REQUIRE(a.characters.size() == b.characters.size());
    REQUIRE(a.ticks == b.ticks);
    CHECK(a.world.world_millis == b.world.world_millis);
    CHECK(a.world.gold == b.world.gold);
    for (size_t i = 0; i < a.characters.size(); ++i) {
        const GameCharacterState& x = a.characters[i];
        const GameCharacterState& y = b.characters[i];
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
    BadlandsGame* a = game_create(nullptr);
    BadlandsGame* b = game_create(nullptr);
    seed_town(a);
    seed_town(b);
    for (int i = 0; i < kRunTicks; ++i) {
        game_tick(a, 1.0f / 30.0f);
        game_tick(b, 1.0f / 30.0f);
    }

    require_same(snapshot(a), snapshot(b));

    // The trace is reproducible too -- not just the state it produced.
    REQUIRE(a->command_log.size() == b->command_log.size());
    REQUIRE(!a->command_log.empty());
    for (size_t i = 0; i < a->command_log.size(); ++i) {
        INFO("command " << i);
        CHECK(same_command(a->command_log[i], b->command_log[i]));
    }

    game_destroy(a);
    game_destroy(b);
}

TEST_CASE("a recorded command log replays into a fresh sim exactly") {
    BadlandsGame* live = game_create(nullptr);
    seed_town(live);
    for (int i = 0; i < kRunTicks; ++i) {
        game_tick(live, 1.0f / 30.0f);
    }
    const std::vector<Command> log = live->command_log;
    REQUIRE(!log.empty());

    // Fresh sim, brains OFF: every decision comes from the log. Note there is no
    // seed_town call -- the seed is IN the log, so the world is rebuilt from the
    // trace alone.
    BadlandsGame* replay = game_create(nullptr);
    replay->replay_log = &log;
    for (int i = 0; i < kRunTicks; ++i) {
        game_tick(replay, 1.0f / 30.0f);
    }

    CHECK(replay->replay_cursor == log.size());  // the whole trace was consumed
    require_same(snapshot(live), snapshot(replay));

    game_destroy(live);
    game_destroy(replay);
}

TEST_CASE("replayed commands re-log identically (the trace round-trips)") {
    BadlandsGame* live = game_create(nullptr);
    seed_town(live);
    for (int i = 0; i < 120; ++i) {
        game_tick(live, 1.0f / 30.0f);
    }
    const std::vector<Command> log = live->command_log;

    BadlandsGame* replay = game_create(nullptr);
    replay->replay_log = &log;
    for (int i = 0; i < 120; ++i) {
        game_tick(replay, 1.0f / 30.0f);
    }

    REQUIRE(replay->command_log.size() == log.size());
    for (size_t i = 0; i < log.size(); ++i) {
        INFO("command " << i);
        CHECK(same_command(replay->command_log[i], log[i]));
    }

    game_destroy(live);
    game_destroy(replay);
}
