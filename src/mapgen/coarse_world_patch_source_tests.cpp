// Tests for CoarseWorldPatchSource -- the provider that cuts an arbitrary
// PatchRequest out of a whole coarse (protogen) world on disk.
//
// HARD RULE: NO TEST HERE MAY RUN THE COARSE SIMULATION. Every fixture is a
// synthetic coarse artifact this file writes itself: an analytic bed, a
// hand-written world.txt, a hand-built rivers.bin. Kept tiny throughout.

#include <catch_amalgamated.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "mapgen/biomes.hpp"
#include "mapgen/coarse_world_patch_source.hpp"
#include "mapgen/river_io.hpp"
#include "mapgen/synthetic_patch_source.hpp"

using namespace badlands::mapgen;

namespace {

constexpr double kPi = 3.14159265358979323846;

// Same TempDir pattern as patch_io_tests.cpp / coarse_io_tests.cpp, its own
// prefix so a parallel ctest run cannot collide with either.
struct TempDir {
  std::filesystem::path path;
  explicit TempDir(const std::string& name)
      : path(std::filesystem::temp_directory_path() / ("bl_coarse_patch_" + name)) {
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);
  }
  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
  std::string str() const { return path.string(); }
};

void write_raw(const std::string& p, const std::vector<float>& v) {
  std::ofstream f(p, std::ios::binary);
  f.write(reinterpret_cast<const char*>(v.data()),
          static_cast<std::streamsize>(v.size() * sizeof(float)));
}

// A smooth, low-frequency bed: a gentle planar slope (which every cubic
// reproduces to machine epsilon) plus a small, long-wavelength ripple (period
// 400 m, far above the 16-32 m cell sizes these tests use) so the fixture is
// not perfectly linear without introducing any real high-frequency content
// for a resample to alias against.
float AnalyticBed(float x, float z) {
  return 100.0f + 0.02f * x + 0.015f * z +
        3.0f * std::sin(2.0 * kPi * x / 400.0) * std::sin(2.0 * kPi * z / 400.0);
}

// Writes a coarse world fixture: world.txt + "<tag>-{height,water,soil}.f32",
// all dry, uniform soil comfortably above both cutoffs (so every dry texel
// classifies Plains and the biome/water logic stays out of the way of the
// resample tests). `bed` supplies height at each texel's WORLD-METRE centre.
template <typename BedFn>
void write_coarse_world(const std::string& dir, int res, float world_size_m,
                        const std::string& tag, BedFn&& bed) {
  CoarseManifest man;
  man.resolution = res;
  man.world_size_m = world_size_m;
  man.seed = 1;
  man.runoff_m_per_yr = 1.0f;
  man.steps = 1;
  man.soil_cut_mountain_m = 0.35f;
  man.soil_cut_hills_m = 1.40f;
  std::string err;
  REQUIRE(write_coarse_manifest(dir, man, &err));

  const float texel_m = world_size_m / static_cast<float>(res);
  const size_t count = static_cast<size_t>(res) * res;
  std::vector<float> height(count), water(count, 0.0f), soil(count, 2.0f);
  for (int y = 0; y < res; ++y) {
    for (int x = 0; x < res; ++x) {
      // NODE convention: texel (x, y) IS world (x*texel_m, y*texel_m), the
      // same lattice patch_data.hpp and river_graph.cpp use. Writing the
      // fixture at (x+0.5)*texel_m instead would encode the resampler's own
      // assumption, so a disagreement with the rest of the pipeline could
      // never show up here -- which is exactly how the half-texel
      // misregistration survived until the ramp test below went looking.
      const float wx = static_cast<float>(x) * texel_m;
      const float wz = static_cast<float>(y) * texel_m;
      height[static_cast<size_t>(y) * res + x] = bed(wx, wz);
    }
  }
  write_raw(dir + "/" + tag + "-height.f32", height);
  write_raw(dir + "/" + tag + "-water.f32", water);
  write_raw(dir + "/" + tag + "-soil.f32", soil);
}

// Box-downsamples a factor-2 finer square field to `out_n`, so a test can
// compare a cubic-upsampled result against a coarser direct fetch without
// reaching into CoarseWorldPatchSource's own (private) box-average code.
Field2D<float> box_downsample_2x(const Field2D<float>& fine, int out_n) {
  Field2D<float> out(out_n, out_n);
  for (int y = 0; y < out_n; ++y)
    for (int x = 0; x < out_n; ++x)
      out.at(x, y) = 0.25f * (fine.at(2 * x, 2 * y) + fine.at(2 * x + 1, 2 * y) +
                              fine.at(2 * x, 2 * y + 1) + fine.at(2 * x + 1, 2 * y + 1));
  return out;
}

// Max abs difference over the INTERIOR of two same-size fields, excluding a
// `margin`-texel border -- the cubic kernel's clamped/renormalised taps near
// the source edge are a real but separate effect from the resample accuracy
// these tests are checking.
float max_interior_diff(const Field2D<float>& a, const Field2D<float>& b, int margin) {
  float worst = 0.0f;
  for (int y = margin; y < a.height - margin; ++y)
    for (int x = margin; x < a.width - margin; ++x)
      worst = std::max(worst, std::fabs(a.at(x, y) - b.at(x, y)));
  return worst;
}

// Structural invariants every PatchData must satisfy, regardless of which
// PatchSource produced it -- the test that the patch contract boundary is
// right (patch_source.hpp): if this needs to know which source it got, the
// interface is wrong.
void CheckPatchInvariants(const PatchSource& src, const PatchRequest& req) {
  const PatchData p = src.Fetch(req);
  REQUIRE_FALSE(empty(p));
  const int n = req.resolution;
  const size_t count = static_cast<size_t>(n) * n;

  CHECK(p.height.data.size() == count);
  CHECK(p.level.data.size() == count);
  CHECK(p.water_depth.data.size() == count);
  CHECK(p.lake_id.data.size() == count);
  CHECK(p.biome.data.size() == count);
  CHECK(p.soil.data.size() == count);
  CHECK(p.texel_m == Catch::Approx(req.world_size_m / static_cast<float>(n)));

  for (size_t i = 0; i < count; ++i) {
    CHECK(p.level.data[i] >= p.height.data[i] - 1e-3f);
    const float expect_depth = std::max(0.0f, p.level.data[i] - p.height.data[i]);
    CHECK(p.water_depth.data[i] == Catch::Approx(expect_depth).margin(1e-3));
    CHECK(p.biome.data[i] < kBiomeCount);
  }

  CHECK(p.elevation_range.min_m <= p.elevation_range.max_m);
  for (float h : p.height.data) {
    CHECK(h >= p.elevation_range.min_m - 1e-3f);
    CHECK(h <= p.elevation_range.max_m + 1e-3f);
  }
}

}  // namespace

TEST_CASE("fetching the same world at two resolutions agrees after "
         "box-downsampling, holding world_size_m fixed", "[patch]") {
  // ONE synthetic coarse artifact -- the same source, queried twice. Holding
  // world_size_m fixed and varying only resolution is the point: it is what
  // a hardcoded source-density assumption cannot survive.
  TempDir dir("res_independence");
  write_coarse_world(dir.str(), 64, 1024.0f, "0001-step", AnalyticBed);

  std::string err;
  auto source = LoadCoarseWorldPatchSource(dir.str(), "0001-step", &err);
  REQUIRE(source);
  CHECK(err.empty());

  PatchRequest req_lo;
  req_lo.origin_m = glm::dvec2(0.0, 0.0);
  req_lo.world_size_m = 1024.0f;
  req_lo.resolution = 64;
  const PatchData lo = source->Fetch(req_lo);
  REQUIRE_FALSE(empty(lo));

  PatchRequest req_hi = req_lo;
  req_hi.resolution = 128;
  const PatchData hi = source->Fetch(req_hi);
  REQUIRE_FALSE(empty(hi));

  // DECIMATE, do not box-average. Under node registration the 64-grid's node
  // j sits at world j*16 m and the 128-grid's node 2j sits at world 2j*8 m --
  // the SAME point, resampled from the same source with the same kernel. So
  // the two must agree to float noise, not merely to within a tolerance.
  //
  // Box-averaging adjacent hi nodes instead (what this test used to do)
  // silently re-introduces a half-cell shift, which is why it needed a 0.2 m
  // slop that was large enough to hide the misregistration it was supposed to
  // be insensitive to.
  Field2D<float> hi_decimated(64, 64, 0.0f);
  for (int j = 0; j < 64; ++j)
    for (int i = 0; i < 64; ++i) hi_decimated.at(i, j) = hi.height.at(2 * i, 2 * j);

  const float worst = max_interior_diff(lo.height, hi_decimated, 3);
  CHECK(worst < 1e-3f);
}

TEST_CASE("fetching the same world_size_m/resolution agrees regardless of "
         "how densely the source was simulated", "[patch]") {
  // Same world_size_m, same REQUESTED resolution, but the coarse artifact
  // itself written at two different densities from the SAME analytic
  // function. This is the test that fails if anyone writes a hardcoded 16.
  TempDir dir_a("density_32");
  TempDir dir_b("density_64");
  write_coarse_world(dir_a.str(), 32, 1024.0f, "0001-step", AnalyticBed);
  write_coarse_world(dir_b.str(), 64, 1024.0f, "0001-step", AnalyticBed);

  std::string err;
  auto source_a = LoadCoarseWorldPatchSource(dir_a.str(), "0001-step", &err);
  REQUIRE(source_a);
  auto source_b = LoadCoarseWorldPatchSource(dir_b.str(), "0001-step", &err);
  REQUIRE(source_b);
  CHECK(source_a->manifest().texel_m == Catch::Approx(32.0f));
  CHECK(source_b->manifest().texel_m == Catch::Approx(16.0f));

  PatchRequest req;
  req.origin_m = glm::dvec2(0.0, 0.0);
  req.world_size_m = 1024.0f;
  req.resolution = 64;

  const PatchData a = source_a->Fetch(req);  // upsample, ratio 16/32 = 0.5
  const PatchData b = source_b->Fetch(req);  // pass-through, ratio 16/16 = 1
  REQUIRE_FALSE(empty(a));
  REQUIRE_FALSE(empty(b));

  const float worst = max_interior_diff(a.height, b.height, 3);
  CHECK(worst < 0.3f);
}

TEST_CASE("a request whose extent is not a whole number of coarse cells "
         "still succeeds", "[patch]") {
  // The old window.cpp tool rejected this (WindowCellsIntegral); the whole
  // point of computing taps per output texel is that alignment is this
  // provider's problem, not the caller's.
  TempDir dir("non_integral");
  write_coarse_world(dir.str(), 64, 1024.0f, "0001-step", AnalyticBed);

  std::string err;
  auto source = LoadCoarseWorldPatchSource(dir.str(), "0001-step", &err);
  REQUIRE(source);

  PatchRequest req;
  req.origin_m = glm::dvec2(7.0, 11.0);   // not a multiple of the 16 m cell
  req.world_size_m = 100.0f;              // 100 / 16 = 6.25 cells
  req.resolution = 50;

  const PatchData p = source->Fetch(req);
  REQUIRE_FALSE(empty(p));
  CHECK(p.height.width == 50);
  CHECK(p.height.height == 50);
  for (float h : p.height.data) CHECK(std::isfinite(h));
  for (float d : p.water_depth.data) CHECK(d >= 0.0f);
  for (uint8_t b : p.biome.data) CHECK(b < kBiomeCount);
}

TEST_CASE("a request coarser than the source produces area-averages, not "
         "garbage", "[patch]") {
  // A pure planar bed: the area-average of a linear function over any box is
  // exactly its value at the box's centroid, so this has an EXACT expected
  // answer rather than merely "looks plausible" -- and it pins the direction
  // (area-average, not a resample filter run backwards).
  TempDir dir("downsample");
  const float world_m = 512.0f;
  auto plane = [](float x, float /*z*/) { return 100.0f + 0.5f * x; };
  write_coarse_world(dir.str(), 128, world_m, "0001-step", plane);  // texel 4 m

  std::string err;
  auto source = LoadCoarseWorldPatchSource(dir.str(), "0001-step", &err);
  REQUIRE(source);

  PatchRequest req;
  req.origin_m = glm::dvec2(0.0, 0.0);
  req.world_size_m = world_m;
  req.resolution = 16;  // out_texel = 32 m, ratio 32/4 = 8 -> downsample

  const PatchData p = source->Fetch(req);
  REQUIRE_FALSE(empty(p));
  for (float h : p.height.data) CHECK(std::isfinite(h));

  // Skip the first node on each axis: its 32 m averaging footprint is CENTRED
  // on world 0, so half of it lies off the world and clamps to the edge value.
  // That is correct border behaviour (and the same reason the ramp test starts
  // clear of the cubic stencil); every interior node reproduces the plane
  // exactly, because a symmetric box average of a linear function is its value
  // at the centre.
  float worst = 0.0f;
  for (int j = 1; j < req.resolution; ++j) {
    for (int i = 1; i < req.resolution; ++i) {
      // NODE registration: output node i IS world i*32 m, so a box average
      // centred there reproduces the plane's value AT that node. The old
      // expectation used (i+0.5)*32 -- the pixel-centre convention -- and so
      // agreed with a resampler that disagreed with the rest of the pipeline
      // by half a source cell.
      const float node_x = static_cast<float>(i) * 32.0f;
      const float expect = 100.0f + 0.5f * node_x;
      worst = std::max(worst, std::fabs(p.height.at(i, j) - expect));
    }
  }
  CHECK(worst < 1e-2f);
}

TEST_CASE("rivers clipped out of the coarse graph are patch-local, not "
         "offset by origin_m", "[patch]") {
  TempDir dir("rivers_local");
  write_coarse_world(dir.str(), 64, 1024.0f, "0001-step",
                     [](float, float) { return 100.0f; });  // flat; irrelevant here

  // A reach crossing the requested rect [200,300) x [200,300) on both sides.
  RiverGraph g;
  g.nodes.resize(2);
  g.nodes[0].kind = RiverNodeKind::Source;
  g.nodes[0].pos_m = glm::vec2(150.0f, 250.0f);
  g.nodes[1].kind = RiverNodeKind::Mouth;
  g.nodes[1].pos_m = glm::vec2(350.0f, 250.0f);
  RiverEdge e;
  e.from = 0;
  e.to = 1;
  e.points_m = {{150.0f, 250.0f}, {220.0f, 250.0f}, {260.0f, 250.0f}, {350.0f, 250.0f}};
  e.discharge_m3_s = {1.0f, 1.0f, 1.0f, 1.0f};
  e.width_m = {2.0f, 2.0f, 2.0f, 2.0f};
  e.depth_m = {0.5f, 0.5f, 0.5f, 0.5f};
  e.speed_m_s = {1.0f, 1.0f, 1.0f, 1.0f};
  g.edges = {e};

  std::string err;
  REQUIRE(write_river_graph(dir.str() + "/rivers.bin", g, &err));

  auto source = LoadCoarseWorldPatchSource(dir.str(), "0001-step", &err);
  REQUIRE(source);

  PatchRequest req;
  req.origin_m = glm::dvec2(200.0, 200.0);
  req.world_size_m = 100.0f;
  req.resolution = 20;

  const PatchData p = source->Fetch(req);
  REQUIRE_FALSE(p.rivers.edges.empty());
  const float eps = 1e-2f;
  for (const RiverNode& n : p.rivers.nodes) {
    CHECK(n.pos_m.x >= -eps);
    CHECK(n.pos_m.x <= 100.0f + eps);
    CHECK(n.pos_m.y >= -eps);
    CHECK(n.pos_m.y <= 100.0f + eps);
  }
  for (const RiverEdge& r : p.rivers.edges) {
    for (const glm::vec2& pt : r.points_m) {
      CHECK(pt.x >= -eps);
      CHECK(pt.x <= 100.0f + eps);
      CHECK(pt.y >= -eps);
      CHECK(pt.y <= 100.0f + eps);
    }
  }
}

TEST_CASE("a synthetic patch and a coarse-world patch satisfy the same "
         "structural invariants", "[patch]") {
  SyntheticPatchSource synthetic;
  PatchRequest synth_req;
  synth_req.origin_m = glm::dvec2(0.0, 0.0);
  synth_req.world_size_m = 128.0f;
  synth_req.resolution = 32;
  CheckPatchInvariants(synthetic, synth_req);

  // A bowl with a pond in the middle -- exercises the water-rebuild path
  // (not just a dry resample) through the same invariant checks.
  TempDir dir("indistinguishable");
  const int n = 32;
  const float world_m = 512.0f;  // texel 16 m
  const float surface_m = 120.0f;
  auto bowl = [&](float x, float z) {
    const float cx = 0.5f * world_m, cz = 0.5f * world_m;
    const float dx = x - cx, dz = z - cz;
    return 100.0f + 0.0006f * (dx * dx + dz * dz);
  };
  write_coarse_world(dir.str(), n, world_m, "0001-step", bowl);
  // Overwrite the water raster with a pond wherever the bowl sits below the
  // surface -- write_coarse_world above wrote it all-dry.
  {
    const float texel_m = world_m / static_cast<float>(n);
    std::vector<float> water(static_cast<size_t>(n) * n, 0.0f);
    for (int y = 0; y < n; ++y)
      for (int x = 0; x < n; ++x) {
        const float wx = (static_cast<float>(x) + 0.5f) * texel_m;
        const float wz = (static_cast<float>(y) + 0.5f) * texel_m;
        const float bed = bowl(wx, wz);
        if (bed < surface_m) water[static_cast<size_t>(y) * n + x] = surface_m - bed;
      }
    write_raw(dir.str() + "/0001-step-water.f32", water);
  }

  std::string err;
  auto coarse = LoadCoarseWorldPatchSource(dir.str(), "0001-step", &err);
  REQUIRE(coarse);
  PatchRequest coarse_req;
  coarse_req.origin_m = glm::dvec2(0.0, 0.0);
  coarse_req.world_size_m = world_m;
  coarse_req.resolution = n;  // ratio == 1: exercises the pass-through path too
  CheckPatchInvariants(*coarse, coarse_req);

  // And the coarse patch really does carry a lake, so the shared invariant
  // check above was not vacuously true for the water fields.
  const PatchData p = coarse->Fetch(coarse_req);
  bool any_wet = false;
  for (float d : p.water_depth.data) any_wet = any_wet || d > 0.0f;
  CHECK(any_wet);
}

TEST_CASE("CoarseWorldPatchSource: a resampled ramp keeps NODE registration",
          "[patch]") {
  // THE REGISTRATION PIN.
  //
  // Every raster in this pipeline is NODE-sampled: texel (i, j) IS world
  // (i*texel_m, j*texel_m). patch_data.hpp states it, synthetic_patch_source
  // samples that way, river_graph emits node coordinates, and river_carve
  // rounds world->texel against them. A resampler that instead treats indices
  // as pixel CENTRES shifts the bed by 0.5*(out_texel - src_texel) relative to
  // the river graph clipped out of the SAME world -- 7.5 m at a 16 m source
  // and a 1 m patch, which cuts every channel that far off its valley.
  //
  // A linear ramp is the sharpest probe there is: Catmull-Rom reproduces a
  // linear function to machine epsilon, so any residual here is registration
  // error and nothing else. Note the fixture is written in NODE convention on
  // purpose -- writing it at (x+0.5)*texel would express the resampler's own
  // assumption and could never catch a disagreement with it.
  TempDir dir("ramp_registration");
  const int src_res = 64;
  const float world_m = 1024.0f;
  const float src_texel = world_m / static_cast<float>(src_res);  // 16 m

  CoarseManifest man;
  man.resolution = src_res;
  man.world_size_m = world_m;
  man.texel_m = src_texel;
  man.soil_cut_mountain_m = 0.35f;
  man.soil_cut_hills_m = 1.40f;
  std::string err;
  REQUIRE(write_coarse_manifest(dir.str(), man, &err));

  const size_t count = static_cast<size_t>(src_res) * src_res;
  // Soil DEEP (>= the relief filter's 3 m fade-out): a 45-degree ramp is
  // squarely in the filter's slope gate, and this test asserts the PURE
  // resample -- registration error and nothing else.
  std::vector<float> height(count), water(count, 0.0f), soil(count, 4.0f);
  for (int y = 0; y < src_res; ++y)
    for (int x = 0; x < src_res; ++x)
      height[static_cast<size_t>(y) * src_res + x] =
          static_cast<float>(x) * src_texel;  // height(wx) == wx
  write_raw(dir.str() + "/0001-step-height.f32", height);
  write_raw(dir.str() + "/0001-step-water.f32", water);
  write_raw(dir.str() + "/0001-step-soil.f32", soil);

  auto src = LoadCoarseWorldPatchSource(dir.str(), "0001-step", &err);
  REQUIRE(src != nullptr);

  PatchRequest req;
  req.origin_m = glm::dvec2(0.0, 0.0);
  req.world_size_m = 128.0f;
  req.resolution = 128;  // 1 m out of 16 m in
  const PatchData p = src->Fetch(req);
  REQUIRE(p.height.width == 128);

  // Interior only, and "interior" means clear of the CUBIC STENCIL, not of the
  // patch: the kernel reaches 2 source cells (32 m) either side, so below
  // j = 32 it clamps against the world edge at x = 0 and reads a flat 0 where
  // the ramp would continue negative. That is correct border behaviour and
  // would mask the registration error under test.
  for (int j = 40; j < 120; ++j) {
    const float want = static_cast<float>(j) * p.texel_m;
    REQUIRE(p.height.at(j, 64) == Catch::Approx(want).margin(1e-3));
  }
}

TEST_CASE(
    "CoarseWorldPatchSource: a fine fetch carries relief detail on "
    "thin-soil steep ground",
    "[patch]") {
  // The relief chain's step 2 (2026-08-06 spec): after the Catmull-Rom
  // resample, the detail filter adds sub-coarse-cell relief. Two worlds
  // differing ONLY in soil depth isolate it -- deep soil closes the
  // physical fade gate exactly, so any height difference between the two
  // fetches IS the filter's delta, and it must exist, stay bounded, and
  // never appear at 16 m output texels.
  auto write_ramp_world = [](const std::string& dir, float soil_m) {
    const int src_res = 64;
    const float world_m = 1024.0f;
    const float src_texel = world_m / static_cast<float>(src_res);
    CoarseManifest man;
    man.resolution = src_res;
    man.world_size_m = world_m;
    man.texel_m = src_texel;
    man.soil_cut_mountain_m = 0.35f;
    man.soil_cut_hills_m = 1.40f;
    std::string err;
    REQUIRE(write_coarse_manifest(dir, man, &err));
    const size_t count = static_cast<size_t>(src_res) * src_res;
    std::vector<float> height(count), water(count, 0.0f), soil(count, soil_m);
    for (int y = 0; y < src_res; ++y)
      for (int x = 0; x < src_res; ++x)
        height[static_cast<size_t>(y) * src_res + x] =
            0.6f * static_cast<float>(x) * src_texel;
    write_raw(dir + "/0001-step-height.f32", height);
    write_raw(dir + "/0001-step-water.f32", water);
    write_raw(dir + "/0001-step-soil.f32", soil);
  };

  TempDir thin_dir("relief_thin"), deep_dir("relief_deep");
  write_ramp_world(thin_dir.str(), 0.2f);
  write_ramp_world(deep_dir.str(), 4.0f);
  std::string err;
  auto thin = LoadCoarseWorldPatchSource(thin_dir.str(), "0001-step", &err);
  auto deep = LoadCoarseWorldPatchSource(deep_dir.str(), "0001-step", &err);
  REQUIRE(thin != nullptr);
  REQUIRE(deep != nullptr);

  PatchRequest req;
  req.origin_m = glm::dvec2(256.0, 256.0);
  req.world_size_m = 256.0f;
  req.resolution = 256;  // 1 m texels: the whole octave band is audible
  const PatchData pt = thin->Fetch(req);
  const PatchData pd = deep->Fetch(req);
  const float diff = max_interior_diff(pt.height, pd.height, 8);
  REQUIRE(diff > 0.05f);
  REQUIRE(diff <= 4.0f);

  // At source density the octave band is below the output Nyquist: the same
  // two worlds must fetch identical heights.
  req.world_size_m = 256.0f;
  req.resolution = 16;  // 16 m texels
  const PatchData ct = thin->Fetch(req);
  const PatchData cd = deep->Fetch(req);
  REQUIRE(max_interior_diff(ct.height, cd.height, 0) == 0.0f);
}
