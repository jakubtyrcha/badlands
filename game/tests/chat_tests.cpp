// Chatting: two bored heroes who meet keep each other company.
//
// The contract has three parts, and they are tested separately because they can
// break independently:
//   1. the BLOCK  -- when a hero wants company and where it walks (pure view).
//   2. the SESSION -- created only by the Chat command, dissolved only by
//      system rules, always symmetric.
//   3. the NEED   -- company decays boredom toward a floor rather than clearing
//      it, which is what keeps the tavern worth the walk.

#include "behaviours/blocks.h"
#include "behaviours/selectors.h"
#include "behaviours/world_view.h"

#include "command.h"
#include "components.h"
#include "game_state.h"
#include "heroes.h"
#include "needs.h"
#include "placement.h"
#include "sim_internal.hpp"
#include "town_brain.h"

#include <catch_amalgamated.hpp>

#include <entt/entt.hpp>

using namespace badlands;

namespace {

WorldView lonely_hero(const SimFactors& f) {
    WorldView v;
    v.slot = 0;
    v.pos = {0.0f, 0.0f};
    v.boredom = f.hero.chat_boredom + 0.2f;  // bored enough to want company
    return v;
}

// Places a guild, hires `count` heroes, and drops them all at `where` so they
// are within conversation range of each other.
struct Town {
    std::unique_ptr<BadlandsGame> owned;
    uint32_t guild = UINT32_MAX;
    std::vector<uint32_t> heroes;

    BadlandsGame& g() { return *owned; }
};

Town make_town(int count, glm::vec2 where) {
    Town t;
    t.owned = make_world(nullptr);
    Action place{ActionKind::PlaceBuilding, 0, -40.0f, 40.0f,
                 static_cast<int32_t>(BuildingKind::FreeCompanyQuarters), 0};
    t.guild = static_cast<uint32_t>(dispatch_into(*t.owned, place));
    for (int i = 0; i < count; ++i) {
        Action hire{ActionKind::RecruitHero, t.guild, 0.0f, 0.0f, 0, 0};
        const uint32_t slot = static_cast<uint32_t>(dispatch_into(*t.owned, hire));
        t.heroes.push_back(slot);
        t.owned->registry.get<Position>(t.owned->slots[slot]).pos =
            where + glm::vec2{static_cast<float>(i) * 0.5f, 0.0f};
    }
    return t;
}

}  // namespace

// --- 1. the block -----------------------------------------------------------

TEST_CASE("a bored hero with a bored companion nearby wants to chat") {
    const SimFactors f;
    WorldView v = lonely_hero(f);
    CHECK(score_chat(v, f) == 0.0f);  // nobody about

    v.has_chat_partner = true;
    v.partner_pos = {10.0f, 0.0f};
    v.partner_dist = 10.0f;
    CHECK(score_chat(v, f) > 0.0f);

    // Contented heroes keep to themselves.
    v.boredom = 0.0f;
    CHECK(score_chat(v, f) == 0.0f);
}

TEST_CASE("act_chat walks over, then strikes it up only once in range") {
    const SimFactors f;
    WorldView v = lonely_hero(f);
    v.has_chat_partner = true;
    v.partner_slot = 4;
    v.partner_pos = {10.0f, 0.0f};
    v.partner_dist = 10.0f;

    BehaviourResult r = act_chat(v, f);
    CHECK(r.id == ActivityId::Chat);
    CHECK(r.target.x == 10.0f);
    CHECK_FALSE(r.follow_up.has_value());  // too far to talk yet

    v.partner_dist = f.hero.chat_radius * 0.5f;
    r = act_chat(v, f);
    REQUIRE(r.follow_up.has_value());
    CHECK(r.follow_up->kind == CommandKind::Chat);
    CHECK(r.follow_up->target_id == 4u);
}

TEST_CASE("a hero already chatting holds position and sees it through") {
    const SimFactors f;
    WorldView v = lonely_hero(f);
    v.pos = {3.0f, 7.0f};
    v.chatting = true;
    v.boredom = 0.0f;  // even once content, the conversation continues

    CHECK(score_chat(v, f) > 0.0f);
    const BehaviourResult r = act_chat(v, f);
    CHECK(r.id == ActivityId::Chat);
    CHECK(r.target.x == 3.0f);
    CHECK(r.target.y == 7.0f);
    CHECK_FALSE(r.follow_up.has_value());  // no second Chat command
}

TEST_CASE("chatting loses to the tavern but beats wandering") {
    // The intended shape of the trade-off: a proper night out first, company
    // when the tavern is shut, pacing about only when alone.
    const SimFactors f;
    WorldView v = lonely_hero(f);
    v.has_chat_partner = true;
    v.partner_pos = {4.0f, 0.0f};
    v.partner_dist = 4.0f;
    v.has_tavern = true;
    v.tavern_door = {8.0f, 0.0f};

    const ActivityWeights& w = f.hero.weights[HERO_MERCENARY];
    CHECK(select_banded(hero_activities(), w, v, f).id == ActivityId::VisitTavern);

    v.night = true;  // tavern block scores 0 at night
    CHECK(select_banded(hero_activities(), w, v, f).id == ActivityId::Chat);

    v.has_chat_partner = false;  // nobody about
    CHECK(select_banded(hero_activities(), w, v, f).id == ActivityId::Roam);
}

TEST_CASE("class weights change how sociable a hero is") {
    // Personality as pure data: same situation, same table, different weights.
    const SimFactors f;
    CHECK(f.hero.weights[HERO_APPRENTICE].of(ActivityId::Chat) >
          f.hero.weights[HERO_HUNTER].of(ActivityId::Chat));
}

// --- 2. the session ---------------------------------------------------------

TEST_CASE("the Chat command creates a session on BOTH heroes") {
    Town t = make_town(2, {0.0f, 0.0f});
    BadlandsGame& g = t.g();
    const entt::entity a = g.slots[t.heroes[0]];
    const entt::entity b = g.slots[t.heroes[1]];

    apply_command(g, Command{CommandKind::Chat, t.heroes[0], t.heroes[1]});

    REQUIRE(g.registry.all_of<ChattingState>(a));
    REQUIRE(g.registry.all_of<ChattingState>(b));
    CHECK(g.registry.get<ChattingState>(a).partner_slot == t.heroes[1]);
    CHECK(g.registry.get<ChattingState>(b).partner_slot == t.heroes[0]);
}

TEST_CASE("the Chat handler is authoritative about distance and availability") {
    Town t = make_town(2, {0.0f, 0.0f});
    BadlandsGame& g = t.g();
    const entt::entity a = g.slots[t.heroes[0]];
    const entt::entity b = g.slots[t.heroes[1]];

    // Too far apart -- a stale command from before they drifted must not land.
    g.registry.get<Position>(b).pos = {50.0f, 50.0f};
    apply_command(g, Command{CommandKind::Chat, t.heroes[0], t.heroes[1]});
    CHECK_FALSE(g.registry.all_of<ChattingState>(a));

    // A hero hidden inside a building is not available.
    g.registry.get<Position>(b).pos = {0.0f, 0.0f};
    g.registry.emplace<InsideBuilding>(b, 0, 99.0f);
    apply_command(g, Command{CommandKind::Chat, t.heroes[0], t.heroes[1]});
    CHECK_FALSE(g.registry.all_of<ChattingState>(a));
    g.registry.remove<InsideBuilding>(b);

    // Talking to yourself is not a conversation.
    apply_command(g, Command{CommandKind::Chat, t.heroes[0], t.heroes[0]});
    CHECK_FALSE(g.registry.all_of<ChattingState>(a));
}

TEST_CASE("a hero already in a conversation cannot be recruited into another") {
    Town t = make_town(3, {0.0f, 0.0f});
    BadlandsGame& g = t.g();

    apply_command(g, Command{CommandKind::Chat, t.heroes[0], t.heroes[1]});
    REQUIRE(g.registry.all_of<ChattingState>(g.slots[t.heroes[0]]));

    // A third hero tries to join in; the pair is already engaged.
    apply_command(g, Command{CommandKind::Chat, t.heroes[2], t.heroes[1]});
    CHECK_FALSE(g.registry.all_of<ChattingState>(g.slots[t.heroes[2]]));
    CHECK(g.registry.get<ChattingState>(g.slots[t.heroes[1]]).partner_slot == t.heroes[0]);
}

TEST_CASE("a conversation ends on expiry, and both leave together") {
    Town t = make_town(2, {0.0f, 0.0f});
    BadlandsGame& g = t.g();
    const entt::entity a = g.slots[t.heroes[0]];
    const entt::entity b = g.slots[t.heroes[1]];

    apply_command(g, Command{CommandKind::Chat, t.heroes[0], t.heroes[1]});
    REQUIRE(g.registry.all_of<ChattingState>(a));

    advance_chats(g, g.factors.hero.chat_duration + 0.1f);
    CHECK_FALSE(g.registry.all_of<ChattingState>(a));
    CHECK_FALSE(g.registry.all_of<ChattingState>(b));  // never one-sided
}

TEST_CASE("a conversation ends when the pair drifts apart") {
    Town t = make_town(2, {0.0f, 0.0f});
    BadlandsGame& g = t.g();
    const entt::entity a = g.slots[t.heroes[0]];
    const entt::entity b = g.slots[t.heroes[1]];

    apply_command(g, Command{CommandKind::Chat, t.heroes[0], t.heroes[1]});
    REQUIRE(g.registry.all_of<ChattingState>(a));

    // A nudge is tolerated; walking off is not.
    g.registry.get<Position>(b).pos = {g.factors.hero.chat_radius * 1.2f, 0.0f};
    advance_chats(g, 1.0f / 30.0f);
    CHECK(g.registry.all_of<ChattingState>(a));

    g.registry.get<Position>(b).pos = {40.0f, 0.0f};
    advance_chats(g, 1.0f / 30.0f);
    CHECK_FALSE(g.registry.all_of<ChattingState>(a));
    CHECK_FALSE(g.registry.all_of<ChattingState>(b));
}

TEST_CASE("a conversation ends when a partner dies") {
    Town t = make_town(2, {0.0f, 0.0f});
    BadlandsGame& g = t.g();
    const entt::entity a = g.slots[t.heroes[0]];

    apply_command(g, Command{CommandKind::Chat, t.heroes[0], t.heroes[1]});
    REQUIRE(g.registry.all_of<ChattingState>(a));

    g.registry.destroy(g.slots[t.heroes[1]]);
    advance_chats(g, 1.0f / 30.0f);
    CHECK_FALSE(g.registry.all_of<ChattingState>(a));  // not left talking to nobody
}

TEST_CASE("a threat breaks up a conversation") {
    Town t = make_town(2, {0.0f, 0.0f});
    BadlandsGame& g = t.g();
    const entt::entity a = g.slots[t.heroes[0]];

    apply_command(g, Command{CommandKind::Chat, t.heroes[0], t.heroes[1]});
    REQUIRE(g.registry.all_of<ChattingState>(a));

    spawn_into(g, GoblinDesc(2.0f, 0.0f));  // hostile team
    advance_chats(g, 1.0f / 30.0f);
    CHECK_FALSE(g.registry.all_of<ChattingState>(a));
}

// --- 3. the need ------------------------------------------------------------

TEST_CASE("company decays boredom toward a floor, never clearing it") {
    // The whole point of chatting being a WEAKER entertainment: if it cleared
    // boredom the tavern would be pointless.
    Town t = make_town(2, {0.0f, 0.0f});
    BadlandsGame& g = t.g();
    const entt::entity a = g.slots[t.heroes[0]];
    apply_command(g, Command{CommandKind::Chat, t.heroes[0], t.heroes[1]});

    g.registry.get<HeroSimulationState>(a).boredom = 1.0f;
    for (int i = 0; i < 200; ++i) {
        advance_needs(g);
    }
    const float settled = g.registry.get<HeroSimulationState>(a).boredom;
    CHECK(settled == Catch::Approx(g.factors.hero.chat_boredom_floor));
    CHECK(settled > 0.0f);  // the tavern still has something to offer
}

TEST_CASE("a hero not chatting still grows bored") {
    Town t = make_town(1, {0.0f, 0.0f});
    BadlandsGame& g = t.g();
    const entt::entity a = g.slots[t.heroes[0]];
    const float before = g.registry.get<HeroSimulationState>(a).boredom;

    advance_needs(g);
    CHECK(g.registry.get<HeroSimulationState>(a).boredom > before);
}

TEST_CASE("chatting is still tiring") {
    // Company answers boredom, not exhaustion -- so a chatting hero eventually
    // still has to go home, which is what stops conversations being a trap.
    Town t = make_town(2, {0.0f, 0.0f});
    BadlandsGame& g = t.g();
    const entt::entity a = g.slots[t.heroes[0]];
    apply_command(g, Command{CommandKind::Chat, t.heroes[0], t.heroes[1]});

    const float before = g.registry.get<HeroSimulationState>(a).fatigue;
    advance_needs(g);
    CHECK(g.registry.get<HeroSimulationState>(a).fatigue > before);
}

// --- end to end -------------------------------------------------------------

TEST_CASE("two bored heroes find each other and talk, through the sim") {
    Town t = make_town(2, {0.0f, 0.0f});
    BadlandsGame& g = t.g();
    const entt::entity a = g.slots[t.heroes[0]];
    const entt::entity b = g.slots[t.heroes[1]];

    // Night, so the tavern is not an option; bored, so they want company.
    g.world_millis = static_cast<int64_t>(kMillisPerDay * 0.9);
    SimFactors f = g.factors;
    f.hero.think_max_millis = 0;  // not a test about deliberation
    set_factors_of(g, f);
    for (entt::entity e : {a, b}) {
        auto& sim = g.registry.get<HeroSimulationState>(e);
        sim.boredom = 0.9f;
        sim.fatigue = 0.0f;
    }

    bool talked = false;
    for (int i = 0; i < 120 && !talked; ++i) {
        auto& sa = g.registry.get<HeroSimulationState>(a);
        auto& sb = g.registry.get<HeroSimulationState>(b);
        sa.fatigue = sb.fatigue = 0.0f;  // keep rest from taking over
        tick_world(g, 1.0f / 30.0f);
        talked = g.registry.all_of<ChattingState>(a) && g.registry.all_of<ChattingState>(b);
    }
    CHECK(talked);

    // And it is in the trace, so a replay reproduces the pairing.
    bool logged = false;
    for (const Command& c : g.command_log) {
        logged = logged || c.kind == CommandKind::Chat;
    }
    CHECK(logged);
}
