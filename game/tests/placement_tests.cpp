// Building placement geometry + poppable (urban-sprawl) system.
//
// The lattice-exact model is pinned here: footprint/margin triangle counts, the
// diagonal span rule, parity snapping, symmetric blocking, and the integer
// urban-score that drives House/Sewer spawning.

#include "game_state.h"
#include "placement.h"
#include "sim_internal.hpp"  // make_world / dispatch_into / buildings_of / world_of / probe_of

#include <catch_amalgamated.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

using namespace badlands;

namespace {

int footprint_count(int kind, int rot, glm::vec2 raw) {
    Footprint fp = make_footprint(kind, rot, snap_center(kind, rot, raw));
    std::vector<TriRef> tris;
    footprint_triangles(fp, tris);
    return static_cast<int>(tris.size());
}

int margin_count(int kind, int rot, glm::vec2 raw) {
    Footprint fp = make_footprint(kind, rot, snap_center(kind, rot, raw));
    std::vector<TriRef> tris;
    margin_triangles(fp, tris);
    return static_cast<int>(tris.size());
}

std::vector<BuildingState> snapshot(const BadlandsGame* game) { return buildings_of(*game); }

// Placement goes through the generic action trigger; this preserves the old
// place_building return semantics (id, or UINT32_MAX on rejection).
uint32_t place(BadlandsGame* game, const PlacementDesc* desc) {
    Action a{ActionKind::PlaceBuilding, 0, desc->world_x, desc->world_z, desc->kind,
             desc->rotation_index};
    int64_t r = dispatch_into(*game, a);
    return (r < 0) ? UINT32_MAX : static_cast<uint32_t>(r);
}

int count_kind(const std::vector<BuildingState>& rows, int kind) {
    return static_cast<int>(std::count_if(rows.begin(), rows.end(), [kind](const BuildingState& b) {
        return static_cast<int>(b.kind) == kind;
    }));
}

}  // namespace

TEST_CASE("orthogonal footprints are exact tile rectangles") {
    CHECK(footprint_count(static_cast<int32_t>(BuildingKind::Watchtower), 0, {0.3f, 0.3f}) == 4);     // 1x1
    CHECK(footprint_count(static_cast<int32_t>(BuildingKind::Castle), 0, {0.1f, -0.2f}) == 64);       // 4x4
    CHECK(footprint_count(static_cast<int32_t>(BuildingKind::Tavern), 0, {5.0f, 5.0f}) == 8);         // 2x1
    // 90 deg is the same footprint area, transposed.
    CHECK(footprint_count(static_cast<int32_t>(BuildingKind::Tavern), 2, {5.0f, 5.0f}) == 8);
}

TEST_CASE("diagonal footprints snap to the 45-degree lattice (round(dim*sqrt2))") {
    CHECK(footprint_count(static_cast<int32_t>(BuildingKind::Watchtower), 1, {0.0f, 0.0f}) == 2);   // 1x1 -> spans (1,1)
    CHECK(footprint_count(static_cast<int32_t>(BuildingKind::HuntersCamp), 1, {0.0f, 0.0f}) == 32);  // 3x3 -> (4,4)
    CHECK(footprint_count(static_cast<int32_t>(BuildingKind::Castle), 1, {0.0f, 0.0f}) == 72);      // 4x4 -> (6,6)
    // 2x2 "diamond": no 2x2 building in the roster, but the rule is the span
    // formula; a 2x1 building gives spans (3,1) = 6 triangles either diagonal.
    CHECK(footprint_count(static_cast<int32_t>(BuildingKind::Tavern), 1, {0.0f, 0.0f}) == 6);
    CHECK(footprint_count(static_cast<int32_t>(BuildingKind::Tavern), 3, {0.0f, 0.0f}) == 6);
}

TEST_CASE("diagonal 45 and 135 transpose the footprint") {
    Footprint a = make_footprint(static_cast<int32_t>(BuildingKind::Tavern), 1, snap_center(static_cast<int32_t>(BuildingKind::Tavern), 1, {0.0f, 0.0f}));
    Footprint b = make_footprint(static_cast<int32_t>(BuildingKind::Tavern), 3, snap_center(static_cast<int32_t>(BuildingKind::Tavern), 3, {0.0f, 0.0f}));
    // Tavern is 2x1: one diagonal is long in u (span 3) and short in v (span 1),
    // the other is the reverse.
    CHECK((a.q - a.p) == (b.s - b.r));
    CHECK((a.s - a.r) == (b.q - b.p));
    CHECK((a.q - a.p) != (a.s - a.r));
}

TEST_CASE("blocking margins are the 1-tile L1 dilation, minus the footprint") {
    CHECK(margin_count(static_cast<int32_t>(BuildingKind::Watchtower), 0, {0.3f, 0.3f}) == 24);  // ortho 1x1 octagon
    CHECK(margin_count(static_cast<int32_t>(BuildingKind::Watchtower), 1, {0.0f, 0.0f}) == 16);  // diag 1x1 ring
    CHECK(margin_count(static_cast<int32_t>(BuildingKind::HuntersCamp), 1, {0.0f, 0.0f}) == 40);  // diag 3x3 ring

    // Footprint and margin are disjoint.
    Footprint fp = make_footprint(static_cast<int32_t>(BuildingKind::Scriptorium), 0, snap_center(static_cast<int32_t>(BuildingKind::Scriptorium), 0, {8.0f, 8.0f}));
    std::vector<TriRef> foot, marg;
    footprint_triangles(fp, foot);
    margin_triangles(fp, marg);
    for (const TriRef& f : foot) {
        int fi = tri_index(f.tx, f.tz, static_cast<int>(f.corner));
        for (const TriRef& m : marg) {
            CHECK(fi != tri_index(m.tx, m.tz, static_cast<int>(m.corner)));
        }
    }
}

TEST_CASE("parity snapping picks the tile-center vs corner lattice") {
    // Odd dimension -> half-integer lattice; even -> integer.
    CHECK(snap_center(static_cast<int32_t>(BuildingKind::Watchtower), 0, {3.2f, -4.7f}) == glm::vec2(3.5f, -4.5f));
    CHECK(snap_center(static_cast<int32_t>(BuildingKind::Castle), 0, {0.4f, 0.3f}) == glm::vec2(0.0f, 0.0f));
    // Tavern 2x1: x even (integer), z odd (half). Rotated 90, the axes swap.
    CHECK(snap_center(static_cast<int32_t>(BuildingKind::Tavern), 0, {3.3f, 2.2f}) == glm::vec2(3.0f, 2.5f));
    CHECK(snap_center(static_cast<int32_t>(BuildingKind::Tavern), 2, {3.3f, 2.2f}) == glm::vec2(3.5f, 2.0f));
}

TEST_CASE("a fresh game holds only the castle, with starting gold") {
    auto owned_game = make_world(BrainDesc{});
    BadlandsGame* game = owned_game.get();
    auto rows = snapshot(game);
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].kind == BuildingKind::Castle);
    CHECK(rows[0].center_x == kCastleSpawnX);
    CHECK(rows[0].center_z == kCastleSpawnZ);

    WorldState world = world_of(*game);
    CHECK(world.gold == 1000);
    CHECK(world.grid_half_extent_tiles == kGridHalfExtentTiles);
    CHECK(world.urban_quarters == 0);
    CHECK(world.queued_poppables == 0);
}

TEST_CASE("placement respects the castle footprint and margin") {
    auto owned_game = make_world(BrainDesc{});
    BadlandsGame* game = owned_game.get();
    // Inside the castle -> invalid, state untouched.
    PlacementDesc on_castle{static_cast<int32_t>(BuildingKind::Watchtower), 0, kCastleSpawnX,
                            kCastleSpawnZ};
    CHECK(place(game, &on_castle) == UINT32_MAX);
    CHECK(snapshot(game).size() == 1);

    // Castle spans tiles [-2,2) around its centre; its margin reaches tile x=2,
    // so a tower there is blocked, but far out is clear.
    PlacementDesc in_margin{static_cast<int32_t>(BuildingKind::Watchtower), 0, kCastleSpawnX + 2.5f,
                            kCastleSpawnZ + 0.5f};
    CHECK(place(game, &in_margin) == UINT32_MAX);
    PlacementDesc clear{static_cast<int32_t>(BuildingKind::Watchtower), 0, 30.5f, 0.5f};
    CHECK(place(game, &clear) != UINT32_MAX);
}

TEST_CASE("blocking is symmetric regardless of placement order") {
    PlacementDesc a{static_cast<int32_t>(BuildingKind::Watchtower), 0, 20.5f, 20.5f};
    PlacementDesc b{static_cast<int32_t>(BuildingKind::Watchtower), 0, 21.5f, 20.5f};  // adjacent tile

    auto owned_g1 = make_world(BrainDesc{});

    BadlandsGame* g1 = owned_g1.get();
    CHECK(place(g1, &a) != UINT32_MAX);
    CHECK(place(g1, &b) == UINT32_MAX);

    auto owned_g2 = make_world(BrainDesc{});

    BadlandsGame* g2 = owned_g2.get();
    CHECK(place(g2, &b) != UINT32_MAX);
    CHECK(place(g2, &a) == UINT32_MAX);
}

namespace {

// The four world corners of a footprint (ortho rect or diagonal (u,v) diamond).
std::vector<glm::vec2> footprint_corners(const Footprint& fp) {
    std::vector<glm::vec2> out;
    if (!fp.diagonal) {
        out = {{fp.x0, fp.z0}, {fp.x1, fp.z0}, {fp.x1, fp.z1}, {fp.x0, fp.z1}};
    } else {
        int us[2] = {fp.p, fp.q}, vs[2] = {fp.r, fp.s};
        for (int u : us)
            for (int v : vs) {
                out.push_back({(u + v) * 0.5f, (u - v) * 0.5f});
            }
    }
    return out;
}

// The four world corners of the render box drawn at `center` (local +/- size/2
// rotated by yaw, matching glam Mat4::from_rotation_y, then translated).
std::vector<glm::vec2> render_box_corners(int kind, int rot, glm::vec2 center) {
    RenderBox box = RenderBoxOf(static_cast<BuildingKind>(kind), rot);
    float hx = box.size_x * 0.5f, hz = box.size_z * 0.5f;
    float c = std::cos(box.yaw_radians), s = std::sin(box.yaw_radians);
    std::vector<glm::vec2> out;
    for (float lx : {-hx, hx})
        for (float lz : {-hz, hz}) {
            out.push_back({center.x + lx * c + lz * s, center.y - lx * s + lz * c});
        }
    return out;
}

// Set-equality of two 4-corner sets within a tolerance.
bool same_corner_set(std::vector<glm::vec2> a, std::vector<glm::vec2> b) {
    if (a.size() != b.size()) return false;
    for (const glm::vec2& pa : a) {
        auto it = std::find_if(b.begin(), b.end(),
                               [&](const glm::vec2& pb) { return glm::distance(pa, pb) < 1e-3f; });
        if (it == b.end()) return false;
        b.erase(it);
    }
    return true;
}

}  // namespace

TEST_CASE("the rendered box matches the grid footprint for every kind and rotation") {
    // Regression: a 2x1 building placed diagonally snaps to a (3,1) lattice
    // diamond whose long axis runs along u=x+z, but the old renderer drew a
    // (2x1) box at rotation_index*45 deg, whose long axis landed PERPENDICULAR
    // to the footprint -- so the cuboid visually overlapped neighbours the real
    // footprint cleared, and the margin appeared on the wrong sides.
    for (int kind = 0; kind < static_cast<int32_t>(BuildingKind::Count); ++kind) {
        for (int rot = 0; rot < 4; ++rot) {
            glm::vec2 c = snap_center(kind, rot, {0.37f, -0.21f});
            Footprint fp = make_footprint(kind, rot, c);
            CHECK(same_corner_set(render_box_corners(kind, rot, c), footprint_corners(fp)));
        }
    }
}

TEST_CASE("the old (w,d)-at-rot*45 box did NOT match the footprint (bug is real)") {
    // Demonstrates the defect the fix closes: for a 2x1 diagonal, the naive box
    // is perpendicular to the footprint, so its corners differ.
    int kind = static_cast<int32_t>(BuildingKind::Tavern), rot = 1;  // 45 deg
    glm::vec2 center = snap_center(kind, rot, {0.37f, -0.21f});
    Footprint fp = make_footprint(kind, rot, center);
    BuildingDef def = BuildingDefOf(static_cast<BuildingKind>(kind));
    float hx = def.width_tiles * 0.5f, hz = def.depth_tiles * 0.5f;
    float yaw = rot * 0.7853981634f;
    float c = std::cos(yaw), s = std::sin(yaw);
    std::vector<glm::vec2> naive;
    for (float lx : {-hx, hx})
        for (float lz : {-hz, hz}) {
            naive.push_back({center.x + lx * c + lz * s, center.y - lx * s + lz * c});
        }
    CHECK_FALSE(same_corner_set(naive, footprint_corners(fp)));
}

TEST_CASE("a diagonal 2x1 overlapping the castle is always rejected") {
    // The placement VALIDITY was already correct (the bug was purely visual);
    // lock that in: no diagonal Tavern that covers a castle-blocked cell is
    // ever accepted.
    for (int rot : {1, 3}) {
        for (int zi = -12; zi <= 12; ++zi) {
            for (int xi = -12; xi <= 12; ++xi) {
                glm::vec2 cb = snap_center(static_cast<int32_t>(BuildingKind::Tavern), rot, {xi * 0.5f, zi * 0.5f});
                auto owned_g = make_world(BrainDesc{});  // castle at origin
                BadlandsGame* g = owned_g.get();
                Footprint fp = make_footprint(static_cast<int32_t>(BuildingKind::Tavern), rot, cb);
                std::vector<TriRef> foot;
                footprint_triangles(fp, foot);
                bool overlaps = false;
                for (const TriRef& t : foot) {
                    if (g->placement.blocked[tri_index(t.tx, t.tz, (int)t.corner)]) {
                        overlaps = true;
                        break;
                    }
                }
                if (overlaps) {
                    CHECK_FALSE(placement_valid(g->placement, fp));
                }
            }
        }
    }
}

TEST_CASE("a candidate whose margin covers an existing footprint is rejected") {
    // Both directions of the 1-tile spacing rule: a footprint may not enter an
    // existing margin (checked below), and a margin may not cover an existing
    // footprint. `place_building(..., player=false)` avoids poppable spawns so
    // the two orderings compare cleanly.
    for (int rot = 0; rot < 4; ++rot) {
        for (int zi = -6; zi <= 6; ++zi) {
            for (int xi = -6; xi <= 6; ++xi) {
                // Stamp A, then verify: if the candidate B's margin lands on A's
                // footprint, B must be invalid.
                glm::vec2 cb = snap_center(static_cast<int32_t>(BuildingKind::Watchtower), rot,
                                           {24.0f + xi * 0.5f, 24.0f + zi * 0.5f});
                auto owned_game = make_world(BrainDesc{});
                BadlandsGame* game = owned_game.get();
                place_building(*game, {static_cast<int32_t>(BuildingKind::Scriptorium), 0, 24.0f, 24.0f}, false);
                Footprint fp = make_footprint(static_cast<int32_t>(BuildingKind::Watchtower), rot, cb);
                std::vector<TriRef> marg;
                margin_triangles(fp, marg);
                bool margin_hits_footprint = false;
                for (const TriRef& t : marg) {
                    if (game->placement.footprint[tri_index(t.tx, t.tz, (int)t.corner)]) {
                        margin_hits_footprint = true;
                        break;
                    }
                }
                if (margin_hits_footprint) {
                    CHECK_FALSE(placement_valid(game->placement, fp));
                }
            }
        }
    }
}

TEST_CASE("blocking is bidirectional: placement order never matters") {
    // A candidate's margin covering an existing footprint must block it, just
    // as its footprint landing in an existing margin does. With both directions
    // checked, validity is order-independent for every pair of buildings.
    // (player=false avoids poppable side effects that would desync the two
    // orderings.) An ortho building vs a swept diagonal tower exercises the
    // discrete corner-cut asymmetry the one-way check used to miss.
    const PlacementDesc a{static_cast<int32_t>(BuildingKind::Tavern), 0, 24.0f, 24.0f};
    for (int rot = 0; rot < 4; ++rot) {
        for (int zi = -7; zi <= 7; ++zi) {
            for (int xi = -7; xi <= 7; ++xi) {
                PlacementDesc b{static_cast<int32_t>(BuildingKind::Watchtower), rot, 24.0f + xi * 0.5f,
                                    24.0f + zi * 0.5f};

                auto owned_g1 = make_world(BrainDesc{});

                BadlandsGame* g1 = owned_g1.get();
                place_building(*g1, a, /*player=*/false);
                bool b_after_a = place_building(*g1, b, false) != UINT32_MAX;

                auto owned_g2 = make_world(BrainDesc{});

                BadlandsGame* g2 = owned_g2.get();
                place_building(*g2, b, false);
                bool a_after_b = place_building(*g2, a, false) != UINT32_MAX;

                CHECK(b_after_a == a_after_b);
            }
        }
    }
}

TEST_CASE("probe reports green for a clear spot and red near a building") {
    auto owned = make_world(BrainDesc{});
    BadlandsGame* game = owned.get();
    std::vector<GridTriangle> tris;

    // Comfortably clear of the castle -> valid (rendered green).
    PlacementDesc clear{static_cast<int32_t>(BuildingKind::HuntersCamp), 0, 30.0f, 0.0f};
    PlacementProbe probe = probe_of(*game, clear, tris);
    CHECK(probe.valid);

    // On top of the castle -> invalid (rendered pale red).
    PlacementDesc on_castle{static_cast<int32_t>(BuildingKind::HuntersCamp), 0, kCastleSpawnX,
                            kCastleSpawnZ};
    probe = probe_of(*game, on_castle, tris);
    CHECK(!probe.valid);
}

TEST_CASE("footprints flush to the grid edge are valid; crossing it is not") {
    auto owned_game = make_world(BrainDesc{});
    BadlandsGame* game = owned_game.get();
    PlacementDesc flush{static_cast<int32_t>(BuildingKind::Watchtower), 0, 47.5f, 0.5f};  // tile (47,0), x1=48
    CHECK(place(game, &flush) != UINT32_MAX);
    PlacementDesc over{static_cast<int32_t>(BuildingKind::Watchtower), 0, 48.5f, 0.5f};  // tile (48,..) OOB
    CHECK(place(game, &over) == UINT32_MAX);
}

TEST_CASE("a normal placement spawns one nearby house") {
    auto owned_game = make_world(BrainDesc{});
    BadlandsGame* game = owned_game.get();
    PlacementDesc tavern{static_cast<int32_t>(BuildingKind::Tavern), 0, 12.0f, 0.0f};
    REQUIRE(place(game, &tavern) != UINT32_MAX);

    auto rows = snapshot(game);
    CHECK(count_kind(rows, static_cast<int32_t>(BuildingKind::House)) == 1);
    CHECK(count_kind(rows, static_cast<int32_t>(BuildingKind::Sewer)) == 0);

    WorldState world = world_of(*game);
    CHECK(world.urban_quarters == 4);
    CHECK(world.queued_poppables == 0);

    // The house lands within the search radius of the placed tavern (measured
    // from the tavern's snapped center, which is the poppable anchor).
    glm::vec2 tavern_center{0.0f, 0.0f}, house_center{0.0f, 0.0f};
    for (const auto& b : rows) {
        if (b.kind == BuildingKind::Tavern) tavern_center = {b.center_x, b.center_z};
        if (b.kind == BuildingKind::House) house_center = {b.center_x, b.center_z};
    }
    CHECK(glm::distance(tavern_center, house_center) <= 6.0f);
}

TEST_CASE("the second normal placement adds a sewer before the house") {
    auto owned_game = make_world(BrainDesc{});
    BadlandsGame* game = owned_game.get();
    PlacementDesc t1{static_cast<int32_t>(BuildingKind::Tavern), 0, 12.0f, 0.0f};
    PlacementDesc t2{static_cast<int32_t>(BuildingKind::Apothecary), 0, 12.0f, 12.0f};
    REQUIRE(place(game, &t1) != UINT32_MAX);
    REQUIRE(place(game, &t2) != UINT32_MAX);

    auto rows = snapshot(game);
    CHECK(count_kind(rows, static_cast<int32_t>(BuildingKind::House)) == 2);
    CHECK(count_kind(rows, static_cast<int32_t>(BuildingKind::Sewer)) == 1);

    // Sewers are processed before houses: the sewer from the 2nd placement gets
    // the lower id.
    uint32_t sewer_id = UINT32_MAX, last_house_id = 0;
    for (const auto& b : rows) {
        if (b.kind == BuildingKind::Sewer) sewer_id = std::min(sewer_id, b.id);
        if (b.kind == BuildingKind::House) last_house_id = std::max(last_house_id, b.id);
    }
    CHECK(sewer_id < last_house_id);
}

TEST_CASE("the urban score drives poppables; watchtowers contribute less") {
    // 4 watchtowers = 12 quarters = 3 houses + 1 sewer.
    auto owned_towers = make_world(BrainDesc{});
    BadlandsGame* towers = owned_towers.get();
    PlacementDesc wt[4] = {
        {static_cast<int32_t>(BuildingKind::Watchtower), 0, -30.5f, -30.5f},
        {static_cast<int32_t>(BuildingKind::Watchtower), 0, -30.5f, 30.5f},
        {static_cast<int32_t>(BuildingKind::Watchtower), 0, 30.5f, -30.5f},
        {static_cast<int32_t>(BuildingKind::Watchtower), 0, 30.5f, 30.5f},
    };
    for (auto& d : wt) REQUIRE(place(towers, &d) != UINT32_MAX);
    auto trows = snapshot(towers);
    CHECK(count_kind(trows, static_cast<int32_t>(BuildingKind::House)) == 3);
    CHECK(count_kind(trows, static_cast<int32_t>(BuildingKind::Sewer)) == 1);
    WorldState tw = world_of(*towers);
    CHECK(tw.urban_quarters == 12);

    // 4 full-weight buildings = 16 quarters = 4 houses + 2 sewers.
    auto owned_halls = make_world(BrainDesc{});
    BadlandsGame* halls = owned_halls.get();
    PlacementDesc fc[4] = {
        {static_cast<int32_t>(BuildingKind::Scriptorium), 0, -30.0f, -30.0f},
        {static_cast<int32_t>(BuildingKind::Scriptorium), 0, -30.0f, 30.0f},
        {static_cast<int32_t>(BuildingKind::Scriptorium), 0, 30.0f, -30.0f},
        {static_cast<int32_t>(BuildingKind::Scriptorium), 0, 30.0f, 30.0f},
    };
    for (auto& d : fc) REQUIRE(place(halls, &d) != UINT32_MAX);
    auto hrows = snapshot(halls);
    CHECK(count_kind(hrows, static_cast<int32_t>(BuildingKind::House)) == 4);
    CHECK(count_kind(hrows, static_cast<int32_t>(BuildingKind::Sewer)) == 2);
}

TEST_CASE("owed poppables stay queued when crowded and drain when space opens") {
    auto owned_game = make_world(BrainDesc{});
    BadlandsGame* game = owned_game.get();
    // White-box: owe a house but leave no room anywhere.
    std::fill(game->placement.blocked.begin(), game->placement.blocked.end(), 1);
    game->placement.urban_quarters = 4;  // owes 1 house
    process_poppables(*game, {0.0f, 0.0f});
    WorldState world = world_of(*game);
    CHECK(world.queued_poppables == 1);

    // Open the map back up; the retry drains the queue.
    std::fill(game->placement.blocked.begin(), game->placement.blocked.end(), 0);
    process_poppables(*game, {0.0f, 0.0f});
    world = world_of(*game);
    CHECK(world.queued_poppables == 0);
}

TEST_CASE("identical placement sequences are deterministic") {
    auto run = []() {
        auto owned_game = make_world(BrainDesc{});
        BadlandsGame* game = owned_game.get();
        PlacementDesc seq[3] = {
            {static_cast<int32_t>(BuildingKind::Tavern), 0, 14.0f, 0.0f},
            {static_cast<int32_t>(BuildingKind::Apothecary), 1, -14.0f, 6.0f},
            {static_cast<int32_t>(BuildingKind::Scriptorium), 0, 0.0f, 20.0f},
        };
        for (auto& d : seq) place(game, &d);
        auto rows = snapshot(game);
        return rows;
    };
    auto a = run();
    auto b = run();
    REQUIRE(a.size() == b.size());
    for (size_t i = 0; i < a.size(); ++i) {
        CHECK(a[i].kind == b[i].kind);
        CHECK(a[i].center_x == b[i].center_x);
        CHECK(a[i].center_z == b[i].center_z);
        CHECK(a[i].rotation_index == b[i].rotation_index);
    }
}

TEST_CASE("v0.3 placement helpers: approach tile, nearest-of-kind, occupancy rebuild") {
    auto owned_game = make_world(BrainDesc{});  // castle id 0 at origin
    BadlandsGame* game = owned_game.get();
    PlacementState& st = game->placement;

    // Place a tavern (id 1) and apothecary (id 2) without triggering poppables.
    PlacementDesc tav{static_cast<int32_t>(BuildingKind::Tavern), 0, 16.0f, 0.0f};
    PlacementDesc apo{static_cast<int32_t>(BuildingKind::Apothecary), 0, 16.0f, 8.0f};
    uint32_t tav_id = place_building(*game, tav, /*player=*/false);
    uint32_t apo_id = place_building(*game, apo, /*player=*/false);
    REQUIRE(tav_id == 1u);
    REQUIRE(apo_id == 2u);

    SECTION("building_approach_tile returns a free exterior tile") {
        glm::vec2 tile;
        REQUIRE(building_approach_tile(st, st.buildings[tav_id], tile));
        int tx = static_cast<int>(std::floor(tile.x));
        int tz = static_cast<int>(std::floor(tile.y));
        for (int c = 0; c < 4; ++c) {
            CHECK(st.blocked[tri_index(tx, tz, c)] == 0);
        }
    }

    SECTION("nearest_building_of is alive-aware") {
        CHECK(nearest_building_of(st, static_cast<int32_t>(BuildingKind::Apothecary), {16.0f, 8.0f}) == apo_id);
        CHECK(nearest_building_of(st, static_cast<int32_t>(BuildingKind::Castle), {0.0f, 0.0f}) == 0u);
        CHECK(nearest_building_of(st, static_cast<int32_t>(BuildingKind::Watchtower), {0.0f, 0.0f}) == UINT32_MAX);
        // Tombstone the apothecary: it drops out of nearest-of-kind queries.
        st.buildings[apo_id].alive = false;
        CHECK(nearest_building_of(st, static_cast<int32_t>(BuildingKind::Apothecary), {16.0f, 8.0f}) == UINT32_MAX);
    }

    SECTION("rebuild_occupancy frees a tombstoned footprint; ids stay stable") {
        Footprint fp = make_footprint(static_cast<int32_t>(BuildingKind::Tavern), 0, st.buildings[tav_id].center);
        std::vector<TriRef> foot;
        footprint_triangles(fp, foot);
        REQUIRE(!foot.empty());
        int idx = tri_index(foot[0].tx, foot[0].tz, static_cast<int>(foot[0].corner));
        CHECK(st.blocked[idx] == 1);

        st.buildings[tav_id].alive = false;
        rebuild_occupancy(st);
        CHECK(st.blocked[idx] == 0);

        // buildings_of skips the dead tavern but the apothecary keeps id 2.
        auto rows = snapshot(game);
        bool saw_apo = false, saw_tav = false;
        for (const auto& r : rows) {
            if (r.id == apo_id) {
                saw_apo = true;
                CHECK(r.kind == BuildingKind::Apothecary);
            }
            if (r.id == tav_id) {
                saw_tav = true;
            }
        }
        CHECK(saw_apo);
        CHECK(!saw_tav);
    }
}

TEST_CASE("Dispatch is the generic action trigger") {
    auto owned = make_world(BrainDesc{});  // castle id 0
    BadlandsGame* game = owned.get();

    SECTION("PLACE_BUILDING round-trips and rejects overlap") {
        Action ok{ActionKind::PlaceBuilding, 0, 24.0f, 24.0f,
                  static_cast<int32_t>(BuildingKind::Tavern), 0};
        int64_t id = dispatch_into(*game, ok);
        CHECK(id >= 0);
        // Same footprint again -> rejected.
        CHECK(dispatch_into(*game, ok) < 0);
    }

    SECTION("unknown action kind returns an error") {
        Action bogus{static_cast<ActionKind>(999), 0, 0.0f, 0.0f, 0, 0};
        CHECK(dispatch_into(*game, bogus) < 0);
    }

    SECTION("DESTROY_BUILDING rejects the non-destructible castle") {
        Action destroy_castle{ActionKind::DestroyBuilding, 0, 0.0f, 0.0f, 0, 0};
        CHECK(dispatch_into(*game, destroy_castle) < 0);
        CHECK(snapshot(game).size() == 1);  // castle still standing
    }

    SECTION("DESTROY_BUILDING removes a user-destructible building") {
        PlacementDesc tower{static_cast<int32_t>(BuildingKind::Watchtower), 0, 24.0f, -24.0f};
        uint32_t id = place(game, &tower);
        REQUIRE(id != UINT32_MAX);
        size_t before = snapshot(game).size();
        Action destroy{ActionKind::DestroyBuilding, id, 0.0f, 0.0f, 0, 0};
        CHECK(dispatch_into(*game, destroy) == 0);
        CHECK(snapshot(game).size() == before - 1);
        // Destroying it again fails (already dead).
        CHECK(dispatch_into(*game, destroy) < 0);
    }
}

TEST_CASE("BuildingDef flags: user_destructible and enemy_targettable are decoupled") {
    // Castle: enemy target but not user-destructible.
    BuildingDef castle = BuildingDefOf(BuildingKind::Castle);
    CHECK(!castle.user_destructible);
    CHECK(castle.enemy_targettable);
    // A guild: user-destructible but not an enemy target.
    BuildingDef guild = BuildingDefOf(BuildingKind::FreeCompanyQuarters);
    CHECK(guild.user_destructible);
    CHECK(!guild.enemy_targettable);
    // House (poppable): enemy target, not user-destructible.
    BuildingDef house = BuildingDefOf(BuildingKind::House);
    CHECK(!house.user_destructible);
    CHECK(house.enemy_targettable);
}
