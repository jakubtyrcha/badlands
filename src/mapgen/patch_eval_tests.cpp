// Tests for the patch evaluator's WORLD MAPPING — the contract that makes a preview a
// literal crop of the world rather than a picture whose meaning depends on its size.
//
// A terrain script is a pure function of world position (scripts/mapgen/biomes/README.md),
// so the evaluator's only job is to hand it the right world coordinate for each sample:
//
//     world = origin + (index + 0.5) * meters_per_sample
//
// These tests pin that with a RAMP script (height = wx), whose value at every sample is
// analytically known. That is what makes them able to fail: a ramp makes a wrong world
// mapping a wrong NUMBER, whereas any real noise script would just look "different" and
// nobody could tell which was right.
//
// Why this matters now: patchgen used to hardwire meters_per_sample = kMetersPerSample
// (the MAP's grid density), welding the preview to the map's 1 m grid so window size and
// sample count were one knob. The view is a separate concept — it picks its own sampling
// rate (extent/res). These tests guard that decoupling.

#include <catch_amalgamated.hpp>

#include <string>
#include <vector>

#include "mapgen/field2d.hpp"
#include "mapgen/script_eval.hpp"

using badlands::mapgen::evaluate_patch_script;
using badlands::mapgen::Field2D;
using badlands::mapgen::PatchDomain;
using badlands::mapgen::ScriptUniform;

namespace {

// height = wx: the world X of the sample, straight back out. Any error in the world
// mapping shows up as a wrong number rather than as terrain that merely looks odd.
constexpr const char* kRampX = R"(
@uni.origin_x: f32 = 0.0;
@uni.origin_z: f32 = 0.0;
@uni.meters_per_sample: f32 = 1.0;
@uni.seed: f32 = 1.0;

let wx = (f32(@warpId.0) + 0.5) * @uni.meters_per_sample + @uni.origin_x;
wx
)";

// height = wz, to catch an axis swap that a wx-only ramp would miss.
constexpr const char* kRampZ = R"(
@uni.origin_x: f32 = 0.0;
@uni.origin_z: f32 = 0.0;
@uni.meters_per_sample: f32 = 1.0;
@uni.seed: f32 = 1.0;

let wz = (f32(@warpId.1) + 0.5) * @uni.meters_per_sample + @uni.origin_z;
wz
)";

Field2D<float> eval(const char* src, const PatchDomain& d) {
  Field2D<float> out;
  std::string err;
  const std::vector<ScriptUniform> none;
  REQUIRE(evaluate_patch_script(src, d, none, out, err));
  REQUIRE(err.empty());
  REQUIRE(out.width == d.size);
  REQUIRE(out.height == d.size);
  return out;
}

PatchDomain view(float extent_m, int res, float ox = 0.0f, float oz = 0.0f) {
  PatchDomain d;
  d.size = res;
  d.origin_x = ox;
  d.origin_z = oz;
  d.meters_per_sample = extent_m / static_cast<float>(res);
  return d;
}

}  // namespace

TEST_CASE("patch world mapping holds at any extent/resolution") {
  // Same 2 km window sampled three ways, plus a 512 m window: the sampling rate must
  // follow extent/res, never a fixed 1 m. Under the old welded mps=kMetersPerSample the
  // 2 km cases collapse to a 512 m ramp and every expectation below fails.
  struct Case {
    float extent_m;
    int res;
    float ox, oz;
  };
  const Case cases[] = {
      {2000.0f, 512, 0.0f, 0.0f},      // structure view: mps = 3.90625
      {2000.0f, 256, 0.0f, 0.0f},      // same window, coarser: mps = 7.8125
      {512.0f, 512, 0.0f, 0.0f},       // detail view: mps = 1.0
      {2000.0f, 512, 4000.0f, -750.0f},  // placed elsewhere in the world
  };

  for (const Case& c : cases) {
    const PatchDomain d = view(c.extent_m, c.res, c.ox, c.oz);
    const float mps = c.extent_m / static_cast<float>(c.res);
    CAPTURE(c.extent_m, c.res, c.ox, mps);

    const Field2D<float> hx = eval(kRampX, d);
    const Field2D<float> hz = eval(kRampZ, d);

    for (int i : {0, 1, c.res / 2, c.res - 1}) {
      const float want_x = c.ox + (static_cast<float>(i) + 0.5f) * mps;
      const float want_z = c.oz + (static_cast<float>(i) + 0.5f) * mps;
      // Row/column constant along the other axis, so any (i, j) probes the same value.
      CHECK(hx.at(i, 0) == Catch::Approx(want_x).margin(1e-3));
      CHECK(hx.at(i, c.res - 1) == Catch::Approx(want_x).margin(1e-3));
      CHECK(hz.at(0, i) == Catch::Approx(want_z).margin(1e-3));
    }

    // The window really spans `extent_m`: last sample centre is half a sample short of
    // the far edge. This is what fails if extent is conflated with sample count.
    const float last = c.ox + (static_cast<float>(c.res) - 0.5f) * mps;
    CHECK(hx.at(c.res - 1, 0) == Catch::Approx(last).margin(1e-3));
    CHECK(hx.at(c.res - 1, 0) - hx.at(0, 0) ==
          Catch::Approx((c.extent_m - mps)).margin(1e-2));
  }
}

TEST_CASE("patch sampling is world-fixed: same world point, any view") {
  // The heart of the contract: a world position has ONE height, no matter which view
  // samples it. Two views whose sample centres coincide must agree exactly.
  //   detail: mps 1.0 -> centres at 0.5, 1.5, 2.5, ...
  //   coarse: mps 4.0, origin shifted +1.5 -> centres at 3.5, 7.5, ... == detail[3], [7]
  const PatchDomain fine = view(512.0f, 512);          // mps 1.0, centres k + 0.5
  const PatchDomain coarse = view(512.0f, 128, 1.5f);  // mps 4.0, centres 1.5 + 4k + 2

  const Field2D<float> hf = eval(kRampX, fine);
  const Field2D<float> hc = eval(kRampX, coarse);

  for (int k = 0; k < 8; ++k) {
    // coarse sample k sits at world 1.5 + (k+0.5)*4 = 3.5 + 4k -> fine index 3 + 4k.
    const int fine_i = 3 + 4 * k;
    REQUIRE(fine_i < fine.size);
    CHECK(hc.at(k, 0) == Catch::Approx(hf.at(fine_i, 0)).margin(1e-3));
  }
}

TEST_CASE("patch origin shift is exact (adjacent patches share a seam)") {
  // Two patches at the same sampling rate, one shifted by a whole number of samples,
  // must agree on the world they share -- otherwise tiled previews would seam.
  const PatchDomain a = view(512.0f, 512, 0.0f);
  const PatchDomain b = view(512.0f, 512, 512.0f);

  const Field2D<float> ha = eval(kRampX, a);
  const Field2D<float> hb = eval(kRampX, b);

  // Patch b starts exactly where a ends: b's first sample is one full patch along.
  CHECK(hb.at(0, 0) == Catch::Approx(ha.at(0, 0) + 512.0f).margin(1e-3));
  CHECK(hb.at(511, 0) == Catch::Approx(ha.at(511, 0) + 512.0f).margin(1e-3));
}
