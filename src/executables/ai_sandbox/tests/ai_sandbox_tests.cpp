// The puppet-master layer: arena layouts and the duel round lifecycle.
//
// Links badlands_game_lib and NOTHING else -- no engine, no Dawn, no SDL. That
// is the point rather than an economy: it is a compile-time proof that a mode
// is view-free, so the same code the window drives can be reasoned about (and
// run) without one.

#include "executables/ai_sandbox/arena.hpp"
#include "executables/ai_sandbox/duel_mode.hpp"
#include "executables/ai_sandbox/sneak_mode.hpp"
#include "executables/ai_sandbox/teleport_mode.hpp"

#include "game_state.h"
#include "nav_world.h"       // rebuild_navmesh_if_stale
#include "threat_table.h"    // threat_target -- the dummy IS its anchor
#include "placement.h"       // tri_index / in_bounds_tile -- the flood fill works in tiles
#include "sim_internal.hpp"  // make_world / buildings_of

#include <catch_amalgamated.hpp>

#include <algorithm>
#include <cmath>
#include <deque>
#include <tuple>
#include <utility>
#include <vector>

using namespace badlands;

namespace {

// A flat, empty world carrying the arena's walls and nothing else.
std::unique_ptr<BadlandsGame> arena_world(const ArenaLayout& layout) {
    WorldConfig cfg;
    cfg.prebuild_colony = false;
    cfg.map = MapKind::FlatPlains;
    cfg.terrain_blocking = true;
    cfg.plops = layout.plops;
    return make_world(BrainDesc{}, cfg);
}

bool tile_free(const BadlandsGame& g, int tx, int tz) {
    if (!in_bounds_tile(tx, tz)) {
        return false;
    }
    for (int c = 0; c < 4; ++c) {
        if (g.placement.footprint[tri_index(tx, tz, c)]) {
            return false;
        }
    }
    return true;
}

glm::ivec2 tile_of(glm::vec2 p) {
    return {static_cast<int>(std::floor(p.x)), static_cast<int>(std::floor(p.y))};
}

// 8-connected flood fill over FREE tiles from `start`, bounded by `limit`.
// Eight and not four deliberately: a body is not a tile-stepper, so a pair of
// blocks touching corner-to-corner is a hole even though a 4-connected fill
// would call it sealed. Returns false if the fill ever reaches the bound --
// i.e. the arena leaks.
bool fill_stays_inside(const BadlandsGame& g, glm::vec2 start, int limit) {
    const glm::ivec2 s = tile_of(start);
    std::vector<uint8_t> seen(static_cast<size_t>(2 * limit + 1) * (2 * limit + 1), 0);
    auto idx = [limit](int tx, int tz) {
        return static_cast<size_t>(tz + limit) * (2 * limit + 1) + (tx + limit);
    };
    std::deque<glm::ivec2> queue{s};
    seen[idx(s.x, s.y)] = 1;
    while (!queue.empty()) {
        const glm::ivec2 t = queue.front();
        queue.pop_front();
        if (std::abs(t.x) >= limit || std::abs(t.y) >= limit) {
            return false;  // escaped
        }
        for (int dz = -1; dz <= 1; ++dz) {
            for (int dx = -1; dx <= 1; ++dx) {
                const glm::ivec2 n{t.x + dx, t.y + dz};
                if ((dx == 0 && dz == 0) || !tile_free(g, n.x, n.y) || seen[idx(n.x, n.y)]) {
                    continue;
                }
                seen[idx(n.x, n.y)] = 1;
                queue.push_back(n);
            }
        }
    }
    return true;
}

std::vector<ArenaShape> all_shapes() {
    std::vector<ArenaShape> out;
    for (int i = 0; i < static_cast<int>(ArenaShape::Count); ++i) {
        out.push_back(static_cast<ArenaShape>(i));
    }
    return out;
}

// A duel reads its verdict off the rows, so every case here hands Observe an
// empty stream. The event channel is for a mode watching for a DECISION.
const std::vector<GameEvent> kNoEvents{};

// A CharacterState carrying only what tally_duel reads.
CharacterState fighter(int32_t team) {
    CharacterState r{};
    r.team = team;
    r.archetype = static_cast<int32_t>(Archetype::Hero);
    return r;
}

}  // namespace

// --- arena layouts -----------------------------------------------------------
// Every case runs for EVERY shape, so a fourth shape is covered the day it is
// added rather than the day someone remembers to add a test for it.

TEST_CASE("every arena plops completely", "[arena]") {
    for (ArenaShape shape : all_shapes()) {
        CAPTURE(arena_shape_name(shape));
        const ArenaLayout layout = build_arena(shape);
        REQUIRE(layout.plops.size() > 0);
        auto owned = arena_world(layout);
        // Not one plop refused. Out of bounds is the only refusal a plop has,
        // so this catches a shape that has wandered off the grid.
        CHECK(buildings_of(*owned).size() == layout.plops.size());

        // ...and no two plops are the same block. Comparing counts alone
        // cannot see this: a duplicate inflates BOTH sides equally. Two
        // coincident blocks are two identical meshes z-fighting, which is
        // exactly what happened where the rhomboid's diagonal runs met at its
        // tips.
        std::vector<std::tuple<int, float, float>> keys;
        for (const PlacementDesc& p : layout.plops) {
            keys.emplace_back(p.rotation_index, p.world_x, p.world_z);
        }
        std::sort(keys.begin(), keys.end());
        CHECK(std::adjacent_find(keys.begin(), keys.end()) == keys.end());
    }
}

TEST_CASE("every arena uses both lattices where its shape needs them", "[arena]") {
    // The point of the diagonal rotation: a 45-degree side is a real slanted
    // wall, not a staircase of axis-aligned boxes. A shape with slanted sides
    // that emitted no rotation-1 blocks would have quietly regressed to one.
    auto counts = [](ArenaShape s) {
        int axis = 0, diag = 0;
        for (const PlacementDesc& p : build_arena(s).plops) {
            (p.rotation_index == 0 ? axis : diag) += 1;
        }
        return std::pair{axis, diag};
    };
    const auto [tube_axis, tube_diag] = counts(ArenaShape::Tube);
    CHECK(tube_axis > 0);
    CHECK(tube_diag == 0);  // a rectangle has no slanted side to build

    const auto [oct_axis, oct_diag] = counts(ArenaShape::Octagon);
    CHECK(oct_axis > 0);   // the flat top and bottom
    CHECK(oct_diag > 0);   // the four slants

    const auto [rhomb_axis, rhomb_diag] = counts(ArenaShape::Rhomboid);
    CHECK(rhomb_diag > 0);  // every side is a slant
    CHECK(rhomb_axis == 4); // the four columns, and nothing else
}

TEST_CASE("every arena is sealed", "[arena]") {
    for (ArenaShape shape : all_shapes()) {
        CAPTURE(arena_shape_name(shape));
        const ArenaLayout layout = build_arena(shape);
        auto owned = arena_world(layout);
        CHECK(fill_stays_inside(*owned, layout.spawn_a, 100));
    }
}

TEST_CASE("both spawn points are free and mutually reachable", "[arena]") {
    for (ArenaShape shape : all_shapes()) {
        CAPTURE(arena_shape_name(shape));
        const ArenaLayout layout = build_arena(shape);
        auto owned = arena_world(layout);
        const glm::ivec2 a = tile_of(layout.spawn_a);
        const glm::ivec2 b = tile_of(layout.spawn_b);
        REQUIRE(tile_free(*owned, a.x, a.y));
        REQUIRE(tile_free(*owned, b.x, b.y));
        // Reachable through the NAVMESH, not just the tile grid: this is what
        // proves the clearance dilation did not quietly seal a corridor the
        // flood fill still walks down.
        rebuild_navmesh_if_stale(*owned);
        REQUIRE_FALSE(owned->navmesh.empty());
        const nav::NavMesh::PathResult r =
            owned->navmesh.FindPath(layout.spawn_a, layout.spawn_b);
        CHECK(r.reachable);
    }
}

TEST_CASE("the rhomboid's columns obstruct", "[arena]") {
    const ArenaLayout layout = build_arena(ArenaShape::Rhomboid);
    auto owned = arena_world(layout);
    rebuild_navmesh_if_stale(*owned);
    REQUIRE_FALSE(owned->navmesh.empty());

    // Straight past the +x column, which sits between these two points. Chosen
    // well clear of the rhombus's sharp points: the walls converge there, so
    // after the navmesh's clearance dilation the last few metres of each tip
    // are not walkable -- correct for a 45-degree corner, and not what this
    // case is about.
    const glm::vec2 from{12.0f, -6.0f};
    const glm::vec2 to{12.0f, 6.0f};
    const nav::NavMesh::PathResult r = owned->navmesh.FindPath(from, to);
    REQUIRE(r.reachable);
    float len = 0.0f;
    glm::vec2 prev = from;
    for (const glm::vec2& w : r.waypoints) {
        len += glm::distance(prev, w);
        prev = w;
    }
    // A detour, not a straight shot: something is genuinely in the way.
    CHECK(len > glm::distance(from, to) + 1.0f);
}

TEST_CASE("arena shape names are distinct", "[arena]") {
    for (ArenaShape a : all_shapes()) {
        for (ArenaShape b : all_shapes()) {
            if (a == b) {
                continue;
            }
            CHECK(std::string(arena_shape_name(a)) != arena_shape_name(b));
        }
    }
}

// --- the duel round ----------------------------------------------------------

TEST_CASE("the duel pool excludes anything that cannot fight", "[duel]") {
    const std::vector<CreatureId> pool = duel_pool(DefaultCreatureCatalog());
    REQUIRE(pool.size() >= 2);
    for (CreatureId id : pool) {
        const CharacterDesc& d = DefaultCreatureCatalog().defs[static_cast<int>(id)];
        CHECK(d.archetype != Archetype::Critter);
        CHECK((d.attack_count > 0 || d.attack_damage > 0.0f));
    }
    // Asserting the RULE, then naming the one creature the rule has to catch:
    // a Deer never registers as a combatant, so a round containing one could
    // never end.
    CHECK(std::find(pool.begin(), pool.end(), CreatureId::Deer) == pool.end());
}

TEST_CASE("sampling is a pure function of seed and round", "[duel]") {
    DuelConfig cfg;
    cfg.seed = 12345;
    const std::vector<CreatureId> pool = duel_pool(DefaultCreatureCatalog());

    for (uint32_t r = 0; r < 8; ++r) {
        const DuelSetup a = sample_duel(cfg, pool, r);
        const DuelSetup b = sample_duel(cfg, pool, r);
        CHECK(a.shape == b.shape);
        CHECK(a.left == b.left);
        CHECK(a.right == b.right);
        CHECK(a.left_level == b.left_level);
        CHECK(a.right_level == b.right_level);
    }
    // ...and it actually varies, or the "samples" axis is decorative.
    bool varies = false;
    for (uint32_t r = 1; r < 32 && !varies; ++r) {
        const DuelSetup s = sample_duel(cfg, pool, r);
        const DuelSetup s0 = sample_duel(cfg, pool, 0);
        varies = s.left != s0.left || s.right != s0.right || s.shape != s0.shape;
    }
    CHECK(varies);
}

TEST_CASE("a duel samples a level in range for each side", "[duel]") {
    // Levels 1-8 is the design doc's early game, and 8 is where the last skill
    // unlocks -- so this range is what makes every skill a class has reachable
    // in a duel at all.
    DuelConfig cfg;
    cfg.seed = 777;
    const std::vector<CreatureId> pool = duel_pool(DefaultCreatureCatalog());

    bool saw_low = false;
    bool saw_high = false;
    for (uint32_t r = 0; r < 64; ++r) {
        const DuelSetup s = sample_duel(cfg, pool, r);
        REQUIRE(s.left_level >= cfg.min_level);
        REQUIRE(s.left_level <= cfg.max_level);
        REQUIRE(s.right_level >= cfg.min_level);
        REQUIRE(s.right_level <= cfg.max_level);
        saw_low = saw_low || s.left_level <= 2;
        saw_high = saw_high || s.left_level >= 7;
    }
    // Spread, not just legality: a sampler pinned to one value would satisfy
    // the range check above and teach us nothing.
    CHECK(saw_low);
    CHECK(saw_high);

    // A single-level range collapses to that level rather than dividing by zero.
    DuelConfig fixed = cfg;
    fixed.min_level = fixed.max_level = 4;
    CHECK(sample_duel(fixed, pool, 0).left_level == 4);
}

TEST_CASE("a duel ends when one side is gone, after the linger", "[duel]") {
    DuelConfig cfg;
    cfg.max_ticks = ticks_of(60.0f);
    cfg.linger_ticks = ticks_of(5.0f);
    DuelMode mode(cfg);
    mode.Configure();

    const std::vector<CharacterState> both{fighter(0), fighter(1)};
    const std::vector<CharacterState> left_only{fighter(0)};

    CHECK_FALSE(mode.Observe(both, kNoEvents, ticks_of(1.0f)));
    CHECK_FALSE(mode.Observe(left_only, kNoEvents, ticks_of(2.0f)));  // decided, but lingering
    CHECK_FALSE(mode.Observe(left_only, kNoEvents,
                             ticks_of(2.0f) + ticks_of(5.0f) - 1));  // still inside the linger
    CHECK(mode.Observe(left_only, kNoEvents,
                       ticks_of(2.0f) + ticks_of(5.0f)));  // 2s + 5s: restage
}

TEST_CASE("a duel with both sides alive times out as a draw", "[duel]") {
    DuelConfig cfg;
    cfg.max_ticks = ticks_of(60.0f);
    cfg.linger_ticks = ticks_of(5.0f);
    DuelMode mode(cfg);
    mode.Configure();

    const std::vector<CharacterState> both{fighter(0), fighter(1)};
    CHECK_FALSE(mode.Observe(both, kNoEvents, ticks_of(60.0f) - 1));
    // No linger on a timeout: there is nothing left to watch.
    CHECK(mode.Observe(both, kNoEvents, ticks_of(60.0f)));
}

TEST_CASE("a duel with both sides gone is a draw, not a win", "[duel]") {
    DuelConfig cfg;
    cfg.linger_ticks = ticks_of(1.0f);
    DuelMode mode(cfg);
    mode.Configure();

    const std::vector<CharacterState> nobody{};
    CHECK_FALSE(mode.Observe(nobody, kNoEvents, ticks_of(2.0f)));  // decided, lingering
    CHECK(mode.Observe(nobody, kNoEvents, ticks_of(2.0f) + ticks_of(1.0f)));
    // A mutual kill leaves nobody standing; the readout must not credit a side.
    const std::string status = mode.Status();
    CHECK(status.find("draw") != std::string::npos);
}

TEST_CASE("a critter is not a side", "[duel]") {
    CharacterState deer{};
    deer.team = 1;
    deer.archetype = static_cast<int32_t>(Archetype::Critter);
    const DuelTally t = tally_duel({fighter(0), deer});
    CHECK(t.left_alive);
    CHECK_FALSE(t.right_alive);  // wildlife on team 1 does not keep the round alive
}

// --- the sneak sandbox -------------------------------------------------------
// Driven with SYNTHETIC events: no Sim, no window. The verdict logic is a pure
// fold over the stream, which is what makes it testable at all -- and what
// makes a FAILED line in a live run mean "the brain did not do it" rather than
// "the mode missed it".

namespace {

GameEvent status_applied(uint32_t target_slot, StatusKind kind, int64_t at) {
    GameEvent ev{};
    ev.kind = GameEventKind::StatusApplied;
    ev.actor_id = target_slot;
    ev.target_id = target_slot;
    ev.target_kind = kEventTargetCharacter;
    ev.amount = static_cast<float>(kind);
    ev.at_ticks = at;
    return ev;
}

GameEvent damage_dealt(uint32_t actor_slot, uint32_t target_slot, float amount, int64_t at) {
    GameEvent ev{};
    ev.kind = GameEventKind::DamageDealt;
    ev.actor_id = actor_slot;
    ev.target_id = target_slot;
    ev.target_kind = kEventTargetCharacter;
    ev.amount = amount;
    ev.at_ticks = at;
    return ev;
}

}  // namespace

TEST_CASE("the sneak mode sees the sneak, then the blow that ends it", "[sneak][mode]") {
    SneakProgress p;
    observe_sneak({damage_dealt(7, 9, 3.0f, 500)}, 7, p);
    CHECK(p.stage == SneakStage::Waiting);  // hitting something is not sneaking

    observe_sneak({status_applied(7, StatusKind::Sneaking, 1800)}, 7, p);
    REQUIRE(p.stage == SneakStage::Sneaked);
    CHECK(p.sneaked_at_ticks == 1800);

    observe_sneak({damage_dealt(7, 9, 31.2f, 7400)}, 7, p);
    REQUIRE(p.stage == SneakStage::Struck);
    CHECK(p.struck_at_ticks == 7400);
    CHECK(p.strike_damage == Catch::Approx(31.2f));
}

TEST_CASE("the sneak mode ignores what the other side does", "[sneak][mode]") {
    // The bandit hitting back is the most common event in this stream by far;
    // crediting it would make every round pass. Keyed on the ACTOR, so it
    // cannot.
    SneakProgress p;
    observe_sneak({status_applied(7, StatusKind::Sneaking, 1000)}, 7, p);
    REQUIRE(p.stage == SneakStage::Sneaked);

    observe_sneak({damage_dealt(/*actor=*/9, /*target=*/7, 5.0f, 2000)}, 7, p);
    CHECK(p.stage == SneakStage::Sneaked);  // still waiting for OUR blow

    // ...and a status that is not Sneaking never starts a round either.
    SneakProgress q;
    observe_sneak({status_applied(7, StatusKind::Stunned, 900)}, 7, q);
    CHECK(q.stage == SneakStage::Waiting);
}

TEST_CASE("the sneak mode reports success and restages", "[sneak][mode]") {
    SneakMode mode(SneakConfig{});
    mode.Configure();

    const std::vector<CharacterState> rows{fighter(0), fighter(1)};
    CHECK_FALSE(mode.Observe(rows, kNoEvents, 500));
    // The round ends on the blow, not on the budget: there is nothing further
    // to learn from watching the fight play out.
    CHECK(mode.Observe(rows, {status_applied(UINT32_MAX, StatusKind::Sneaking, 0)}, 600) ==
          false);
    CHECK(mode.Status().find("approaching") != std::string::npos);
}

TEST_CASE("the sneak mode reports a failure on timeout, and still restages", "[sneak][mode]") {
    // A round that produced nothing must say so out loud. Silently restaging
    // would read exactly like a pass.
    SneakConfig cfg;
    cfg.max_ticks = ticks_of(30.0f);
    SneakMode mode(cfg);
    mode.Configure();

    const std::vector<CharacterState> rows{fighter(0), fighter(1)};
    CHECK_FALSE(mode.Observe(rows, kNoEvents, ticks_of(30.0f) - 1));
    CHECK(mode.Observe(rows, kNoEvents, ticks_of(30.0f)));
    CHECK(mode.Status().find("FAILED") != std::string::npos);
}

// --- the teleport sandbox ----------------------------------------------------

TEST_CASE("the teleport mode counts only a teleport", "[teleport][mode]") {
    // A level-8 apprentice also knows Curse and Calcify. Keying on the caster
    // alone would make every round pass on the first thing it cast.
    TeleportProgress p;
    GameEvent curse{};
    curse.kind = GameEventKind::SkillUsed;
    curse.actor_id = 3;
    curse.amount = static_cast<float>(SkillId::Curse);
    curse.at_ticks = 500;
    observe_teleport({curse}, 3, p);
    CHECK_FALSE(p.blinked);

    GameEvent blink = curse;
    blink.amount = static_cast<float>(SkillId::Teleport);
    blink.at_ticks = 2300;
    observe_teleport({blink}, 3, p);
    REQUIRE(p.blinked);
    CHECK(p.blinked_at_ticks == 2300);

    // ...and somebody ELSE blinking is not our apprentice blinking.
    TeleportProgress q;
    GameEvent other = blink;
    other.actor_id = 9;
    observe_teleport({other}, 3, q);
    CHECK_FALSE(q.blinked);
}

TEST_CASE("the teleport mode reports a failure on timeout, and still restages",
          "[teleport][mode]") {
    TeleportConfig cfg;
    cfg.max_ticks = ticks_of(30.0f);
    TeleportMode mode(cfg);
    mode.Configure();

    const std::vector<CharacterState> rows{fighter(0), fighter(1)};
    CHECK_FALSE(mode.Observe(rows, kNoEvents, ticks_of(30.0f) - 1));
    CHECK(mode.Observe(rows, kNoEvents, ticks_of(30.0f)));
    CHECK(mode.Status().find("FAILED") != std::string::npos);
}

TEST_CASE("the training dummy is worth its anchor and swings at nothing", "[threat]") {
    // The whole creature IS the number. It has to be perceivable (Monster, not
    // Critter -- nearest_enemy skips wildlife entirely) and it has to be
    // harmless, so what a brain does about it can be watched without a race
    // against the observer's own death.
    const CharacterDesc& d =
        DefaultCreatureCatalog().defs[static_cast<int>(CreatureId::TrainingDummy)];
    CHECK(d.archetype == Archetype::Monster);
    CHECK(d.attack_count == 0);
    CHECK(d.attack_damage == 0.0f);  // nor the legacy single-attack field
    CHECK(threat_target(CreatureId::TrainingDummy, 1) == 20.0f);

    // ...and it stays out of the duel pool BY THE UNARMED RULE, with no name
    // check anywhere: assert the rule, not the roster.
    const std::vector<CreatureId> pool = duel_pool(DefaultCreatureCatalog());
    CHECK(std::find(pool.begin(), pool.end(), CreatureId::TrainingDummy) == pool.end());
}
