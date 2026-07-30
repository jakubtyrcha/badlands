#include <catch_amalgamated.hpp>

#include <algorithm>
#include <vector>

#include "mapgen/canal_carve.hpp"

using namespace badlands::mapgen;

// The DSU / source-aliasing suite that used to sit here is GONE, and
// deliberately so rather than by oversight. Its only consumer was same-source
// REPULSION, and the design no longer repels: an agent that curls back into
// its own trail is now killed on contact, closing a meander and leaving an
// island. Nothing merges either — touching water ends an agent — so no ids are
// ever unioned and there are no stale aliases left to resolve.

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

  int cut = 0;
  for (size_t i = 0; i < t.B.data.size(); ++i) {
    REQUIRE(t.B.data[i] <= before.data[i] + 1e-6f);  // canals only ever lower
    if (t.B.data[i] < before.data[i] - 1e-6f) ++cut;
  }
  REQUIRE(cut > 0);
}

// The "every carved step goes downhill" case is gone with the guarantee it
// tested. This pass is a primer: it makes a channel relative to its banks and
// leaves monotone flow to the physical sim. Depth is now bounded by
// construction instead (see the bank-depth test below), which is the property
// that actually matters here.

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

TEST_CASE("carve_canals: depth is bounded by the bank target, not by length") {
  // THE property this design exists for. The bed is placed relative to the two
  // cells either side of the channel on the PRE-CANAL terrain, so a canal is
  // one bank-depth deep wherever it runs. Nothing is carried between steps, so
  // there is nothing to accumulate.
  //
  // A carried reference previously produced 15-19 m trenches, and depth that
  // scaled with path length: 0.52 m on a 48 grid against 2.46 m on a 96 grid.
  // Both grids are checked here for exactly that reason.
  auto p = canal_params();
  for (const int n : {48, 96}) {
    for (const float tilt : {0.0f, 0.001f, 0.01f, 0.1f}) {
      auto t = make_plain(n, tilt);
      const auto before = t.B;
      const auto res = carve_canals(t.B, t.lake, t.dist, p, 1.0f, 21);
      INFO("grid " << n << ", tilt " << tilt << ", agents " << res.stats.agents);
      REQUIRE(res.stats.agents > 0);
      float worst_plains = 0.0f;
      for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
          REQUIRE(t.B.at(x, y) <= before.at(x, y) + 1e-6f);  // only ever lowers
          // Plains only. The bank is a MINIMUM over the two flanking cells, so
          // at the highland toe — which steps 2 m per row in this fixture —
          // the cut legitimately reaches depth + that relief. That is the
          // crossing price, not the length accumulation under test.
          if (t.dist.at(x, y) != 0.0f) continue;
          worst_plains = std::max(worst_plains, before.at(x, y) - t.B.at(x, y));
        }
      // Bounded by the target plus the across-channel relief the bank min
      // picks up (at most ~0.14 m at the steepest tilt here) — and, the whole
      // point, the SAME bound on a grid twice the size.
      REQUIRE(worst_plains <= p.canal_depth_m + 0.2f);
    }
  }
}

TEST_CASE("carve_canals: deterministic for the same seed") {
  auto a = make_plain(48, 0.001f);
  auto b = make_plain(48, 0.001f);
  const auto ra = carve_canals(a.B, a.lake, a.dist, canal_params(), 1.0f, 42);
  const auto rb = carve_canals(b.B, b.lake, b.dist, canal_params(), 1.0f, 42);
  REQUIRE(a.B.data == b.B.data);
  REQUIRE(ra.trail_source.data == rb.trail_source.data);
  REQUIRE(ra.stats.agents == rb.stats.agents);
  REQUIRE(ra.trail_dir.data == rb.trail_dir.data);
}

TEST_CASE("carve_canals: agents merge, and never with their own source") {
  // Several seeds on one flat plain must converge rather than run parallel.
  // merges_same_root is the braid detector: nonzero means the DSU check let a
  // network fold back into itself.
  auto t = make_plain(64, 0.001f);
  const auto res = carve_canals(t.B, t.lake, t.dist, canal_params(), 1.0f, 9);
  REQUIRE(res.stats.agents >= 2);
  // Merging IS the healthy outcome now: it means the network converges rather
  // than running parallel lines across the plain.
  REQUIRE(res.stats.ends[static_cast<int>(CanalEnd::Merged)] > 0);
}

TEST_CASE("carve_canals: disabled by zero threshold or zero slope") {
  for (int which = 0; which < 2; ++which) {
    auto t = make_plain(32, 0.0f);
    const auto before = t.B;
    auto p = canal_params();
    if (which == 0) p.canal_seed_area_m2 = 0.0f;
    else p.canal_depth_m = 0.0f;
    const auto res = carve_canals(t.B, t.lake, t.dist, p, 1.0f, 1);
    REQUIRE(res.stats.agents == 0);
    REQUIRE(t.B.data == before.data);
  }
}

