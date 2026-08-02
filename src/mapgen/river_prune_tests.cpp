#include <catch_amalgamated.hpp>

#include <algorithm>

#include "mapgen/river_clip.hpp"
#include "mapgen/river_prune.hpp"

using namespace badlands::mapgen;

// --- prune_river_graph_by_length --------------------------------------------

TEST_CASE(
   "prune_river_graph_by_length: a trunk entering via FrameEntry survives a "
   "short in-frame stretch with peak discharge unchanged",
   "[river]") {
  // THE REGRESSION PIN. Deliberately goes through clip_river_graph_to_rect
  // rather than hand-setting kind = FrameEntry, so this test is sensitive to
  // what river_clip.cpp actually mints, not just to what this file asserts.
  //
  // The reach is hand-built (no routing, no rasters) to run from OUTSIDE the
  // rect at (-10,5) to a real in-window node at (5,5) -- a trunk that clips
  // through the edge of the window and immediately reaches a confluence, an
  // in-frame stretch of only 5 m. min_length_m is 100 m, so a genuine headwater
  // this short would be deleted.
  //
  // Fails by construction without FrameEntry: RiverNodeKind::FrameEntry has to
  // exist for the enum reference below to compile. And with the OLD mint
  // (Source instead of FrameEntry, see river_clip.cpp), in_deg == 0 alone made
  // prune_river_graph_by_length treat the entry node as a genuine headwater and
  // delete this whole chain, since 5 m < 100 m and it does not end in water --
  // exactly the bug this test pins. (Verified directly: reverting the mint in
  // river_clip.cpp back to Source makes this test fail.)
  RiverGraph g;
  RiverNode confluence;
  confluence.pos_m = glm::vec2(5.0f, 5.0f);
  confluence.kind = RiverNodeKind::Confluence;
  g.nodes = {confluence};

  RiverEdge trunk;
  trunk.from = -1;  // further upstream, outside the rect -- never dereferenced
  trunk.to = 0;
  trunk.points_m = {glm::vec2(-10.0f, 5.0f), glm::vec2(5.0f, 5.0f)};
  trunk.discharge_m3_s = {5.0f, 5.2f};  // high-discharge trunk, peak 5.2
  trunk.width_m = {8.0f, 8.2f};
  trunk.depth_m = {1.0f, 1.1f};
  trunk.speed_m_s = {0.9f, 0.95f};
  g.edges = {trunk};

  clip_river_graph_to_rect(g, glm::vec2(0.0f, 0.0f), glm::vec2(100.0f, 100.0f));
  // Sanity check on the fixture itself: clipping must actually have produced a
  // FrameEntry-rooted, sub-threshold reach, or this test would pass vacuously.
  REQUIRE(g.edges.size() == 1);
  REQUIRE(g.edges[0].from >= 0);
  REQUIRE(g.nodes[g.edges[0].from].kind == RiverNodeKind::FrameEntry);
  const float clipped_len_m =
      glm::length(g.edges[0].points_m[1] - g.edges[0].points_m[0]);
  REQUIRE(clipped_len_m < 100.0f);

  prune_river_graph_by_length(g, 100.0f);

  REQUIRE(g.edges.size() == 1);
  REQUIRE(g.edges[0].discharge_m3_s.size() == 2);
  const float peak_q = *std::max_element(g.edges[0].discharge_m3_s.begin(),
                                         g.edges[0].discharge_m3_s.end());
  REQUIRE(peak_q == Catch::Approx(5.2f));
}

TEST_CASE(
   "prune_river_graph_by_length: a genuine stubby headwater branch is removed",
   "[river]") {
  // Same short length (5 m << 100 m min), same "doesn't end in water" shape as
  // the regression pin above -- but rooted at a real Source, not a FrameEntry.
  // This is the case the pass is SUPPOSED to delete.
  RiverGraph g;
  RiverNode headwater, tip;
  headwater.pos_m = glm::vec2(0.0f, 0.0f);
  headwater.kind = RiverNodeKind::Source;
  tip.pos_m = glm::vec2(5.0f, 0.0f);
  tip.kind = RiverNodeKind::Mouth;
  g.nodes = {headwater, tip};

  RiverEdge stub;
  stub.from = 0;
  stub.to = 1;
  stub.points_m = {glm::vec2(0.0f, 0.0f), glm::vec2(5.0f, 0.0f)};
  stub.discharge_m3_s = {0.001f, 0.001f};
  stub.width_m = {0.1f, 0.1f};
  stub.depth_m = {0.02f, 0.02f};
  stub.speed_m_s = {0.05f, 0.05f};
  g.edges = {stub};

  prune_river_graph_by_length(g, 100.0f);

  REQUIRE(g.edges.empty());
}

// --- prune_river_graph_by_width ---------------------------------------------

TEST_CASE(
   "prune_river_graph_by_width: drops an end-to-end hairline and trims a "
   "reach back to where it first qualifies",
   "[river]") {
  constexpr float kMinWidth = 2.0f;

  RiverGraph g;
  RiverNode a_from, a_to, b_from, b_to;
  a_from.kind = RiverNodeKind::Source;
  a_to.kind = RiverNodeKind::Mouth;
  b_from.kind = RiverNodeKind::Source;
  b_to.kind = RiverNodeKind::Mouth;
  g.nodes = {a_from, a_to, b_from, b_to};

  // Edge A: a hairline that never reaches kMinWidth anywhere along its length
  // -- dropped entirely, not just trimmed.
  RiverEdge hairline;
  hairline.from = 0;
  hairline.to = 1;
  hairline.points_m = {glm::vec2(0.0f, 0.0f), glm::vec2(10.0f, 0.0f),
                       glm::vec2(20.0f, 0.0f), glm::vec2(30.0f, 0.0f)};
  hairline.discharge_m3_s = {0.1f, 0.1f, 0.1f, 0.1f};
  hairline.width_m = {0.5f, 0.6f, 0.7f, 0.8f};  // all < kMinWidth
  hairline.depth_m = {0.05f, 0.05f, 0.05f, 0.05f};
  hairline.speed_m_s = {0.1f, 0.1f, 0.1f, 0.1f};

  // Edge B: widens downstream, crossing kMinWidth between sample 2 (1.5 m,
  // below) and sample 3 (2.5 m, at/above). Width is monotone downstream, so
  // the survivor should be trimmed back to sample 2 (one point upstream of
  // the crossing) through the end.
  RiverEdge widening;
  widening.from = 2;
  widening.to = 3;
  widening.points_m = {glm::vec2(0.0f, 10.0f), glm::vec2(10.0f, 10.0f),
                       glm::vec2(20.0f, 10.0f), glm::vec2(30.0f, 10.0f),
                       glm::vec2(40.0f, 10.0f)};
  widening.discharge_m3_s = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f};
  widening.width_m = {0.5f, 1.0f, 1.5f, 2.5f, 3.0f};
  widening.depth_m = {0.1f, 0.2f, 0.3f, 0.4f, 0.5f};
  widening.speed_m_s = {0.1f, 0.2f, 0.3f, 0.4f, 0.5f};

  g.edges = {hairline, widening};

  prune_river_graph_by_width(g, kMinWidth);

  // The hairline is gone; only the trimmed widening reach survives.
  REQUIRE(g.edges.size() == 1);
  const RiverEdge& survivor = g.edges[0];
  REQUIRE(survivor.points_m.size() == 3);
  REQUIRE(survivor.points_m.front() == glm::vec2(20.0f, 10.0f));
  REQUIRE(survivor.points_m.back() == glm::vec2(40.0f, 10.0f));
  REQUIRE(survivor.width_m.front() == Catch::Approx(1.5f));  // kept upstream point
  REQUIRE(survivor.width_m.back() == Catch::Approx(3.0f));
  REQUIRE(survivor.discharge_m3_s.size() == 3);
  REQUIRE(survivor.discharge_m3_s.front() == Catch::Approx(3.0f));
  REQUIRE(survivor.discharge_m3_s.back() == Catch::Approx(5.0f));
}
