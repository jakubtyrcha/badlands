// The puppet-master layer: arena layouts and the duel round lifecycle.
//
// Links badlands_game_lib and NOTHING else -- no engine, no Dawn, no SDL. That
// is the point rather than an economy: it is a compile-time proof that a mode
// is view-free, so the same code the window drives can be reasoned about (and
// run) without one.

#include "executables/ai_sandbox/arena.hpp"
#include "executables/ai_sandbox/duel_mode.hpp"

#include "game_state.h"
#include "nav_world.h"       // rebuild_navmesh_if_stale
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
    cfg.max_millis = 60000;
    cfg.linger_millis = 5000;
    DuelMode mode(cfg);
    mode.Configure();

    const std::vector<CharacterState> both{fighter(0), fighter(1)};
    const std::vector<CharacterState> left_only{fighter(0)};

    CHECK_FALSE(mode.Observe(both, 1000));
    CHECK_FALSE(mode.Observe(left_only, 2000));   // decided here, but lingering
    CHECK_FALSE(mode.Observe(left_only, 6999));   // still inside the linger
    CHECK(mode.Observe(left_only, 7000));         // 2000 + 5000: restage
}

TEST_CASE("a duel with both sides alive times out as a draw", "[duel]") {
    DuelConfig cfg;
    cfg.max_millis = 60000;
    cfg.linger_millis = 5000;
    DuelMode mode(cfg);
    mode.Configure();

    const std::vector<CharacterState> both{fighter(0), fighter(1)};
    CHECK_FALSE(mode.Observe(both, 59999));
    // No linger on a timeout: there is nothing left to watch.
    CHECK(mode.Observe(both, 60000));
}

TEST_CASE("a duel with both sides gone is a draw, not a win", "[duel]") {
    DuelConfig cfg;
    cfg.linger_millis = 1000;
    DuelMode mode(cfg);
    mode.Configure();

    const std::vector<CharacterState> nobody{};
    CHECK_FALSE(mode.Observe(nobody, 2000));  // decided, lingering
    CHECK(mode.Observe(nobody, 3000));
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
