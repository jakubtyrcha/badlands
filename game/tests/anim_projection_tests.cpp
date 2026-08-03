// The animation projection (game/src/anim_projection.cpp): the one summary the
// render layer reads to decide what a character should be playing.
//
// These drive step_world so the projector runs in its real position (last),
// against the real mechanics -- a strike declared through declare_strike, a
// stun through apply_status. Asserting the projection against hand-built
// components would only prove the translation agrees with itself.

#include "anim_projection.h"
#include "components.h"
#include "game_state.h"
#include "heroes.h"
#include "movement.h"
#include "sim_internal.hpp"
#include "skill_focus.h"
#include "skills.h"
#include "status.h"
#include "strike.h"

#include <catch_amalgamated.hpp>

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <cmath>
#include <vector>

using namespace badlands;

namespace {

uint32_t spawn_fighter(BadlandsGame& g, glm::vec2 pos) {
    CharacterDesc d = MercenaryDesc(pos.x, pos.y);
    d.move_speed = 6.0f;
    return spawn_into(g, d);
}

const CharacterAnim& anim_of(const BadlandsGame& g, uint32_t slot) {
    const entt::entity e = g.slots[slot];
    REQUIRE(g.registry.valid(e));
    const auto* a = g.registry.try_get<CharacterAnim>(e);
    REQUIRE(a != nullptr);
    return *a;
}

// A bounded action carries a real window; an unbounded one collapses to a point.
bool bounded(const CharacterAnim& a) {
    return a.action_end_ticks > a.action_start_ticks;
}

// Move under own power for one step, then project -- the same order step_world
// uses (follow_paths early, the projector last).
//
// This drives the movement pipeline directly rather than through step_world,
// for the reason facing_tests gives: a world built with no wasm brain leaves
// its heroes BRAINLESS, and mock_think zeroes a brainless entity's MoveTarget
// every tick (sim.cpp) before plan_paths sees it. A goal set from a test would
// be wiped before it could move anything, so locomotion has to be driven from
// the NavPath the planner would have produced.
void walk_one_step(BadlandsGame& g, entt::entity e, glm::vec2 goal) {
    NavPath& np = g.registry.get<NavPath>(e);
    np.waypoints = {goal};
    np.cursor = 0;
    follow_paths(g);
    project_anim_state(g);
}

}  // namespace

TEST_CASE("every live character gets a projection after one step", "[anim]") {
    auto owned = make_flat_world();
    BadlandsGame& g = *owned;
    const uint32_t hero = spawn_fighter(g, {0.0f, 0.0f});

    // Nothing is attached at spawn -- the projector emplaces it, so no spawn
    // path had to learn about animation.
    CHECK(g.registry.try_get<CharacterAnim>(g.slots[hero]) == nullptr);

    step_world(g);
    const CharacterAnim& a = anim_of(g, hero);
    CHECK(a.action == AnimAction::Idle);
    CHECK(a.action_param == -1);
}

TEST_CASE("a standing character is Idle at zero speed", "[anim]") {
    auto owned = make_flat_world();
    BadlandsGame& g = *owned;
    const uint32_t hero = spawn_fighter(g, {0.0f, 0.0f});

    for (int i = 0; i < 5; ++i) step_world(g);

    const CharacterAnim& a = anim_of(g, hero);
    CHECK(a.action == AnimAction::Idle);
    CHECK(a.speed == 0.0f);
    CHECK(std::isfinite(a.speed));
    CHECK_FALSE(bounded(a));  // a loop has no window to stretch a clip to
}

TEST_CASE("a walking character is Locomotion with a real speed", "[anim]") {
    auto owned = make_flat_world();
    BadlandsGame& g = *owned;
    const uint32_t hero = spawn_fighter(g, {0.0f, 0.0f});
    const entt::entity e = g.slots[hero];

    walk_one_step(g, e, {60.0f, 0.0f});

    const CharacterAnim& a = anim_of(g, hero);
    CHECK(a.action == AnimAction::Locomotion);
    CHECK(a.speed > 0.0f);
    CHECK(std::isfinite(a.speed));
    CHECK_FALSE(bounded(a));
}

TEST_CASE("a declared strike projects its wind-up window, then its recovery",
          "[anim]") {
    auto owned = make_flat_world();
    BadlandsGame& g = *owned;
    const uint32_t attacker = spawn_fighter(g, {0.0f, 0.0f});
    const uint32_t victim = spawn_fighter(g, {1.0f, 0.0f});
    g.registry.get<Team>(g.slots[victim]).id = 99;  // hostile, so the swing is legal

    const entt::entity e = g.slots[attacker];
    REQUIRE(declare_strike(g, e, 0, victim));
    const StrikeInProgress declared = g.registry.get<StrikeInProgress>(e);
    REQUIRE(declared.resolve_at_ticks > declared.declared_ticks);

    step_world(g);
    {
        const CharacterAnim& a = anim_of(g, attacker);
        REQUIRE(a.action == AnimAction::AttackWindUp);
        // The window is the MECHANIC's, not one the projector invented -- this
        // is what lets the view stretch a clip to the authored swing.
        CHECK(a.action_start_ticks == declared.declared_ticks);
        CHECK(a.action_end_ticks == declared.resolve_at_ticks);
        CHECK(a.action_param == 0);
        CHECK(bounded(a));
    }

    // Run until the blow has landed and the strike is in its recovery.
    while (g.registry.all_of<StrikeInProgress>(g.slots[attacker]) &&
           anim_of(g, attacker).action == AnimAction::AttackWindUp) {
        step_world(g);
    }

    if (g.registry.all_of<StrikeInProgress>(g.slots[attacker])) {
        const StrikeInProgress& s = g.registry.get<StrikeInProgress>(g.slots[attacker]);
        const CharacterAnim& a = anim_of(g, attacker);
        CHECK(a.action == AnimAction::AttackRecovery);
        CHECK(a.action_start_ticks == s.resolve_at_ticks);
        CHECK(a.action_end_ticks == s.free_at_ticks);
        CHECK(bounded(a));
    }
}

TEST_CASE("the projection leaves the strike phases once the strike is gone",
          "[anim]") {
    auto owned = make_flat_world();
    BadlandsGame& g = *owned;
    const uint32_t attacker = spawn_fighter(g, {0.0f, 0.0f});
    const uint32_t victim = spawn_fighter(g, {1.0f, 0.0f});
    g.registry.get<Team>(g.slots[victim]).id = 99;

    REQUIRE(declare_strike(g, g.slots[attacker], 0, victim));
    step_world(g);
    REQUIRE(anim_of(g, attacker).action == AnimAction::AttackWindUp);

    // Cancelling drops the component; the projection must follow the state
    // rather than remembering a swing that no longer exists.
    g.registry.remove<StrikeInProgress>(g.slots[attacker]);
    step_world(g);

    const CharacterAnim& a = anim_of(g, attacker);
    CHECK(a.action != AnimAction::AttackWindUp);
    CHECK(a.action != AnimAction::AttackRecovery);
    CHECK(a.action_param == -1);
}

TEST_CASE("a stun outranks the strike phases", "[anim]") {
    auto owned = make_flat_world();
    BadlandsGame& g = *owned;
    const uint32_t attacker = spawn_fighter(g, {0.0f, 0.0f});
    const uint32_t victim = spawn_fighter(g, {1.0f, 0.0f});
    g.registry.get<Team>(g.slots[victim]).id = 99;

    const entt::entity e = g.slots[attacker];
    REQUIRE(declare_strike(g, e, 0, victim));
    apply_status(g, e, StatusKind::Stunned, 1000, UINT32_MAX);
    step_world(g);

    // Priority, asserted rather than assumed: whatever the strike is doing, a
    // stunned character is visibly staggering.
    const CharacterAnim& a = anim_of(g, attacker);
    CHECK(a.action == AnimAction::Stunned);
    CHECK(a.action_param == -1);
    // A stun records what is LEFT, not when it began, so its end is real and
    // its start is stamped when the action changed.
    CHECK(a.action_end_ticks > a.action_start_ticks);
}

TEST_CASE("a stun's projected window stays put while it ticks down", "[anim]") {
    auto owned = make_flat_world();
    BadlandsGame& g = *owned;
    const uint32_t hero = spawn_fighter(g, {0.0f, 0.0f});

    apply_status(g, g.slots[hero], StatusKind::Stunned, 1000, UINT32_MAX);
    step_world(g);
    const CharacterAnim first = anim_of(g, hero);
    REQUIRE(first.action == AnimAction::Stunned);

    step_world(g);
    const CharacterAnim second = anim_of(g, hero);
    REQUIRE(second.action == AnimAction::Stunned);

    // remaining_ticks falls by exactly one step as world_ticks rises by one, so
    // the window a view stretches its clip to must not slide underneath it.
    CHECK(second.action_start_ticks == first.action_start_ticks);
    CHECK(second.action_end_ticks == first.action_end_ticks);
}

TEST_CASE("back-to-back strikes get distinct start ticks", "[anim]") {
    auto owned = make_flat_world();
    BadlandsGame& g = *owned;
    const uint32_t attacker = spawn_fighter(g, {0.0f, 0.0f});
    const uint32_t victim = spawn_fighter(g, {1.0f, 0.0f});
    g.registry.get<Team>(g.slots[victim]).id = 99;
    g.registry.get<Health>(g.slots[victim]).hp = 1.0e6f;  // survive the whole test

    const entt::entity e = g.slots[attacker];
    REQUIRE(declare_strike(g, e, 0, victim));
    step_world(g);
    const int64_t first_start = anim_of(g, attacker).action_start_ticks;

    // Let the first strike finish completely, then throw another.
    while (g.registry.all_of<StrikeInProgress>(e)) step_world(g);
    REQUIRE(declare_strike(g, e, 0, victim));
    step_world(g);

    // THE property the edge detector rests on: an observer that remembers the
    // last start tick can tell a second swing from the first without any event.
    CHECK(anim_of(g, attacker).action_start_ticks != first_start);
}

TEST_CASE("an unbounded action keeps its start tick while it continues",
          "[anim]") {
    auto owned = make_flat_world();
    BadlandsGame& g = *owned;
    const uint32_t hero = spawn_fighter(g, {0.0f, 0.0f});

    step_world(g);
    const int64_t idle_start = anim_of(g, hero).action_start_ticks;
    REQUIRE(anim_of(g, hero).action == AnimAction::Idle);

    for (int i = 0; i < 5; ++i) step_world(g);

    // Still the same idle, so the same start: a view must not re-trigger a
    // transition every frame just because the action is unbounded.
    CHECK(anim_of(g, hero).action == AnimAction::Idle);
    CHECK(anim_of(g, hero).action_start_ticks == idle_start);
}

TEST_CASE("an unbounded action is re-stamped when its kind changes", "[anim]") {
    auto owned = make_flat_world();
    BadlandsGame& g = *owned;
    const uint32_t hero = spawn_fighter(g, {0.0f, 0.0f});
    const entt::entity e = g.slots[hero];

    for (int i = 0; i < 3; ++i) step_world(g);
    const int64_t idle_start = anim_of(g, hero).action_start_ticks;
    REQUIRE(anim_of(g, hero).action == AnimAction::Idle);

    walk_one_step(g, e, {60.0f, 0.0f});

    REQUIRE(anim_of(g, hero).action == AnimAction::Locomotion);
    CHECK(anim_of(g, hero).action_start_ticks > idle_start);
}

TEST_CASE("a repositioned character reports no speed", "[anim]") {
    auto owned = make_flat_world();
    BadlandsGame& g = *owned;
    const uint32_t hero = spawn_fighter(g, {0.0f, 0.0f});
    const entt::entity e = g.slots[hero];

    step_world(g);
    REQUIRE(anim_of(g, hero).speed == 0.0f);

    // A discontinuous placement -- what happens when a hero reappears from a
    // building (heroes.cpp). Differencing positions would read this as an
    // enormous speed for one tick and blur the walk cycle; own-power movement
    // is the only thing that counts, so it must read zero.
    g.registry.get<Position>(e).pos = glm::vec2{500.0f, 500.0f};
    step_world(g);

    const CharacterAnim& a = anim_of(g, hero);
    CHECK(a.speed == 0.0f);
    CHECK(a.action == AnimAction::Idle);
    CHECK(std::isfinite(a.speed));
}

TEST_CASE("a focused cast projects its whole window", "[anim]") {
    auto owned = make_flat_world();
    BadlandsGame& g = *owned;
    const uint32_t caster = spawn_fighter(g, {0.0f, 0.0f});
    const entt::entity e = g.slots[caster];

    // Drive SkillFocus directly: which skills a mercenary happens to know is
    // catalog data, and this test is about the projection, not the catalog.
    const int64_t now = g.world_ticks;
    g.registry.emplace<SkillFocus>(
        e, SkillFocus{.resolve_at_ticks = now + 240,
                      .declared_ticks = now,
                      .id = SkillId::Count,
                      .skill_index = 0,
                      .target_slot = caster,
                      .point = glm::vec2{0.0f, 0.0f}});

    step_world(g);

    const CharacterAnim& a = anim_of(g, caster);
    CHECK(a.action == AnimAction::CastFocus);
    CHECK(a.action_start_ticks == now);
    CHECK(a.action_end_ticks == now + 240);
    CHECK(bounded(a));
    CHECK(a.action_param == static_cast<int32_t>(SkillId::Count));
}

TEST_CASE("the projection drops with the character", "[anim]") {
    auto owned = make_flat_world();
    BadlandsGame& g = *owned;
    const uint32_t hero = spawn_fighter(g, {0.0f, 0.0f});

    step_world(g);
    REQUIRE(g.registry.valid(g.slots[hero]));

    // Entities are destroyed the same tick their HP reaches zero (sim.cpp), so
    // the projector never sees a dead character -- which is exactly why there
    // is no downed posture in the contract.
    g.registry.get<Health>(g.slots[hero]).hp = 0.0f;
    step_world(g);

    CHECK_FALSE(g.registry.valid(g.slots[hero]));
}

TEST_CASE("an attacker turns to face what it is striking", "[anim]") {
    // Facing is written in exactly one place -- the direction of TRAVEL
    // (movement.cpp) -- so before this, a fighter that stopped moving kept
    // whatever way it happened to be pointing and swung at empty air. Invisible
    // on a symmetric capsule; unmissable once a skeleton is drawn on it.
    //
    // Facing also aims the vision cone, so turning to strike means a unit looks
    // at what it is fighting. That is the intent, not a side effect.
    auto owned = make_flat_world();
    BadlandsGame& g = *owned;
    const uint32_t attacker = spawn_fighter(g, {0.0f, 0.0f});
    const uint32_t victim = spawn_fighter(g, {1.0f, 0.0f});  // due +X
    g.registry.get<Team>(g.slots[victim]).id = 99;

    const entt::entity e = g.slots[attacker];
    // Point it deliberately AWAY from the target first.
    g.registry.get<Facing>(e).dir = glm::vec2{-1.0f, 0.0f};

    REQUIRE(declare_strike(g, e, 0, victim));

    const glm::vec2 facing = g.registry.get<Facing>(e).dir;
    CHECK(facing.x == Catch::Approx(1.0f).margin(1e-3f));
    CHECK(facing.y == Catch::Approx(0.0f).margin(1e-3f));
    CHECK(glm::length(facing) == Catch::Approx(1.0f).margin(1e-3f));
}

TEST_CASE("a strike at a target on top of the attacker keeps its facing",
          "[anim]") {
    // Degenerate case: no direction to turn toward, so the last facing stands
    // rather than becoming a zero vector -- Facing is documented as always
    // normalized, and the vision cone divides by it.
    auto owned = make_flat_world();
    BadlandsGame& g = *owned;
    const uint32_t attacker = spawn_fighter(g, {0.0f, 0.0f});
    const uint32_t victim = spawn_fighter(g, {0.0f, 0.0f});  // exactly coincident
    g.registry.get<Team>(g.slots[victim]).id = 99;

    const entt::entity e = g.slots[attacker];
    g.registry.get<Facing>(e).dir = glm::vec2{0.0f, 1.0f};
    REQUIRE(declare_strike(g, e, 0, victim));

    const glm::vec2 facing = g.registry.get<Facing>(e).dir;
    CHECK(glm::length(facing) == Catch::Approx(1.0f).margin(1e-3f));
    CHECK(facing.y == Catch::Approx(1.0f).margin(1e-3f));
}
