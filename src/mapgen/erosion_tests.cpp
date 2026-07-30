#include <catch_amalgamated.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>
#include "mapgen/erosion.hpp"
#include "mapgen/generator.hpp"
#include "mapgen/hydrology.hpp"
#include "mapgen/resample.hpp"

using namespace badlands::mapgen;

// Brute-force nearest-non-mask-cell Euclidean world distance from (xi, yi),
// scanning every cell — an independent oracle for distance_to_mask's exact
// F-H EDT, used to cross-check carve_cavities' conical depth formula.
float brute_force_dist_to_non_mask(const Field2D<uint8_t>& mask, int xi, int yi,
                                   glm::vec2 texel_m) {
  float best = std::numeric_limits<float>::infinity();
  for (int y = 0; y < mask.height; ++y) {
    for (int x = 0; x < mask.width; ++x) {
      if (mask.at(x, y)) continue;
      const float dx = static_cast<float>(x - xi) * texel_m.x;
      const float dy = static_cast<float>(y - yi) * texel_m.y;
      best = std::min(best, std::sqrt(dx * dx + dy * dy));
    }
  }
  return best;
}

TEST_CASE("carve_cavities: coverage ~= lake_frac, depth == slope * EDT-to-rim, only lowers") {
  // bedrock ramp 0..1 over 100x100 -> bottom 5% is a crisp quantile
  Field2D<float> bedrock(100, 100);
  for (int y = 0; y < 100; ++y)
    for (int x = 0; x < 100; ++x)
      bedrock.at(x, y) = (y * 100 + x) / 9999.0f;
  Field2D<float> B(100, 100, 50.0f);
  const auto B_before = B.data;
  const float slope = 0.25f;
  const glm::vec2 texel_m{1.0f, 1.0f};
  const auto mask = carve_cavities(B, bedrock, 0.05f, slope, texel_m);
  double carved = 0.0;
  for (size_t i = 0; i < mask.data.size(); ++i) {
    const int xi = static_cast<int>(i % 100);
    const int yi = static_cast<int>(i / 100);
    const float cut = B_before[i] - B.data[i];
    if (!mask.data[i]) {
      REQUIRE(cut == 0.0f);  // non-mask cells untouched
      continue;
    }
    carved += 1.0;
    REQUIRE(cut >= 0.0f);  // carving only lowers
    const float d = brute_force_dist_to_non_mask(mask, xi, yi, texel_m);
    REQUIRE(cut == Catch::Approx(slope * d).margin(1e-3f));
  }
  // v1.3.1: this fixture's bedrock is just the flattened linear index
  // (monotonic in y*100+x), so the raw 5% quantile is exactly rows y=0..4
  // (499 cells: strict < excludes index 499 at (99,4)) -- and the bedrock minimum
  // sits at corner (0,0), the deliberately adversarial case for the border-margin
  // rim. The kBasinBorderMarginTexels=3 ring clips: rows y<3 are dropped entirely
  // (0.05 -> the top 2 surviving rows), and within those 2 rows, x<3 and x>=97
  // are also dropped -- 2 rows * 94 cols = 188 cells / 10000.
  REQUIRE(carved / mask.data.size() == Catch::Approx(0.0188).margin(1e-4));
}

TEST_CASE("carve_cavities: single circular basin center depth ~= slope * radius") {
  // bedrock = radial distance (in texels) from grid center -> the bottom
  // quantile is a disk. frac = 193/1680 puts t_lake at EXACTLY 8.0 texels
  // (four axis-aligned cells at (0,+-8),(+-8,0) sit exactly on that radius,
  // calibrated offline against this fixture: 41x41 grid, center (20,20)), so
  // the mask is precisely {r < 8} and the nearest non-mask cell to the
  // center is exactly 8 texels away.
  constexpr int n = 41, cx = 20, cy = 20;
  constexpr float texel = 2.0f;
  Field2D<float> bedrock(n, n);
  for (int y = 0; y < n; ++y)
    for (int x = 0; x < n; ++x) {
      const float dx = static_cast<float>(x - cx), dy = static_cast<float>(y - cy);
      bedrock.at(x, y) = std::sqrt(dx * dx + dy * dy);
    }
  Field2D<float> B(n, n, 0.0f);
  const float slope = 0.25f;
  const auto mask = carve_cavities(B, bedrock, 0.115f, slope, {texel, texel});
  REQUIRE(mask.at(cx, cy) == 1);
  const float expected = slope * 8.0f * texel;
  const float margin = slope * texel;  // one texel of radial uncertainty
  REQUIRE(-B.at(cx, cy) == Catch::Approx(expected).margin(margin));
}

TEST_CASE("carve_cavities: deterministic across repeated calls") {
  Field2D<float> bedrock(60, 60);
  for (int y = 0; y < 60; ++y)
    for (int x = 0; x < 60; ++x)
      bedrock.at(x, y) = (y * 60 + x) / 3599.0f;
  Field2D<float> B1(60, 60, 20.0f);
  Field2D<float> B2(60, 60, 20.0f);
  const auto mask1 = carve_cavities(B1, bedrock, 0.05f, 0.25f, {1.5f, 1.5f});
  const auto mask2 = carve_cavities(B2, bedrock, 0.05f, 0.25f, {1.5f, 1.5f});
  REQUIRE(mask1.data == mask2.data);
  REQUIRE(B1.data == B2.data);
}

TEST_CASE("carve_cavities: lake_frac > 1 clamped to 1, no crash") {
  // bedrock ramp 0..1 over 50x50; lake_frac = 2.0f should clamp to 1.0f
  Field2D<float> bedrock(50, 50);
  for (int y = 0; y < 50; ++y)
    for (int x = 0; x < 50; ++x)
      bedrock.at(x, y) = (y * 50 + x) / 2499.0f;
  Field2D<float> B(50, 50, 100.0f);
  const auto B_before = B.data;
  const auto mask = carve_cavities(B, bedrock, 2.0f, 0.25f, {1.0f, 1.0f});  // lake_frac > 1
  double carved = 0.0;
  for (size_t i = 0; i < mask.data.size(); ++i) {
    if (mask.data[i]) carved += 1.0;
    const float cut = B_before[i] - B.data[i];
    REQUIRE(cut >= 0.0f);  // only lowers
  }
  const double coverage = carved / mask.data.size();
  // v1.3.1: the kBasinBorderMarginTexels rim is excluded from the mask
  // regardless of quantile, so full coverage now tops out at interior / total,
  // not ~1.0. Interior is (50 - 2*margin)^2; the global max cell at (49,49) sits
  // in the border ring so there's no extra exclusion beyond the margin.
  const int interior = (50 - 2 * kBasinBorderMarginTexels) * (50 - 2 * kBasinBorderMarginTexels);
  const double expected_max = interior / 2500.0;
  REQUIRE(coverage == Catch::Approx(expected_max).margin(1e-4));
}

TEST_CASE("carve_cavities: v1.3.1 — mask never touches the border margin ring") {
  // Same adversarial ramp fixture as the coverage/EDT-oracle test above: the
  // bedrock minimum sits exactly at corner (0,0), so pre-fix the bottom-5%
  // quantile mask includes border cells there. RED pre-fix: mask.at(0,0) (and
  // its neighborhood) is 1.
  Field2D<float> bedrock(100, 100);
  for (int y = 0; y < 100; ++y)
    for (int x = 0; x < 100; ++x)
      bedrock.at(x, y) = (y * 100 + x) / 9999.0f;
  Field2D<float> B(100, 100, 50.0f);
  const auto mask = carve_cavities(B, bedrock, 0.05f, 0.25f, {1.0f, 1.0f});

  int carved = 0;
  for (int y = 0; y < 100; ++y) {
    for (int x = 0; x < 100; ++x) {
      const bool in_margin = x < kBasinBorderMarginTexels || y < kBasinBorderMarginTexels ||
                              x >= 100 - kBasinBorderMarginTexels ||
                              y >= 100 - kBasinBorderMarginTexels;
      if (in_margin) {
        REQUIRE(mask.at(x, y) == 0);
      } else if (mask.at(x, y)) {
        ++carved;
      }
    }
  }
  REQUIRE(carved > 0);  // interior basin still carved
}

TEST_CASE("carve_cavities: v1.3.1 — boundary basin holds water instead of "
          "draining through the border") {
  // bedrock = distance from the middle of the LEFT edge -> the bottom
  // quantile is a basin centered ON x=0, touching the grid border by
  // construction. Pre-fix, the basin's deepest point (its geometric center,
  // (0, 20)) IS a border cell, so the priority-flood seeds the flood at the
  // bowl floor and the whole bowl drains dry through it. Post-fix, the
  // uncarved kBasinBorderMarginTexels rim (including the x=0 column) keeps
  // its original (uncarved) height, forming a dam the flood can't breach.
  constexpr int n = 40;
  Field2D<float> bedrock(n, n);
  for (int y = 0; y < n; ++y)
    for (int x = 0; x < n; ++x) {
      const float dx = static_cast<float>(x), dy = static_cast<float>(y) - 20.0f;
      bedrock.at(x, y) = std::sqrt(dx * dx + dy * dy);
    }
  Field2D<float> B(n, n, 0.0f);  // flat plate: any carve is a genuine local low
  const float slope = 0.5f;
  const float texel = 1.0f;
  const auto mask = carve_cavities(B, bedrock, 0.1f, slope, {texel, texel});

  // Sanity: the RAW quantile (pre-margin-clip) does reach the border --
  // otherwise this fixture wouldn't exercise the regression at all. Recompute
  // the same quantile threshold carve_cavities uses internally, independent
  // of its (now margin-clipped) returned mask.
  {
    std::vector<float> v = bedrock.data;
    const size_t n2 = v.size();
    const size_t i_lake = static_cast<size_t>(0.1f * (n2 - 1));
    std::nth_element(v.begin(), v.begin() + i_lake, v.end());
    REQUIRE(bedrock.at(0, 20) < v[i_lake]);  // basin center is in the raw quantile
  }

  const auto routing = route_flow(B, texel, kEpsilonM);
  int deep_wet = 0;
  for (int i = 0; i < n * n; ++i) {
    if (!mask.data[i]) continue;
    if (!routing.in_lake[i]) continue;
    const float fill = routing.water_level[i] - B.data[i];
    if (fill > 0.5f) ++deep_wet;
  }
  REQUIRE(deep_wet > 0);  // the rim holds water where pre-fix the flood drained it
}

TEST_CASE("carve_cavities: v1.3.1 — rim exclusion is deterministic") {
  constexpr int n = 40;
  Field2D<float> bedrock(n, n);
  for (int y = 0; y < n; ++y)
    for (int x = 0; x < n; ++x) {
      const float dx = static_cast<float>(x), dy = static_cast<float>(y) - 20.0f;
      bedrock.at(x, y) = std::sqrt(dx * dx + dy * dy);
    }
  Field2D<float> B1(n, n, 0.0f);
  Field2D<float> B2(n, n, 0.0f);
  const auto mask1 = carve_cavities(B1, bedrock, 0.1f, 0.5f, {1.0f, 1.0f});
  const auto mask2 = carve_cavities(B2, bedrock, 0.1f, 0.5f, {1.0f, 1.0f});
  REQUIRE(mask1.data == mask2.data);
  REQUIRE(B1.data == B2.data);
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

TEST_CASE("incise: bare-bedrock erodes at k_bedrock rate (no double rescale)") {
  // 8-cell hand-built chain: receiver i-1, base level at 0. S=0 everywhere
  // (bare bedrock throughout), dry (water_level == ground). k_bedrock and
  // k_sediment are set far apart so an errant k_bedrock/k_sediment rescale
  // on a bare-bedrock cell is loudly wrong (~10x too slow).
  const int n = 8;
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
  p.k_bedrock = 0.05f;
  p.k_sediment = 0.5f;
  p.dt = 1.0f;

  // Independent reference in double precision: the implicit update at the
  // bedrock rate, CHAINED through the already-updated (in this sweep)
  // receiver height — the water-level clamp applies only to flooded
  // (in_lake) receivers, so on dry ground z_rcv is simply the receiver's
  // current height, which upstream cells (processed earlier in r.order)
  // have already eroded this same call. Mirrors the Braun–Willett scheme:
  // walk r.order so the receiver is already updated.
  std::vector<double> href(n);
  for (int i = 0; i < n; ++i) href[i] = B.at(i, 0);
  for (int i = 1; i < n; ++i) {
    const double F = static_cast<double>(p.k_bedrock) *
                      std::sqrt(static_cast<double>(area.at(i, 0))) *
                      static_cast<double>(p.dt) / 1.0;
    const double z_rcv = href[i - 1];  // dry: chain through the updated receiver
    href[i] = (href[i] + F * z_rcv) / (1.0 + F);
  }

  incise(B, S, r, area, p, 1.0f);
  for (int i = 0; i < n; ++i) {
    const double rel = std::abs(static_cast<double>(B.at(i, 0)) - href[i]) /
                        std::max(1.0, std::abs(href[i]));
    REQUIRE(rel < 1e-5);
  }
}

TEST_CASE("incise: erodes toward the effective lake water level, not the lake floor") {
  // 3-cell hand-built chain: 2 (donor) -> 1 (in_lake, ground=2, water_level=6)
  // -> 0 (base). Cell 1 must be skipped entirely (in_lake); cell 2 must
  // erode toward the water surface (6), never below it, and never all the
  // way down to the (unmodified) lake-floor ground level (2).
  FlowRouting r;
  r.width = 3; r.height = 1;
  r.receiver = {-1, 0, 1};
  r.in_lake = {0, 1, 0};
  r.water_level = {0.0f, 6.0f, 0.0f};
  r.order = {0, 1, 2};
  Field2D<float> B(3, 1), S(3, 1, 0.0f);
  B.at(0, 0) = 0.0f;
  B.at(1, 0) = 2.0f;   // lake floor, well below its own water_level (6)
  B.at(2, 0) = 10.0f;  // donor, high above the lake surface
  Field2D<float> area(3, 1, 1.0f);
  ErosionParams p;
  p.k_bedrock = 5.0f;  // strong: push hard toward the effective receiver level
  p.k_sediment = 5.0f;
  p.dt = 1.0f;

  const auto eroded = incise(B, S, r, area, p, 1.0f);

  REQUIRE(eroded.at(1, 0) == 0.0f);          // in_lake: skipped entirely
  REQUIRE(B.at(1, 0) == 2.0f);               // lake floor untouched
  REQUIRE(S.at(1, 0) == 0.0f);
  REQUIRE(B.at(2, 0) + S.at(2, 0) >= 6.0f);  // never eroded below the water surface
  REQUIRE(B.at(2, 0) < 10.0f);               // but it did erode
}

TEST_CASE("incise: diagonal receiver uses d = texel_m * sqrt(2)") {
  ErosionParams p;
  p.k_bedrock = 0.02f;
  p.k_sediment = 0.02f;
  p.dt = 1.0f;
  const float texel_m = 2.0f;  // non-trivial value so cardinal/diagonal differ clearly
  const float area_val = 9.0f;
  const float h_donor = 10.0f;

  // Cardinal: 2 cells in a row, donor -> base (dx=1, dy=0).
  {
    FlowRouting r;
    r.width = 2; r.height = 1;
    r.receiver = {-1, 0};
    r.in_lake = {0, 0};
    r.water_level = {0.0f, 0.0f};
    r.order = {0, 1};
    Field2D<float> B(2, 1), S(2, 1, 0.0f);
    B.at(1, 0) = h_donor;
    Field2D<float> area(2, 1, area_val);
    const auto eroded = incise(B, S, r, area, p, texel_m);

    const double d = texel_m;
    const double F = static_cast<double>(p.k_bedrock) * std::pow(static_cast<double>(area_val), static_cast<double>(p.m)) *
                      static_cast<double>(p.dt) / d;
    const double h_new = (h_donor + F * 0.0) / (1.0 + F);
    const double expected = h_donor - h_new;
    REQUIRE(static_cast<double>(eroded.at(1, 0)) == Catch::Approx(expected).epsilon(1e-5));
  }

  // Diagonal: 2x2 grid, donor (index 3, i.e. (1,1)) -> base (index 0): dx=1, dy=1.
  {
    FlowRouting r;
    r.width = 2; r.height = 2;
    r.receiver = {-1, -1, -1, 0};
    r.in_lake = {0, 0, 0, 0};
    r.water_level = {0.0f, 0.0f, 0.0f, 0.0f};
    r.order = {0, 1, 2, 3};
    Field2D<float> B(2, 2), S(2, 2, 0.0f);
    B.at(1, 1) = h_donor;
    Field2D<float> area(2, 2, area_val);
    const auto eroded = incise(B, S, r, area, p, texel_m);

    const double d = texel_m * std::sqrt(2.0);
    const double F = static_cast<double>(p.k_bedrock) * std::pow(static_cast<double>(area_val), static_cast<double>(p.m)) *
                      static_cast<double>(p.dt) / d;
    const double h_new = (h_donor + F * 0.0) / (1.0 + F);
    const double expected = h_donor - h_new;
    REQUIRE(static_cast<double>(eroded.at(1, 1)) == Catch::Approx(expected).epsilon(1e-5));
  }
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

TEST_CASE("deposit: lake cells fill at most to water level; conservation holds") {
  // 1D ramp with a flooded pocket: reuse the bowl idea in a row
  Field2D<float> B(16, 3);
  for (int y = 0; y < 3; ++y)
    for (int x = 0; x < 16; ++x) B.at(x, y) = static_cast<float>(x);
  B.at(4, 1) = 1.0f;  // pit below its neighbors (floods to ~5 via the rim at x=5)
  Field2D<float> S(16, 3, 0.0f);
  Field2D<float> h(16, 3);
  for (size_t i = 0; i < h.data.size(); ++i) h.data[i] = B.data[i] + S.data[i];
  const auto r = route_flow(h, 1.0f, 1e-4f);
  const auto area = accumulate_drainage(r, 1.0f);
  REQUIRE(r.in_lake[1 * 16 + 4] == 1);  // sanity: the pit is flooded

  Field2D<float> eroded(16, 3, 5.0f);  // increased artificial erosion to bind the cap
  ErosionParams p;
  p.deposition_g = 1.0f;
  const float S_before = 0.0f;
  const float exported = deposit(B, S, eroded, r, area, p, 1.0f);
  const int pit = 1 * 16 + 4;
  REQUIRE(S.data[pit] >= S_before);  // pit received sediment
  REQUIRE(B.data[pit] + S.data[pit] == Catch::Approx(r.water_level[pit]).margin(1e-4));  // filled EXACTLY to water level (cap binds)
  REQUIRE(B.data[pit] + S.data[pit] <= r.water_level[pit] + 1e-4f);  // never above water
  // conservation: total eroded volume = total deposited + exported
  double dep_total = 0.0, ero_total = 0.0;
  for (size_t i = 0; i < S.data.size(); ++i) dep_total += S.data[i];
  for (float e : eroded.data) ero_total += e;
  REQUIRE(dep_total + exported == Catch::Approx(ero_total).epsilon(0.01));
}

TEST_CASE("deposit: G=0 exports everything") {
  auto t = make_ramp(16, 0.0f);
  Field2D<float> eroded(16, 3, 0.2f);
  ErosionParams p;
  p.deposition_g = 0.0f;
  const auto S_before = t.S.data;
  const float exported = deposit(t.B, t.S, eroded, t.r, t.area, p, 1.0f);
  REQUIRE(t.S.data == S_before);  // nothing deposited anywhere (no flooded cells)
  REQUIRE(exported == Catch::Approx(16 * 3 * 0.2).epsilon(0.01));
}

namespace {
// 4-cell hand-built chain: idx0 dry base <- idx1 (shallow, depth 1) <- idx2
// (mid, depth 2) <- idx3 (deep, depth 3), all sharing water_level 10. Inflow
// is injected as idx3's own eroded volume (own eroded volume is bucketed
// exactly like q_in per the contract) so the whole component's capacity
// (3+2+1 = 6 m over the 1 m^2 texel) is known up front.
struct LadderLake {
  Field2D<float> B, S, area, eroded;
  FlowRouting r;
};
LadderLake make_ladder_lake(float inflow) {
  LadderLake t{Field2D<float>(4, 1), Field2D<float>(4, 1, 0.0f),
              Field2D<float>(4, 1, 1.0f), Field2D<float>(4, 1, 0.0f), {}};
  t.B.at(0, 0) = 0.0f;
  t.B.at(1, 0) = 9.0f;  // shallow: depth 1
  t.B.at(2, 0) = 8.0f;  // mid: depth 2
  t.B.at(3, 0) = 7.0f;  // deep: depth 3
  t.eroded.at(3, 0) = inflow;
  t.r.width = 4; t.r.height = 1;
  t.r.receiver = {-1, 0, 1, 2};
  t.r.in_lake = {0, 1, 1, 1};
  t.r.water_level = {0.0f, 10.0f, 10.0f, 10.0f};
  t.r.order = {0, 1, 2, 3};
  return t;
}
}  // namespace

TEST_CASE("deposit: lake pour is bottom-up — small inflow only reaches the deepest cell") {
  auto t = make_ladder_lake(0.3f);  // << the 1 m needed to raise deep to mid's level
  ErosionParams p;
  const float exported = deposit(t.B, t.S, t.eroded, t.r, t.area, p, 1.0f);
  REQUIRE(t.S.at(3, 0) == Catch::Approx(0.3f).margin(1e-5));  // deepest gains it all
  REQUIRE(t.S.at(2, 0) == 0.0f);                              // mid untouched
  REQUIRE(t.S.at(1, 0) == 0.0f);                              // shallow untouched
  REQUIRE(exported == Catch::Approx(0.0f).margin(1e-5));      // fully absorbed
}

TEST_CASE("deposit: lake pour is bottom-up — larger inflow equalizes the two deepest before the shallow gets any") {
  auto t = make_ladder_lake(2.5f);  // 1 m to reach mid's level, then 2 m more (2 members) to reach 8.75
  ErosionParams p;
  const float exported = deposit(t.B, t.S, t.eroded, t.r, t.area, p, 1.0f);
  REQUIRE(t.S.at(3, 0) == Catch::Approx(1.75f).margin(1e-5));  // deep: 7 -> 8.75
  REQUIRE(t.S.at(2, 0) == Catch::Approx(0.75f).margin(1e-5));  // mid:  8 -> 8.75
  REQUIRE(t.S.at(1, 0) == 0.0f);                                // shallow: untouched
  REQUIRE(exported == Catch::Approx(0.0f).margin(1e-5));
}

TEST_CASE("deposit: equal-depth lake spreads inflow evenly across members, not concentrated at the entry") {
  // 5-cell chain: idx0 dry base <- idx1 <- idx2 <- idx3 <- idx4, all four
  // lake members at the SAME ground height (5) and water_level (10).
  // Old entry-cell deposition piles sediment up near idx4 (the "entry" the
  // flux enters the sweep from); the pour must spread it flat instead.
  Field2D<float> B(5, 1), S(5, 1, 0.0f), area(5, 1, 1.0f), eroded(5, 1, 0.0f);
  B.at(0, 0) = 0.0f;
  for (int i = 1; i <= 4; ++i) B.at(i, 0) = 5.0f;
  eroded.at(4, 0) = 6.0f;  // well under the component's full capacity (4 * 5 = 20)
  FlowRouting r;
  r.width = 5; r.height = 1;
  r.receiver = {-1, 0, 1, 2, 3};
  r.in_lake = {0, 1, 1, 1, 1};
  r.water_level = {0.0f, 10.0f, 10.0f, 10.0f, 10.0f};
  r.order = {0, 1, 2, 3, 4};
  ErosionParams p;
  const float exported = deposit(B, S, eroded, r, area, p, 1.0f);
  float lo = 1e30f, hi = -1e30f;
  for (int i = 1; i <= 4; ++i) {
    lo = std::min(lo, S.at(i, 0));
    hi = std::max(hi, S.at(i, 0));
  }
  REQUIRE(hi - lo <= 1e-4f);                            // evenly spread
  REQUIRE(lo == Catch::Approx(1.5f).margin(1e-4));       // 6 m / 4 members
  REQUIRE(exported == Catch::Approx(0.0f).margin(1e-4));
}

namespace {
// Two 1-cell lake components chained by a dry connector: idx3 (lake A, deep,
// upstream) -> idx2 (dry connector) -> idx1 (lake B, downstream) -> idx0
// (dry base). G=0 so no dry cell ever deposits anything itself — isolates
// the cascade/merge bookkeeping from the ordinary dry rule's arithmetic.
struct CascadeLakes {
  Field2D<float> B, S, area, eroded;
  FlowRouting r;
};
CascadeLakes make_cascade() {
  CascadeLakes t{Field2D<float>(4, 1), Field2D<float>(4, 1, 0.0f),
                Field2D<float>(4, 1, 1.0f), Field2D<float>(4, 1, 0.0f), {}};
  t.B.at(0, 0) = 0.0f;    // dry base
  t.B.at(1, 0) = 7.0f;    // lake B floor (cap to 10 = 3 m)
  t.B.at(2, 0) = 50.0f;   // dry connector (height irrelevant with G=0)
  t.B.at(3, 0) = 13.0f;   // lake A floor (cap to 15 = 2 m)
  t.eroded.at(3, 0) = 10.0f;
  t.r.width = 4; t.r.height = 1;
  t.r.receiver = {-1, 0, 1, 2};
  t.r.in_lake = {0, 1, 0, 1};
  t.r.water_level = {0.0f, 10.0f, 50.0f, 15.0f};
  t.r.order = {0, 1, 2, 3};
  return t;
}
}  // namespace

TEST_CASE("deposit: an upstream lake's overflow cascades into a downstream lake before exporting") {
  auto t = make_cascade();
  ErosionParams p;
  p.deposition_g = 0.0f;
  const float exported = deposit(t.B, t.S, t.eroded, t.r, t.area, p, 1.0f);
  REQUIRE(t.S.at(3, 0) == Catch::Approx(2.0f).margin(1e-4));  // A: 13 -> 15 (cap)
  REQUIRE(t.S.at(1, 0) == Catch::Approx(3.0f).margin(1e-4));  // B: 7 -> 10 (cap)
  REQUIRE(t.S.at(2, 0) == 0.0f);                              // dry pass-through, G=0
  REQUIRE(t.S.at(0, 0) == 0.0f);
  REQUIRE(exported == Catch::Approx(5.0f).margin(1e-4));      // 10 - 2 - 3
  // conservation
  double dep_total = 0.0;
  for (float s : t.S.data) dep_total += s;
  REQUIRE(dep_total + exported == Catch::Approx(10.0).epsilon(1e-6));
}

TEST_CASE("deposit: lake pour is deterministic across repeated runs") {
  auto t1 = make_cascade();
  auto t2 = make_cascade();
  ErosionParams p;
  p.deposition_g = 0.0f;
  const float e1 = deposit(t1.B, t1.S, t1.eroded, t1.r, t1.area, p, 1.0f);
  const float e2 = deposit(t2.B, t2.S, t2.eroded, t2.r, t2.area, p, 1.0f);
  REQUIRE(e1 == e2);
  REQUIRE(t1.S.data == t2.S.data);
  REQUIRE(t1.B.data == t2.B.data);
}

TEST_CASE("diffuse: smooths a spike, conserves interior mass, respects layers") {
  Field2D<float> B(9, 9, 10.0f);
  Field2D<float> S(9, 9, 0.0f);
  S.at(4, 4) = 8.0f;  // sediment spike
  ErosionParams p;
  p.diffusion = 2.0f;  // deliberately > stability bound at dt=1 -> must sub-step
  p.dt = 1.0f;
  double mass_before = 0.0;
  for (size_t i = 0; i < S.data.size(); ++i) mass_before += B.data[i] + S.data[i];
  diffuse(B, S, p, 1.0f);
  REQUIRE(S.at(4, 4) < 8.0f);          // spike lowered
  REQUIRE(S.at(3, 4) > 0.0f);          // neighbors received sediment
  for (float v : S.data) REQUIRE(v >= 0.0f);
  REQUIRE(std::isfinite(S.at(4, 4)));  // sub-stepping kept it stable
  double mass_after = 0.0;
  for (size_t i = 0; i < S.data.size(); ++i) mass_after += B.data[i] + S.data[i];
  // some diffusion toward boundary is expected; allow 0.2% relative tolerance
  REQUIRE(mass_after == Catch::Approx(mass_before).epsilon(0.002));
  // bedrock at the spike was never touched (only its sediment moved)
  REQUIRE(B.at(4, 4) == 10.0f);
}

namespace {
// Hill world: 33x33 cone sloping DOWN to the border (the drain), with a deep
// pocket punched into the summit. The pocket floods to its own rim and spills
// downhill — a genuine cavity lake. (A raised rim at the border would instead
// flood the whole map: the border is base level.)
struct BowlWorld {
  Field2D<float> B, S;
};
BowlWorld make_bowl() {
  BowlWorld t{Field2D<float>(33, 33), Field2D<float>(33, 33, 0.0f)};
  for (int y = 0; y < 33; ++y)
    for (int x = 0; x < 33; ++x) {
      const float dx = x - 16.0f, dy = y - 16.0f;
      const float rad = std::sqrt(dx * dx + dy * dy);
      t.B.at(x, y) = 30.0f - rad;               // hill: high center, low border
      if (rad < 4.0f) t.B.at(x, y) = -10.0f;    // deep summit pocket
    }
  return t;
}
}  // namespace

TEST_CASE("erode: cavity floods, per-lake level uniform, W >= 0") {
  auto t = make_bowl();
  ErosionParams p;
  p.iterations = 10;
  p.dump_every = 0;
  p.min_lake_area_m2 = 4.0f;
  p.min_lake_depth_m = 0.1f;
  const auto out = erode(t.B, t.S, p, 1.0f, nullptr);
  float level_min = 1e30f, level_max = -1e30f;
  int wet = 0;
  for (int y = 0; y < 33; ++y)
    for (int x = 0; x < 33; ++x) {
      const float w = out.water_depth.at(x, y);
      REQUIRE(w >= 0.0f);
      if (w > 0.0f) {
        ++wet;
        const float lvl = t.B.at(x, y) + t.S.at(x, y) + w;
        level_min = std::min(level_min, lvl);
        level_max = std::max(level_max, lvl);
      }
    }
  REQUIRE(wet > 10);                                  // the cavity holds a lake
  REQUIRE(level_max - level_min < 0.05f);             // one flat surface
}

TEST_CASE("erode: pruning removes puddles") {
  auto t = make_bowl();
  ErosionParams p;
  p.iterations = 10;
  p.dump_every = 0;
  p.min_lake_area_m2 = 1e6f;  // nothing can qualify
  const auto out = erode(t.B, t.S, p, 1.0f, nullptr);
  for (float w : out.water_depth.data) REQUIRE(w == 0.0f);
}

TEST_CASE("erode: deterministic, and the sink sees the loop film strip") {
  struct CountingSink : MapDebugSink {
    int floats = 0, masks = 0;
    void dump(std::string_view, int, const Field2D<float>&) override { ++floats; }
    void dump(std::string_view, int, const Field2D<uint8_t>&) override { ++masks; }
  };
  auto t1 = make_bowl();
  auto t2 = make_bowl();
  ErosionParams p;
  p.iterations = 4;
  p.dump_every = 2;  // dumps after iterations 2 and 4
  CountingSink sink;
  const auto o1 = erode(t1.B, t1.S, p, 1.0f, &sink);
  const auto o2 = erode(t2.B, t2.S, p, 1.0f, nullptr);
  REQUIRE(o1.water_depth.data == o2.water_depth.data);
  REQUIRE(o1.flow.data == o2.flow.data);
  REQUIRE(t1.B.data == t2.B.data);
  REQUIRE(t1.S.data == t2.S.data);
  REQUIRE(sink.floats == 2 * 3);  // height+flow+sediment × 2 dumps
  REQUIRE(sink.masks == 2 * 1);   // lakes mask × 2 dumps
}

namespace {
// 1D ramp world (like make_ramp: one interior row, y=1, borders y=0/y=2 —
// avoids spurious flats from a y-invariant field, which route_flow also
// flags in_lake per its documented flat-leveling behavior) with a
// flat-bottomed pit whose floor sits `depth_m` below the pit's lowest
// boundary (the west rim, since the ramp rises with x): a genuine,
// uniformly-deep closed depression, independent of route_flow's own
// semantics so the expected fill volume (width * depth_m) is knowable up
// front.
struct PitWorld {
  Field2D<float> B, S;
  int x0, x1;  // pit footprint [x0,x1) on row y=1, interior to the grid
};
PitWorld make_pit(float depth_m) {
  PitWorld t{Field2D<float>(25, 3), Field2D<float>(25, 3, 0.0f), 10, 15};
  for (int y = 0; y < 3; ++y)
    for (int x = 0; x < 25; ++x) t.B.at(x, y) = 0.05f * static_cast<float>(x);
  const float west_rim = 0.05f * static_cast<float>(t.x0 - 1);
  for (int x = t.x0; x < t.x1; ++x) t.B.at(x, 1) = west_rim - depth_m;
  return t;
}
}  // namespace

TEST_CASE("micro_fill: shallow bowl fills fully, no residual in_lake, volume ~= bowl volume") {
  auto t = make_pit(0.5f);
  Field2D<uint8_t> basins(25, 3, 0);
  const float expected_volume = static_cast<float>(t.x1 - t.x0) * 0.5f;

  const float filled = micro_fill(t.B, t.S, basins, 1.0f);
  REQUIRE(filled == Catch::Approx(expected_volume).epsilon(0.02));

  Field2D<float> h(25, 3);
  for (size_t i = 0; i < h.data.size(); ++i) h.data[i] = t.B.data[i] + t.S.data[i];
  const auto r = route_flow(h, 1.0f, 1e-4f);
  // fully drained: any surviving in_lake flag is float-rounding noise (the
  // fill matches route_flow's own epsilon-cascaded water_level to a ULP),
  // not a real residual depression, so bound the depth, not the flag.
  float max_residual_depth = 0.0f;
  for (size_t i = 0; i < r.in_lake.size(); ++i)
    if (r.in_lake[i])
      max_residual_depth = std::max(max_residual_depth, r.water_level[i] - h.data[i]);
  REQUIRE(max_residual_depth < 1e-5f);
}

TEST_CASE("micro_fill: deep pocket left untouched") {
  auto t = make_pit(5.0f);
  Field2D<uint8_t> basins(25, 3, 0);
  const auto S_before = t.S.data;

  const float filled = micro_fill(t.B, t.S, basins, 1.0f);

  REQUIRE(filled == Catch::Approx(0.0f).margin(1e-4));
  REQUIRE(t.S.data == S_before);
}

TEST_CASE("micro_fill: component touching basin_mask untouched even when shallow") {
  auto t = make_pit(0.5f);
  Field2D<uint8_t> basins(25, 3, 0);
  basins.at(t.x0, 1) = 1;  // one pit member seeded as a cavity
  const auto S_before = t.S.data;

  const float filled = micro_fill(t.B, t.S, basins, 1.0f);

  REQUIRE(filled == Catch::Approx(0.0f).margin(1e-4));
  REQUIRE(t.S.data == S_before);
}

TEST_CASE("micro_fill: deterministic") {
  auto t1 = make_pit(0.5f);
  auto t2 = make_pit(0.5f);
  Field2D<uint8_t> basins(25, 3, 0);

  const float f1 = micro_fill(t1.B, t1.S, basins, 1.0f);
  const float f2 = micro_fill(t2.B, t2.S, basins, 1.0f);

  REQUIRE(f1 == f2);
  REQUIRE(t1.S.data == t2.S.data);
  REQUIRE(t1.B.data == t2.B.data);
}

TEST_CASE("resample_bilinear: identity when grids coincide, linear in between") {
  Field2D<float> src(8, 8);
  for (int y = 0; y < 8; ++y)
    for (int x = 0; x < 8; ++x) src.at(x, y) = static_cast<float>(x);  // ramp
  // same grid: identity
  const auto same = resample_bilinear(src, 1.0f, 0.0f, 8, 1.0f);
  REQUIRE(same.data == src.data);
  // 2x finer: midpoints are averages of the ramp -> still linear in world x
  const auto fine = resample_bilinear(src, 1.0f, 0.0f, 16, 0.5f);
  REQUIRE(fine.at(3, 4) == Catch::Approx(1.5f));  // world x = 1.5
  // origin shift: src texel 0 sits at world -2 -> world 0 is src coord 2
  const auto shifted = resample_bilinear(src, 1.0f, -2.0f, 8, 1.0f);
  REQUIRE(shifted.at(0, 0) == Catch::Approx(2.0f));
}


TEST_CASE("generate_map: debug sink sees the full stage sequence") {
  struct Recorder : MapDebugSink {
    std::vector<std::string> stages;
    void dump(std::string_view s, int, const Field2D<float>&) override {
      stages.emplace_back(s);
    }
    void dump(std::string_view s, int, const Field2D<uint8_t>&) override {
      stages.emplace_back(s);
    }
  };
  MapGenParams p;
  p.resolution = 48;
  p.world_size_m = 192.0f;
  p.erosion.sim_resolution = 48;
  p.erosion.iterations = 2;
  p.erosion.dump_every = 1;
  Recorder rec;
  generate_map(p, &rec);
  const std::vector<std::string> expected = {
      "bedrock", "biome-sim", "cone", "cavities", "cavities-height",
      "canals", "sediment-init", "micro-fill",
      "loop-height", "loop-flow", "loop-sediment", "loop-lakes",
      "loop-height", "loop-flow", "loop-sediment", "loop-lakes",
      "water", "detail-delta", "river", "final-height", "biome"};
  REQUIRE(rec.stages == expected);
}

TEST_CASE("finalize_lakes: freeboard leaves a dry bank inside the carved bowl") {
  // Filling to the brim leaves no coast: carve_cavities makes the basin cone
  // zero exactly at the mask boundary and priority-flood then floods right up
  // to it. The freeboard lowers the OUTPUT level below the spill so a band of
  // already-carved bowl stays dry, which is what lets the Lake biome cover
  // only water.
  auto run_with = [](float freeboard_m, float frac) {
    auto t = make_bowl();
    ErosionParams p;
    p.iterations = 10;
    p.dump_every = 0;
    p.min_lake_area_m2 = 4.0f;
    p.min_lake_depth_m = 0.1f;
    p.lake_freeboard_m = freeboard_m;
    p.lake_freeboard_frac = frac;
    const auto out = erode(t.B, t.S, p, 1.0f, nullptr);
    int wet = 0;
    float deepest = 0.0f, surface = -1e30f;
    for (size_t i = 0; i < out.water_depth.data.size(); ++i) {
      const float w = out.water_depth.data[i];
      if (w <= 0.0f) continue;
      ++wet;
      deepest = std::max(deepest, w);
      surface = std::max(surface, t.B.data[i] + t.S.data[i] + w);
    }
    struct R { int wet; float deepest, surface; };
    return R{wet, deepest, surface};
  };

  const auto brim = run_with(0.0f, 0.0f);
  const auto banked = run_with(0.4f, 0.25f);

  REQUIRE(brim.wet > 0);
  REQUIRE(banked.wet > 0);
  // A dry bank exists: fewer wet cells, and the water surface sits strictly
  // lower than when filled to the spill point.
  REQUIRE(banked.wet < brim.wet);
  REQUIRE(banked.surface < brim.surface);
  // The lake is shallower by the freeboard, but the fractional cap keeps it
  // from being drained away.
  REQUIRE(banked.deepest < brim.deepest);
  REQUIRE(banked.deepest > 0.5f * brim.deepest);
}

namespace {
// A conical bowl set into ground that slopes gently toward +x, so water can
// LEAVE. Getting this wrong is easy: if the surroundings rise away from the
// bowl the whole map is one depression, the flood level is set by the map
// border rather than the bowl's rim, and the notch has nothing to lower
// relative to.
struct NotchWorld {
  Field2D<float> B;
  Field2D<uint8_t> mask;
};
// `drain_slope` is squeezed from both sides, and both bounds bite:
//   - too GENTLE and the notch channel cannot reach naturally lower ground
//     within kMaxNotchSteps (at 0.02 m/m a 1 m notch needs a 50-texel trench);
//   - too STEEP and the bowl is not a closed depression at all, because the
//     base drops more across its diameter than the cone is deep. That needs
//     drain_slope < cone_slope / 2, i.e. under 0.25 here — at 0.3 the "bowl"
//     simply drained through and the notch had nothing to lower.
// 0.15 sits in the window: 7 m of cone against 4.2 m of base drop, and a 2 m
// notch completes in ~13 steps.
NotchWorld make_bowl_with_drain(int n, float radius, float cone_slope,
                                float drain_slope = 0.15f) {
  NotchWorld t{Field2D<float>(n, n, 0.0f), Field2D<uint8_t>(n, n, 0)};
  const float cx = n * 0.5f, cy = n * 0.5f;
  for (int y = 0; y < n; ++y)
    for (int x = 0; x < n; ++x) {
      t.B.at(x, y) = -drain_slope * static_cast<float>(x);  // drains toward +x
      const float dx = x - cx, dy = y - cy;
      const float rad = std::sqrt(dx * dx + dy * dy);
      if (rad < radius) {
        t.mask.at(x, y) = 1;
        t.B.at(x, y) -= cone_slope * (radius - rad);
      }
    }
  return t;
}
}  // namespace

TEST_CASE("carve_outlet_notches: the spill drops by the notch, exposing a bank") {
  // A bowl fills to its lowest rim point, so carve_cavities' cone — zero
  // exactly at the mask boundary — leaves no bank at all. The notch lowers one
  // rim cell so the water stops below the rim, and the band above it is coast.
  auto build = [](float notch_depth) {
    auto t = make_bowl_with_drain(31, 8.0f, 0.5f);
    if (notch_depth > 0.0f) carve_outlet_notches(t.B, t.mask, notch_depth);
    const auto r = route_flow(t.B, 1.0f, kEpsilonM);
    float level = 1e30f;
    int wet = 0;
    for (size_t i = 0; i < t.B.data.size(); ++i)
      if (r.in_lake[i] && t.mask.data[i]) {
        level = std::min(level, r.water_level[i]);
        ++wet;
      }
    struct R { float level; int wet; };
    return R{level, wet};
  };

  const auto brim = build(0.0f);
  const auto notched = build(1.0f);

  REQUIRE(brim.wet > 0);
  REQUIRE(notched.wet > 0);
  // The spill level drops by the notch depth: the epsilon tilt and the
  // one-texel neighbourhood shift it by a fraction of a texel at most.
  REQUIRE(notched.level == Catch::Approx(brim.level - 1.0f).margin(0.05));
  // and that leaves a dry band — fewer basin cells hold water than before
  REQUIRE(notched.wet < brim.wet);
}

TEST_CASE("carve_outlet_notches: the level drops by exactly the notch depth") {
  // The crisp invariant. Bank AREA is not the thing to pin: the dry region is
  // an annulus bounded by the basin itself (616 cells here), so it saturates —
  // measured 280 / 394 / 483 / 540 for notches of 0 / 1 / 2 / 3 m. The level
  // relationship stays exact at any depth, and the bank follows from it via
  // the cone slope.
  auto measure = [](float notch_depth) {
    auto t = make_bowl_with_drain(41, 14.0f, 0.5f);
    if (notch_depth > 0.0f) carve_outlet_notches(t.B, t.mask, notch_depth);
    const auto r = route_flow(t.B, 1.0f, kEpsilonM);
    float level = 1e30f;
    int dry = 0;
    for (size_t i = 0; i < t.B.data.size(); ++i)
      if (t.mask.data[i]) {
        if (r.in_lake[i]) level = std::min(level, r.water_level[i]);
        else ++dry;
      }
    struct R { float level; int dry; };
    return R{level, dry};
  };

  const auto base = measure(0.0f);
  int prev_dry = base.dry;
  for (const float d : {1.0f, 2.0f, 3.0f}) {
    const auto m = measure(d);
    INFO("notch " << d << " m");
    REQUIRE(m.level == Catch::Approx(base.level - d).margin(0.02));
    REQUIRE(m.dry > prev_dry);  // strictly more bank each time, though sublinear
    prev_dry = m.dry;
  }
}

TEST_CASE("erode: the lake tag steers routing away from epsilon flats") {
  // L5. Without a tag, route_flow excludes every in_lake cell from steepest
  // descent — and on level ground the flood front always arrives above a
  // cell's own height, so `in_lake` flags every FLAT as well as every lake.
  // Passing the seeded basins lets erode() rebuild a tag each iteration from
  // whole components ponded deeper than kPondedMinDepthM, so flats route by
  // gradient while genuine lakes stay excluded.
  //
  // A bowl set into ground that drains, with a broad flat shelf around it: the
  // shelf is what a tagless run misroutes.
  const int n = 41;
  Field2D<float> B(n, n, 0.0f), S(n, n, 0.0f);
  Field2D<uint8_t> basins(n, n, 0);
  for (int y = 0; y < n; ++y)
    for (int x = 0; x < n; ++x) {
      const float dx = x - 20.0f, dy = y - 20.0f;
      const float rad = std::sqrt(dx * dx + dy * dy);
      B.at(x, y) = -0.02f * static_cast<float>(x);  // gentle drain toward +x
      if (rad < 6.0f) {
        basins.at(x, y) = 1;
        B.at(x, y) -= 0.5f * (6.0f - rad);
      }
    }

  ErosionParams p;
  p.iterations = 3;
  p.dump_every = 0;
  auto run = [&](bool with_tag) {
    Field2D<float> b = B, s = S;
    const auto out = erode(b, s, p, 1.0f, nullptr, with_tag ? &basins : nullptr);
    // How many cells realise the steepest D8 descent on the filled surface?
    static constexpr int DX[8] = {1, -1, 0, 0, 1, 1, -1, -1};
    static constexpr int DY[8] = {0, 0, 1, -1, 1, -1, 1, -1};
    const float diag = std::sqrt(2.0f);
    int total = 0, matched = 0;
    const auto& r = out.routing;
    for (int y = 1; y < n - 1; ++y)
      for (int x = 1; x < n - 1; ++x) {
        const size_t i = static_cast<size_t>(y) * n + x;
        if (r.receiver[i] < 0) continue;
        int best = -1;
        float bs = 0.0f;
        for (int k = 0; k < 8; ++k) {
          const size_t j = static_cast<size_t>(y + DY[k]) * n + (x + DX[k]);
          const float d = (DX[k] && DY[k]) ? diag : 1.0f;
          const float sl = (r.water_level[i] - r.water_level[j]) / d;
          if (sl > bs) { bs = sl; best = static_cast<int>(j); }
        }
        if (best < 0) continue;
        ++total;
        if (r.receiver[i] == best) ++matched;
      }
    return total > 0 ? static_cast<float>(matched) / static_cast<float>(total) : 0.0f;
  };

  const float untagged = run(false);
  const float tagged = run(true);
  INFO("steepest-descent share: untagged " << untagged << ", tagged " << tagged);
  REQUIRE(tagged > untagged);
}

TEST_CASE("finalize_lakes: seeded lakes survive pruning, emergent ones must earn it") {
  // L2. A seeded basin is a deliberate map feature, so it is a lake however
  // small or shallow it came out; an emergent pond has to clear the area and
  // depth thresholds. The distinction is provenance, not size — which is why
  // the same fixture is run with and without the basin mask.
  auto t = make_bowl();
  ErosionParams p;
  p.iterations = 6;
  p.dump_every = 0;
  p.min_lake_area_m2 = 1e6f;   // nothing can qualify on shape alone
  p.min_lake_depth_m = 1e6f;

  Field2D<uint8_t> basins(t.B.width, t.B.height, 0);
  for (int y = 12; y <= 20; ++y)
    for (int x = 12; x <= 20; ++x) basins.at(x, y) = 1;  // the summit pocket

  {  // no mask: thresholds prune everything
    auto b = t.B, s = t.S;
    const auto out = erode(b, s, p, 1.0f, nullptr, nullptr);
    for (float wd : out.water_depth.data) REQUIRE(wd == 0.0f);
    REQUIRE(out.lakes.empty());
  }
  {  // with the mask: the seeded basin is kept regardless
    auto b = t.B, s = t.S;
    const auto out = erode(b, s, p, 1.0f, nullptr, &basins);
    REQUIRE_FALSE(out.lakes.empty());
    bool any_seeded = false;
    for (const auto& l : out.lakes)
      if (l.kind == LakeKind::Seeded) any_seeded = true;
    REQUIRE(any_seeded);

    // lake_id and water_depth must agree cell for cell, and every id must
    // index a real record.
    int wet = 0;
    for (size_t i = 0; i < out.water_depth.data.size(); ++i) {
      const bool has_water = out.water_depth.data[i] > 0.0f;
      REQUIRE(has_water == (out.lake_id.data[i] >= 0));
      if (!has_water) continue;
      ++wet;
      REQUIRE(static_cast<size_t>(out.lake_id.data[i]) < out.lakes.size());
    }
    REQUIRE(wet > 0);

    for (const auto& l : out.lakes) {
      REQUIRE(l.area_m2 > 0.0f);
      REQUIRE(l.max_depth_m > 0.0f);
      // The sill is a real cell OUTSIDE the lake — that is what makes it an
      // outlet rather than a member.
      if (l.outlet_cell >= 0) {
        REQUIRE(static_cast<size_t>(l.outlet_cell) < out.water_depth.data.size());
        REQUIRE(out.water_depth.data[static_cast<size_t>(l.outlet_cell)] == 0.0f);
      }
    }
  }
}
