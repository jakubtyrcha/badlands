// Nav core (game/src/navmesh): the quadtree decomposition, its adjacency graph,
// A* + string-pulling, and the HPA* abstract cost. Pure CPU over a synthetic
// NavSource -- no sim, no MapData -- which is the whole point of the injected
// source boundary (navmesh/source.h).

#include "navmesh/graph.h"
#include "navmesh/navmesh.h"
#include "navmesh/quadtree.h"
#include "navmesh/source.h"

#include <catch_amalgamated.hpp>

#include <algorithm>
#include <vector>

using namespace badlands::nav;

namespace {

// A hand-built square grid source. Defaults: all passable, cost 1, flat.
struct GridSource : NavSource {
    int n;
    float cell = 1.0f;
    glm::vec2 org{0.0f, 0.0f};
    std::vector<float> c;
    std::vector<float> h;
    std::vector<uint8_t> b;

    explicit GridSource(int side, float cost = 1.0f, float height = 0.0f)
        : n(side),
          c(static_cast<size_t>(side) * side, cost),
          h(static_cast<size_t>(side) * side, height),
          b(static_cast<size_t>(side) * side, kMaskFree) {}

    int side() const override { return n; }
    float cell_size_m() const override { return cell; }
    glm::vec2 origin_m() const override { return org; }
    float cost(int x, int z) const override { return c[idx(x, z)]; }
    float height(int x, int z) const override { return h[idx(x, z)]; }
    uint8_t blocked_mask(int x, int z) const override { return b[idx(x, z)]; }

    // A whole-cell obstacle (all four corner triangles), which is what an
    // axis-aligned footprint stamps -- so every pre-triangle case still means
    // exactly what it meant.
    void set_blocked(int x, int z) { b[idx(x, z)] = kMaskSolid; }
    // One corner triangle: the half-covered cell a diagonal footprint leaves.
    void set_tri(int x, int z, int corner) { b[idx(x, z)] |= tri_bit(corner); }
    void set_cost(int x, int z, float v) { c[idx(x, z)] = v; }
    void set_height(int x, int z, float v) { h[idx(x, z)] = v; }

   private:
    size_t idx(int x, int z) const { return static_cast<size_t>(z) * n + x; }
};

}  // namespace

TEST_CASE("quadtree merges a uniform passable grid into one root leaf", "[nav]") {
    GridSource src(16);  // 16x16, all passable, cost 1, flat
    Quadtree qt;
    qt.Build(src, NavParams{});

    REQUIRE(qt.leaves().size() == 1);
    CHECK(qt.leaves()[0].size == 16);
    CHECK(qt.leaves()[0].passable);
    CHECK(qt.leaves()[0].x0 == 0);
    CHECK(qt.leaves()[0].z0 == 0);
}

TEST_CASE("a single obstacle splits down to a 1-cell impassable leaf", "[nav]") {
    GridSource src(16);
    src.set_blocked(5, 6);
    Quadtree qt;
    qt.Build(src, NavParams{/*cost_epsilon=*/0.05f, /*height_epsilon=*/0.25f,
                            /*clearance_m=*/0.0f});

    const int li = qt.LeafAt(5, 6);
    REQUIRE(li >= 0);
    const Leaf& blk = qt.leaves()[li];
    CHECK(blk.size == 1);
    CHECK_FALSE(blk.passable);
    // A far corner stays in a large passable leaf (the obstacle only refines its
    // own neighbourhood, not the whole map).
    const Leaf& far = qt.leaves()[qt.LeafAt(15, 15)];
    CHECK(far.passable);
    CHECK(far.size > 1);
}

TEST_CASE("clearance dilation blocks the cells around an obstacle", "[nav]") {
    GridSource src(16);
    src.set_blocked(8, 8);
    Quadtree qt;
    qt.Build(src, NavParams{0.05f, 0.25f, /*clearance_m=*/1.0f});
    // The 8 neighbours of the obstacle are now impassable too.
    for (int dz = -1; dz <= 1; ++dz) {
        for (int dx = -1; dx <= 1; ++dx) {
            const Leaf& l = qt.leaves()[qt.LeafAt(8 + dx, 8 + dz)];
            CHECK_FALSE(l.passable);
        }
    }
    CHECK(qt.leaves()[qt.LeafAt(0, 0)].passable);
}

TEST_CASE("differing terrain cost splits a block (cost error stays bounded)", "[nav]") {
    GridSource src(16, 1.0f);
    // Right half is expensive swamp; the split must separate the two costs.
    for (int z = 0; z < 16; ++z)
        for (int x = 8; x < 16; ++x) src.set_cost(x, z, 2.5f);
    Quadtree qt;
    qt.Build(src, NavParams{0.05f, 0.25f, 0.0f});
    CHECK(qt.leaves().size() > 1);
    CHECK(qt.leaves()[qt.LeafAt(2, 2)].cost == Catch::Approx(1.0f));
    CHECK(qt.leaves()[qt.LeafAt(12, 2)].cost == Catch::Approx(2.5f));
}

TEST_CASE("every merged leaf respects the cost and height error bounds", "[nav]") {
    // A varied source: cost stripes + a diagonal height ramp + an obstacle.
    GridSource src(32);
    for (int z = 0; z < 32; ++z)
        for (int x = 0; x < 32; ++x) {
            src.set_cost(x, z, 1.0f + 0.5f * static_cast<float>((x / 4) % 3));
            src.set_height(x, z, 0.1f * static_cast<float>(x + z));
        }
    src.set_blocked(20, 7);
    const NavParams p{0.05f, 0.25f, 1};
    Quadtree qt;
    qt.Build(src, p);

    for (const Leaf& l : qt.leaves()) {
        if (!l.passable) continue;
        float minc = 1e9f, maxc = -1e9f, minh = 1e9f, maxh = -1e9f;
        for (int z = l.z0; z < l.z0 + l.size; ++z)
            for (int x = l.x0; x < l.x0 + l.size; ++x) {
                minc = std::min(minc, src.cost(x, z));
                maxc = std::max(maxc, src.cost(x, z));
                minh = std::min(minh, src.height(x, z));
                maxh = std::max(maxh, src.height(x, z));
            }
        CHECK((maxc - minc) <= p.cost_epsilon + 1e-5f);
        CHECK((maxh - minh) <= p.height_epsilon + 1e-5f);
    }
}

TEST_CASE("every cell is covered by a leaf that contains it", "[nav]") {
    GridSource src(16);
    src.set_blocked(3, 4);
    for (int z = 0; z < 16; ++z)
        for (int x = 8; x < 16; ++x) src.set_cost(x, z, 2.0f);
    Quadtree qt;
    qt.Build(src, NavParams{0.05f, 0.25f, 1.0f});
    for (int z = 0; z < 16; ++z)
        for (int x = 0; x < 16; ++x) {
            const int li = qt.LeafAt(x, z);
            REQUIRE(li >= 0);
            const Leaf& l = qt.leaves()[li];
            CHECK(x >= l.x0);
            CHECK(x < l.x0 + l.size);
            CHECK(z >= l.z0);
            CHECK(z < l.z0 + l.size);
        }
}

TEST_CASE("quadtree build is deterministic", "[nav]") {
    GridSource src(32);
    src.set_blocked(10, 10);
    src.set_blocked(21, 5);
    for (int z = 0; z < 32; ++z)
        for (int x = 16; x < 32; ++x) src.set_cost(x, z, 1.7f);
    const NavParams p{0.05f, 0.25f, 1};

    Quadtree a, b;
    a.Build(src, p);
    b.Build(src, p);
    REQUIRE(a.leaves().size() == b.leaves().size());
    for (size_t i = 0; i < a.leaves().size(); ++i) {
        CHECK(a.leaves()[i].x0 == b.leaves()[i].x0);
        CHECK(a.leaves()[i].z0 == b.leaves()[i].z0);
        CHECK(a.leaves()[i].size == b.leaves()[i].size);
        CHECK(a.leaves()[i].passable == b.leaves()[i].passable);
    }
}

// --- adjacency graph --------------------------------------------------------

namespace {
// Does `node` have an edge to `other`?
bool linked(const NavGraph& g, int node, int other) {
    for (const Edge& e : g.edges(node))
        if (e.to == other) return true;
    return false;
}
}  // namespace

TEST_CASE("graph nodes are the passable leaves, joined across shared borders", "[nav]") {
    // Left half cheap, right half dear: the root splits into four quadrant
    // leaves, all passable -> four nodes in a ring of shared borders.
    GridSource src(16, 1.0f);
    for (int z = 0; z < 16; ++z)
        for (int x = 8; x < 16; ++x) src.set_cost(x, z, 2.5f);
    Quadtree qt;
    qt.Build(src, NavParams{0.05f, 0.25f, 0.0f});
    NavGraph g;
    g.Build(qt);

    REQUIRE(g.node_count() == 4);
    const int nw = g.NodeAt(2, 2, kTriN), ne = g.NodeAt(12, 2, kTriN);
    const int sw = g.NodeAt(2, 12, kTriN), se = g.NodeAt(12, 12, kTriN);
    REQUIRE(nw >= 0);
    REQUIRE(ne >= 0);
    // NW and NE share the x=8 border -> linked, both directions.
    CHECK(linked(g, nw, ne));
    CHECK(linked(g, ne, nw));
    CHECK(linked(g, nw, sw));
    CHECK(linked(g, ne, se));
    // Diagonal quadrants touch only at a corner -> NOT linked (no corner-cut).
    CHECK_FALSE(linked(g, nw, se));
}

TEST_CASE("impassable leaves are excluded from the graph", "[nav]") {
    GridSource src(16);
    src.set_blocked(5, 6);
    Quadtree qt;
    qt.Build(src, NavParams{0.05f, 0.25f, 0.0f});
    NavGraph g;
    g.Build(qt);
    // The obstacle cell has no node; a passable cell does.
    CHECK(g.NodeAt(5, 6, kTriN) == -1);
    CHECK(g.NodeAt(0, 0, kTriN) >= 0);
    // Node count equals the number of passable leaves.
    int passable_leaves = 0;
    for (const Leaf& l : qt.leaves())
        if (l.passable) ++passable_leaves;
    CHECK(g.node_count() == passable_leaves);
}

TEST_CASE("A* routes around a wall through the gap", "[nav]") {
    // A vertical wall on x=8, blocked for z>=2, leaving a gap along the top.
    GridSource src(16);
    for (int z = 2; z < 16; ++z) src.set_blocked(8, z);
    Quadtree qt;
    qt.Build(src, NavParams{0.05f, 0.25f, 0.0f});
    NavGraph g;
    g.Build(qt);

    const int start = g.NodeAt(2, 10, kTriN);
    const int goal = g.NodeAt(14, 10, kTriN);
    REQUIRE(start >= 0);
    REQUIRE(goal >= 0);
    float cost = 0.0f;
    const std::vector<int> path = g.AStar(start, goal, cost);
    REQUIRE(path.size() >= 2);
    CHECK(path.front() == start);
    CHECK(path.back() == goal);
    CHECK(cost > 0.0f);
    // Detour through the gap is longer than the (blocked) straight shot.
    const float straight = glm::distance(g.center_cells(start), g.center_cells(goal));
    CHECK(cost > straight);
}

TEST_CASE("A* returns empty when the goal is unreachable", "[nav]") {
    // A full wall on x=8 severs left from right.
    GridSource src(16);
    for (int z = 0; z < 16; ++z) src.set_blocked(8, z);
    Quadtree qt;
    qt.Build(src, NavParams{0.05f, 0.25f, 0.0f});
    NavGraph g;
    g.Build(qt);

    const int start = g.NodeAt(2, 8, kTriN);
    const int goal = g.NodeAt(14, 8, kTriN);
    REQUIRE(start >= 0);
    REQUIRE(goal >= 0);
    float cost = -1.0f;
    const std::vector<int> path = g.AStar(start, goal, cost);
    CHECK(path.empty());
    CHECK(cost == 0.0f);
}

TEST_CASE("A* is deterministic across runs", "[nav]") {
    GridSource src(32);
    for (int z = 4; z < 32; ++z) src.set_blocked(16, z);
    for (int z = 0; z < 28; ++z) src.set_blocked(8, z);
    Quadtree qt;
    qt.Build(src, NavParams{0.05f, 0.25f, 0.0f});
    NavGraph g;
    g.Build(qt);
    const int start = g.NodeAt(2, 30, kTriN), goal = g.NodeAt(30, 2, kTriN);
    REQUIRE(start >= 0);
    REQUIRE(goal >= 0);
    float c1 = 0.0f, c2 = 0.0f;
    const std::vector<int> p1 = g.AStar(start, goal, c1);
    const std::vector<int> p2 = g.AStar(start, goal, c2);
    REQUIRE(!p1.empty());
    CHECK(p1 == p2);
    CHECK(c1 == c2);
}

// --- NavMesh facade (world coordinates) -------------------------------------

namespace {
// Length of a waypoint polyline.
float poly_len(const std::vector<glm::vec2>& w) {
    float s = 0.0f;
    for (size_t i = 1; i < w.size(); ++i) s += glm::distance(w[i - 1], w[i]);
    return s;
}
}  // namespace

TEST_CASE("FindPath in the open is a straight two-point shot", "[nav]") {
    GridSource src(16);  // all passable, cell_size 1, origin (0,0)
    NavMesh nm;
    nm.Build(src, NavParams{0.05f, 0.25f, 0.0f});

    const glm::vec2 from{2.5f, 2.5f}, to{13.5f, 13.5f};
    const NavMesh::PathResult r = nm.FindPath(from, to);
    REQUIRE(r.reachable);
    REQUIRE(r.waypoints.size() == 2);
    CHECK(glm::distance(r.waypoints.front(), from) < 1e-3f);
    CHECK(glm::distance(r.waypoints.back(), to) < 1e-3f);
    CHECK(r.cost > 0.0f);
}

TEST_CASE("FindPath detours around a wall and every segment is clear", "[nav]") {
    // Wall on x in [7,8], blocked for z>=2 (gap along the top).
    GridSource src(16);
    for (int z = 2; z < 16; ++z) {
        src.set_blocked(7, z);
        src.set_blocked(8, z);
    }
    NavMesh nm;
    nm.Build(src, NavParams{0.05f, 0.25f, 0.0f});

    const glm::vec2 from{2.5f, 12.5f}, to{13.5f, 12.5f};
    const NavMesh::PathResult r = nm.FindPath(from, to);
    REQUIRE(r.reachable);
    REQUIRE(r.waypoints.size() >= 3);  // needed a detour
    // The detour is longer than the (blocked) straight line.
    CHECK(poly_len(r.waypoints) > glm::distance(from, to));

    // No waypoint sits on a blocked triangle.
    for (const glm::vec2& w : r.waypoints) {
        const TriId t = nm.WorldToTri(w);
        CHECK_FALSE(mask_has(src.blocked_mask(t.cx, t.cz), t.corner));
    }
}

TEST_CASE("FindPath reports unreachable across a full wall", "[nav]") {
    GridSource src(16);
    for (int z = 0; z < 16; ++z) src.set_blocked(8, z);
    NavMesh nm;
    nm.Build(src, NavParams{0.05f, 0.25f, 0.0f});
    const NavMesh::PathResult r = nm.FindPath({2.5f, 8.5f}, {13.5f, 8.5f});
    CHECK_FALSE(r.reachable);
    CHECK(r.waypoints.empty());
}

TEST_CASE("Cost rises when an obstacle forces a detour", "[nav]") {
    const glm::vec2 from{2.5f, 8.5f}, to{13.5f, 8.5f};
    GridSource open(16);
    NavMesh a;
    a.Build(open, NavParams{0.05f, 0.25f, 0.0f});
    const float open_cost = a.Cost(from, to);

    GridSource walled(16);
    for (int z = 2; z < 16; ++z) walled.set_blocked(8, z);
    NavMesh b;
    b.Build(walled, NavParams{0.05f, 0.25f, 0.0f});
    const float walled_cost = b.Cost(from, to);

    CHECK(open_cost > 0.0f);
    CHECK(walled_cost > open_cost);
}

TEST_CASE("DebugCells cover the map and mark obstacles impassable", "[nav]") {
    GridSource src(16);
    src.set_blocked(4, 4);
    NavMesh nm;
    nm.Build(src, NavParams{0.05f, 0.25f, 0.0f});
    std::vector<NavMesh::DebugCell> cells;
    nm.DebugCells(cells);
    REQUIRE(!cells.empty());
    // Total area of all leaves equals the map area (16x16 = 256 m^2).
    float area = 0.0f;
    for (const auto& c : cells) {
        const glm::vec2 d = c.max_world - c.min_world;
        area += d.x * d.y;
    }
    CHECK(area == Catch::Approx(256.0f));
    // At least one impassable cell exists (the obstacle).
    bool any_blocked = false;
    for (const auto& c : cells) any_blocked |= !c.passable;
    CHECK(any_blocked);
}

TEST_CASE("Cost matches FindPath and is stable across repeated (cached) calls", "[nav]") {
    GridSource src(16);
    for (int z = 2; z < 16; ++z) src.set_blocked(8, z);
    NavMesh nm;
    nm.Build(src, NavParams{0.05f, 0.25f, 0.0f});
    const glm::vec2 from{2.5f, 12.5f}, to{13.5f, 12.5f};

    const float c1 = nm.Cost(from, to);
    const float c2 = nm.Cost(from, to);  // served from cache
    CHECK(c1 == c2);
    CHECK(c1 == Catch::Approx(nm.FindPath(from, to).cost));
}

TEST_CASE("Cost cache is invalidated when the SAME mesh is rebuilt", "[nav]") {
    const glm::vec2 from{2.5f, 8.5f}, to{13.5f, 8.5f};
    NavMesh nm;

    GridSource open(16);
    nm.Build(open, NavParams{0.05f, 0.25f, 0.0f});
    const float open_cost = nm.Cost(from, to);  // populates the cache

    GridSource walled(16);
    for (int z = 2; z < 16; ++z) walled.set_blocked(8, z);
    nm.Build(walled, NavParams{0.05f, 0.25f, 0.0f});  // must drop the stale entry
    const float walled_cost = nm.Cost(from, to);

    CHECK(walled_cost > open_cost);  // not the stale cached value
}

TEST_CASE("exempt-building opens a doorway its own clearance sealed", "[nav]") {
    // A wall on cell x=16 with a 2-cell doorway at z=15,16. With clearance 1 the
    // wall's own dilation seals that doorway, splitting the map in two.
    GridSource src(32);
    for (int z = 0; z < 32; ++z)
        if (z != 15 && z != 16) src.set_blocked(16, z);
    NavMesh nm;
    nm.Build(src, NavParams{0.05f, 0.25f, /*clearance=*/1});

    const glm::vec2 from{5.5f, 15.5f}, to{25.5f, 15.5f};
    // Without the exemption the sealed doorway makes the goal unreachable.
    CHECK_FALSE(nm.FindPath(from, to).reachable);

    // Exempting the wall's footprint (world x in [16,17]) lifts its clearance at
    // the doorway, so the goal becomes reachable.
    const NavMesh::PathResult r = nm.FindPath(from, to, glm::vec2{16.0f, 0.0f}, glm::vec2{17.0f, 32.0f});
    CHECK(r.reachable);
    CHECK(r.waypoints.size() >= 2);
    CHECK(glm::distance(r.waypoints.back(), to) < 1e-3f);
}

// --- the triangle obstacle layer --------------------------------------------
// What a square grid cannot express: a footprint that covers HALF a cell. The
// (u,v) lattice a rotation-1 building snaps to is the tile diagonals, so its
// boundary runs through cells rather than between them.

namespace {

// A 45-degree wall: every triangle whose centroid falls in the (x+z) band
// [lo, hi). Rasterizes the way a diagonal footprint does, and leaves the
// half-covered boundary cells that are the whole point.
void diagonal_wall(GridSource& src, float lo, float hi) {
    for (int z = 0; z < src.n; ++z) {
        for (int x = 0; x < src.n; ++x) {
            for (int c = 0; c < kTriPerCell; ++c) {
                const glm::vec2 p = tri_centroid_cells(x, z, c);
                if (p.x + p.y >= lo && p.x + p.y < hi) {
                    src.set_tri(x, z, c);
                }
            }
        }
    }
}

}  // namespace

TEST_CASE("a half-covered cell keeps its free corners walkable", "[nav]") {
    GridSource src(16);
    src.set_tri(8, 8, kTriN);
    src.set_tri(8, 8, kTriW);
    Quadtree qt;
    qt.Build(src, NavParams{0.05f, 0.25f, /*clearance_m=*/0.0f});

    const Leaf& l = qt.leaves()[qt.LeafAt(8, 8)];
    CHECK(l.size == 1);  // a partial cell can never merge
    CHECK(l.passable);   // there is somewhere to stand in it
    CHECK(l.tri_mask == static_cast<uint8_t>(tri_bit(kTriN) | tri_bit(kTriW)));
    CHECK_FALSE(qt.TriPassable(8, 8, kTriN));
    CHECK_FALSE(qt.TriPassable(8, 8, kTriW));
    CHECK(qt.TriPassable(8, 8, kTriE));
    CHECK(qt.TriPassable(8, 8, kTriS));
}

TEST_CASE("clearance reaches across a tile border without filling the cell", "[nav]") {
    // The band that makes the triangle mask worth having (NavParams::
    // clearance_m). Above sqrt(2)/6 ~ 0.236 a free corner falls inside its OWN
    // cell's solid corner radius, every partial cell fills in, and the mask
    // stops meaning anything -- so this pins both ends.
    GridSource src(16);
    src.set_tri(8, 8, kTriN);
    src.set_tri(8, 8, kTriW);

    Quadtree tight;
    tight.Build(src, NavParams{0.05f, 0.25f, /*clearance_m=*/0.2f});
    CHECK(tight.MaskAt(8, 8) == static_cast<uint8_t>(tri_bit(kTriN) | tri_bit(kTriW)));
    CHECK(tight.leaves()[tight.LeafAt(8, 8)].passable);
    // ...and the standoff did land, on the far side of each border.
    CHECK_FALSE(tight.TriPassable(8, 7, kTriS));  // across kTriN's outer edge
    CHECK_FALSE(tight.TriPassable(7, 8, kTriE));  // across kTriW's outer edge

    Quadtree fat;
    fat.Build(src, NavParams{0.05f, 0.25f, /*clearance_m=*/0.25f});
    CHECK(fat.MaskAt(8, 8) == kMaskSolid);  // the regression this guards against
}

TEST_CASE("a diagonal wall blocks without fattening into whole cells", "[nav]") {
    GridSource src(32);
    diagonal_wall(src, 24.0f, 25.0f);
    Quadtree qt;
    qt.Build(src, NavParams{0.05f, 0.25f, /*clearance_m=*/0.0f});

    // Every cell the wall touches is HALF covered, and stays half covered. The
    // OR this replaced turned each of them fully solid -- a metre of wall that
    // is not in the footprint, on both faces, the length of the run.
    int partial = 0;
    for (int z = 0; z < 32; ++z) {
        for (int x = 0; x < 32; ++x) {
            const uint8_t m = qt.MaskAt(x, z);
            if (m == kMaskFree) {
                continue;
            }
            INFO("cell " << x << "," << z << " mask " << int(m));
            CHECK(m != kMaskSolid);
            CHECK(qt.leaves()[qt.LeafAt(x, z)].passable);
            ++partial;
        }
    }
    CHECK(partial > 20);  // the run really did cross the map
}

TEST_CASE("a diagonal wall is still a wall", "[nav]") {
    GridSource src(32);
    diagonal_wall(src, 24.0f, 25.0f);
    NavMesh nm;
    nm.Build(src, NavParams{0.05f, 0.25f, /*clearance_m=*/0.2f});

    // Corners on opposite sides of the run. Half-covered cells must not add up
    // to a leak: being permissive per triangle is not the same as being porous.
    const NavMesh::PathResult blocked = nm.FindPath({2.5f, 2.5f}, {29.5f, 29.5f});
    CHECK_FALSE(blocked.reachable);

    // Punch a hole and it routes through, hugging the diagonal rather than
    // standing a metre off it. The hole has to clear the STANDOFF as well as
    // the wall -- 0.2 m from each face is real, so a one-cell notch stays shut.
    GridSource holed(32);
    diagonal_wall(holed, 24.0f, 25.0f);
    for (int z = 10; z <= 14; ++z) {
        for (int x = 10; x <= 14; ++x) {
            holed.b[static_cast<size_t>(z) * 32 + x] = kMaskFree;
        }
    }
    NavMesh open;
    open.Build(holed, NavParams{0.05f, 0.25f, /*clearance_m=*/0.2f});
    const NavMesh::PathResult r = open.FindPath({2.5f, 2.5f}, {29.5f, 29.5f});
    REQUIRE(r.reachable);
    // Every waypoint stands on a free triangle, and so does every step along
    // the way -- the string-pull may only shortcut through open ground.
    for (const glm::vec2& w : r.waypoints) {
        const TriId t = open.WorldToTri(w);
        INFO("waypoint " << w.x << "," << w.y);
        CHECK(open.PassableAt(w));
        CHECK_FALSE(mask_has(holed.blocked_mask(t.cx, t.cz), t.corner));
    }
    for (size_t i = 1; i < r.waypoints.size(); ++i) {
        for (int s = 0; s <= 40; ++s) {
            const glm::vec2 p =
                glm::mix(r.waypoints[i - 1], r.waypoints[i], static_cast<float>(s) / 40.0f);
            const TriId t = open.WorldToTri(p);
            INFO("step " << p.x << "," << p.y);
            CHECK_FALSE(mask_has(holed.blocked_mask(t.cx, t.cz), t.corner));
        }
    }
}

TEST_CASE("a partial-cell build is deterministic", "[nav]") {
    GridSource src(32);
    diagonal_wall(src, 24.0f, 25.0f);
    src.set_blocked(4, 4);
    const NavParams p{0.05f, 0.25f, 0.2f};

    Quadtree qa, qb;
    qa.Build(src, p);
    qb.Build(src, p);
    REQUIRE(qa.leaves().size() == qb.leaves().size());
    for (size_t i = 0; i < qa.leaves().size(); ++i) {
        CHECK(qa.leaves()[i].x0 == qb.leaves()[i].x0);
        CHECK(qa.leaves()[i].z0 == qb.leaves()[i].z0);
        CHECK(qa.leaves()[i].size == qb.leaves()[i].size);
        CHECK(qa.leaves()[i].tri_mask == qb.leaves()[i].tri_mask);
    }
    NavGraph ga, gb;
    ga.Build(qa);
    gb.Build(qb);
    REQUIRE(ga.node_count() == gb.node_count());
    for (int nd = 0; nd < ga.node_count(); ++nd) {
        CHECK(ga.tri_of(nd) == gb.tri_of(nd));
        REQUIRE(ga.edges(nd).size() == gb.edges(nd).size());
        for (size_t e = 0; e < ga.edges(nd).size(); ++e) {
            CHECK(ga.edges(nd)[e].to == gb.edges(nd)[e].to);
        }
    }
}

TEST_CASE("triangle nodes join only across shared edges", "[nav]") {
    GridSource src(16);
    src.set_tri(8, 8, kTriN);  // one corner solid: three free nodes in the cell
    Quadtree qt;
    qt.Build(src, NavParams{0.05f, 0.25f, /*clearance_m=*/0.0f});
    NavGraph g;
    g.Build(qt);

    const int e = g.NodeAt(8, 8, kTriE);
    const int s = g.NodeAt(8, 8, kTriS);
    const int w = g.NodeAt(8, 8, kTriW);
    REQUIRE(e >= 0);
    REQUIRE(s >= 0);
    REQUIRE(w >= 0);
    CHECK(g.NodeAt(8, 8, kTriN) == -1);  // solid
    // E-S and S-W share a half-diagonal; E-W meet only at the cell centre, so
    // joining them would be the corner-cut the 4-connected rule forbids.
    CHECK(linked(g, e, s));
    CHECK(linked(g, s, w));
    CHECK_FALSE(linked(g, e, w));
}

TEST_CASE("a point on a cell boundary is walkable if any triangle holding it is", "[nav]") {
    // Boundary points are not a curiosity here: the string-pull emits leaf
    // centres as waypoints, and an even-sized leaf's centre has INTEGER
    // coordinates -- i.e. lands exactly on a cell corner, which four cells
    // share. Answering only for floor()'s cell made PassableAt call ground
    // impassable that the pathfinder had just routed across, and
    // nav_point_free would refuse a skill cast there.
    GridSource src(16);
    src.set_blocked(8, 8);
    NavMesh nm;
    nm.Build(src, NavParams{0.05f, 0.25f, /*clearance_m=*/0.0f});

    CHECK(nm.PassableAt({8.0f, 8.0f}));        // the shared corner: 3 of 4 cells free
    CHECK_FALSE(nm.PassableAt({8.5f, 8.5f}));  // strictly inside: still a wall

    // Same on a DIAGONAL boundary, where two triangles of ONE cell share the point.
    GridSource d(16);
    d.set_tri(2, 2, kTriN);
    NavMesh dm;
    dm.Build(d, NavParams{0.05f, 0.25f, /*clearance_m=*/0.0f});
    CHECK(dm.PassableAt({2.25f, 2.25f}));      // on z=x, between solid N and free W
    CHECK_FALSE(dm.PassableAt({2.5f, 2.1f}));  // strictly inside N
}

TEST_CASE("recovery picks the free corner nearest the point", "[nav]") {
    // Opposite corners of a cell are not graph-joined (they meet at a point),
    // so on a cell split by a wall the two free halves can be far apart in the
    // graph. Recovering a displaced body to whichever corner is scanned first
    // would put it on the wrong side and plan the long way round.
    GridSource src(16);
    src.set_tri(8, 8, kTriE);
    src.set_tri(8, 8, kTriW);
    NavMesh nm;
    nm.Build(src, NavParams{0.05f, 0.25f, /*clearance_m=*/0.0f});

    const glm::vec2 stuck{8.8f, 8.6f};  // inside the solid +X corner, biased +Z
    REQUIRE_FALSE(nm.PassableAt(stuck));
    const NavMesh::PathResult south = nm.FindPath(stuck, {8.5f, 12.0f});
    REQUIRE(south.reachable);
    // Recovered to the +Z half, so the route heads straight out; it never
    // doubles back through the -Z half above the wall.
    for (const glm::vec2& w : south.waypoints) {
        INFO("waypoint " << w.x << "," << w.y);
        CHECK(w.y > 8.2f);
    }
}
