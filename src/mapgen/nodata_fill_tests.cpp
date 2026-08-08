#include "mapgen/nodata_fill.hpp"

#include <catch_amalgamated.hpp>

#include <cmath>
#include <limits>

using badlands::mapgen::Field2D;
using badlands::mapgen::fill_nodata;

namespace {

constexpr float kNodata = -9999.0f;

// A field that is a plain ramp in x, so a correct nearest fill is predictable
// from the coordinate alone.
Field2D<float> ramp(int w, int h) {
  Field2D<float> f(w, h, 0.0f);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) f.at(x, y) = static_cast<float>(x);
  }
  return f;
}

size_t count_set(const Field2D<uint8_t>& m) {
  size_t n = 0;
  for (uint8_t v : m.data) n += (v != 0);
  return n;
}

}  // namespace

TEST_CASE("a field with no nodata is left bitwise unchanged", "[nodata]") {
  Field2D<float> f = ramp(8, 8);
  const std::vector<float> before = f.data;

  const Field2D<uint8_t> mask = fill_nodata(f, kNodata);

  REQUIRE(f.data == before);
  REQUIRE(mask.width == 8);
  REQUIRE(mask.height == 8);
  REQUIRE(count_set(mask) == 0);
}

TEST_CASE("an interior hole takes its nearest valid neighbour", "[nodata]") {
  Field2D<float> f = ramp(9, 9);
  // A 3x3 void centred at (4, 4). Column 4's nearest valid columns are 3 and 5,
  // which hold 3 and 5 -- so the fill must not invent 4.
  for (int y = 3; y <= 5; ++y) {
    for (int x = 3; x <= 5; ++x) f.at(x, y) = kNodata;
  }

  const Field2D<uint8_t> mask = fill_nodata(f, kNodata);

  REQUIRE(count_set(mask) == 9);
  for (int y = 3; y <= 5; ++y) {
    for (int x = 3; x <= 5; ++x) {
      REQUIRE(mask.at(x, y) == 1);
      // Every filled texel took a real sample from the ramp, never the sentinel.
      REQUIRE(f.at(x, y) != kNodata);
      REQUIRE(std::isfinite(f.at(x, y)));
    }
  }
  // The rim columns are one step from valid ground and must take it exactly.
  REQUIRE(f.at(3, 4) == 2.0f);
  REQUIRE(f.at(5, 4) == 6.0f);
  // Untouched ground is untouched.
  REQUIRE(mask.at(0, 0) == 0);
  REQUIRE(f.at(0, 0) == 0.0f);
}

TEST_CASE("a hole touching the edge still fills", "[nodata]") {
  Field2D<float> f = ramp(6, 6);
  // The whole first column, including both corners -- there is no valid
  // neighbour on three of its four sides.
  for (int y = 0; y < 6; ++y) f.at(0, y) = kNodata;

  const Field2D<uint8_t> mask = fill_nodata(f, kNodata);

  REQUIRE(count_set(mask) == 6);
  for (int y = 0; y < 6; ++y) {
    REQUIRE(mask.at(0, y) == 1);
    REQUIRE(f.at(0, y) == 1.0f);  // column 1 is the only valid neighbour
  }
}

TEST_CASE("NaN counts as nodata whatever the sentinel is", "[nodata]") {
  Field2D<float> f = ramp(5, 5);
  f.at(2, 2) = std::numeric_limits<float>::quiet_NaN();

  const Field2D<uint8_t> mask = fill_nodata(f, kNodata);

  REQUIRE(mask.at(2, 2) == 1);
  REQUIRE(std::isfinite(f.at(2, 2)));
}

TEST_CASE("an entirely-invalid field is left alone with a full mask", "[nodata]") {
  Field2D<float> f(4, 4, kNodata);

  const Field2D<uint8_t> mask = fill_nodata(f, kNodata);

  REQUIRE(count_set(mask) == 16);
  // Nothing to fill FROM, so quietly writing zeros would hide the case the
  // caller has to act on.
  for (float v : f.data) REQUIRE(v == kNodata);
}

TEST_CASE("the fill is deterministic across runs", "[nodata]") {
  Field2D<float> a = ramp(16, 16);
  for (int y = 4; y < 12; ++y) {
    for (int x = 4; x < 12; ++x) a.at(x, y) = kNodata;
  }
  Field2D<float> b = a;

  const Field2D<uint8_t> ma = fill_nodata(a, kNodata);
  const Field2D<uint8_t> mb = fill_nodata(b, kNodata);

  REQUIRE(a.data == b.data);
  REQUIRE(ma.data == mb.data);
}

TEST_CASE("a degenerate field returns an empty mask rather than failing",
          "[nodata]") {
  Field2D<float> f;
  const Field2D<uint8_t> mask = fill_nodata(f, kNodata);
  REQUIRE(mask.size() == 0);
}
