#include <algorithm>

#include <catch_amalgamated.hpp>

#include "mapgen/river_clip.hpp"
#include "mapgen/river_prune.hpp"

using namespace badlands::mapgen;

namespace {

// Builds a two-node, one-edge graph by hand: `from` at `a`, `to` at `b`, with
// discharge/width/depth/speed linearly interpolated between the `*a`/`*b`
// values at the two sample points. No routing, no rasters -- just the graph
// algebra clip_river_graph_to_rect operates on.
RiverGraph make_two_point_graph(glm::vec2 a, glm::vec2 b, float qa, float qb,
                                float wa, float wb, float da, float db,
                                float sa, float sb) {
  RiverGraph g;
  RiverNode from_n, to_n;
  from_n.pos_m = a;
  from_n.kind = RiverNodeKind::Source;
  to_n.pos_m = b;
  to_n.kind = RiverNodeKind::Confluence;
  g.nodes = {from_n, to_n};

  RiverEdge e;
  e.from = 0;
  e.to = 1;
  e.points_m = {a, b};
  e.discharge_m3_s = {qa, qb};
  e.width_m = {wa, wb};
  e.depth_m = {da, db};
  e.speed_m_s = {sa, sb};
  g.edges = {e};
  return g;
}

}  // namespace

TEST_CASE("clip_river_graph_to_rect: an entering crossing mints FrameEntry, not Source",
         "[river]") {
  // Reach runs from (-10,5) OUTSIDE the rect to (10,5) INSIDE it, crossing the
  // x=0 boundary at t=0.5.
  RiverGraph g = make_two_point_graph(glm::vec2(-10.0f, 5.0f), glm::vec2(10.0f, 5.0f),
                                      1.0f, 3.0f,   // discharge
                                      2.0f, 6.0f,   // width
                                      0.5f, 1.5f,   // depth
                                      0.2f, 0.6f);  // speed
  clip_river_graph_to_rect(g, glm::vec2(0.0f, 0.0f), glm::vec2(100.0f, 100.0f));

  REQUIRE(g.edges.size() == 1);
  const RiverEdge& e = g.edges[0];
  REQUIRE(e.points_m.size() == 2);
  REQUIRE(e.from >= 0);
  const RiverNode& minted = g.nodes[e.from];
  // THE POINT OF THIS TEST: before FrameEntry existed, this node was minted
  // Source, and both prune passes re-derive Source from in_deg == 0 -- which a
  // trunk that merely enters the frame also has. That mislabelling is what let
  // the length prune eat a 700 m trunk one fragment at a time (see
  // river_prune.hpp and river_prune_tests.cpp for the regression pin).
  REQUIRE(minted.kind == RiverNodeKind::FrameEntry);
  REQUIRE(minted.kind != RiverNodeKind::Source);
}

TEST_CASE(
   "clip_river_graph_to_rect: the minted node's properties are interpolated "
   "between the straddling samples",
   "[river]") {
  RiverGraph g = make_two_point_graph(glm::vec2(-10.0f, 5.0f), glm::vec2(10.0f, 5.0f),
                                      1.0f, 3.0f, 2.0f, 6.0f, 0.5f, 1.5f, 0.2f,
                                      0.6f);
  clip_river_graph_to_rect(g, glm::vec2(0.0f, 0.0f), glm::vec2(100.0f, 100.0f));

  REQUIRE(g.edges.size() == 1);
  const RiverNode& minted = g.nodes[g.edges[0].from];
  REQUIRE(minted.discharge_m3_s > 1.0f);
  REQUIRE(minted.discharge_m3_s < 3.0f);
  REQUIRE(minted.width_m > 2.0f);
  REQUIRE(minted.width_m < 6.0f);
  REQUIRE(minted.depth_m > 0.5f);
  REQUIRE(minted.depth_m < 1.5f);
  REQUIRE(minted.speed_m_s > 0.2f);
  REQUIRE(minted.speed_m_s < 0.6f);
  // The crossing (x=0) sits exactly midway between x=-10 and x=10, so the
  // interpolated values should land exactly at the midpoint.
  REQUIRE(minted.discharge_m3_s == Catch::Approx(2.0f));
  REQUIRE(minted.width_m == Catch::Approx(4.0f));
  REQUIRE(minted.depth_m == Catch::Approx(1.0f));
  REQUIRE(minted.speed_m_s == Catch::Approx(0.4f));
}

TEST_CASE("clip_river_graph_to_rect: a reach entirely inside the rect is untouched",
         "[river]") {
  RiverGraph g = make_two_point_graph(glm::vec2(10.0f, 10.0f), glm::vec2(20.0f, 20.0f),
                                      1.0f, 2.0f, 3.0f, 4.0f, 0.1f, 0.2f, 0.3f,
                                      0.4f);
  const RiverGraph before = g;
  clip_river_graph_to_rect(g, glm::vec2(0.0f, 0.0f), glm::vec2(100.0f, 100.0f));

  REQUIRE(g.edges.size() == 1);
  REQUIRE(g.edges[0].points_m.size() == before.edges[0].points_m.size());
  REQUIRE(g.edges[0].points_m[0] == before.edges[0].points_m[0]);
  REQUIRE(g.edges[0].points_m[1] == before.edges[0].points_m[1]);
  REQUIRE(g.edges[0].from == before.edges[0].from);
  REQUIRE(g.edges[0].to == before.edges[0].to);
  REQUIRE(g.edges[0].discharge_m3_s == before.edges[0].discharge_m3_s);
  REQUIRE(g.nodes.size() == before.nodes.size());  // no crossing node minted
}

TEST_CASE("clip_river_graph_to_rect: a reach entirely outside the rect is dropped",
         "[river]") {
  RiverGraph g = make_two_point_graph(glm::vec2(-60.0f, -60.0f), glm::vec2(-50.0f, -50.0f),
                                      1.0f, 2.0f, 3.0f, 4.0f, 0.1f, 0.2f, 0.3f,
                                      0.4f);
  clip_river_graph_to_rect(g, glm::vec2(0.0f, 0.0f), glm::vec2(100.0f, 100.0f));

  REQUIRE(g.edges.empty());
}

TEST_CASE(
   "clip_river_graph_to_rect: a non-zero lo_m clips against that rect, not "
   "the origin",
   "[river]") {
  // This reach sits inside [0,100]^2 but entirely below lo_m=(50,50). An
  // implementation that silently assumed the lower bound was the origin (the
  // old clip_river_graph_to_window behaviour, which hardcoded [0,size]^2)
  // would keep it; the generalized rect must drop it.
  RiverGraph g = make_two_point_graph(glm::vec2(10.0f, 10.0f), glm::vec2(20.0f, 20.0f),
                                      1.0f, 2.0f, 3.0f, 4.0f, 0.1f, 0.2f, 0.3f,
                                      0.4f);
  clip_river_graph_to_rect(g, glm::vec2(50.0f, 50.0f), glm::vec2(150.0f, 150.0f));

  REQUIRE(g.edges.empty());
}

TEST_CASE(
    "clip_river_graph_to_rect: a sample landing EXACTLY on the frame still "
    "mints FrameEntry, and the trunk survives the length cull",
    "[river]") {
  // THE DEGENERATE ENTRY. boundary_crossing_t rejects t >= 1, so an entry whose
  // first inside sample sits exactly ON the boundary produces no crossing
  // parameter and takes the fallback path rather than the interpolated one. A
  // lattice-aligned channel -- which is what a resampled D8 chain and every
  // analytic fixture produce -- does this routinely, so it is the MOST likely
  // way to hit the trunk-eating bug, not the least.
  //
  // Minting Source there and then pruning by length deletes the reach: its
  // in-frame stretch is 8 m against a 32 m minimum, and in_deg == 0 makes it
  // look like a stubby headwater.
  RiverGraph g;
  RiverNode a, b;
  a.pos_m = glm::vec2(-10.0f, 50.0f);
  a.kind = RiverNodeKind::Source;  // the TRUE headwater, outside the rect
  b.pos_m = glm::vec2(8.0f, 50.0f);
  b.kind = RiverNodeKind::Confluence;
  g.nodes = {a, b};

  RiverEdge e;
  e.from = 0;
  e.to = 1;
  e.points_m = {glm::vec2(-10.0f, 50.0f), glm::vec2(0.0f, 50.0f),
                glm::vec2(8.0f, 50.0f)};
  e.discharge_m3_s = {5.0f, 5.0f, 5.0f};  // a trunk, not a hairline
  e.width_m = {4.0f, 4.0f, 4.0f};
  e.depth_m = {1.0f, 1.0f, 1.0f};
  e.speed_m_s = {1.0f, 1.0f, 1.0f};
  g.edges = {e};

  clip_river_graph_to_rect(g, glm::vec2(0.0f, 0.0f), glm::vec2(100.0f, 100.0f));

  REQUIRE(g.edges.size() == 1);
  REQUIRE(g.edges[0].from >= 0);
  REQUIRE(g.nodes[g.edges[0].from].kind == RiverNodeKind::FrameEntry);

  float peak_before = 0.0f;
  for (float q : g.edges[0].discharge_m3_s) peak_before = std::max(peak_before, q);

  prune_river_graph_by_length(g, 32.0f);  // 4x the in-frame stretch

  REQUIRE(g.edges.size() == 1);
  float peak_after = 0.0f;
  for (float q : g.edges[0].discharge_m3_s) peak_after = std::max(peak_after, q);
  REQUIRE(peak_after == peak_before);
}
