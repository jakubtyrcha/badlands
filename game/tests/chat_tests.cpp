// Chatting: two bored heroes who meet keep each other company.
//
// The contract has three parts, and they are tested separately because they can
// break independently:
//   1. the DECISION -- when a hero wants company and where it walks. That
//      logic (score_chat/act_chat) died with the C++ hero decision layer and
//      now lives only in the Nim brain -- proven here end to end, through the
//      real wasm brain, not as a block-level unit test.
//   2. the SESSION -- created only by the Chat command, dissolved only by
//      system rules, always symmetric.
//   3. the NEED   -- company decays boredom toward a floor rather than clearing
//      it, which is what keeps the tavern worth the walk.

#include "command.h"
#include "components.h"
#include "game_state.h"
#include "heroes.h"
#include "needs.h"
#include "placement.h"
#include "sim_internal.hpp"

#include "fixtures/wasm_hero.h"

#include <catch_amalgamated.hpp>

#include <entt/entt.hpp>
#include <glm/glm.hpp>

using namespace badlands;

namespace {

// Places a guild, hires `count` heroes, and drops them all at `where` so they
// are within conversation range of each other. `wasm=true` builds the world
// on the shipping hero brain (only the end-to-end decision case below needs
// it -- the session/need mechanics tests drive Commands/systems directly and
// need no brain at all, mock or wasm).
struct Town {
    std::unique_ptr<BadlandsGame> owned;
    uint32_t guild = UINT32_MAX;
    std::vector<uint32_t> heroes;

    BadlandsGame& g() { return *owned; }
};

Town make_town(int count, glm::vec2 where, bool wasm = false) {
    Town t;
    t.owned = wasm ? testfix::make_wasm_world() : make_world(BrainDesc{});
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
// (The score_chat/act_chat block-level cases, and the hero_activities()
// trade-off case, died with the C++ hero decision layer -- that logic now
// lives only in the Nim brain, scripts/brains/nim/blocks.nim, unreachable
// from a plain WorldView. The end-to-end case below -- two heroes actually
// finding each other and talking, through the real wasm brain -- is what
// proves the SAME trade-off still holds.)

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
    g.registry.emplace<InsideBuilding>(b, 0, static_cast<int32_t>(ActivityId::GoHome));
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

TEST_CASE("company refills content toward a ceiling, never the whole way") {
    // The whole point of chatting being a WEAKER entertainment: if it filled
    // content completely the tavern would be pointless.
    Town t = make_town(2, {0.0f, 0.0f});
    BadlandsGame& g = t.g();
    const entt::entity a = g.slots[t.heroes[0]];
    apply_command(g, Command{CommandKind::Chat, t.heroes[0], t.heroes[1]});

    g.registry.get<HeroSimulationState>(a).content = 0.0f;
    for (int i = 0; i < 100000; ++i) {
        advance_needs(g);
    }
    const float settled = g.registry.get<HeroSimulationState>(a).content;
    CHECK(settled == Catch::Approx(g.factors.hero.chat_content_ceiling));
    CHECK(settled < 1.0f);  // the tavern still has something to offer
}

TEST_CASE("a hero not chatting keeps growing less entertained") {
    Town t = make_town(1, {0.0f, 0.0f});
    BadlandsGame& g = t.g();
    const entt::entity a = g.slots[t.heroes[0]];
    const float before = g.registry.get<HeroSimulationState>(a).content;

    advance_needs(g);
    CHECK(g.registry.get<HeroSimulationState>(a).content < before);  // content drains
}

TEST_CASE("chatting is still tiring") {
    // Company answers entertainment, not exhaustion -- so a chatting hero still
    // runs its fatigue down and must eventually go home, which is what stops
    // conversations being a trap.
    Town t = make_town(2, {0.0f, 0.0f});
    BadlandsGame& g = t.g();
    const entt::entity a = g.slots[t.heroes[0]];
    apply_command(g, Command{CommandKind::Chat, t.heroes[0], t.heroes[1]});

    const float before = g.registry.get<HeroSimulationState>(a).fatigue;
    advance_needs(g);
    CHECK(g.registry.get<HeroSimulationState>(a).fatigue < before);  // fatigue drains
}

// --- end to end -------------------------------------------------------------

TEST_CASE("two bored heroes find each other and talk, through the sim") {
    Town t = make_town(2, {0.0f, 0.0f}, /*wasm=*/true);
    BadlandsGame& g = t.g();
    const entt::entity a = g.slots[t.heroes[0]];
    const entt::entity b = g.slots[t.heroes[1]];

    // Night, so the tavern is not an option; starved of diversion, so they want
    // company. Rested (fatigue 1.0), so rest never takes over.
    g.world_millis = static_cast<int64_t>(g.millis_per_day * 0.9);
    for (entt::entity e : {a, b}) {
        auto& sim = g.registry.get<HeroSimulationState>(e);
        sim.content = 0.1f;
        sim.fatigue = 1.0f;
    }

    bool talked = false;
    for (int i = 0; i < 120 && !talked; ++i) {
        auto& sa = g.registry.get<HeroSimulationState>(a);
        auto& sb = g.registry.get<HeroSimulationState>(b);
        sa.fatigue = sb.fatigue = 1.0f;  // keep rest from taking over
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

    // Review fix (Fix 3): the session must not dissolve early via drift. A
    // hero mid-walk toward its partner at the exact moment Chat lands would,
    // under a movement pipeline that did not exclude ChattingState, keep
    // walking -- drifting the pair beyond chat_radius*2 and breaking
    // advance_chats' own drift check within a handful of ticks, well short
    // of chat_duration (6s / 180 ticks at 30Hz). Hold both positions frozen
    // and the session alive for a further stretch comfortably inside
    // chat_duration to pin that it does not.
    const glm::vec2 a_pos = g.registry.get<Position>(a).pos;
    const glm::vec2 b_pos = g.registry.get<Position>(b).pos;
    for (int i = 0; i < 60; ++i) {
        auto& sa = g.registry.get<HeroSimulationState>(a);
        auto& sb = g.registry.get<HeroSimulationState>(b);
        sa.fatigue = sb.fatigue = 1.0f;  // keep rest from taking over
        tick_world(g, 1.0f / 30.0f);
        REQUIRE(g.registry.all_of<ChattingState>(a));  // not dissolved early
        REQUIRE(g.registry.all_of<ChattingState>(b));
        CHECK(glm::distance(g.registry.get<Position>(a).pos, a_pos) < 0.5f);
        CHECK(glm::distance(g.registry.get<Position>(b).pos, b_pos) < 0.5f);
    }
}

// Finding B: while a chat session runs, hero.nim's chat wake used to yield
// BL_INT_IDLE (actChat's "nothing new to decide" branch, blocks.nim) --
// Idle is NEVER treated as an identical restatement
// (is_identical_restatement, intention.cpp), so that adoption overwrote
// CurrentIntention.kind with Idle (and reset its started marker) the moment
// the hero was re-consulted mid-conversation. advance_intentions' Chat
// branch -- the one that watches ChattingState for the session's real end --
// then never ran again for the rest of the conversation: the completion it
// eventually produced (if any) came from the Idle branch instead, timed
// against an unrelated deadline, not against ChattingState actually ending.
TEST_CASE("a chatting hero's CurrentIntention stays Chat across a mid-chat wake, and its "
         "own completion fires when the session truly ends") {
    Town t = make_town(2, {0.0f, 0.0f}, /*wasm=*/true);
    BadlandsGame& g = t.g();
    const entt::entity a = g.slots[t.heroes[0]];
    const entt::entity b = g.slots[t.heroes[1]];

    // Same setup as "two bored heroes find each other and talk" above: night
    // (no tavern), starved of diversion, rested (so rest never takes over).
    g.world_millis = static_cast<int64_t>(g.millis_per_day * 0.9);
    for (entt::entity e : {a, b}) {
        auto& sim = g.registry.get<HeroSimulationState>(e);
        sim.content = 0.1f;
        sim.fatigue = 1.0f;
    }

    bool talked = false;
    for (int i = 0; i < 120 && !talked; ++i) {
        auto& sa = g.registry.get<HeroSimulationState>(a);
        auto& sb = g.registry.get<HeroSimulationState>(b);
        sa.fatigue = sb.fatigue = 1.0f;
        tick_world(g, 1.0f / 30.0f);
        talked = g.registry.all_of<ChattingState>(a) && g.registry.all_of<ChattingState>(b);
    }
    REQUIRE(talked);

    // Ride the session out to its natural expiry (chat_duration == 6s ==
    // 180 ticks) plus margin, holding position (same drift guard as above)
    // so nothing but the bug itself can end it early. The load-bearing
    // check: CurrentIntention.kind must stay Chat on EVERY tick the session
    // is still live -- in particular across the first idle-hint wake after
    // adoption (500-2000ms, well inside this window), which is exactly
    // where the pre-fix brain swapped in Idle.
    const glm::vec2 a_pos = g.registry.get<Position>(a).pos;
    const glm::vec2 b_pos = g.registry.get<Position>(b).pos;
    bool session_ended = false;
    for (int i = 0; i < 220 && !session_ended; ++i) {
        auto& sa = g.registry.get<HeroSimulationState>(a);
        auto& sb = g.registry.get<HeroSimulationState>(b);
        sa.fatigue = sb.fatigue = 1.0f;
        g.registry.get<Position>(a).pos = a_pos;
        g.registry.get<Position>(b).pos = b_pos;
        tick_world(g, 1.0f / 30.0f);
        if (g.registry.all_of<ChattingState>(a)) {
            CHECK(g.registry.get<CurrentIntention>(a).kind == IntentionKind::Chat);
        } else {
            session_ended = true;
        }
    }
    REQUIRE(session_ended);  // it must actually run its course within the budget

    // InboxEvent (components.h) carries only {kind, source_slot, param} --
    // no field records WHICH intention kind ended, so "the ended kind is
    // Chat, not Idle" cannot be read directly off the event itself. What IS
    // checkable, and equivalent given the loop above already pins
    // CurrentIntention.kind == Chat on every tick up to and including the
    // one right before ChattingState vanished: a completion (param == 1.0)
    // landing at all can only have come from advance_intentions' Chat
    // branch (ci.arg == 1, ChattingState gone) in that case, never its Idle
    // branch -- which the fix never lets CurrentIntention become while
    // chatting in the first place. That, combined with the loop's own
    // CHECKs above, is what stands in for "the ended kind is Chat".
    const EventInbox& inbox = g.registry.get<EventInbox>(a);
    bool found_completed = false;
    for (int i = 0; i < inbox.count; ++i) {
        if (inbox.events[i].kind == InboxEventKind::IntentionEnded &&
            inbox.events[i].param == 1.0f) {
            found_completed = true;
        }
    }
    CHECK(found_completed);
}
