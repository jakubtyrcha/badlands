#include <catch_amalgamated.hpp>

#include <cmath>
#include <vector>

#include "mapgen/river_graph.hpp"

using namespace badlands::mapgen;

namespace {
// Ratio of a quantity at 2Q vs Q (or 2S vs S) — the exponent shows up directly
// as log2 of the ratio, so these read as the hydraulic-geometry exponents.
float log2_ratio(float a, float b) { return std::log2(a / b); }

// --- artificial heightmaps --------------------------------------------------
//
// Two builders cover every topology below. Design constraints that matter:
//   - WALLS ARE STEEP, so hillslope flow reaches the channel in a cell or two
//     and wall cells accumulate only a few texel areas. That leaves three
//     orders of magnitude between wall and channel drainage, so the channel
//     threshold separates them cleanly instead of manufacturing order-1 stubs.
//   - BRANCHES ARE LONG, so even the shortest leaf clears the threshold. A
//     leaf that never becomes a channel silently collapses the tree's Strahler
//     order without failing anything.

struct Segment {
  glm::vec2 a, b;
};

float point_seg_dist(glm::vec2 p, glm::vec2 a, glm::vec2 b) {
  const glm::vec2 ab = b - a;
  const float l2 = glm::dot(ab, ab);
  if (l2 <= 0.0f) return glm::length(p - a);
  const float t = std::clamp(glm::dot(p - a, ab) / l2, 0.0f, 1.0f);
  return glm::length(p - (a + t * ab));
}

// V-valley network: height falls along +y at `along_slope` and rises away from
// the nearest skeleton segment at `wall_slope`. Computed analytically per
// segment, so headings carry no lattice bias.
Field2D<float> make_valley_network(int n, const std::vector<Segment>& skeleton,
                                   float along_slope, float wall_slope) {
  Field2D<float> h(n, n);
  for (int y = 0; y < n; ++y)
    for (int x = 0; x < n; ++x) {
      const glm::vec2 p(static_cast<float>(x), static_cast<float>(y));
      float d = 1e9f;
      for (const auto& s : skeleton) d = std::min(d, point_seg_dist(p, s.a, s.b));
      h.at(x, y) = -along_slope * static_cast<float>(y) + wall_slope * d;
    }
  return h;
}

struct Extracted {
  Field2D<float> h, area, depth, ground;
  FlowRouting r;
  RiverGraph g;
};

// The channel threshold must clear the WALL drainage, not merely a few texels.
// On these fixtures a wall flowline is ~20-30 cells long, so a wall cell just
// before it joins the thalweg carries ~20-30 m^2; the thalweg itself gathers a
// full catchment row (~40 m^2) per cell of descent. 200 m^2 sits an order of
// magnitude above the walls and is reached a few cells below each valley tip.
// Setting this too low turns every hillslope cell into a channel — an 80x80 Y
// fixture produced 324 edges and 248 "sources" at 8 m^2.
Extracted run(const Field2D<float>& h, float min_area_texels = 200.0f,
              float texel_m = 1.0f) {
  Extracted e;
  e.h = h;
  e.r = route_flow(e.h, texel_m, kEpsilonM);
  e.area = accumulate_drainage(e.r, texel_m * texel_m);
  e.depth = Field2D<float>(h.width, h.height, 0.0f);  // no lakes unless set
  e.ground = e.h;
  ErosionParams p;
  p.min_channel_area_m2 = min_area_texels * texel_m * texel_m;
  e.g = extract_river_graph(e.r, e.area, e.depth, e.ground, p, texel_m, 0.0f);
  return e;
}

int count_kind(const RiverGraph& g, RiverNodeKind k) {
  int c = 0;
  for (const auto& n : g.nodes)
    if (n.kind == k) ++c;
  return c;
}
}  // namespace

TEST_CASE("channel_hydraulics: continuity w*d*v == Q holds identically") {
  ErosionParams p;
  for (const float q : {1e-5f, 1e-4f, 1e-3f, 1e-2f, 1e-1f, 1.0f, 10.0f}) {
    for (const float s : {1e-4f, 1e-3f, 1e-2f, 0.05f, 0.2f, 0.6f}) {
      const auto h = channel_hydraulics(q, s, p);
      INFO("Q=" << q << " S=" << s);
      REQUIRE(h.width_m > 0.0f);
      REQUIRE(h.depth_m > 0.0f);
      REQUIRE(h.speed_m_s > 0.0f);
      REQUIRE(h.width_m * h.depth_m * h.speed_m_s == Catch::Approx(q).epsilon(1e-4));
    }
  }
}

TEST_CASE("channel_hydraulics: discharge exponents are 0.5 / 0.3 / 0.2") {
  // Downstream hydraulic geometry: w ~ Q^0.5, d ~ Q^0.3, v ~ Q^0.2. They sum
  // to 1.0, which IS continuity — if any one drifts, w*d*v stops equalling Q.
  ErosionParams p;
  const float s = 0.02f;
  for (const float q : {1e-4f, 1e-2f, 1.0f}) {
    const auto a = channel_hydraulics(q, s, p);
    const auto b = channel_hydraulics(2.0f * q, s, p);
    INFO("Q=" << q);
    REQUIRE(log2_ratio(b.width_m, a.width_m) == Catch::Approx(0.5f).margin(1e-3));
    REQUIRE(log2_ratio(b.depth_m, a.depth_m) == Catch::Approx(0.3f).margin(1e-3));
    REQUIRE(log2_ratio(b.speed_m_s, a.speed_m_s) == Catch::Approx(0.2f).margin(1e-3));
  }
}

TEST_CASE("channel_hydraulics: a steeper reach is shallower and faster") {
  // Rapids vs slow water. At fixed Q: d ~ S^-0.3, v ~ S^0.3, w unchanged
  // (regime width depends on discharge alone).
  ErosionParams p;
  const float q = 0.01f;
  for (const float s : {1e-3f, 1e-2f, 0.1f}) {
    const auto a = channel_hydraulics(q, s, p);
    const auto b = channel_hydraulics(q, 2.0f * s, p);
    INFO("S=" << s);
    REQUIRE(b.speed_m_s > a.speed_m_s);
    REQUIRE(b.depth_m < a.depth_m);
    REQUIRE(b.width_m == Catch::Approx(a.width_m));
    REQUIRE(log2_ratio(b.speed_m_s, a.speed_m_s) == Catch::Approx(0.3f).margin(1e-3));
    REQUIRE(log2_ratio(b.depth_m, a.depth_m) == Catch::Approx(-0.3f).margin(1e-3));
  }
}

TEST_CASE("channel_hydraulics: slope is floored, and Q<=0 is inert") {
  ErosionParams p;
  const auto floored = channel_hydraulics(0.01f, kMinChannelSlope, p);
  // Manning divides by sqrt(S), so without the floor a flat reach would give
  // infinite depth.
  for (const float s : {0.0f, 1e-9f, kMinChannelSlope * 0.5f}) {
    const auto h = channel_hydraulics(0.01f, s, p);
    INFO("S=" << s);
    REQUIRE(std::isfinite(h.depth_m));
    REQUIRE(h.depth_m == Catch::Approx(floored.depth_m));
    REQUIRE(h.speed_m_s == Catch::Approx(floored.speed_m_s));
  }
  for (const float q : {0.0f, -1.0f}) {
    const auto h = channel_hydraulics(q, 0.01f, p);
    REQUIRE(h.width_m == 0.0f);
    REQUIRE(h.depth_m == 0.0f);
    REQUIRE(h.speed_m_s == 0.0f);
  }
}

TEST_CASE("classify_discharge: absolute decade boundaries, monotone") {
  REQUIRE(classify_discharge(0.0f) == RiverClass::None);
  REQUIRE(classify_discharge(-1.0f) == RiverClass::None);
  // Any positive flow is at least a Rill, so a rasterized channel never
  // classifies as None — the class field doubles as the channel mask.
  REQUIRE(classify_discharge(1e-12f) == RiverClass::Rill);

  REQUIRE(classify_discharge(9.9e-5f) == RiverClass::Rill);
  REQUIRE(classify_discharge(1e-4f) == RiverClass::Brook);
  REQUIRE(classify_discharge(9.9e-4f) == RiverClass::Brook);
  REQUIRE(classify_discharge(1e-3f) == RiverClass::Stream);
  REQUIRE(classify_discharge(1e-2f) == RiverClass::Creek);
  REQUIRE(classify_discharge(1e-1f) == RiverClass::River);
  REQUIRE(classify_discharge(1.0f) == RiverClass::Major);
  REQUIRE(classify_discharge(1e6f) == RiverClass::Major);

  int prev = 0;
  for (float q = 1e-6f; q < 1e3f; q *= 1.3f) {
    const int c = static_cast<int>(classify_discharge(q));
    REQUIRE(c >= prev);
    prev = c;
  }
}

TEST_CASE("simplify_polyline: collapses a D8 staircase to its true heading") {
  // A 22.5-degree heading is the worst case for D8: tan(22.5) = 0.4142, so the
  // chain alternates orthogonal and diagonal steps, and its sinuosity reaches
  // the analytic maximum [(1-f) + f*sqrt(2)] / sqrt(1+f*f) = 1.0824. The
  // heading is not lost — it is encoded in the step RATIO — so simplification
  // must recover it rather than merely smooth it.
  const float f = std::tan(22.5f * 3.14159265358979f / 180.0f);
  std::vector<glm::vec2> staircase;
  float x = 0.0f, y = 0.0f, debt = 0.0f;
  for (int i = 0; i < 200; ++i) {
    staircase.push_back({x, y});
    debt += f;
    x += 1.0f;
    if (debt >= 1.0f) {
      y += 1.0f;
      debt -= 1.0f;
    }
  }
  staircase.push_back({x, y});

  auto sinuosity = [](const std::vector<glm::vec2>& p) {
    float len = 0.0f;
    for (size_t i = 1; i < p.size(); ++i) len += glm::length(p[i] - p[i - 1]);
    return len / glm::length(p.back() - p.front());
  };
  REQUIRE(sinuosity(staircase) == Catch::Approx(1.0824f).margin(0.005));

  const auto simple = simplify_polyline(staircase, 0.9f);
  REQUIRE(simple.size() <= 3);
  REQUIRE(sinuosity(simple) <= 1.02f);
  REQUIRE(simple.front() == staircase.front());
  REQUIRE(simple.back() == staircase.back());
  // and the recovered heading is the true 22.5 degrees, not a lattice multiple
  const glm::vec2 d = simple.back() - simple.front();
  REQUIRE(std::atan2(d.y, d.x) * 180.0f / 3.14159265358979f ==
          Catch::Approx(22.5f).margin(0.5));
}

TEST_CASE("simplify_polyline: keeps genuine turns, and degenerate inputs") {
  const std::vector<glm::vec2> elbow{{0, 0}, {5, 0}, {10, 0}, {10, 5}, {10, 10}};
  const auto s = simplify_polyline(elbow, 0.9f);
  REQUIRE(s.size() == 3);  // both ends plus the corner
  REQUIRE(s[1] == glm::vec2{10, 0});

  REQUIRE(simplify_polyline({}, 1.0f).empty());
  REQUIRE(simplify_polyline({{1, 1}}, 1.0f).size() == 1);
  REQUIRE(simplify_polyline({{1, 1}, {2, 2}}, 1.0f).size() == 2);
}

TEST_CASE("resample_polyline: uniform arc length, endpoints pinned") {
  const std::vector<glm::vec2> line{{0, 0}, {10, 0}};
  const auto r = resample_polyline(line, 2.0f);
  REQUIRE(r.front() == glm::vec2{0, 0});
  REQUIRE(r.back() == glm::vec2{10, 0});
  for (size_t i = 1; i + 1 < r.size(); ++i)
    REQUIRE(glm::length(r[i] - r[i - 1]) == Catch::Approx(2.0f).margin(1e-3));

  // total length is preserved for a straight line
  float len = 0.0f;
  for (size_t i = 1; i < r.size(); ++i) len += glm::length(r[i] - r[i - 1]);
  REQUIRE(len == Catch::Approx(10.0f).margin(1e-3));

  // a polyline shorter than one spacing keeps just its endpoints
  const auto tiny = resample_polyline({{0, 0}, {0.5f, 0}}, 2.0f);
  REQUIRE(tiny.size() == 2);
}

TEST_CASE("extract_river_graph: a single straight valley is one directed edge") {
  // Valley heading straight down +y, walls steep so no wall cell becomes a
  // channel. Expect exactly one reach: source -> mouth.
  const int n = 64;
  const auto h = make_valley_network(n, {{{32.0f, 0.0f}, {32.0f, 63.0f}}}, 1.0f, 6.0f);
  const auto e = run(h);

  REQUIRE(e.g.edges.size() == 1);
  REQUIRE(count_kind(e.g, RiverNodeKind::Source) == 1);
  REQUIRE(count_kind(e.g, RiverNodeKind::Confluence) == 0);
  REQUIRE(e.g.edges[0].strahler_order == 1);
  REQUIRE(e.g.edges[0].shreve_magnitude == 1);

  // Directed downhill: the edge runs from higher ground to lower.
  const auto& edge = e.g.edges[0];
  REQUIRE(e.g.nodes[edge.from].ground_m > e.g.nodes[edge.to].ground_m);
  // and discharge is non-decreasing downstream along the reach
  for (size_t k = 1; k < edge.discharge_m3_s.size(); ++k)
    REQUIRE(edge.discharge_m3_s[k] >= edge.discharge_m3_s[k - 1] * 0.999f);
}

TEST_CASE("extract_river_graph: a Y junction gives 3 edges and Strahler 2") {
  // Two branches meeting a trunk. Strahler promotes only where equal orders
  // meet, so the trunk is 2; Shreve is additive, so the trunk is 2 as well.
  const int n = 80;
  const std::vector<Segment> y{
      {{20.0f, 5.0f}, {40.0f, 40.0f}},   // left branch
      {{60.0f, 5.0f}, {40.0f, 40.0f}},   // right branch
      {{40.0f, 40.0f}, {40.0f, 75.0f}},  // trunk
  };
  const auto e = run(make_valley_network(n, y, 1.0f, 6.0f));

  REQUIRE(e.g.edges.size() == 3);
  REQUIRE(count_kind(e.g, RiverNodeKind::Source) == 2);
  REQUIRE(count_kind(e.g, RiverNodeKind::Confluence) == 1);

  int trunk = -1;
  for (size_t i = 0; i < e.g.edges.size(); ++i)
    if (e.g.nodes[e.g.edges[i].from].kind == RiverNodeKind::Confluence)
      trunk = static_cast<int>(i);
  REQUIRE(trunk >= 0);
  REQUIRE(e.g.edges[trunk].strahler_order == 2);
  REQUIRE(e.g.edges[trunk].shreve_magnitude == 2);
  for (size_t i = 0; i < e.g.edges.size(); ++i) {
    if (static_cast<int>(i) == trunk) continue;
    REQUIRE(e.g.edges[i].strahler_order == 1);
    REQUIRE(e.g.edges[i].shreve_magnitude == 1);
  }

  // Discharge is conserved at the confluence: the trunk carries at least the
  // sum of its branches, plus the hillslope area between them.
  //
  // Measure the branches STRICTLY UPSTREAM of the junction. A branch's last
  // point is the confluence cell itself — walk() stops there — and so is the
  // trunk's first point, so summing branch endpoints would count the junction
  // twice and demand the trunk carry double what it should.
  float in_sum = 0.0f;
  for (size_t i = 0; i < e.g.edges.size(); ++i) {
    if (static_cast<int>(i) == trunk) continue;
    const auto& q = e.g.edges[i].discharge_m3_s;
    REQUIRE(q.size() >= 2);
    in_sum += q[q.size() - 2];
  }
  REQUIRE(e.g.edges[trunk].discharge_m3_s.front() >= in_sum * 0.99f);
}

TEST_CASE("extract_river_graph: comb — Strahler holds at 2, Shreve keeps climbing") {
  // A trunk with N tributaries joining one at a time. This is the fixture that
  // pins the DEFINITIONAL difference: a strictly lower-order tributary never
  // promotes Strahler, while Shreve increments at every junction.
  const int n = 96;
  std::vector<Segment> comb{{{48.0f, 5.0f}, {48.0f, 90.0f}}};  // trunk
  const int kTribs = 4;
  for (int t = 0; t < kTribs; ++t) {
    const float y = 20.0f + 15.0f * static_cast<float>(t);
    comb.push_back({{12.0f, y - 12.0f}, {48.0f, y}});  // joins from the left
  }
  const auto e = run(make_valley_network(n, comb, 1.0f, 6.0f));

  REQUIRE(count_kind(e.g, RiverNodeKind::Source) == kTribs + 1);
  REQUIRE(count_kind(e.g, RiverNodeKind::Confluence) == kTribs);

  int32_t max_strahler = 0, max_shreve = 0;
  for (const auto& ed : e.g.edges) {
    max_strahler = std::max(max_strahler, ed.strahler_order);
    max_shreve = std::max(max_shreve, ed.shreve_magnitude);
  }
  REQUIRE(max_strahler == 2);                // never promotes past 2
  REQUIRE(max_shreve == kTribs + 1);         // one per source
}

TEST_CASE("extract_river_graph: stream orders obey their invariants everywhere") {
  const int n = 80;
  const std::vector<Segment> y{
      {{20.0f, 5.0f}, {40.0f, 40.0f}},
      {{60.0f, 5.0f}, {40.0f, 40.0f}},
      {{40.0f, 40.0f}, {40.0f, 75.0f}},
  };
  const auto e = run(make_valley_network(n, y, 1.0f, 6.0f));

  std::vector<std::vector<int>> incoming(e.g.nodes.size());
  for (size_t i = 0; i < e.g.edges.size(); ++i)
    incoming[e.g.edges[i].to].push_back(static_cast<int>(i));

  for (const auto& ed : e.g.edges) {
    REQUIRE(ed.strahler_order >= 1);
    REQUIRE(ed.shreve_magnitude >= 1);
    for (const int u : incoming[ed.from]) {
      // non-decreasing downstream, and strictly increasing in Shreve at a join
      REQUIRE(ed.strahler_order >= e.g.edges[u].strahler_order);
      REQUIRE(ed.shreve_magnitude >= e.g.edges[u].shreve_magnitude);
    }
    if (incoming[ed.from].size() >= 2) {
      int sum = 0;
      for (const int u : incoming[ed.from]) sum += e.g.edges[u].shreve_magnitude;
      REQUIRE(ed.shreve_magnitude == sum);
    }
  }
}

TEST_CASE("extract_river_graph: no cycles — every walk reaches a terminus") {
  const int n = 80;
  const std::vector<Segment> y{
      {{20.0f, 5.0f}, {40.0f, 40.0f}},
      {{60.0f, 5.0f}, {40.0f, 40.0f}},
      {{40.0f, 40.0f}, {40.0f, 75.0f}},
  };
  const auto e = run(make_valley_network(n, y, 1.0f, 6.0f));

  std::vector<int32_t> out_edge(e.g.nodes.size(), -1);
  for (size_t i = 0; i < e.g.edges.size(); ++i) out_edge[e.g.edges[i].from] = static_cast<int32_t>(i);
  for (size_t start = 0; start < e.g.nodes.size(); ++start) {
    size_t cur = start, steps = 0;
    while (out_edge[cur] >= 0) {
      cur = static_cast<size_t>(e.g.edges[static_cast<size_t>(out_edge[cur])].to);
      REQUIRE(++steps <= e.g.edges.size());
    }
  }
}

TEST_CASE("extract_river_graph: deterministic") {
  const int n = 64;
  const auto h = make_valley_network(n, {{{32.0f, 0.0f}, {32.0f, 63.0f}}}, 1.0f, 6.0f);
  const auto a = run(h);
  const auto b = run(h);
  REQUIRE(a.g.nodes.size() == b.g.nodes.size());
  REQUIRE(a.g.edges.size() == b.g.edges.size());
  for (size_t i = 0; i < a.g.edges.size(); ++i) {
    REQUIRE(a.g.edges[i].points_m.size() == b.g.edges[i].points_m.size());
    REQUIRE(a.g.edges[i].strahler_order == b.g.edges[i].strahler_order);
    for (size_t k = 0; k < a.g.edges[i].points_m.size(); ++k)
      REQUIRE(a.g.edges[i].points_m[k] == b.g.edges[i].points_m[k]);
  }
}

TEST_CASE("extract_river_graph: a diagonal valley de-lattices to its true heading") {
  // A valley at 22.5 degrees — the worst case for D8. The extracted polyline
  // must recover the heading rather than preserve the staircase.
  const int n = 96;
  const float dx = 80.0f * std::tan(22.5f * 3.14159265358979f / 180.0f);
  const auto h = make_valley_network(
      n, {{{48.0f - dx * 0.5f, 5.0f}, {48.0f + dx * 0.5f, 85.0f}}}, 1.0f, 6.0f);
  const auto e = run(h);
  REQUIRE(e.g.edges.size() >= 1);

  const auto& pts = e.g.edges[0].points_m;
  REQUIRE(pts.size() >= 2);
  float len = 0.0f;
  for (size_t k = 1; k < pts.size(); ++k) len += glm::length(pts[k] - pts[k - 1]);
  const float straight = glm::length(pts.back() - pts.front());
  // A raw D8 chain at this heading has sinuosity ~1.0824; de-latticed it must
  // be far closer to straight.
  REQUIRE(len / straight <= 1.03f);
}

// --- lakes in the graph -----------------------------------------------------
//
// No fixture here built a lake before, so extraction passes 2 and 3 — the lake
// outflow and the through-lake edges — were entirely unexercised. Three real
// defects lived in that gap.

namespace {
// A valley running into a bowl, with the bowl marked wet, so the extraction
// sees an actual lake with an inlet and an outflow.
struct LakeWorld {
  Field2D<float> h, area, depth, ground;
  Field2D<int32_t> lake_id;
  std::vector<LakeInfo> lakes;
  FlowRouting r;
  RiverGraph g;
};

LakeWorld run_with_lake(LakeKind kind) {
  const int n = 80;
  LakeWorld t;
  t.h = Field2D<float>(n, n, 0.0f);
  for (int y = 0; y < n; ++y)
    for (int x = 0; x < n; ++x) {
      const float d = std::abs(static_cast<float>(x) - 40.0f);
      t.h.at(x, y) = -1.0f * static_cast<float>(y) + 6.0f * d;  // valley down +y
    }
  // A bowl partway down, deep enough to hold water.
  t.depth = Field2D<float>(n, n, 0.0f);
  for (int y = 40; y <= 50; ++y)
    for (int x = 35; x <= 45; ++x) {
      t.h.at(x, y) = -60.0f;
      t.depth.at(x, y) = 3.0f;
    }
  t.ground = t.h;
  t.r = route_flow(t.h, 1.0f, kEpsilonM);
  t.area = accumulate_drainage(t.r, 1.0f);

  t.lake_id = Field2D<int32_t>(n, n, -1);
  for (size_t i = 0; i < t.depth.data.size(); ++i)
    if (t.depth.data[i] > 0.0f) t.lake_id.data[i] = 0;
  LakeInfo info;
  info.kind = kind;
  t.lakes.push_back(info);

  ErosionParams p;
  p.min_channel_area_m2 = 60.0f;
  t.g = extract_river_graph(t.r, t.area, t.depth, t.ground, p, 1.0f, 0.0f,
                            &t.lake_id, &t.lakes);
  return t;
}
}  // namespace

TEST_CASE("extract_river_graph: a lake's outflow is emitted once, as a LakeOutlet") {
  // A lake's sill has ZERO channel donors — the only thing draining into it is
  // the lake, and lake cells are not in the channel set — so pass 1 also saw
  // it as a source and walked the identical chain. Two byte-identical edges,
  // and add_node's dedup meant the node kept kind=Source so LakeOutlet never
  // stuck. The duplicate then gave the shared `to` node two equal-order
  // inflows, spuriously promoting Strahler and doubling Shreve.
  const auto t = run_with_lake(LakeKind::Seeded);
  REQUIRE_FALSE(t.g.edges.empty());

  for (size_t a = 0; a < t.g.edges.size(); ++a)
    for (size_t b = a + 1; b < t.g.edges.size(); ++b) {
      INFO("edges " << a << " and " << b);
      const bool same_ends = t.g.edges[a].from == t.g.edges[b].from &&
                             t.g.edges[a].to == t.g.edges[b].to;
      const bool same_geom = t.g.edges[a].points_m == t.g.edges[b].points_m;
      REQUIRE_FALSE((same_ends && same_geom));
    }
}

TEST_CASE("extract_river_graph: lake nodes report the lake's provenance") {
  // lake_kind was read at the NODE's cell, but a LakeInlet sits on the last
  // dry channel cell before the shore and a LakeOutlet on the sill outside the
  // lake — neither is ever a lake cell, so the lookup always found -1 and
  // every node stayed Emergent.
  for (const LakeKind kind : {LakeKind::Seeded, LakeKind::Emergent}) {
    const auto t = run_with_lake(kind);
    int lake_nodes = 0;
    for (const auto& nd : t.g.nodes) {
      if (nd.kind != RiverNodeKind::LakeInlet && nd.kind != RiverNodeKind::LakeOutlet)
        continue;
      ++lake_nodes;
      INFO("node kind " << static_cast<int>(nd.kind));
      REQUIRE(nd.lake_id >= 0);
      REQUIRE(nd.lake_kind == kind);
    }
    REQUIRE(lake_nodes > 0);
  }
}

TEST_CASE("extract_river_graph: every edge gets a stream order assigned") {
  // Kahn's pass kept ONE outgoing edge per node, so any edge not reachable
  // from the downstream walk was never processed and silently kept order 1 /
  // magnitude 1 whatever its upstream network. Shreve at a confluence is the
  // sum of its inflows, which an unprocessed edge breaks.
  const auto t = run_with_lake(LakeKind::Seeded);
  std::vector<std::vector<int>> incoming(t.g.nodes.size());
  for (size_t i = 0; i < t.g.edges.size(); ++i)
    incoming[t.g.edges[i].to].push_back(static_cast<int>(i));

  for (const auto& ed : t.g.edges) {
    if (incoming[ed.from].empty()) continue;  // a true source
    int sum = 0, best = 0, best_n = 0;
    for (const int u : incoming[ed.from]) {
      sum += t.g.edges[u].shreve_magnitude;
      const int o = t.g.edges[u].strahler_order;
      if (o > best) { best = o; best_n = 1; }
      else if (o == best) { ++best_n; }
    }
    REQUIRE(ed.shreve_magnitude == sum);
    REQUIRE(ed.strahler_order == (best_n >= 2 ? best + 1 : best));
  }
}

TEST_CASE("extract_river_graph: resampled vertices keep their reach's discharge") {
  // Resampling inserts NEW vertices between the Douglas-Peucker survivors, and
  // those can land on a hillslope cell beside the staircased chain. Sampling
  // drainage AT the vertex read that cell's near-zero area: measured -99.2% on
  // a straight 22.5-degree valley, which classified stretches of a real reach
  // down to Rill and banded the raster.
  const int n = 96;
  const float dx = 80.0f * std::tan(22.5f * 3.14159265358979f / 180.0f);
  const auto h = make_valley_network(
      n, {{{48.0f - dx * 0.5f, 5.0f}, {48.0f + dx * 0.5f, 85.0f}}}, 1.0f, 6.0f);
  const auto e = run(h);
  REQUIRE(!e.g.edges.empty());

  for (const auto& ed : e.g.edges) {
    const auto& q = ed.discharge_m3_s;
    for (size_t k = 1; k < q.size(); ++k) {
      INFO("vertex " << k << " of " << q.size());
      // Discharge is non-decreasing downstream along a reach; a resampled
      // vertex must never read a hillslope cell's area.
      REQUIRE(q[k] >= q[k - 1] * 0.99f);
    }
  }
}
