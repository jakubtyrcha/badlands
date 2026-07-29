#include <catch_amalgamated.hpp>

#include <algorithm>
#include <vector>

#include "mapgen/canal_carve.hpp"

using namespace badlands::mapgen;

// --- source aliasing --------------------------------------------------------
//
// These come first on purpose. Trail cells are never rewritten on merge, so
// their source ids go stale, and a raw `cell.source == agent.source`
// comparison passes essentially every other test in the suite — it only
// misbehaves on cells laid BEFORE a merge. These are constructed so the raw
// comparison fails.

TEST_CASE("SourceSets: a stale id resolves to the merged root") {
  SourceSets s;
  const int32_t a = s.add();
  const int32_t b = s.add();
  REQUIRE(a != b);
  REQUIRE_FALSE(s.same(a, b));

  s.merge(a, b);

  // THE case: a trail cell still carries the literal id `a`, while the agent
  // travelling over it carries `b`. The raw ids differ — only the roots agree.
  // A raw comparison would call this a DIFFERENT source, attract, and braid.
  REQUIRE(a != b);           // the ids genuinely differ...
  REQUIRE(s.same(a, b));     // ...and only find() sees that they are one
}

TEST_CASE("SourceSets: aliasing is transitive across three networks") {
  SourceSets s;
  const int32_t a = s.add(), b = s.add(), c = s.add();
  s.merge(a, b);  // A joins B
  s.merge(c, a);  // C joins A, which is already B
  // C and B never merged directly, yet must read as one source.
  REQUIRE(s.same(b, c));
  REQUIRE(s.same(a, c));
  REQUIRE(s.find(a) == s.find(b));
  REQUIRE(s.find(b) == s.find(c));
}

TEST_CASE("SourceSets: the root does not depend on merge order") {
  // Union by SMALLER ROOT rather than by rank or size, so the representative
  // is reproducible however the unions are sequenced. Without that, agent
  // processing order would leak into which id every cell resolves to.
  auto build = [](bool reversed) {
    SourceSets s;
    std::vector<int32_t> id;
    for (int i = 0; i < 5; ++i) id.push_back(s.add());
    if (reversed) {
      s.merge(id[4], id[3]);
      s.merge(id[2], id[4]);
      s.merge(id[0], id[2]);
      s.merge(id[1], id[0]);
    } else {
      s.merge(id[0], id[1]);
      s.merge(id[2], id[0]);
      s.merge(id[3], id[4]);
      s.merge(id[4], id[2]);
    }
    std::vector<int32_t> roots;
    for (int32_t i : id) roots.push_back(s.find(i));
    return roots;
  };
  const auto forward = build(false);
  const auto reverse = build(true);
  REQUIRE(forward == reverse);
  for (int32_t r : forward) REQUIRE(r == forward[0]);  // all one set
}

TEST_CASE("SourceSets: disjoint networks stay disjoint") {
  SourceSets s;
  const int32_t a = s.add(), b = s.add(), c = s.add(), d = s.add();
  s.merge(a, b);
  s.merge(c, d);
  REQUIRE(s.same(a, b));
  REQUIRE(s.same(c, d));
  // Different catchments: these must still ATTRACT each other, so they must
  // not be conflated.
  REQUIRE_FALSE(s.same(a, c));
  REQUIRE_FALSE(s.same(b, d));
}

TEST_CASE("SourceSets: find is idempotent and merge is a no-op within a set") {
  SourceSets s;
  const int32_t a = s.add(), b = s.add();
  s.merge(a, b);
  const int32_t root = s.find(a);
  REQUIRE(s.find(root) == root);
  s.merge(a, b);  // already one set
  REQUIRE(s.find(a) == root);
  REQUIRE(s.find(b) == root);
}

// --- agent behaviour --------------------------------------------------------

namespace {
struct World {
  Field2D<float> B;
  Field2D<uint8_t> lake;
  Field2D<float> dist;  // 0 == plains; > 0 == highland
};

// A plain that is highland along its top edge, so the highland/plains boundary
// runs across the map and can seed agents. `tilt` is the (gentle) fall toward
// +y; at 0 the plain is dead flat, which is the case canals exist for.
World make_plain(int n, float tilt, float highland_rows = 4.0f) {
  World t{Field2D<float>(n, n, 0.0f), Field2D<uint8_t>(n, n, 0),
          Field2D<float>(n, n, 0.0f)};
  for (int y = 0; y < n; ++y)
    for (int x = 0; x < n; ++x) {
      const float hl = std::max(0.0f, highland_rows - static_cast<float>(y));
      t.dist.at(x, y) = hl;
      t.B.at(x, y) = -tilt * static_cast<float>(y) + 2.0f * hl;
    }
  return t;
}

// A highland that funnels ALL its runoff to one point on the boundary, so a
// single seed dominates. On the plain `make_plain` builds, every highland-edge
// cell drains roughly the same few square metres, so no threshold can isolate
// one agent — the highland has to converge first.
World make_funnelled_plain(int n, float tilt, int mouth_x) {
  World t{Field2D<float>(n, n, 0.0f), Field2D<uint8_t>(n, n, 0),
          Field2D<float>(n, n, 0.0f)};
  for (int y = 0; y < n; ++y)
    for (int x = 0; x < n; ++x) {
      const float hl = std::max(0.0f, 4.0f - static_cast<float>(y));
      t.dist.at(x, y) = hl;
      // Inside the highland, tilt toward mouth_x so its water converges there.
      const float funnel = hl > 0.0f ? 0.05f * std::abs(x - mouth_x) : 0.0f;
      t.B.at(x, y) = -tilt * static_cast<float>(y) + 2.0f * hl + funnel;
    }
  return t;
}

ErosionParams canal_params(float seed_area = 4.0f) {
  ErosionParams p;
  p.canal_seed_area_m2 = seed_area;  // tiny fixtures: a few texels of drainage
  return p;
}

}  // namespace

TEST_CASE("carve_canals: a near-flat plain gets a canal that reaches the map edge") {
  // A DEAD-flat plain is pathological as a fixture: with no gradient the
  // epsilon flood concentrates drainage along whichever row its wavefront
  // sweeps, so seeds inherit an artificial direction and crowd into a two-row
  // band against the highland toe. Give it a real fall — gentler than
  // canal_slope, so canals still have something to cut.
  auto t = make_plain(48, 0.001f);
  const auto before = t.B;
  const auto res = carve_canals(t.B, t.lake, t.dist, canal_params(), 1.0f, 7);

  REQUIRE(res.stats.agents > 0);
  REQUIRE(res.stats.ends[static_cast<int>(CanalEnd::LeftMap)] > 0);
  // Rule 1 forbids non-termination outright, so a step-cap hit is a bug.
  REQUIRE(res.stats.ends[static_cast<int>(CanalEnd::StepCap)] == 0);
  // A braid would mean the source rule failed.
  REQUIRE(res.stats.merges_same_root == 0);

  int cut = 0;
  for (size_t i = 0; i < t.B.data.size(); ++i) {
    REQUIRE(t.B.data[i] <= before.data[i] + 1e-6f);  // canals only ever lower
    if (t.B.data[i] < before.data[i] - 1e-6f) ++cut;
  }
  REQUIRE(cut > 0);
}

TEST_CASE("carve_canals: every carved step goes downhill") {
  // THE structural guarantee, asserted at CARVE TIME through the counter
  // rather than by walking the finished network.
  //
  // Walking afterwards would be the wrong test. The guarantee is per-agent at
  // the moment of cutting, and the height field is SHARED: a later agent
  // crossing a cell with a lower ref deepens it, which can invert the
  // relationship inside an EARLIER agent's channel. Those are genuine uphill
  // steps in the finished field that were never carved as such — a limitation
  // of the design (recorded in the spec), not a violation of the rule. Testing
  // at carve time separates the two.
  for (const float tilt : {0.0f, 0.001f, 0.02f, 0.5f}) {
    auto t = make_plain(48, tilt);
    const auto res = carve_canals(t.B, t.lake, t.dist, canal_params(), 1.0f, 11);
    INFO("tilt " << tilt << ", agents " << res.stats.agents);
    REQUIRE(res.stats.agents > 0);
    REQUIRE(res.stats.uphill_carve_steps == 0);
  }
}

TEST_CASE("carve_canals: many agents still only ever lower the ground") {
  // The multi-agent weaker property. Per-canal monotonicity is not asserted
  // here — see the single-agent case above for why.
  auto t = make_plain(64, 0.001f);
  const auto before = t.B;
  const auto res = carve_canals(t.B, t.lake, t.dist, canal_params(), 1.0f, 11);
  REQUIRE(res.stats.agents > 1);
  for (size_t i = 0; i < t.B.data.size(); ++i)
    REQUIRE(t.B.data[i] <= before.data[i] + 1e-6f);
}

TEST_CASE("carve_canals: an agent is absorbed by a lake ahead of it") {
  auto t = make_plain(48, 0.001f);
  for (int y = 30; y < 40; ++y)
    for (int x = 14; x < 34; ++x) {
      t.lake.at(x, y) = 1;
      t.B.at(x, y) = -3.0f;  // a real basin, well below the plain
    }
  const auto res = carve_canals(t.B, t.lake, t.dist, canal_params(), 1.0f, 3);
  REQUIRE(res.stats.agents > 0);
  REQUIRE(res.stats.ends[static_cast<int>(CanalEnd::Lake)] > 0);
}

TEST_CASE("carve_canals: steep ground is barely touched") {
  // Where the ground already falls faster than the forced descent there is
  // almost nothing to fix, and `min` must never RAISE terrain.
  //
  // Not exactly zero, though: a step taken ACROSS the slope is level in that
  // direction, so it still gets the one canal_slope of forced descent. The
  // bound is therefore a couple of steps' worth, not nothing — asserting zero
  // would be asserting that agents only ever move straight downhill.
  auto t = make_plain(48, 0.5f);  // 0.5 m/m, far steeper than canal_slope
  const auto before = t.B;
  auto p = canal_params();
  const auto res = carve_canals(t.B, t.lake, t.dist, p, 1.0f, 5);
  REQUIRE(res.stats.agents > 0);
  // The bound is max_climb_m, not zero and not one canal_slope: an agent is
  // ALLOWED to step uphill (the user's rule — the carve is what makes it
  // downhill), and on 0.5 m/m ground one uphill step is half a metre of rock.
  // What must hold is that terrain is never RAISED and never cut deeper than a
  // single permitted climb.
  for (size_t i = 0; i < t.B.data.size(); ++i) {
    REQUIRE(t.B.data[i] <= before.data[i] + 1e-6f);          // never raised
    REQUIRE(t.B.data[i] >= before.data[i] - p.canal_max_climb_m);
  }
  REQUIRE(res.stats.max_carve_m <= p.canal_max_climb_m);
}

TEST_CASE("carve_canals: deterministic for the same seed") {
  auto a = make_plain(48, 0.001f);
  auto b = make_plain(48, 0.001f);
  const auto ra = carve_canals(a.B, a.lake, a.dist, canal_params(), 1.0f, 42);
  const auto rb = carve_canals(b.B, b.lake, b.dist, canal_params(), 1.0f, 42);
  REQUIRE(a.B.data == b.B.data);
  REQUIRE(ra.trail_source.data == rb.trail_source.data);
  REQUIRE(ra.trail_discharge_m3_s.data == rb.trail_discharge_m3_s.data);
  REQUIRE(ra.stats.agents == rb.stats.agents);
  REQUIRE(ra.stats.merges == rb.stats.merges);
}

TEST_CASE("carve_canals: agents merge, and never with their own source") {
  // Several seeds on one flat plain must converge rather than run parallel.
  // merges_same_root is the braid detector: nonzero means the DSU check let a
  // network fold back into itself.
  auto t = make_plain(64, 0.001f);
  const auto res = carve_canals(t.B, t.lake, t.dist, canal_params(), 1.0f, 9);
  REQUIRE(res.stats.agents >= 2);
  REQUIRE(res.stats.merges_same_root == 0);
}

TEST_CASE("carve_canals: disabled by zero threshold or zero slope") {
  for (int which = 0; which < 2; ++which) {
    auto t = make_plain(32, 0.0f);
    const auto before = t.B;
    auto p = canal_params();
    if (which == 0) p.canal_seed_area_m2 = 0.0f;
    else p.canal_slope = 0.0f;
    const auto res = carve_canals(t.B, t.lake, t.dist, p, 1.0f, 1);
    REQUIRE(res.stats.agents == 0);
    REQUIRE(t.B.data == before.data);
  }
}
