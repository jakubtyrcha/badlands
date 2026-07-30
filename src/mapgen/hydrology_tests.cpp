#include <catch_amalgamated.hpp>
#include <algorithm>
#include <cmath>
#include <vector>
#include "mapgen/hydrology.hpp"

using namespace badlands::mapgen;

namespace {
Field2D<float> tilted_plane(int w, int h, float dz_per_col) {
  Field2D<float> f(w, h);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) f.at(x, y) = x * dz_per_col;
  return f;
}

constexpr int kDx8[8] = {1, -1, 0, 0, 1, 1, -1, -1};
constexpr int kDy8[8] = {0, 0, 1, -1, 1, -1, 1, -1};

// A plane whose gradient points along (cos a, sin a) with magnitude `slope`
// (m per m of WORLD distance). Sampled at world = texel index * texel_m, so
// the plane's heading is independent of the grid resolution.
Field2D<float> make_plane(int n, float angle_rad, float slope, float texel_m) {
  Field2D<float> f(n, n);
  const float cx = std::cos(angle_rad), sy = std::sin(angle_rad);
  for (int y = 0; y < n; ++y)
    for (int x = 0; x < n; ++x)
      f.at(x, y) = slope * (static_cast<float>(x) * texel_m * cx +
                            static_cast<float>(y) * texel_m * sy);
  return f;
}

// The steepest D8 descent available at (x, y): max over neighbours of
// (drop / horizontal distance). Diagonals are sqrt(2) further, which is
// exactly the distance term priority-flood's claim order omits.
float max_descent_slope(const Field2D<float>& h, int x, int y, float texel_m) {
  const float diag = texel_m * std::sqrt(2.0f);
  float best = 0.0f;
  for (int k = 0; k < 8; ++k) {
    const int nx = x + kDx8[k], ny = y + kDy8[k];
    if (!h.in_bounds(nx, ny)) continue;
    const float d = (kDx8[k] != 0 && kDy8[k] != 0) ? diag : texel_m;
    best = std::max(best, (h.at(x, y) - h.at(nx, ny)) / d);
  }
  return best;
}

// The descent slope actually realised by the receiver the router picked.
float chosen_descent_slope(const Field2D<float>& h, const FlowRouting& r, int i,
                           float texel_m) {
  const int32_t rcv = r.receiver[i];
  if (rcv < 0) return 0.0f;
  const int x = i % r.width, y = i / r.width;
  const int rx = rcv % r.width, ry = rcv / r.width;
  const bool diagonal = (rx != x) && (ry != y);
  const float d = diagonal ? texel_m * std::sqrt(2.0f) : texel_m;
  return (h.data[i] - h.data[rcv]) / d;
}

bool receiver_is_diagonal(const FlowRouting& r, int i) {
  const int32_t rcv = r.receiver[i];
  if (rcv < 0) return false;
  return (rcv % r.width != i % r.width) && (rcv / r.width != i / r.width);
}
}  // namespace

// The Defect A regression. route_flow used to assign the priority-flood claim
// parent as the receiver, which ranks candidates by ABSOLUTE elevation and
// ignores how far away the neighbour is. Because a diagonal drops by
// |cos|+|sin| against the best orthogonal's max(|cos|,|sin|), the diagonal was
// always at least as low and always won — measured at 100% diagonal on every
// tilted plane, and 96.1% on the production sim.
TEST_CASE("route_flow: receivers realise the steepest D8 descent on a plane") {
  const float texel = 1.0f, slope = 0.05f;
  const int n = 48;
  // 22.5 deg is the exact tie angle: cos+sin == sqrt(2)*cos(22.5), so the
  // orthogonal and diagonal descents are equally steep there. Asserting the
  // receiver ACHIEVES the maximum slope (rather than equals one specific cell)
  // is therefore the tie-proof formulation.
  for (const float deg : {0.0f, 15.0f, 22.5f, 30.0f, 45.0f, 60.0f, 90.0f}) {
    const auto h = make_plane(n, deg * 3.14159265358979f / 180.0f, slope, texel);
    const auto r = route_flow(h, texel, 1e-4f);
    INFO("plane tilted at " << deg << " deg");
    for (int y = 1; y < n - 1; ++y)
      for (int x = 1; x < n - 1; ++x) {
        const int i = y * n + x;
        REQUIRE(r.in_lake[i] == 0);  // a plane has no depressions
        REQUIRE(r.receiver[i] >= 0);
        REQUIRE(chosen_descent_slope(h, r, i, texel) ==
                Catch::Approx(max_descent_slope(h, x, y, texel)).epsilon(1e-5));
      }
  }
}

TEST_CASE("route_flow: an axis-aligned plane drains orthogonally, not diagonally") {
  // At 0 and 15 deg the orthogonal neighbour strictly wins on drop/distance
  // (1.0 vs 0.707 at 0 deg; 0.966 vs 0.866 at 15 deg), so there is no tie to
  // hide behind: every interior receiver must be orthogonal. The old
  // flood-parent rule produced 100% diagonal here.
  const float texel = 1.0f;
  const int n = 48;
  for (const float deg : {0.0f, 15.0f}) {
    const auto h = make_plane(n, deg * 3.14159265358979f / 180.0f, 0.05f, texel);
    const auto r = route_flow(h, texel, 1e-4f);
    INFO("plane tilted at " << deg << " deg");
    for (int y = 1; y < n - 1; ++y)
      for (int x = 1; x < n - 1; ++x)
        REQUIRE_FALSE(receiver_is_diagonal(r, y * n + x));
  }
}

TEST_CASE("route_flow: receivers strictly descend the filled surface, so the "
          "graph is acyclic and `order` stays topological") {
  // Three consumers (incise, deposit, accumulate_drainage) walk `order` and
  // rely on a receiver being processed before its donors. That holds iff
  // receivers strictly decrease the filled surface hf = water_level.
  auto check = [](const Field2D<float>& h) {
    const auto r = route_flow(h, 1.0f, 1e-4f);
    std::vector<int> pop_index(r.receiver.size(), -1);
    for (size_t k = 0; k < r.order.size(); ++k)
      pop_index[static_cast<size_t>(r.order[k])] = static_cast<int>(k);
    for (size_t i = 0; i < r.receiver.size(); ++i) {
      const int32_t rcv = r.receiver[i];
      if (rcv < 0) continue;
      REQUIRE(r.water_level[static_cast<size_t>(rcv)] < r.water_level[i]);
      REQUIRE(pop_index[static_cast<size_t>(rcv)] < pop_index[i]);
    }
    // and every chain terminates at base level without cycling
    for (size_t start = 0; start < r.receiver.size(); ++start) {
      size_t i = start, steps = 0;
      while (r.receiver[i] >= 0) {
        i = static_cast<size_t>(r.receiver[i]);
        REQUIRE(++steps <= r.receiver.size());
      }
    }
  };
  check(make_plane(32, 0.4f, 0.05f, 1.0f));
  check(tilted_plane(24, 24, 0.0f));  // all-flat: every step is an epsilon step
  {  // a bowl, so flooded interiors are exercised too
    Field2D<float> h(21, 21, 0.0f);
    for (int y = 5; y <= 15; ++y)
      for (int x = 5; x <= 15; ++x)
        h.at(x, y) = (x == 5 || x == 15 || y == 5 || y == 15) ? 10.0f : 2.0f;
    h.at(10, 5) = 5.0f;  // notch
    check(h);
  }
}

TEST_CASE("route_flow: tilted plane — receivers never uphill, border drains") {
  const auto h = tilted_plane(16, 8, 1.0f);
  const auto r = route_flow(h, 1.0f, 1e-4f);
  REQUIRE(r.order.size() == h.size());
  for (int y = 0; y < 8; ++y)
    for (int x = 0; x < 16; ++x) {
      const int i = y * 16 + x;
      if (x == 0 || y == 0 || x == 15 || y == 7) continue;  // border: seeds
      REQUIRE(r.receiver[i] >= 0);
      REQUIRE(h.data[r.receiver[i]] <= h.data[i]);  // never uphill
      REQUIRE(r.in_lake[i] == 0);
    }
}

TEST_CASE("route_flow: flat plate — epsilon drains everything to the border") {
  Field2D<float> h(12, 12, 5.0f);
  const auto r = route_flow(h, 1.0f, 1e-4f);
  for (int y = 1; y < 11; ++y)
    for (int x = 1; x < 11; ++x) {
      // walk receivers; must reach a border cell (receiver -1) without cycling
      int i = y * 12 + x, steps = 0;
      while (r.receiver[i] >= 0 && steps++ < 12 * 12) i = r.receiver[i];
      REQUIRE(r.receiver[i] == -1);
    }
}

TEST_CASE("route_flow: walled bowl floods to its notch level, uniform per lake") {
  // CAREFUL with synthetic terrains here: the border is the drain (base
  // level), so the bowl's wall must be INTERIOR and the ground outside it
  // must slope freely to the border — a bowl formed by raising the border
  // itself would flood the entire map instead.
  // 11x11: ground 0, a ring wall at 10 (Chebyshev radius 2 around center),
  // bowl floor 2 inside, one notch at 5 in the wall.
  Field2D<float> h(11, 11, 0.0f);
  for (int y = 3; y <= 7; ++y)
    for (int x = 3; x <= 7; ++x)
      if (x == 3 || x == 7 || y == 3 || y == 7) h.at(x, y) = 10.0f;  // wall
      else h.at(x, y) = 2.0f;                                        // floor
  h.at(5, 3) = 5.0f;  // notch: the spill
  const auto r = route_flow(h, 1.0f, 1e-4f);
  for (int y = 4; y <= 6; ++y)
    for (int x = 4; x <= 6; ++x) {
      const int i = y * 11 + x;
      REQUIRE(r.in_lake[i] == 1);
      // flooded to the 5 m notch (+ a few epsilon steps at most)
      REQUIRE(r.water_level[i] == Catch::Approx(5.0f).margin(0.01));
    }
}

TEST_CASE("accumulate_drainage: a plane tilted in +x drains through x=0") {
  // This used to assert only that x=0 beat x=15, because the flood-parent
  // receiver sent most of the water sideways: of 512 m^3, just 116 reached
  // x=0 while 340 left via y=0. That was recorded as a consequence of the
  // index tie-break rather than what it actually was — the diagonal bias of
  // ranking receivers by elevation instead of gradient.
  //
  // Steepest descent makes the answer exact. The plane is constant in y, so
  // every interior cell's orthogonal -x neighbour drops 1.0/1.0 while both
  // diagonals drop 1.0/sqrt(2); -x wins outright, with no tie for the
  // tie-break to resolve. All 14*6 interior cells therefore funnel into the
  // x=0 column, which also keeps its own 8 border cells.
  const auto h = tilted_plane(16, 8, 1.0f);
  const auto r = route_flow(h, 1.0f, 1e-4f);
  const auto a = accumulate_drainage(r, 4.0f);  // 2 m texels

  double total_conserved = 0.0;
  for (size_t i = 0; i < a.data.size(); ++i)
    if (r.receiver[i] < 0) total_conserved += a.data[i];
  REQUIRE(total_conserved == Catch::Approx(16 * 8 * 4.0));

  double total_x0 = 0.0, total_x15 = 0.0;
  for (int y = 0; y < 8; ++y) {
    total_x0 += a.at(0, y);
    total_x15 += a.at(15, y);
  }
  REQUIRE(total_x0 == Catch::Approx((8 + 14 * 6) * 4.0));  // 368 of 512
  REQUIRE(total_x15 == Catch::Approx(8 * 4.0));            // its own cells only

  // Every interior row is strictly increasing downstream, not merely
  // non-decreasing: each cell adds its own rain to a single -x chain.
  for (int y = 1; y < 7; ++y)
    for (int x = 14; x > 1; --x) REQUIRE(a.at(x - 1, y) > a.at(x, y));
}

TEST_CASE("route_flow + accumulate_drainage: deterministic") {
  const auto h = tilted_plane(16, 8, 0.0f);  // all-flat: worst case for ties
  const auto r1 = route_flow(h, 1.0f, 1e-4f);
  const auto r2 = route_flow(h, 1.0f, 1e-4f);
  REQUIRE(r1.receiver == r2.receiver);
  REQUIRE(r1.order == r2.order);
  REQUIRE(accumulate_drainage(r1, 1.0f).data == accumulate_drainage(r2, 1.0f).data);
}

TEST_CASE("route_flow: a lake tag replaces the in_lake exclusion") {
  // A broad flat basin. Without a tag, every ponded cell — and on level
  // ground that means every FLAT cell too, since the flood front always
  // arrives above a flat cell's own height — keeps its flood-tree parent. With
  // a tag naming only the real lake, the flat routes by gradient instead.
  // A FLAT plate, which is the case that matters: on level ground every
  // interior cell is pushed at parent_level + epsilon, so `in_lake` flags the
  // entire plate even though none of it is a lake. A sloped fixture would not
  // exercise this at all — nothing outside the pond gets flooded.
  const int n = 24;
  Field2D<float> h(n, n, 0.0f);
  for (int y = 8; y <= 15; ++y)  // a genuine pond, 1 m deep
    for (int x = 8; x <= 15; ++x) h.at(x, y) = -1.0f;

  const auto untagged = route_flow(h, 1.0f, 1e-4f);
  Field2D<uint8_t> tag(n, n, 0);
  for (int y = 8; y <= 15; ++y)
    for (int x = 8; x <= 15; ++x) tag.at(x, y) = 1;
  const auto tagged = route_flow(h, 1.0f, 1e-4f, &tag);

  // The flood itself is unchanged — only the direction pass sees the tag.
  REQUIRE(tagged.order == untagged.order);
  REQUIRE(tagged.water_level == untagged.water_level);
  REQUIRE(tagged.in_lake == untagged.in_lake);

  // Tagged cells keep exactly the receivers the untagged run gave them.
  for (int y = 8; y <= 15; ++y)
    for (int x = 8; x <= 15; ++x) {
      const int i = y * n + x;
      REQUIRE(tagged.receiver[i] == untagged.receiver[i]);
    }

  // Cells the flood flagged but the tag did not are now steepest-descent
  // routed, so at least some of them must have changed receiver — that is the
  // whole point of the tag.
  int flooded_untagged = 0, changed = 0;
  for (int i = 0; i < n * n; ++i) {
    if (!untagged.in_lake[i] || tag.data[static_cast<size_t>(i)]) continue;
    ++flooded_untagged;
    if (tagged.receiver[i] != untagged.receiver[i]) ++changed;
  }
  REQUIRE(flooded_untagged > 0);
  REQUIRE(changed > 0);

  // And the invariants that make `order` usable still hold under the tag.
  for (size_t i = 0; i < tagged.receiver.size(); ++i) {
    const int32_t rcv = tagged.receiver[i];
    if (rcv < 0) continue;
    REQUIRE(tagged.water_level[static_cast<size_t>(rcv)] < tagged.water_level[i]);
  }
}

TEST_CASE("route_flow: a null tag is bit-identical to the in_lake fallback") {
  Field2D<float> h(20, 20, 0.0f);
  for (int y = 0; y < 20; ++y)
    for (int x = 0; x < 20; ++x)
      h.at(x, y) = 0.03f * static_cast<float>(x) + ((x > 6 && x < 13 && y > 6 && y < 13) ? -2.0f : 0.0f);
  const auto a = route_flow(h, 1.0f, 1e-4f);
  const auto b = route_flow(h, 1.0f, 1e-4f, nullptr);
  REQUIRE(a.receiver == b.receiver);
  REQUIRE(a.order == b.order);
  REQUIRE(a.in_lake == b.in_lake);
}
