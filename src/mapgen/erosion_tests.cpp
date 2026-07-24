#include <catch_amalgamated.hpp>
#include <cmath>
#include "mapgen/erosion.hpp"
#include "mapgen/hydrology.hpp"

using namespace badlands::mapgen;

TEST_CASE("carve_cavities: coverage ~= lake_frac, depth bounded, only lowers") {
  // bedrock ramp 0..1 over 100x100 -> bottom 5% is a crisp quantile
  Field2D<float> bedrock(100, 100);
  for (int y = 0; y < 100; ++y)
    for (int x = 0; x < 100; ++x)
      bedrock.at(x, y) = (y * 100 + x) / 9999.0f;
  Field2D<float> B(100, 100, 50.0f);
  const auto B_before = B.data;
  const auto mask = carve_cavities(B, bedrock, 0.05f, 12.0f);
  double carved = 0.0;
  float max_cut = 0.0f;
  for (size_t i = 0; i < mask.data.size(); ++i) {
    if (mask.data[i]) carved += 1.0;
    const float cut = B_before[i] - B.data[i];
    REQUIRE(cut >= 0.0f);            // carving only lowers
    REQUIRE(cut <= 12.0f + 1e-4f);   // bounded by lake_depth_m
    if (!mask.data[i]) REQUIRE(cut == 0.0f);
    max_cut = std::max(max_cut, cut);
  }
  REQUIRE(carved / mask.data.size() == Catch::Approx(0.05).margin(0.005));
  REQUIRE(max_cut == Catch::Approx(12.0f).margin(0.5));  // minimum gets full depth
}

TEST_CASE("carve_cavities: lake_frac > 1 clamped to 1, no crash") {
  // bedrock ramp 0..1 over 50x50; lake_frac = 2.0f should clamp to 1.0f
  Field2D<float> bedrock(50, 50);
  for (int y = 0; y < 50; ++y)
    for (int x = 0; x < 50; ++x)
      bedrock.at(x, y) = (y * 50 + x) / 2499.0f;
  Field2D<float> B(50, 50, 100.0f);
  const auto B_before = B.data;
  const auto mask = carve_cavities(B, bedrock, 2.0f, 8.0f);  // lake_frac > 1
  double carved = 0.0;
  for (size_t i = 0; i < mask.data.size(); ++i) {
    if (mask.data[i]) carved += 1.0;
    const float cut = B_before[i] - B.data[i];
    REQUIRE(cut >= 0.0f);            // only lowers
    REQUIRE(cut <= 8.0f + 1e-4f);    // bounded by lake_depth_m
  }
  const double coverage = carved / mask.data.size();
  REQUIRE(coverage > 0.9);  // clamped to 1.0: nearly everything carved (all but max)
}

TEST_CASE("init_sediment: tapers off plains, zero in basins, never negative") {
  Field2D<float> dist(64, 64);
  for (int y = 0; y < 64; ++y)
    for (int x = 0; x < 64; ++x) dist.at(x, y) = static_cast<float>(x);  // 0..63 m
  Field2D<uint8_t> basins(64, 64, 0);
  basins.at(2, 2) = 1;
  ErosionParams p;  // taper 60 m, initial 4 m, noise 1 m
  const auto s = init_sediment(dist, basins, p, 1.0f, 0.0f, 7);
  REQUIRE(s.at(2, 2) == 0.0f);                       // basin floor
  REQUIRE(s.at(1, 30) >= 4.0f - 1.0f - 1e-4f);       // near plains: full blanket ± noise
  REQUIRE(s.at(63, 30) <= 1.0f + 1e-4f);             // past taper: noise only
  for (float v : s.data) REQUIRE(v >= 0.0f);
  // deterministic
  REQUIRE(init_sediment(dist, basins, p, 1.0f, 0.0f, 7).data == s.data);
}

namespace {
// 1D ramp world: one row high, ground rises +1 m per column from the border.
struct Ramp1D {
  Field2D<float> B, S;
  FlowRouting r;
  Field2D<float> area;
};
Ramp1D make_ramp(int w, float sediment) {
  Ramp1D t{Field2D<float>(w, 3), Field2D<float>(w, 3, sediment), {}, {}};
  for (int y = 0; y < 3; ++y)
    for (int x = 0; x < w; ++x) t.B.at(x, y) = static_cast<float>(x);
  Field2D<float> h(w, 3);
  for (size_t i = 0; i < h.data.size(); ++i) h.data[i] = t.B.data[i] + t.S.data[i];
  t.r = route_flow(h, 1.0f, 1e-4f);
  t.area = accumulate_drainage(t.r, 1.0f);
  return t;
}
}  // namespace

TEST_CASE("incise: matches an explicit-Euler reference on a hand-built chain") {
  // Hand-built 1D chain (NOT route_flow — 2D routing would pick diagonal
  // receivers and muddy the geometry): cell i's receiver is i-1, cell 0 is
  // base level. B ramp 0..n-1, dry, A[i] = upstream cell count.
  const int n = 32;
  FlowRouting r;
  r.width = n; r.height = 1;
  r.receiver.assign(n, -1);
  r.in_lake.assign(n, 0);
  r.water_level.assign(n, 0.0f);
  for (int i = 1; i < n; ++i) r.receiver[i] = i - 1;
  for (int i = 0; i < n; ++i) r.order.push_back(i);
  Field2D<float> B(n, 1), S(n, 1, 0.0f);
  Field2D<float> area(n, 1);
  for (int i = 0; i < n; ++i) {
    B.at(i, 0) = static_cast<float>(i);
    r.water_level[i] = static_cast<float>(i);  // dry: level = ground
    area.at(i, 0) = static_cast<float>(n - i);
  }
  ErosionParams p;
  p.k_bedrock = 1e-3f;  // F <= 1e-3 * sqrt(32) ~ 5.7e-3 << 1
  p.dt = 1.0f;

  // independent reference: explicit Euler, 1000 sub-steps on the same graph
  std::vector<double> href(n);
  for (int i = 0; i < n; ++i) href[i] = B.at(i, 0);
  const int M = 1000;
  for (int step = 0; step < M; ++step) {
    std::vector<double> next = href;
    for (int i = 1; i < n; ++i) {
      const double slope = href[i] - href[i - 1];  // d = 1
      next[i] -= p.k_bedrock * std::pow(area.at(i, 0), p.m) * slope * (p.dt / M);
    }
    href = next;
  }
  const auto eroded = incise(B, S, r, area, p, 1.0f);
  for (int i = 1; i < n; ++i) {
    REQUIRE(eroded.at(i, 0) > 0.0f);
    REQUIRE(B.at(i, 0) == Catch::Approx(href[i]).epsilon(0.02));
  }
  REQUIRE(B.at(0, 0) == 0.0f);  // base level pinned
}

TEST_CASE("incise: never erodes a cell below its receiver") {
  auto t = make_ramp(32, 1.0f);
  ErosionParams p;
  p.k_sediment = 10.0f;  // absurdly strong: the clamp must still hold
  p.k_bedrock = 1.0f;
  incise(t.B, t.S, t.r, t.area, p, 1.0f);
  for (int y = 0; y < 3; ++y)
    for (int x = 0; x < 32; ++x) {
      const int i = y * 32 + x;
      const int32_t rcv = t.r.receiver[i];
      if (rcv < 0) continue;
      const float hi = t.B.data[i] + t.S.data[i];
      const float hr = t.B.data[rcv] + t.S.data[rcv];
      REQUIRE(hi >= hr - 1e-4f);
    }
}

TEST_CASE("incise: sediment strips before bedrock; border cells never erode") {
  auto t = make_ramp(32, 0.5f);
  ErosionParams p;
  p.k_sediment = 5e-2f;
  p.k_bedrock = 0.0f;  // bedrock immune -> only sediment may move
  const auto B_before = t.B.data;
  incise(t.B, t.S, t.r, t.area, p, 1.0f);
  REQUIRE(t.B.data == B_before);                    // bedrock untouched
  REQUIRE(t.S.at(2, 1) < 0.5f);                     // sediment eroded
  // border cells are base level (receiver -1): untouched entirely
  REQUIRE(t.S.at(0, 0) == 0.5f);
}
