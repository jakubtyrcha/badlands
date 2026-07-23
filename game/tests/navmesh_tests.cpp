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
    std::vector<char> b;

    explicit GridSource(int side, float cost = 1.0f, float height = 0.0f)
        : n(side),
          c(static_cast<size_t>(side) * side, cost),
          h(static_cast<size_t>(side) * side, height),
          b(static_cast<size_t>(side) * side, 0) {}

    int side() const override { return n; }
    float cell_size_m() const override { return cell; }
    glm::vec2 origin_m() const override { return org; }
    float cost(int x, int z) const override { return c[idx(x, z)]; }
    float height(int x, int z) const override { return h[idx(x, z)]; }
    bool blocked(int x, int z) const override { return b[idx(x, z)] != 0; }

    void set_blocked(int x, int z) { b[idx(x, z)] = 1; }
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
                            /*clearance_cells=*/0});

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
    qt.Build(src, NavParams{0.05f, 0.25f, /*clearance_cells=*/1});
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
    qt.Build(src, NavParams{0.05f, 0.25f, 0});
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
    qt.Build(src, NavParams{0.05f, 0.25f, 1});
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
    qt.Build(src, NavParams{0.05f, 0.25f, 0});
    NavGraph g;
    g.Build(qt);

    REQUIRE(g.node_count() == 4);
    const int nw = g.NodeAt(2, 2), ne = g.NodeAt(12, 2);
    const int sw = g.NodeAt(2, 12), se = g.NodeAt(12, 12);
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
    qt.Build(src, NavParams{0.05f, 0.25f, 0});
    NavGraph g;
    g.Build(qt);
    // The obstacle cell has no node; a passable cell does.
    CHECK(g.NodeAt(5, 6) == -1);
    CHECK(g.NodeAt(0, 0) >= 0);
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
    qt.Build(src, NavParams{0.05f, 0.25f, 0});
    NavGraph g;
    g.Build(qt);

    const int start = g.NodeAt(2, 10);
    const int goal = g.NodeAt(14, 10);
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
    qt.Build(src, NavParams{0.05f, 0.25f, 0});
    NavGraph g;
    g.Build(qt);

    const int start = g.NodeAt(2, 8);
    const int goal = g.NodeAt(14, 8);
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
    qt.Build(src, NavParams{0.05f, 0.25f, 0});
    NavGraph g;
    g.Build(qt);
    const int start = g.NodeAt(2, 30), goal = g.NodeAt(30, 2);
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
    nm.Build(src, NavParams{0.05f, 0.25f, 0});

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
    nm.Build(src, NavParams{0.05f, 0.25f, 0});

    const glm::vec2 from{2.5f, 12.5f}, to{13.5f, 12.5f};
    const NavMesh::PathResult r = nm.FindPath(from, to);
    REQUIRE(r.reachable);
    REQUIRE(r.waypoints.size() >= 3);  // needed a detour
    // The detour is longer than the (blocked) straight line.
    CHECK(poly_len(r.waypoints) > glm::distance(from, to));

    // No waypoint sits on a blocked cell.
    for (const glm::vec2& w : r.waypoints) {
        const glm::ivec2 c = nm.WorldToCell(w);
        CHECK_FALSE(src.blocked(c.x, c.y));
    }
}

TEST_CASE("FindPath reports unreachable across a full wall", "[nav]") {
    GridSource src(16);
    for (int z = 0; z < 16; ++z) src.set_blocked(8, z);
    NavMesh nm;
    nm.Build(src, NavParams{0.05f, 0.25f, 0});
    const NavMesh::PathResult r = nm.FindPath({2.5f, 8.5f}, {13.5f, 8.5f});
    CHECK_FALSE(r.reachable);
    CHECK(r.waypoints.empty());
}

TEST_CASE("Cost rises when an obstacle forces a detour", "[nav]") {
    const glm::vec2 from{2.5f, 8.5f}, to{13.5f, 8.5f};
    GridSource open(16);
    NavMesh a;
    a.Build(open, NavParams{0.05f, 0.25f, 0});
    const float open_cost = a.Cost(from, to);

    GridSource walled(16);
    for (int z = 2; z < 16; ++z) walled.set_blocked(8, z);
    NavMesh b;
    b.Build(walled, NavParams{0.05f, 0.25f, 0});
    const float walled_cost = b.Cost(from, to);

    CHECK(open_cost > 0.0f);
    CHECK(walled_cost > open_cost);
}

TEST_CASE("DebugCells cover the map and mark obstacles impassable", "[nav]") {
    GridSource src(16);
    src.set_blocked(4, 4);
    NavMesh nm;
    nm.Build(src, NavParams{0.05f, 0.25f, 0});
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
    nm.Build(src, NavParams{0.05f, 0.25f, 0});
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
    nm.Build(open, NavParams{0.05f, 0.25f, 0});
    const float open_cost = nm.Cost(from, to);  // populates the cache

    GridSource walled(16);
    for (int z = 2; z < 16; ++z) walled.set_blocked(8, z);
    nm.Build(walled, NavParams{0.05f, 0.25f, 0});  // must drop the stale entry
    const float walled_cost = nm.Cost(from, to);

    CHECK(walled_cost > open_cost);  // not the stale cached value
}
