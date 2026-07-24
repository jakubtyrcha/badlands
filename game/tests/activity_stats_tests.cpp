// Goal statistics.
//
// The histogram is a pure fold over snapshot rows, so most of this needs no sim
// at all -- which is the property worth protecting. If these tests ever need a
// world to run, the counting has leaked back into the simulation.

#include "badlands_sim.hpp"

#include "components.h"
#include "game_state.h"
#include "placement.h"
#include "sim_internal.hpp"

#include <catch_amalgamated.hpp>

#include <entt/entt.hpp>

#include <vector>

using namespace badlands;

namespace {

CharacterState row(ActivityId activity, int32_t hero_class) {
    CharacterState r{};
    r.behavior = static_cast<int32_t>(activity);
    r.hero_class = hero_class;
    return r;
}

}  // namespace

TEST_CASE("the histogram counts entity-ticks per activity") {
    ActivityHistogram h;
    const std::vector<CharacterState> tick{
        row(ActivityId::Roam, HERO_MERCENARY),
        row(ActivityId::Roam, HERO_HUNTER),
        row(ActivityId::Explore, HERO_HUNTER),
    };
    h.Accumulate(tick);
    h.Accumulate(tick);

    CHECK(h.Total(ActivityId::Roam) == 4);
    CHECK(h.Total(ActivityId::Explore) == 2);
    CHECK(h.Total(ActivityId::GoHome) == 0);
    CHECK(h.Samples() == 6);
}

TEST_CASE("the histogram attributes activity to the hero's class") {
    // The point of the per-class split: "apprentices never explore" is the kind
    // of thing that should be visible at a glance, not inferred.
    ActivityHistogram h;
    h.Accumulate(std::vector<CharacterState>{
        row(ActivityId::Explore, HERO_HUNTER),
        row(ActivityId::Explore, HERO_HUNTER),
        row(ActivityId::VisitTavern, HERO_APPRENTICE),
    });

    CHECK(h.ForClass(HERO_HUNTER, ActivityId::Explore) == 2);
    CHECK(h.ForClass(HERO_APPRENTICE, ActivityId::Explore) == 0);
    CHECK(h.ForClass(HERO_APPRENTICE, ActivityId::VisitTavern) == 1);
    CHECK(h.Total(ActivityId::Explore) == 2);
}

TEST_CASE("non-heroes count toward the total but toward no class") {
    // A grazing deer is a real sample; it just has no class to attribute to.
    ActivityHistogram h;
    h.Accumulate(std::vector<CharacterState>{
        row(ActivityId::Graze, -1),
        row(ActivityId::Flee, -1),
    });

    CHECK(h.Total(ActivityId::Graze) == 1);
    CHECK(h.Total(ActivityId::Flee) == 1);
    CHECK(h.Samples() == 2);
    for (int32_t c = 0; c < HERO_CLASS_COUNT; ++c) {
        CHECK(h.ForClass(static_cast<HeroClassId>(c), ActivityId::Graze) == 0);
    }
}

TEST_CASE("an undecided entity is a sample attributed to nothing") {
    // behavior == -1 is "has not decided yet". Counting it as Idle would
    // overstate idling on the tick everything spawns.
    ActivityHistogram h;
    h.Accumulate(std::vector<CharacterState>{row(ActivityId::Idle, -1)});
    CharacterState undecided{};
    undecided.behavior = -1;
    undecided.hero_class = HERO_MERCENARY;
    h.Accumulate(std::vector<CharacterState>{undecided});

    CHECK(h.Samples() == 2);
    CHECK(h.Total(ActivityId::Idle) == 1);
    uint64_t attributed = 0;
    for (const ActivityInfo& info : ActivityCatalog()) {
        attributed += h.Total(info.id);
    }
    CHECK(attributed == 1);  // the undecided row landed nowhere
}

TEST_CASE("out-of-range ids and classes are answered, not crashed into") {
    ActivityHistogram h;
    CharacterState bogus{};
    bogus.behavior = kActivityCount + 7;  // e.g. a log from a newer build
    bogus.hero_class = 99;
    h.Accumulate(std::vector<CharacterState>{bogus});

    CHECK(h.Samples() == 1);
    CHECK(h.Total(static_cast<ActivityId>(-1)) == 0);
    CHECK(h.Total(static_cast<ActivityId>(kActivityCount)) == 0);
    CHECK(h.ForClass(static_cast<HeroClassId>(HERO_CLASS_COUNT), ActivityId::Roam) == 0);
}

TEST_CASE("Reset clears everything") {
    ActivityHistogram h;
    h.Accumulate(std::vector<CharacterState>{row(ActivityId::Roam, HERO_MERCENARY)});
    REQUIRE(h.Samples() == 1);

    h.Reset();
    CHECK(h.Samples() == 0);
    CHECK(h.Total(ActivityId::Roam) == 0);
    CHECK(h.ForClass(HERO_MERCENARY, ActivityId::Roam) == 0);
}

TEST_CASE("every activity in the catalog is countable") {
    // Adding an activity must not leave a hole the histogram silently drops.
    ActivityHistogram h;
    std::vector<CharacterState> all;
    for (const ActivityInfo& info : ActivityCatalog()) {
        all.push_back(row(info.id, HERO_MERCENARY));
    }
    h.Accumulate(all);

    for (const ActivityInfo& info : ActivityCatalog()) {
        INFO("activity " << info.name);
        CHECK(h.Total(info.id) == 1);
    }
    CHECK(h.Samples() == static_cast<uint64_t>(kActivityCount));
}

// --- through the Sim --------------------------------------------------------

TEST_CASE("Sim::Tick folds statistics, and the totals reconcile") {
    // The reconciliation is the real assertion: samples must equal the number
    // of (entity, tick) pairs that actually happened. If counting ever drifted
    // from the snapshot, this is what would catch it.
    Sim sim(BrainDesc{});
    sim.Spawn(MercenaryDesc(0.0f, kCastleSpawnZ));
    sim.Spawn(MercenaryDesc(4.0f, kCastleSpawnZ));

    uint64_t expected = 0;
    for (int i = 0; i < 60; ++i) {
        sim.Tick(1.0f / 30.0f);
        expected += sim.Characters().size();
    }

    const ActivityHistogram& h = sim.ActivityStats();
    CHECK(h.Samples() == expected);

    uint64_t attributed = 0;
    for (const ActivityInfo& info : ActivityCatalog()) {
        attributed += h.Total(info.id);
    }
    // Every sample is attributed, except the first tick of each entity before
    // it has decided anything.
    CHECK(attributed <= h.Samples());
    CHECK(attributed > 0);
}

TEST_CASE("statistics survive a reset and keep accumulating") {
    Sim sim(BrainDesc{});
    sim.Spawn(MercenaryDesc(0.0f, kCastleSpawnZ));
    for (int i = 0; i < 10; ++i) {
        sim.Tick(1.0f / 30.0f);
    }
    REQUIRE(sim.ActivityStats().Samples() > 0);

    sim.ResetActivityStats();
    CHECK(sim.ActivityStats().Samples() == 0);

    sim.Tick(1.0f / 30.0f);
    CHECK(sim.ActivityStats().Samples() == 1);
}

TEST_CASE("the snapshot carries the hero class the histogram attributes by") {
    Sim sim(BrainDesc{});
    sim.Spawn(MercenaryDesc(0.0f, kCastleSpawnZ));
    sim.Tick(1.0f / 30.0f);

    const std::vector<CharacterState> rows = sim.Characters();
    REQUIRE(rows.size() == 1);
    // MercenaryDesc's class comes from the creature catalog (the single
    // source of truth, badlands_sim.hpp's CharacterDesc::hero_class) even
    // though this spawn is homeless -- a directly-spawned Mercenary IS a
    // Mercenary regardless of guild membership.
    CHECK(rows[0].hero_class == HERO_MERCENARY);
    CHECK(rows[0].archetype == static_cast<int32_t>(Archetype::Hero));
}

TEST_CASE("a recruited hero reports its guild's class") {
    Sim sim(BrainDesc{});
    Action place{ActionKind::PlaceBuilding, 0, -20.0f, 20.0f,
                 static_cast<int32_t>(BuildingKind::HuntersCamp), 0};
    const int64_t camp = sim.Dispatch(place);
    REQUIRE(camp >= 0);
    Action hire{ActionKind::RecruitHero, static_cast<uint32_t>(camp), 0.0f, 0.0f, 0, 0};
    REQUIRE(sim.Dispatch(hire) >= 0);
    sim.Tick(1.0f / 30.0f);

    bool found = false;
    for (const CharacterState& r : sim.Characters()) {
        found = found || r.hero_class == HERO_HUNTER;
    }
    CHECK(found);
}
