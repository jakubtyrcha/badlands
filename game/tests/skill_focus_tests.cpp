// The Intention trigger, executed: a skill that takes TIME to cast.
//
// Every case pins the combat gates at a deterministic extreme (a chance of 0 or
// 1 makes the seeded roll certain), the same discipline combat_tests.cpp and
// shield_bash_tests.cpp use -- so nothing here depends on a particular draw
// while still running the real resolver.
//
// The through-line: NOTHING is captured when a focus begins. That is what makes
// two seconds a genuine commitment rather than a delayed certainty, and most of
// what is below is one way or another a test of that sentence.

#include "skill_focus.h"

#include "combat.h"
#include "command.h"
#include "components.h"
#include "game_state.h"
#include "intention.h"
#include "movement.h"
#include "sim_internal.hpp"
#include "skill_cast.h"
#include "skills.h"
#include "nav_world.h"
#include "placement.h"
#include "status.h"

#include <catch_amalgamated.hpp>

#include <entt/entt.hpp>

#include <algorithm>
#include <memory>
#include <vector>

using namespace badlands;

namespace {

// A shooter with PrecisionShot at slot 0 and a victim well inside the skill's
// authored 30 m range but well OUTSIDE the shooter's own 8 m bow -- which is
// the point: the shot's reach comes from its own constant, not the weapon whose
// test it borrows.
struct FocusFixture {
    std::unique_ptr<BadlandsGame> game;
    uint32_t caster_slot = 0;
    uint32_t victim_slot = 0;

    explicit FocusFixture(float apart = 20.0f, float victim_defense = 1.0f,
                          float victim_evasion = 1.0f) {
        game = make_flat_world();

        CharacterDesc shooter{};
        shooter.archetype = Archetype::Hero;
        shooter.hero_class = HERO_HUNTER;
        shooter.team = 1;
        shooter.hp = 22.0f;
        shooter.move_speed = 3.0f;
        shooter.size_x = shooter.size_y = shooter.size_z = 1.0f;
        shooter.accuracy = 0.0f;  // hopeless on its own: the guarantee is the test
        shooter.attack_count = 1;
        shooter.attacks[0] = {AttackCategory::Ranged, DamageType::Piercing,
                              6.0f, 8.0f, 1.2f, 0.0f};
        caster_slot = spawn_into(*game, shooter);

        CharacterDesc victim{};
        victim.archetype = Archetype::Monster;
        victim.team = 2;
        victim.pos_x = apart;
        victim.hp = 200.0f;  // survives the shot, so a case can keep going
        victim.move_speed = 2.0f;
        victim.size_x = victim.size_y = victim.size_z = 1.0f;
        // Gates dialled to certainty in the DEFENDER's favour: without the
        // guarantee nothing could ever land here.
        victim.defense = victim_defense;
        victim.evasion = victim_evasion;
        victim.attack_count = 1;
        victim.attacks[0] = {AttackCategory::Melee, DamageType::Slashing, 3.0f, 1.2f, 0.8f, 0.0f};
        victim_slot = spawn_into(*game, victim);

        learn_skill(game->registry.get<Skills>(caster()), SkillId::PrecisionShot);
    }

    entt::entity caster() const { return game->slots[caster_slot]; }
    entt::entity victim() const { return game->slots[victim_slot]; }

    Intention shot_at(uint32_t target) const {
        Intention i;
        i.kind = IntentionKind::UseSkill;
        i.target_slot = target;
        i.arg = 0;  // skill slot, not a SkillId
        return i;
    }
    // Adoption QUEUES a FocusSkill command (command.h) rather than starting the
    // focus in place -- a replay never thinks, so the start has to be logged.
    // Draining here is what the live tick does immediately afterwards.
    bool adopt() {
        const bool ok = apply_intention(*game, caster_slot, shot_at(victim_slot));
        apply_commands(*game);
        return ok;
    }

    float victim_hp() const { return game->registry.get<Health>(victim()).hp; }
    bool is_focusing() const { return focusing(game->registry, caster()); }
    int64_t deadline() const {
        const auto* f = game->registry.try_get<SkillFocus>(caster());
        return f != nullptr ? f->resolve_at_millis : -1;
    }
    float cooldown() const {
        return game->registry.get<Skills>(caster()).cooldown_remaining[0];
    }

    // Advances the world clock and runs the focus sweep, without the rest of a
    // tick: these cases are about the deadline, not about everything else the
    // world does while it elapses.
    void step() {
        game->world_millis += kMillisPerTick;
        advance_focus(*game);
    }
};

}  // namespace

TEST_CASE("a focus resolves at its deadline, not before", "[focus]") {
    FocusFixture f;
    REQUIRE(f.adopt());
    REQUIRE(f.is_focusing());

    // The DEADLINE, computed from the authored duration -- not a tick count.
    const int64_t duration =
        f.game->skills.specs[static_cast<size_t>(SkillId::PrecisionShot)]
            .intention_duration_seconds * 1000.0f;
    CHECK(f.deadline() == duration);

    const float hp_before = f.victim_hp();
    while (f.game->world_millis + kMillisPerTick < duration) {
        f.step();
        REQUIRE(f.victim_hp() == Catch::Approx(hp_before));
        REQUIRE(f.is_focusing());
    }
    f.step();  // crosses it
    CHECK(f.victim_hp() < hp_before);
    CHECK_FALSE(f.is_focusing());
    CHECK(f.cooldown() > 0.0f);
}

TEST_CASE("a guaranteed test never blocks or dodges and always crits", "[focus][combat]") {
    // The victim's defense and evasion are both 1.0 -- certainties, in its
    // favour -- and the shooter's accuracy is 0. An ordinary attack could not
    // possibly land; this one cannot possibly miss.
    FocusFixture f;
    REQUIRE(f.adopt());
    const float hp_before = f.victim_hp();
    while (f.is_focusing()) {
        f.step();
    }
    const float dealt = hp_before - f.victim_hp();
    CHECK(dealt > 0.0f);

    // ...and it crits at the skill's own multiplier, not the global one.
    const SkillSpec& spec = f.game->skills.specs[static_cast<size_t>(SkillId::PrecisionShot)];
    const float mult = spec.constant("crit_multiplier", kBaseCritMultiplier);
    CHECK(mult > kBaseCritMultiplier);
    CHECK(dealt == Catch::Approx(6.0f * mult));  // base damage, no armour in the way
}

TEST_CASE("a stun mid-focus drops it with no cast", "[focus][status]") {
    FocusFixture f;
    REQUIRE(f.adopt());
    const float hp_before = f.victim_hp();

    f.step();
    REQUIRE(f.is_focusing());
    apply_status(*f.game, f.caster(), StatusKind::Stunned, 3000, f.victim_slot);
    CHECK_FALSE(f.is_focusing());

    // ...and the shot never arrives afterwards, nor was the cooldown spent: an
    // interrupted focus costs the seconds and nothing else.
    for (int i = 0; i < 200; ++i) {
        f.step();
    }
    CHECK(f.victim_hp() == Catch::Approx(hp_before));
    CHECK(f.cooldown() == 0.0f);
}

TEST_CASE("adopting a different intention cancels the focus", "[focus][intention]") {
    // The ONLY way a focus is abandoned by choice. A focusing entity does not
    // move, so deciding to do something else is the whole of "moving abandons
    // it" -- and it costs the seconds already spent.
    FocusFixture f;
    REQUIRE(f.adopt());
    f.step();
    REQUIRE(f.is_focusing());

    Intention walk;
    walk.kind = IntentionKind::MoveTo;
    walk.point = {5.0f, 5.0f};
    REQUIRE(apply_intention(*f.game, f.caster_slot, walk));
    apply_commands(*f.game);
    CHECK_FALSE(f.is_focusing());
}

TEST_CASE("restating the identical focus does not restart it", "[focus][intention]") {
    // Otherwise a brain that restates every wake -- which this one does, by
    // design -- would push its own deadline forever and never fire.
    FocusFixture f;
    REQUIRE(f.adopt());
    const int64_t first = f.deadline();
    f.step();
    REQUIRE(f.adopt());
    CHECK(f.is_focusing());
    CHECK(f.deadline() == first);
}

TEST_CASE("a focusing entity does not move", "[focus][movement]") {
    FocusFixture f;
    // Give it somewhere to be going, then commit it.
    Intention walk;
    walk.kind = IntentionKind::MoveTo;
    walk.point = {0.0f, 25.0f};
    REQUIRE(apply_intention(*f.game, f.caster_slot, walk));
    apply_commands(*f.game);
    plan_paths(*f.game, 0.033f);
    REQUIRE(f.adopt());  // ...which cancels the walk's intention, but not its path

    const glm::vec2 before = f.game->registry.get<Position>(f.caster()).pos;
    for (int i = 0; i < 10; ++i) {
        follow_paths(*f.game, 0.033f);
    }
    CHECK(f.game->registry.get<Position>(f.caster()).pos == before);
}

TEST_CASE("a target out of range at the deadline gets no shot", "[focus]") {
    // Nothing is captured when the focus begins, so everything is re-checked at
    // the end. This is the difference between a long cast and a slow certainty.
    FocusFixture f;
    REQUIRE(f.adopt());
    const float hp_before = f.victim_hp();
    f.step();

    f.game->registry.get<Position>(f.victim()).pos = {90.0f, 0.0f};  // well past 30 m
    while (f.is_focusing()) {
        f.step();
    }
    CHECK(f.victim_hp() == Catch::Approx(hp_before));
    CHECK(f.cooldown() == 0.0f);  // never cast, so never stamped
}

TEST_CASE("the skill's own range beats the weapon it borrows a test from", "[focus][skill]") {
    // 20 m is far outside the shooter's 8 m bow and well inside PrecisionShot's
    // authored 30. If the weapon decided, this cast could never be made at all.
    FocusFixture f(/*apart=*/20.0f);
    const SkillSpec& spec = f.game->skills.specs[static_cast<size_t>(SkillId::PrecisionShot)];
    CHECK(skill_cast_range(f.game->registry, f.caster(), spec) ==
          Catch::Approx(spec.constant("range", 0.0f)));
    CHECK(f.adopt());
}

TEST_CASE("the two cast channels refuse each other's skills", "[focus][skill]") {
    // Neither channel can be used to skip the other's rules: an Intention skill
    // fired as an action would cost nothing to cast, and an Action skill
    // adopted as a focus would idle for a duration it never declared.
    FocusFixture f(/*apart=*/2.0f);
    learn_skill(f.game->registry.get<Skills>(f.caster()), SkillId::DressWounds);

    // PrecisionShot (Intention) through the action gateway: refused.
    CastPlan plan;
    CHECK_FALSE(validate_cast(*f.game, f.caster_slot, 0, f.victim_slot, plan,
                              SkillTrigger::Action));
    // DressWounds (Action) adopted as a focus: refused.
    Intention as_focus;
    as_focus.kind = IntentionKind::UseSkill;
    as_focus.target_slot = f.caster_slot;
    as_focus.arg = 1;
    CHECK_FALSE(apply_intention(*f.game, f.caster_slot, as_focus));
    apply_commands(*f.game);
    CHECK_FALSE(f.is_focusing());
}

// --- point targeting ---------------------------------------------------------
// Teleport is the only Point-targeted skill, and the only thing that moves an
// entity outside the movement pipeline. The through-line here is the op: it
// carries NO destination, so an effect can ask for somebody to be moved but
// never for where.

namespace {

// An apprentice on flat ground WITH terrain blocking on, so there is a real
// navmesh to be passable or not against -- the whole point of the cast's
// second check.
struct BlinkFixture {
    std::unique_ptr<BadlandsGame> game;
    uint32_t caster_slot = 0;

    BlinkFixture() {
        WorldConfig cfg;
        cfg.prebuild_colony = false;
        cfg.map = MapKind::FlatPlains;
        cfg.terrain_blocking = true;
        // A wall to aim into: the point check has to have something to refuse.
        cfg.plops.push_back(
            PlacementDesc{static_cast<int32_t>(BuildingKind::Wall), 0, 12.0f, 0.0f});
        game = make_world(BrainDesc{}, cfg);

        CharacterDesc app{};
        app.archetype = Archetype::Hero;
        app.hero_class = HERO_APPRENTICE;
        app.team = 1;
        app.hp = 16.0f;
        app.move_speed = 2.4f;
        app.size_x = app.size_y = app.size_z = 1.0f;
        app.attack_count = 1;
        app.attacks[0] = {AttackCategory::Ranged, DamageType::Piercing, 4.0f, 6.0f, 1.5f, 0.0f};
        caster_slot = spawn_into(*game, app);
        learn_skill(game->registry.get<Skills>(caster()), SkillId::Teleport);
    }

    entt::entity caster() const { return game->slots[caster_slot]; }
    glm::vec2 pos() const { return game->registry.get<Position>(caster()).pos; }

    // Through the authoritative gate, with the point riding the Command's own
    // point field -- exactly as a brain's action does.
    int64_t blink_to(glm::vec2 p) {
        return apply_command(*game, Command{CommandKind::UseSkill, caster_slot, caster_slot,
                                            p, /*param_a=*/0});
    }
};

}  // namespace

TEST_CASE("a legal teleport moves the caster to exactly the cast point", "[skill][point]") {
    BlinkFixture f;
    const glm::vec2 to{-20.0f, 6.0f};
    REQUIRE(f.blink_to(to) >= 0);
    // EXACTLY there, not near it: the engine validated that spot, so that spot
    // is where the caster ends up.
    CHECK(f.pos() == to);
}

TEST_CASE("a point cast beyond range is refused", "[skill][point]") {
    BlinkFixture f;
    const glm::vec2 before = f.pos();
    const SkillSpec& spec = f.game->skills.specs[static_cast<size_t>(SkillId::Teleport)];
    const float range = spec.constant("range", 0.0f);
    REQUIRE(range > 0.0f);

    f.blink_to({before.x + range + 5.0f, before.y});
    CHECK(f.pos() == before);
    CHECK(f.game->registry.get<Skills>(f.caster()).cooldown_remaining[0] == 0.0f);
}

TEST_CASE("a point cast onto an impassable cell is refused", "[skill][point]") {
    // Into the wall's own footprint. In range, and still not somewhere anything
    // could stand -- which is a separate check from the range one, and this is
    // what proves both run.
    BlinkFixture f;
    const glm::vec2 before = f.pos();
    f.blink_to({12.0f, 0.0f});
    CHECK(f.pos() == before);
}

TEST_CASE("a teleport op cannot name a point of its own", "[skill][point]") {
    // STRUCTURAL: BL_FX_TELEPORT has no coordinate fields, so an effect that
    // wanted an arbitrary destination has no way to express one. Here the op is
    // handed deliberate garbage in the fields it DOES have, and the caster
    // still lands on the context's point -- the only place it can.
    BlinkFixture f;
    const glm::vec2 to{-15.0f, -4.0f};
    BlSkillCastContext ctx{};
    ctx.version = BL_SKILL_ABI_VERSION;
    ctx.skill_id = static_cast<int32_t>(SkillId::Teleport);
    ctx.caster.slot = f.caster_slot;
    ctx.point_x = to.x;
    ctx.point_z = to.y;

    BlSkillEffectBatch batch{};
    batch.count = 1;
    batch.ops[0] = BlSkillEffectOp{BL_FX_TELEPORT, f.caster_slot, /*param_i=*/999,
                                   /*param_f=*/12345.0f};
    apply_effect_batch(*f.game, f.caster_slot, ctx, batch);
    CHECK(f.pos() == to);
}

TEST_CASE("nav cells near a point come back nearest-first and capped", "[nav]") {
    BlinkFixture f;
    rebuild_navmesh_if_stale(*f.game);
    REQUIRE_FALSE(f.game->navmesh.empty());

    std::vector<nav::NavMesh::DebugCell> cells;
    f.game->navmesh.CellsNear(f.pos(), /*radius=*/30.0f, /*max_out=*/8, cells);
    CHECK(cells.size() <= 8);
    REQUIRE(!cells.empty());
    // Non-decreasing in centre distance: what survives the cap is always the
    // CLOSEST ground, which is what makes a bounded window usable at all.
    float last = -1.0f;
    for (const nav::NavMesh::DebugCell& c : cells) {
        const glm::vec2 centre = (c.min_world + c.max_world) * 0.5f;
        const float d = glm::distance(centre, f.pos());
        CHECK(d >= last - 1e-3f);
        last = d;
    }
    // ...and the walls are in there too: a brain choosing where to stand needs
    // to see what it cannot stand on.
    std::vector<nav::NavMesh::DebugCell> wide;
    f.game->navmesh.CellsNear({12.0f, 0.0f}, /*radius=*/6.0f, /*max_out=*/64, wide);
    CHECK(std::any_of(wide.begin(), wide.end(),
                      [](const nav::NavMesh::DebugCell& c) { return !c.passable; }));
}

TEST_CASE("the nav window is clipped to the view cone", "[nav]") {
    // You may not pick ground you cannot see. The cone is applied BEFORE the
    // thinning, so a narrow one spends the whole budget on what is actually in
    // front rather than returning a handful of survivors.
    BlinkFixture f;
    rebuild_navmesh_if_stale(*f.game);
    const glm::vec2 from = f.pos();
    const glm::vec2 facing{1.0f, 0.0f};  // +x

    std::vector<nav::NavMesh::DebugCell> all;
    f.game->navmesh.CellsNear(from, 30.0f, 64, all);  // full circle by default
    std::vector<nav::NavMesh::DebugCell> ahead;
    f.game->navmesh.CellsNear(from, 30.0f, 64, ahead, facing, /*cone_half_cos=*/0.5f);  // 60 deg

    REQUIRE(!ahead.empty());
    CHECK(ahead.size() < all.size());  // a cone is not the whole disc
    for (const nav::NavMesh::DebugCell& c : ahead) {
        const glm::vec2 centre = (c.min_world + c.max_world) * 0.5f;
        const glm::vec2 d = centre - from;
        if (glm::dot(d, d) < 1e-6f) {
            continue;  // the cell underfoot has no direction; always kept
        }
        CHECK(glm::dot(glm::normalize(d), facing) >= 0.5f - 1e-3f);
    }
}
