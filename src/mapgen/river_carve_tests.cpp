// Pure-CPU tests for the river corridor mask and the carved-height field.
//
// Four things are load-bearing, and each fails silently rather than loudly:
//   - the corridor COVERS the centreline and is connected, or the channel is
//     pinched wherever the mask has a hole;
//   - outside it the height is the base surface BITWISE, or the carve dishes
//     the whole map by an invisible fraction of a metre;
//   - the bed only ever descends, or the river runs uphill;
//   - overlapping channels take the DEEPEST, not the sum, or a confluence
//     punches a pit.

#include <catch_amalgamated.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

#include "mapgen/river_carve.hpp"

using namespace badlands;

namespace {

constexpr int kN = 64;            // texels per side
constexpr float kWorldM = 64.0f;  // => 1 m texels, the production lattice
constexpr float kTexelM = kWorldM / static_cast<float>(kN);

// The base surface, re-derived here rather than asked of the carve: the whole
// point of the bitwise contract is that an INDEPENDENT reader of the height
// field agrees with HeightAt outside the corridor. Node (i, j) carries texel
// (min(i, w-1), min(j, h-1)) and sits at (i*texel, j*texel).
float BaseAt(const mapgen::Field2D<float>& f, float wx, float wz) {
  const int w = f.width, h = f.height;
  const float fx = std::clamp(wx / kTexelM, 0.0f, static_cast<float>(w));
  const float fz = std::clamp(wz / kTexelM, 0.0f, static_cast<float>(h));
  const int i0 = std::clamp(static_cast<int>(std::floor(fx)), 0, w);
  const int j0 = std::clamp(static_cast<int>(std::floor(fz)), 0, h);
  const int i1 = std::min(i0 + 1, w);
  const int j1 = std::min(j0 + 1, h);
  const float tx = fx - static_cast<float>(i0);
  const float tz = fz - static_cast<float>(j0);
  auto node = [&](int i, int j) {
    return f.at(std::min(i, w - 1), std::min(j, h - 1));
  };
  const float a = node(i0, j0) + (node(i1, j0) - node(i0, j0)) * tx;
  const float b = node(i0, j1) + (node(i1, j1) - node(i0, j1)) * tx;
  return a + (b - a) * tz;
}

mapgen::Field2D<float> FlatBase(float v) {
  return mapgen::Field2D<float>(kN, kN, v);
}

// A tilted plane: exact float equality outside the corridor is only a real test
// when the base actually varies between texels.
mapgen::Field2D<float> TiltedBase() {
  mapgen::Field2D<float> f(kN, kN, 0.0f);
  for (int z = 0; z < kN; ++z)
    for (int x = 0; x < kN; ++x)
      f.at(x, z) = 100.0f - 0.05f * static_cast<float>(x) +
                   0.013f * static_cast<float>(z);
  return f;
}

// One reach through `pts`, constant width and flow depth throughout. `from` and
// `to` stay at -1 unless a test wants the chains linked through a node.
mapgen::RiverEdge Reach(const std::vector<glm::vec2>& pts, float width,
                        float depth) {
  mapgen::RiverEdge e;
  e.points_m = pts;
  e.width_m.assign(pts.size(), width);
  e.depth_m.assign(pts.size(), depth);
  e.speed_m_s.assign(pts.size(), 1.0f);
  e.discharge_m3_s.assign(pts.size(), 0.1f);
  return e;
}

std::vector<glm::vec2> Line(glm::vec2 a, glm::vec2 b, int segs = 8) {
  std::vector<glm::vec2> pts;
  for (int i = 0; i <= segs; ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(segs);
    pts.push_back(a + (b - a) * t);
  }
  return pts;
}

std::vector<glm::vec2> Arc(glm::vec2 c, float r, float a0, float a1,
                           int segs = 12) {
  std::vector<glm::vec2> pts;
  for (int i = 0; i <= segs; ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(segs);
    const float a = a0 + (a1 - a0) * t;
    pts.push_back(c + r * glm::vec2(std::cos(a), std::sin(a)));
  }
  return pts;
}

mapgen::RiverGraph OneReach(const mapgen::RiverEdge& e) {
  mapgen::RiverGraph g;
  g.nodes.resize(2);
  g.edges.push_back(e);
  return g;
}

struct Built {
  mapgen::RiverGraph graph;
  std::vector<mapgen::RiverArcChain> chains;
  mapgen::RiverCarve carve;
};

Built Build(const mapgen::RiverGraph& g, const mapgen::Field2D<float>& base) {
  Built b;
  b.graph = g;
  b.chains = mapgen::build_river_arcs(b.graph, 0.5f);
  b.carve = mapgen::build_river_carve(b.graph, b.chains, base, kWorldM);
  return b;
}

// Every point on every centreline, at the same 0.25 m spacing the carve uses.
std::vector<glm::vec2> CentrelinePoints(
    const std::vector<mapgen::RiverArcChain>& chains) {
  std::vector<glm::vec2> out;
  for (const mapgen::RiverArcChain& c : chains)
    for (const mapgen::RiverArc& a : c.arcs) {
      const int n = std::max(1, static_cast<int>(std::ceil(a.length_m / 0.25f)));
      for (int i = 0; i <= n; ++i)
        out.push_back(mapgen::arc_point(
            a, a.length_m * static_cast<float>(i) / static_cast<float>(n)));
    }
  return out;
}

glm::ivec2 TexelOf(glm::vec2 p) {
  return glm::ivec2(static_cast<int>(std::floor(p.x / kTexelM + 0.5f)),
                    static_cast<int>(std::floor(p.y / kTexelM + 0.5f)));
}

size_t MaskCount(const mapgen::Field2D<uint8_t>& m) {
  size_t n = 0;
  for (const uint8_t v : m.data) n += (v != 0) ? 1 : 0;
  return n;
}

// Size of the 8-connected component containing the first masked texel.
size_t LargestComponentFromFirst(const mapgen::Field2D<uint8_t>& m) {
  std::vector<uint8_t> seen(m.data.size(), 0);
  std::vector<int> stack;
  for (size_t i = 0; i < m.data.size(); ++i)
    if (m.data[i]) {
      stack.push_back(static_cast<int>(i));
      seen[i] = 1;
      break;
    }
  size_t count = 0;
  while (!stack.empty()) {
    const int i = stack.back();
    stack.pop_back();
    ++count;
    const int x = i % m.width, y = i / m.width;
    for (int dy = -1; dy <= 1; ++dy)
      for (int dx = -1; dx <= 1; ++dx) {
        const int nx = x + dx, ny = y + dy;
        if (!m.in_bounds(nx, ny)) continue;
        const size_t k = static_cast<size_t>(ny) * m.width + nx;
        if (seen[k] || !m.data[k]) continue;
        seen[k] = 1;
        stack.push_back(static_cast<int>(k));
      }
  }
  return count;
}

}  // namespace

TEST_CASE("the corridor covers the centreline and is 8-connected",
          "[river_carve]") {
  // Both a curve (which the mask walks as chords) and a sub-texel channel
  // (which no centre-sampling rasterizer would cover at all).
  struct Case {
    const char* name;
    std::vector<glm::vec2> pts;
    float width;
  };
  const std::vector<Case> cases = {
      {"straight", Line({8.0f, 32.0f}, {56.0f, 32.0f}), 2.0f},
      {"curved", Arc({32.0f, 32.0f}, 20.0f, 0.0f, 1.5f), 3.0f},
      {"sub-texel", Arc({32.0f, 32.0f}, 14.0f, 0.4f, 2.6f), 0.3f},
  };
  for (const Case& c : cases) {
    INFO(c.name);
    const Built b = Build(OneReach(Reach(c.pts, c.width, 0.1f)), FlatBase(100.0f));
    REQUIRE(b.chains.size() == 1);
    REQUIRE(MaskCount(b.carve.mask) > 0);

    for (const glm::vec2 p : CentrelinePoints(b.chains)) {
      const glm::ivec2 t = TexelOf(p);
      REQUIRE(b.carve.mask.in_bounds(t.x, t.y));
      INFO("at " << p.x << ", " << p.y);
      CHECK(b.carve.mask.at(t.x, t.y) != 0);
    }
    CHECK(LargestComponentFromFirst(b.carve.mask) == MaskCount(b.carve.mask));
  }
}

TEST_CASE("outside the corridor the height is the base surface, bitwise",
          "[river_carve]") {
  const mapgen::Field2D<float> base = TiltedBase();
  // width 4 => corridor half-width max(1.5*4, 1) = 6 m.
  const Built b = Build(
      OneReach(Reach(Line({8.0f, 32.0f}, {56.0f, 32.0f}), 4.0f, 0.2f)), base);
  REQUIRE(MaskCount(b.carve.mask) > 0);

  // 6.0 .. 6.4 is the load-bearing band: those points are OUTSIDE the corridor
  // but INSIDE masked texels (the mask is conservative, so it over-covers), so
  // only the profile reaching exactly zero can answer them. The far probes then
  // pin that the mask's own extent is finite and that the outside path returns
  // the base surface through the same expression an independent reader uses.
  for (const float x : {9.0f, 17.3f, 32.0f, 41.7f, 55.0f}) {
    for (const float d : {6.0f, 6.2f, 6.4f, 7.5f, 9.0f, 15.0f, 25.0f}) {
      for (const float side : {-1.0f, 1.0f}) {
        const float z = 32.0f + side * d;
        INFO("at " << x << ", " << z);
        // EXACT equality, not Approx: compact support makes "outside is
        // unchanged" a property of the construction, not of a tolerance.
        CHECK(b.carve.HeightAt(x, z) == BaseAt(base, x, z));
      }
    }
  }
  // The band above is only a test while those texels really are masked.
  for (const float z : {38.0f, 38.4f, 26.0f, 25.6f}) {
    const glm::ivec2 t = TexelOf(glm::vec2(32.0f, z));
    INFO("texel " << t.x << ", " << t.y);
    CHECK(b.carve.mask.at(t.x, t.y) != 0);
  }
  // ... and the channel is genuinely cut where it should be.
  CHECK(b.carve.HeightAt(32.0f, 32.0f) < BaseAt(base, 32.0f, 32.0f));
}

TEST_CASE("the cavity ladder matches the calibrated percentiles",
          "[river_carve]") {
  // (width, flow depth) at W7's p10 / p50 / p90 / trunk at 3 m/yr runoff, and
  // the cavity 1.390*d + 0.45*w^0.6 owes for each. A ladder, not a switch:
  // if this collapses to one depth the model has stopped being physical.
  struct Rung {
    float w, d, cavity;
  };
  const Rung rungs[] = {{0.33f, 0.036f, 0.281f},
                        {0.52f, 0.026f, 0.340f},
                        {0.93f, 0.294f, 0.840f},
                        {7.34f, 0.243f, 1.825f}};
  const mapgen::Field2D<float> base = FlatBase(100.0f);
  for (const Rung& r : rungs) {
    const Built b = Build(
        OneReach(Reach(Line({8.0f, 32.0f}, {56.0f, 32.0f}), r.w, r.d)), base);
    const float cut = BaseAt(base, 32.0f, 32.0f) - b.carve.HeightAt(32.0f, 32.0f);
    INFO("width " << r.w << " depth " << r.d);
    CHECK(cut == Catch::Approx(r.cavity).margin(0.01));
  }
}

TEST_CASE("the bed only ever descends downstream", "[river_carve]") {
  // A rippled base. Subtracting the profile from the LOCAL height would make
  // the bed ride every bump; taking the running downstream minimum of the
  // centreline is what keeps the channel flowing.
  mapgen::Field2D<float> base(kN, kN, 0.0f);
  for (int z = 0; z < kN; ++z)
    for (int x = 0; x < kN; ++x)
      base.at(x, z) = 100.0f - 0.08f * static_cast<float>(x) +
                      2.0f * std::sin(0.7f * static_cast<float>(x));
  const Built b = Build(
      OneReach(Reach(Line({8.0f, 32.0f}, {56.0f, 32.0f}), 1.0f, 0.1f)), base);
  REQUIRE(b.chains.size() == 1);

  float prev = 1e30f;
  int rises = 0;
  for (const glm::vec2 p : CentrelinePoints(b.chains)) {
    const float h = b.carve.HeightAt(p.x, p.y);
    if (h > prev + 1e-4f) ++rises;
    prev = h;
  }
  CHECK(rises == 0);
}

TEST_CASE("the carve never raises terrain", "[river_carve]") {
  mapgen::Field2D<float> base(kN, kN, 0.0f);
  for (int z = 0; z < kN; ++z)
    for (int x = 0; x < kN; ++x)
      base.at(x, z) = 100.0f + 3.0f * std::sin(0.4f * static_cast<float>(x)) *
                                   std::cos(0.3f * static_cast<float>(z));
  const Built b = Build(
      OneReach(Reach(Arc({32.0f, 32.0f}, 18.0f, 0.2f, 2.4f), 2.5f, 0.3f)), base);
  REQUIRE(MaskCount(b.carve.mask) > 0);

  // A fixed irrational-ish lattice, so the probes are not the texel centres and
  // not a seeded RNG either.
  int carved = 0;
  for (float z = 0.13f; z < kWorldM; z += 0.37f) {
    for (float x = 0.07f; x < kWorldM; x += 0.37f) {
      const float h = b.carve.HeightAt(x, z);
      const float base_h = BaseAt(base, x, z);
      INFO("at " << x << ", " << z);
      REQUIRE(h <= base_h);
      if (h < base_h) ++carved;
    }
  }
  CHECK(carved > 0);  // the probe grid actually crossed the corridor
}

TEST_CASE("a confluence takes the deepest cavity, not the sum",
          "[river_carve]") {
  // Two reaches crossing at (32, 32), left unlinked in the graph so each keeps
  // its own bed. Additive carving would double-cut the junction into a pit.
  const mapgen::Field2D<float> base = FlatBase(100.0f);
  const mapgen::RiverEdge across = Reach(Line({8.0f, 32.0f}, {56.0f, 32.0f}), 1.0f, 0.05f);
  const mapgen::RiverEdge down = Reach(Line({32.0f, 8.0f}, {32.0f, 56.0f}), 6.0f, 0.4f);

  const float cut_a = 100.0f - Build(OneReach(across), base).carve.HeightAt(32.0f, 32.0f);
  const float cut_b = 100.0f - Build(OneReach(down), base).carve.HeightAt(32.0f, 32.0f);
  REQUIRE(cut_a > 0.0f);
  REQUIRE(cut_b > cut_a + 0.2f);  // the two rungs must actually differ

  mapgen::RiverGraph both;
  both.nodes.resize(2);
  both.edges.push_back(across);
  both.edges.push_back(down);
  const float cut = 100.0f - Build(both, base).carve.HeightAt(32.0f, 32.0f);
  CHECK(cut >= cut_a - 1e-4f);
  CHECK(cut >= cut_b - 1e-4f);
  CHECK(cut == Catch::Approx(std::max(cut_a, cut_b)).margin(1e-3));
}

TEST_CASE("degenerate inputs carve nothing rather than crashing",
          "[river_carve]") {
  const mapgen::Field2D<float> base = TiltedBase();

  SECTION("an empty graph") {
    const mapgen::RiverCarve carve =
        mapgen::build_river_carve(mapgen::RiverGraph{}, {}, base, kWorldM);
    CHECK(MaskCount(carve.mask) == 0);
    for (const float x : {0.0f, 13.7f, 32.0f, 63.9f})
      for (const float z : {0.0f, 7.2f, 48.5f, 63.9f})
        CHECK(carve.HeightAt(x, z) == BaseAt(base, x, z));
  }

  SECTION("a chain pointing outside the graph is skipped, not dereferenced") {
    const mapgen::RiverGraph g =
        OneReach(Reach(Line({8.0f, 32.0f}, {56.0f, 32.0f}), 4.0f, 0.2f));
    std::vector<mapgen::RiverArcChain> chains = mapgen::build_river_arcs(g, 0.5f);
    REQUIRE(chains.size() == 1);
    for (const int32_t bad : {-1, 7, 100000}) {
      chains[0].edge = bad;
      const mapgen::RiverCarve carve =
          mapgen::build_river_carve(g, chains, base, kWorldM);
      INFO("edge " << bad);
      CHECK(MaskCount(carve.mask) == 0);
      CHECK(carve.HeightAt(32.0f, 32.0f) == BaseAt(base, 32.0f, 32.0f));
    }
  }

  SECTION("a through-lake reach carries no geometry to carve") {
    mapgen::RiverGraph g;
    g.nodes.resize(2);
    mapgen::RiverEdge e;
    e.points_m.push_back(glm::vec2(32.0f, 32.0f));
    e.width_m.push_back(4.0f);
    e.depth_m.push_back(0.2f);
    g.edges.push_back(e);
    mapgen::RiverArcChain chain;
    chain.edge = 0;
    const mapgen::RiverCarve carve =
        mapgen::build_river_carve(g, {chain}, base, kWorldM);
    CHECK(MaskCount(carve.mask) == 0);
  }

  SECTION("an empty base height field") {
    const mapgen::RiverGraph g =
        OneReach(Reach(Line({8.0f, 32.0f}, {56.0f, 32.0f}), 4.0f, 0.2f));
    const mapgen::RiverCarve carve = mapgen::build_river_carve(
        g, mapgen::build_river_arcs(g, 0.5f), mapgen::Field2D<float>{}, kWorldM);
    CHECK(carve.mask.size() == 0);
    CHECK(carve.HeightAt(32.0f, 32.0f) == 0.0f);
  }
}
