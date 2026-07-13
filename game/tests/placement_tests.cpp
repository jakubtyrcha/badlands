// Building placement geometry + poppable (urban-sprawl) system.
//
// The lattice-exact model is pinned here: footprint/margin triangle counts, the
// diagonal span rule, parity snapping, symmetric blocking, and the integer
// urban-score that drives House/Sewer spawning.

#include "badlands_game.h"
#include "game_state.h"
#include "placement.h"

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

std::vector<GameBuildingState> snapshot(const BadlandsGame* game) {
    std::vector<GameBuildingState> rows(256);
    uint32_t n = game_buildings(game, rows.data(), static_cast<uint32_t>(rows.size()));
    rows.resize(n);
    return rows;
}

int count_kind(const std::vector<GameBuildingState>& rows, int kind) {
    return static_cast<int>(std::count_if(rows.begin(), rows.end(),
                                          [kind](const GameBuildingState& b) { return b.kind == kind; }));
}

}  // namespace

TEST_CASE("orthogonal footprints are exact tile rectangles") {
    CHECK(footprint_count(GAME_BUILDING_WATCHTOWER, 0, {0.3f, 0.3f}) == 4);     // 1x1
    CHECK(footprint_count(GAME_BUILDING_CASTLE, 0, {0.1f, -0.2f}) == 64);       // 4x4
    CHECK(footprint_count(GAME_BUILDING_TAVERN, 0, {5.0f, 5.0f}) == 8);         // 2x1
    // 90 deg is the same footprint area, transposed.
    CHECK(footprint_count(GAME_BUILDING_TAVERN, 2, {5.0f, 5.0f}) == 8);
}

TEST_CASE("diagonal footprints snap to the 45-degree lattice (round(dim*sqrt2))") {
    CHECK(footprint_count(GAME_BUILDING_WATCHTOWER, 1, {0.0f, 0.0f}) == 2);   // 1x1 -> spans (1,1)
    CHECK(footprint_count(GAME_BUILDING_HUNTERS_CAMP, 1, {0.0f, 0.0f}) == 32);  // 3x3 -> (4,4)
    CHECK(footprint_count(GAME_BUILDING_CASTLE, 1, {0.0f, 0.0f}) == 72);      // 4x4 -> (6,6)
    // 2x2 "diamond": no 2x2 building in the roster, but the rule is the span
    // formula; a 2x1 building gives spans (3,1) = 6 triangles either diagonal.
    CHECK(footprint_count(GAME_BUILDING_TAVERN, 1, {0.0f, 0.0f}) == 6);
    CHECK(footprint_count(GAME_BUILDING_TAVERN, 3, {0.0f, 0.0f}) == 6);
}

TEST_CASE("diagonal 45 and 135 transpose the footprint") {
    Footprint a = make_footprint(GAME_BUILDING_TAVERN, 1, snap_center(GAME_BUILDING_TAVERN, 1, {0.0f, 0.0f}));
    Footprint b = make_footprint(GAME_BUILDING_TAVERN, 3, snap_center(GAME_BUILDING_TAVERN, 3, {0.0f, 0.0f}));
    // Tavern is 2x1: one diagonal is long in u (span 3) and short in v (span 1),
    // the other is the reverse.
    CHECK((a.q - a.p) == (b.s - b.r));
    CHECK((a.s - a.r) == (b.q - b.p));
    CHECK((a.q - a.p) != (a.s - a.r));
}

TEST_CASE("blocking margins are the 1-tile L1 dilation, minus the footprint") {
    CHECK(margin_count(GAME_BUILDING_WATCHTOWER, 0, {0.3f, 0.3f}) == 24);  // ortho 1x1 octagon
    CHECK(margin_count(GAME_BUILDING_WATCHTOWER, 1, {0.0f, 0.0f}) == 16);  // diag 1x1 ring
    CHECK(margin_count(GAME_BUILDING_HUNTERS_CAMP, 1, {0.0f, 0.0f}) == 40);  // diag 3x3 ring

    // Footprint and margin are disjoint.
    Footprint fp = make_footprint(GAME_BUILDING_SCRIPTORIUM, 0, snap_center(GAME_BUILDING_SCRIPTORIUM, 0, {8.0f, 8.0f}));
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
    CHECK(snap_center(GAME_BUILDING_WATCHTOWER, 0, {3.2f, -4.7f}) == glm::vec2(3.5f, -4.5f));
    CHECK(snap_center(GAME_BUILDING_CASTLE, 0, {0.4f, 0.3f}) == glm::vec2(0.0f, 0.0f));
    // Tavern 2x1: x even (integer), z odd (half). Rotated 90, the axes swap.
    CHECK(snap_center(GAME_BUILDING_TAVERN, 0, {3.3f, 2.2f}) == glm::vec2(3.0f, 2.5f));
    CHECK(snap_center(GAME_BUILDING_TAVERN, 2, {3.3f, 2.2f}) == glm::vec2(3.5f, 2.0f));
}

TEST_CASE("a fresh game holds only the castle, with starting gold") {
    BadlandsGame* game = game_create(nullptr);
    auto rows = snapshot(game);
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].kind == GAME_BUILDING_CASTLE);
    CHECK(rows[0].center_x == 0.0f);
    CHECK(rows[0].center_z == 0.0f);

    GameWorldState world;
    game_world(game, &world);
    CHECK(world.gold == 1000);
    CHECK(world.grid_half_extent_tiles == GAME_GRID_HALF_EXTENT_TILES);
    CHECK(world.urban_quarters == 0);
    CHECK(world.queued_poppables == 0);
    game_destroy(game);
}

TEST_CASE("placement respects the castle footprint and margin") {
    BadlandsGame* game = game_create(nullptr);
    // Inside the castle -> invalid, state untouched.
    GamePlacementDesc on_castle{GAME_BUILDING_WATCHTOWER, 0, 0.0f, 0.0f};
    CHECK(game_place_building(game, &on_castle) == UINT32_MAX);
    CHECK(snapshot(game).size() == 1);

    // Castle spans tiles [-2,2); its margin reaches tile x=2, so a tower there
    // is blocked, but far out is clear.
    GamePlacementDesc in_margin{GAME_BUILDING_WATCHTOWER, 0, 2.5f, 0.5f};
    CHECK(game_place_building(game, &in_margin) == UINT32_MAX);
    GamePlacementDesc clear{GAME_BUILDING_WATCHTOWER, 0, 30.5f, 0.5f};
    CHECK(game_place_building(game, &clear) != UINT32_MAX);
    game_destroy(game);
}

TEST_CASE("blocking is symmetric regardless of placement order") {
    GamePlacementDesc a{GAME_BUILDING_WATCHTOWER, 0, 20.5f, 20.5f};
    GamePlacementDesc b{GAME_BUILDING_WATCHTOWER, 0, 21.5f, 20.5f};  // adjacent tile

    BadlandsGame* g1 = game_create(nullptr);
    CHECK(game_place_building(g1, &a) != UINT32_MAX);
    CHECK(game_place_building(g1, &b) == UINT32_MAX);
    game_destroy(g1);

    BadlandsGame* g2 = game_create(nullptr);
    CHECK(game_place_building(g2, &b) != UINT32_MAX);
    CHECK(game_place_building(g2, &a) == UINT32_MAX);
    game_destroy(g2);
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
    GameRenderBox box = game_render_box(kind, rot);
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
    for (int kind = 0; kind < GAME_BUILDING_KIND_COUNT; ++kind) {
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
    int kind = GAME_BUILDING_TAVERN, rot = 1;  // 45 deg
    glm::vec2 center = snap_center(kind, rot, {0.37f, -0.21f});
    Footprint fp = make_footprint(kind, rot, center);
    GameBuildingDef def = game_building_def(kind);
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
                glm::vec2 cb = snap_center(GAME_BUILDING_TAVERN, rot, {xi * 0.5f, zi * 0.5f});
                BadlandsGame* g = game_create(nullptr);  // castle at origin
                Footprint fp = make_footprint(GAME_BUILDING_TAVERN, rot, cb);
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
                game_destroy(g);
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
                glm::vec2 cb = snap_center(GAME_BUILDING_WATCHTOWER, rot,
                                           {24.0f + xi * 0.5f, 24.0f + zi * 0.5f});
                BadlandsGame* game = game_create(nullptr);
                place_building(*game, {GAME_BUILDING_SCRIPTORIUM, 0, 24.0f, 24.0f}, false);
                Footprint fp = make_footprint(GAME_BUILDING_WATCHTOWER, rot, cb);
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
                game_destroy(game);
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
    const GamePlacementDesc a{GAME_BUILDING_TAVERN, 0, 24.0f, 24.0f};
    for (int rot = 0; rot < 4; ++rot) {
        for (int zi = -7; zi <= 7; ++zi) {
            for (int xi = -7; xi <= 7; ++xi) {
                GamePlacementDesc b{GAME_BUILDING_WATCHTOWER, rot, 24.0f + xi * 0.5f,
                                    24.0f + zi * 0.5f};

                BadlandsGame* g1 = game_create(nullptr);
                place_building(*g1, a, /*player=*/false);
                bool b_after_a = place_building(*g1, b, false) != UINT32_MAX;
                game_destroy(g1);

                BadlandsGame* g2 = game_create(nullptr);
                place_building(*g2, b, false);
                bool a_after_b = place_building(*g2, a, false) != UINT32_MAX;
                game_destroy(g2);

                CHECK(b_after_a == a_after_b);
            }
        }
    }
}

TEST_CASE("probe reports green for a clear spot and red near a building") {
    BadlandsGame* game = game_create(nullptr);
    std::vector<GameGridTriangle> tris(4096);
    GamePlacementProbe probe{};

    // Comfortably clear of the castle -> valid (rendered green).
    GamePlacementDesc clear{GAME_BUILDING_HUNTERS_CAMP, 0, 30.0f, 0.0f};
    game_probe_placement(game, &clear, &probe, tris.data(),
                         static_cast<uint32_t>(tris.size()));
    CHECK(probe.valid == 1);

    // On top of the castle -> invalid (rendered pale red).
    GamePlacementDesc on_castle{GAME_BUILDING_HUNTERS_CAMP, 0, 0.0f, 0.0f};
    game_probe_placement(game, &on_castle, &probe, tris.data(),
                         static_cast<uint32_t>(tris.size()));
    CHECK(probe.valid == 0);
    game_destroy(game);
}

TEST_CASE("footprints flush to the grid edge are valid; crossing it is not") {
    BadlandsGame* game = game_create(nullptr);
    GamePlacementDesc flush{GAME_BUILDING_WATCHTOWER, 0, 47.5f, 0.5f};  // tile (47,0), x1=48
    CHECK(game_place_building(game, &flush) != UINT32_MAX);
    GamePlacementDesc over{GAME_BUILDING_WATCHTOWER, 0, 48.5f, 0.5f};  // tile (48,..) OOB
    CHECK(game_place_building(game, &over) == UINT32_MAX);
    game_destroy(game);
}

TEST_CASE("a normal placement spawns one nearby house") {
    BadlandsGame* game = game_create(nullptr);
    GamePlacementDesc tavern{GAME_BUILDING_TAVERN, 0, 12.0f, 0.0f};
    REQUIRE(game_place_building(game, &tavern) != UINT32_MAX);

    auto rows = snapshot(game);
    CHECK(count_kind(rows, GAME_BUILDING_HOUSE) == 1);
    CHECK(count_kind(rows, GAME_BUILDING_SEWER) == 0);

    GameWorldState world;
    game_world(game, &world);
    CHECK(world.urban_quarters == 4);
    CHECK(world.queued_poppables == 0);

    // The house lands within the search radius of the placed tavern (measured
    // from the tavern's snapped center, which is the poppable anchor).
    glm::vec2 tavern_center{0.0f, 0.0f}, house_center{0.0f, 0.0f};
    for (const auto& b : rows) {
        if (b.kind == GAME_BUILDING_TAVERN) tavern_center = {b.center_x, b.center_z};
        if (b.kind == GAME_BUILDING_HOUSE) house_center = {b.center_x, b.center_z};
    }
    CHECK(glm::distance(tavern_center, house_center) <= 6.0f);
    game_destroy(game);
}

TEST_CASE("the second normal placement adds a sewer before the house") {
    BadlandsGame* game = game_create(nullptr);
    GamePlacementDesc t1{GAME_BUILDING_TAVERN, 0, 12.0f, 0.0f};
    GamePlacementDesc t2{GAME_BUILDING_APOTHECARY, 0, 12.0f, 12.0f};
    REQUIRE(game_place_building(game, &t1) != UINT32_MAX);
    REQUIRE(game_place_building(game, &t2) != UINT32_MAX);

    auto rows = snapshot(game);
    CHECK(count_kind(rows, GAME_BUILDING_HOUSE) == 2);
    CHECK(count_kind(rows, GAME_BUILDING_SEWER) == 1);

    // Sewers are processed before houses: the sewer from the 2nd placement gets
    // the lower id.
    uint32_t sewer_id = UINT32_MAX, last_house_id = 0;
    for (const auto& b : rows) {
        if (b.kind == GAME_BUILDING_SEWER) sewer_id = std::min(sewer_id, b.id);
        if (b.kind == GAME_BUILDING_HOUSE) last_house_id = std::max(last_house_id, b.id);
    }
    CHECK(sewer_id < last_house_id);
    game_destroy(game);
}

TEST_CASE("the urban score drives poppables; watchtowers contribute less") {
    // 4 watchtowers = 12 quarters = 3 houses + 1 sewer.
    BadlandsGame* towers = game_create(nullptr);
    GamePlacementDesc wt[4] = {
        {GAME_BUILDING_WATCHTOWER, 0, -30.5f, -30.5f},
        {GAME_BUILDING_WATCHTOWER, 0, -30.5f, 30.5f},
        {GAME_BUILDING_WATCHTOWER, 0, 30.5f, -30.5f},
        {GAME_BUILDING_WATCHTOWER, 0, 30.5f, 30.5f},
    };
    for (auto& d : wt) REQUIRE(game_place_building(towers, &d) != UINT32_MAX);
    auto trows = snapshot(towers);
    CHECK(count_kind(trows, GAME_BUILDING_HOUSE) == 3);
    CHECK(count_kind(trows, GAME_BUILDING_SEWER) == 1);
    GameWorldState tw;
    game_world(towers, &tw);
    CHECK(tw.urban_quarters == 12);
    game_destroy(towers);

    // 4 full-weight buildings = 16 quarters = 4 houses + 2 sewers.
    BadlandsGame* halls = game_create(nullptr);
    GamePlacementDesc fc[4] = {
        {GAME_BUILDING_SCRIPTORIUM, 0, -30.0f, -30.0f},
        {GAME_BUILDING_SCRIPTORIUM, 0, -30.0f, 30.0f},
        {GAME_BUILDING_SCRIPTORIUM, 0, 30.0f, -30.0f},
        {GAME_BUILDING_SCRIPTORIUM, 0, 30.0f, 30.0f},
    };
    for (auto& d : fc) REQUIRE(game_place_building(halls, &d) != UINT32_MAX);
    auto hrows = snapshot(halls);
    CHECK(count_kind(hrows, GAME_BUILDING_HOUSE) == 4);
    CHECK(count_kind(hrows, GAME_BUILDING_SEWER) == 2);
    game_destroy(halls);
}

TEST_CASE("owed poppables stay queued when crowded and drain when space opens") {
    BadlandsGame* game = game_create(nullptr);
    // White-box: owe a house but leave no room anywhere.
    std::fill(game->placement.blocked.begin(), game->placement.blocked.end(), 1);
    game->placement.urban_quarters = 4;  // owes 1 house
    process_poppables(*game, {0.0f, 0.0f});
    GameWorldState world;
    game_world(game, &world);
    CHECK(world.queued_poppables == 1);

    // Open the map back up; the retry drains the queue.
    std::fill(game->placement.blocked.begin(), game->placement.blocked.end(), 0);
    process_poppables(*game, {0.0f, 0.0f});
    game_world(game, &world);
    CHECK(world.queued_poppables == 0);
    game_destroy(game);
}

TEST_CASE("identical placement sequences are deterministic") {
    auto run = []() {
        BadlandsGame* game = game_create(nullptr);
        GamePlacementDesc seq[3] = {
            {GAME_BUILDING_TAVERN, 0, 14.0f, 0.0f},
            {GAME_BUILDING_APOTHECARY, 1, -14.0f, 6.0f},
            {GAME_BUILDING_SCRIPTORIUM, 0, 0.0f, 20.0f},
        };
        for (auto& d : seq) game_place_building(game, &d);
        auto rows = snapshot(game);
        game_destroy(game);
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
    BadlandsGame* game = game_create(nullptr);  // castle id 0 at origin
    PlacementState& st = game->placement;

    // Place a tavern (id 1) and apothecary (id 2) without triggering poppables.
    GamePlacementDesc tav{GAME_BUILDING_TAVERN, 0, 16.0f, 0.0f};
    GamePlacementDesc apo{GAME_BUILDING_APOTHECARY, 0, 16.0f, 8.0f};
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
        CHECK(nearest_building_of(st, GAME_BUILDING_APOTHECARY, {16.0f, 8.0f}) == apo_id);
        CHECK(nearest_building_of(st, GAME_BUILDING_CASTLE, {0.0f, 0.0f}) == 0u);
        CHECK(nearest_building_of(st, GAME_BUILDING_WATCHTOWER, {0.0f, 0.0f}) == UINT32_MAX);
        // Tombstone the apothecary: it drops out of nearest-of-kind queries.
        st.buildings[apo_id].alive = false;
        CHECK(nearest_building_of(st, GAME_BUILDING_APOTHECARY, {16.0f, 8.0f}) == UINT32_MAX);
    }

    SECTION("rebuild_occupancy frees a tombstoned footprint; ids stay stable") {
        Footprint fp = make_footprint(GAME_BUILDING_TAVERN, 0, st.buildings[tav_id].center);
        std::vector<TriRef> foot;
        footprint_triangles(fp, foot);
        REQUIRE(!foot.empty());
        int idx = tri_index(foot[0].tx, foot[0].tz, static_cast<int>(foot[0].corner));
        CHECK(st.blocked[idx] == 1);

        st.buildings[tav_id].alive = false;
        rebuild_occupancy(st);
        CHECK(st.blocked[idx] == 0);

        // game_buildings skips the dead tavern but the apothecary keeps id 2.
        auto rows = snapshot(game);
        bool saw_apo = false, saw_tav = false;
        for (const auto& r : rows) {
            if (r.id == apo_id) {
                saw_apo = true;
                CHECK(r.kind == GAME_BUILDING_APOTHECARY);
            }
            if (r.id == tav_id) {
                saw_tav = true;
            }
        }
        CHECK(saw_apo);
        CHECK(!saw_tav);
    }

    game_destroy(game);
}
