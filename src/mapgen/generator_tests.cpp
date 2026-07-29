// The bedrock+quantile map generator — mechanisms, not looks: determinism,
// structural area fractions, cutoff/classification logic, and
// resolution-independent world sampling. Ridge/clump "look" is judged by eye
// via --preview-image-only, deliberately not pinned here.

#include <catch_amalgamated.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

#include "mapgen/biomes.hpp"
#include "mapgen/generator.hpp"

using badlands::mapgen::Biome;
using badlands::mapgen::BiomeCutoffs;
using badlands::mapgen::classify_biomes;
using badlands::mapgen::compute_cutoffs;
using badlands::mapgen::distance_to_plains;
using badlands::mapgen::Field2D;
using badlands::mapgen::generate_map;
using badlands::mapgen::kPadTexels;
using badlands::mapgen::MapDebugSink;
using badlands::mapgen::MapGenParams;

TEST_CASE("generate_map: same params -> byte-identical artifacts") {
  MapGenParams p;
  p.seed = 7;
  p.resolution = 64;
  p.world_size_m = 256.0f;
  p.erosion.sim_resolution = 64;
  p.erosion.iterations = 8;   // keep the test fast
  const auto a = generate_map(p);
  const auto b = generate_map(p);
  REQUIRE(a.bedrock.data == b.bedrock.data);
  REQUIRE(a.biome.data == b.biome.data);
  REQUIRE(a.heightmap.data == b.heightmap.data);
  REQUIRE(a.water_depth.data == b.water_depth.data);
  REQUIRE(a.flow.data == b.flow.data);
  REQUIRE(a.sediment.data == b.sediment.data);
  REQUIRE(a.river_class.data == b.river_class.data);
  REQUIRE(a.river_discharge_m3_s.data == b.river_discharge_m3_s.data);
  // The GRAPH must be deterministic too, not just its raster: topology and
  // vertex positions, since downstream carving consumes them directly.
  REQUIRE(a.river_graph.nodes.size() == b.river_graph.nodes.size());
  REQUIRE(a.river_graph.edges.size() == b.river_graph.edges.size());
  for (size_t e = 0; e < a.river_graph.edges.size(); ++e) {
    REQUIRE(a.river_graph.edges[e].from == b.river_graph.edges[e].from);
    REQUIRE(a.river_graph.edges[e].to == b.river_graph.edges[e].to);
    REQUIRE(a.river_graph.edges[e].strahler_order == b.river_graph.edges[e].strahler_order);
    REQUIRE(a.river_graph.edges[e].shreve_magnitude == b.river_graph.edges[e].shreve_magnitude);
    REQUIRE(a.river_graph.edges[e].points_m == b.river_graph.edges[e].points_m);
  }
}

TEST_CASE("generate_map: lakes are consistent — Lake biome iff standing water") {
  MapGenParams p;
  p.seed = 2;
  p.resolution = 96;
  p.world_size_m = 384.0f;
  p.erosion.sim_resolution = 96;
  p.erosion.iterations = 8;
  const auto a = generate_map(p);
  for (size_t i = 0; i < a.biome.data.size(); ++i) {
    const bool lake = a.biome.data[i] == static_cast<uint8_t>(Biome::Lake);
    const bool has_water = a.water_depth.data[i] > 0.0f;
    // Lake covers EXACTLY the water now — the freeboard in finalize_lakes
    // leaves the rest of the carved bowl dry, so a coast exists.
    REQUIRE(lake == has_water);
    REQUIRE(a.water_depth.data[i] >= 0.0f);
    REQUIRE(a.flow.data[i] > 0.0f);       // every texel drains something
    REQUIRE(a.sediment.data[i] >= 0.0f);
  }
}

TEST_CASE("generate_map: quantile cutoffs pin the biome area fractions") {
  for (uint32_t seed : {1u, 2u, 3u}) {
    MapGenParams p;
    p.seed = seed;
    p.resolution = 128;
    p.world_size_m = 512.0f;
    // Match the sim grid to the output resolution (default sim_resolution=512
    // would run a much bigger erosion sim per call for no benefit here);
    // iterations stays at its production default (40x dt2, v1.2).
    p.erosion.sim_resolution = 128;
    const auto a = generate_map(p);
    const double n = static_cast<double>(a.biome.data.size());
    double plains = 0.0, mountain = 0.0;
    for (uint8_t b : a.biome.data) {
      if (b == static_cast<uint8_t>(Biome::Plains)) plains += 1.0;
      if (b == static_cast<uint8_t>(Biome::Mountain)) mountain += 1.0;
    }
    // Order statistics are exact up to ties (none in float noise), so a tight
    // margin holds for ANY seed — that is the whole point of quantile cutoffs.
    // Plains gets a much wider margin: erosion is a fixed-iteration "young
    // terrain" sim, not run to drainage equilibrium, so at the production
    // default a seed-dependent amount of transiently undrained low ground is
    // still flooded and gets stamped Lake. Mountain is never touched:
    // cavities only ever carve the BOTTOM lake_frac quantile of bedrock,
    // disjoint from the top-quantile Mountain cutoff.
    //
    // v1.3: lake_frac 0.03 -> 0.08 and the cavity cone slope doubled (deeper
    // basins) both grow Lake coverage, so this margin was re-measured with a
    // local (not committed) sweep at this test's exact params (resolution
    // 128, world_size_m 512, sim_resolution 128, production erosion
    // defaults): seeds 1-20, worst deficit 0.2113 (seed 7); seeds 1-150,
    // worst deficit 0.3077 (seed 94, a fat-tailed outlier — p90 0.159, p95
    // 0.207, p99 0.281 over the same 150). 0.15 no longer holds even at
    // seeds 1-3 reliably as a general claim (it happens to pass them: 0.0661,
    // 0.1021, 0.0154). 0.35 clears the worst 150-seed outlier with headroom;
    // plains fraction is one-sided here (erosion only ever REMOVES plains to
    // Lake, never adds beyond the quantile cutoff — kPlainsFrac + 0.35 is
    // never approached, observed max delta ~0.0001 at the cutoff).
    REQUIRE(plains / n ==
            Catch::Approx(badlands::mapgen::kPlainsFrac).margin(0.35));
    REQUIRE(mountain / n ==
            Catch::Approx(badlands::mapgen::kMountainFrac).margin(0.02));
  }
}

TEST_CASE("generate_map: plains gain drainage relief (no longer flat)") {
  // v1.1: a base-height term applied everywhere makes bedrock a potential
  // whose plains gradients point toward the cavities/lakes. Isolate it here:
  // iterations=0 skips the sim loop's incise/deposit/diffuse entirely, and
  // sediment_noise_m=0 removes the only other source of plains variation —
  // without the override, dist_to_plains is exactly 0 on every plains texel
  // (a plains texel's nearest plains texel is itself), so the taper term
  // alone is spatially CONSTANT across plains and the sediment fBm would be
  // the only thing moving the needle, masking the relief term under test.
  //
  // Population filter, not a seed pin. `a.biome` (the OUTPUT-grid Plains
  // classification) and the sim grid's own biome_sim (which the relief/cone
  // terms are actually keyed to) come from TWO INDEPENDENT compute_cutoffs
  // calls over different populations (output-res bedrock vs. the wider,
  // padded sim-res bedrock) — a pre-existing generator characteristic. When
  // sim_t_hills != t_hills, a band of output-Plains texels sits on the wrong
  // side of the sim classification, and the cone term (proportional to that
  // texel's real distance to sim-plains) spikes there. Symmetrically,
  // carve_cavities stamps conical basins (uncapped depth, scaling with basin
  // size) on sim texels in the bottom lake_frac bedrock quantile — those are
  // sim-Plains too (the cavity carve runs before biome classification), so
  // they're not excluded by a biome check alone.
  //
  // A bedrock-value-proximity heuristic (exclude texels near the quantile
  // cutoffs) does not reliably bound this: a 150-seed sweep found 4/150
  // seeds still leaking mismatched cells past that filter (worst observed
  // hi-lo 15.5 m against a 4.0 m ceiling). The only filter that agrees with
  // the artifacts BY CONSTRUCTION is checking the sim-side classification
  // directly, via the debug sink: keep an output texel iff it is Plains in
  // BOTH the output classification (a.biome) AND the sim classification
  // (biome_sim, captured below), AND it was not carved as a cavity
  // (basins/"cavities" mask, also captured) — the two independent sources of
  // mismatch, gone by definition rather than by measured margin.
  struct BiomeSimCapture : MapDebugSink {
    Field2D<uint8_t> biome_sim;
    Field2D<uint8_t> cavities;
    void dump(std::string_view, int, const Field2D<float>&) override {}
    void dump(std::string_view stage, int, const Field2D<uint8_t>& m) override {
      if (stage == "biome-sim") biome_sim = m;
      if (stage == "cavities") cavities = m;
    }
  };
  for (uint32_t seed : {1u, 2u, 3u}) {
    MapGenParams p;
    p.seed = seed;
    p.resolution = 96;
    p.world_size_m = 384.0f;
    p.erosion.sim_resolution = 96;  // == resolution: padded-sim <-> output texel map is exact
    p.erosion.iterations = 0;
    p.erosion.sediment_noise_m = 0.0f;
    BiomeSimCapture capture;
    const auto a = generate_map(p, &capture);

    // resolution == sim_resolution, so output (x, y) <-> padded-sim
    // (x + kPadTexels, y + kPadTexels) exactly (see generator.cpp's sim-grid
    // sampling: same texel size, origin offset by exactly the pad).
    double sum = 0.0, sum2 = 0.0;
    float lo = std::numeric_limits<float>::infinity();
    float hi = -std::numeric_limits<float>::infinity();
    int n = 0;
    for (int y = 0; y < p.resolution; ++y) {
      for (int x = 0; x < p.resolution; ++x) {
        if (a.biome.at(x, y) != static_cast<uint8_t>(Biome::Plains)) continue;
        const int sx = x + kPadTexels, sy = y + kPadTexels;
        if (capture.biome_sim.at(sx, sy) != static_cast<uint8_t>(Biome::Plains))
          continue;
        if (capture.cavities.at(sx, sy)) continue;
        const float v = a.heightmap.at(x, y);
        sum += v;
        sum2 += static_cast<double>(v) * v;
        lo = std::min(lo, v);
        hi = std::max(hi, v);
        ++n;
      }
    }
    REQUIRE(n > 0);
    const double mean = sum / n;
    const double variance = sum2 / n - mean * mean;
    REQUIRE(variance > 0.0);
    // Envelope measured directly on this by-construction population across a
    // local 150-seed sweep (seeds 1-150; not committed): hi-lo in
    // [1.76, 5.68] m, population size (n) always in the thousands (630 min),
    // zero seeds outside that range. That's wider than the old
    // bedrock-proximity-heuristic filter's [1.34, 3.58] m because this
    // population is honest (no residual sim/output mismatch leaking in a few
    // outlier cells) rather than narrowed by a margin tuned to hide them.
    // [1.0, 6.0] keeps comfortable headroom on both sides of the measured
    // range.
    REQUIRE((hi - lo) >= 1.0f);
    REQUIRE((hi - lo) <= 6.0f);
  }
}

TEST_CASE("generate_map: plains relief term blends smoothly (no biome-cutoff seam)") {
  // Same setup as the variance test, but with the detail filter zeroed:
  // gully_detail_delta is a separate additive layer with its own continuity
  // invariants (detail_filter_tests.cpp) — this test isolates the base
  // relief term's smoothness, not the detail filter's.
  //
  // lake_frac=0 disables carve_cavities: its conical subtraction (uncapped
  // depth, over just a few texels near the bottom bedrock quantile) is a
  // separate, much steeper feature not covered by the Lipschitz argument
  // below, so leaving cavities on makes this bound false in general
  // (measured: ~40% of seeds violate it at the production
  // lake_frac=0.03). With cavities off, the bound genuinely isolates the
  // relief term + cone contribution the comment describes.
  for (uint32_t seed : {1u, 2u, 3u}) {
    MapGenParams p;
    p.seed = seed;
    p.resolution = 96;
    p.world_size_m = 384.0f;
    p.erosion.sim_resolution = 96;  // == resolution: one unambiguous texel size
    p.erosion.iterations = 0;
    p.erosion.detail_octaves = 0;
    p.erosion.lake_frac = 0.0f;
    const auto a = generate_map(p);

    // Mirrors generator.cpp's private kSlopeMPerM (0.75) — the cone term's
    // neighbor step is bounded by kSlopeMPerM * texel (EDT distance changes at
    // most 1 texel per neighbor step); the relief term adds at most its own
    // 2 m amplitude per step (smoothstep is bounded to [0, kPlainsReliefM]).
    const float kSlopeMPerM = 0.75f;
    const float texel = p.world_size_m / static_cast<float>(p.resolution);
    const float bound = kSlopeMPerM * texel + 2.0f;
    for (int y = 0; y < p.resolution; ++y) {
      for (int x = 0; x < p.resolution; ++x) {
        const float h = a.heightmap.at(x, y);
        if (x + 1 < p.resolution)
          REQUIRE(std::abs(h - a.heightmap.at(x + 1, y)) <= bound);
        if (y + 1 < p.resolution)
          REQUIRE(std::abs(h - a.heightmap.at(x, y + 1)) <= bound);
      }
    }
  }
}

TEST_CASE("compute_cutoffs + classify_biomes: exact on a known ramp") {
  // 101 samples 0.00 .. 1.00: rank floor(0.55*100)=55 -> 0.55, rank 88 -> 0.88.
  Field2D<float> ramp(101, 1);
  for (int x = 0; x < 101; ++x) ramp.at(x, 0) = static_cast<float>(x) / 100.0f;
  const BiomeCutoffs c = compute_cutoffs(ramp);
  REQUIRE(c.t_hills == Catch::Approx(0.55f));
  REQUIRE(c.t_mountain == Catch::Approx(0.88f));

  const auto biome = classify_biomes(ramp, c);
  REQUIRE(biome.at(0, 0) == static_cast<uint8_t>(Biome::Plains));
  REQUIRE(biome.at(54, 0) == static_cast<uint8_t>(Biome::Plains));
  REQUIRE(biome.at(55, 0) == static_cast<uint8_t>(Biome::Hills));  // == t_hills
  REQUIRE(biome.at(87, 0) == static_cast<uint8_t>(Biome::Hills));
  REQUIRE(biome.at(88, 0) ==
          static_cast<uint8_t>(Biome::Mountain));  // == t_mountain
  REQUIRE(biome.at(100, 0) == static_cast<uint8_t>(Biome::Mountain));
}

TEST_CASE("generate_map: bedrock is sampled in world meters "
          "(resolution-independent)") {
  MapGenParams lo;
  lo.seed = 5;
  lo.resolution = 64;
  lo.world_size_m = 512.0f;  // 8 m texels
  lo.erosion.sim_resolution = 64;
  lo.erosion.iterations = 8;
  MapGenParams hi = lo;
  hi.resolution = 128;  // 4 m texels
  const auto a = generate_map(lo);
  const auto b = generate_map(hi);
  // Texel (x, y) of the coarse map sits at the same world point as (2x, 2y) of
  // the fine one (world = x * texel_m), so bedrock must agree EXACTLY there —
  // identical float inputs into the same noise.
  for (int y = 0; y < 64; y += 7) {
    for (int x = 0; x < 64; x += 7) {
      REQUIRE(a.bedrock.at(x, y) == b.bedrock.at(2 * x, 2 * y));
    }
  }
}

TEST_CASE("generate_map: degenerate resolution yields empty artifacts, no throw") {
  MapGenParams p;
  p.resolution = 0;
  REQUIRE(generate_map(p).bedrock.size() == 0);
  p.resolution = -1;
  const auto a = generate_map(p);
  REQUIRE(a.bedrock.size() == 0);
  REQUIRE(a.biome.size() == 0);
  REQUIRE(a.heightmap.size() == 0);
  REQUIRE(a.water_depth.size() == 0);
  REQUIRE(a.flow.size() == 0);
  REQUIRE(a.sediment.size() == 0);
  REQUIRE(a.river_class.size() == 0);
  REQUIRE(a.river_graph.edges.empty());
}

TEST_CASE("generate_map: degenerate sim_resolution yields empty artifacts, no throw") {
  MapGenParams p;
  p.erosion.sim_resolution = 0;
  const auto a = generate_map(p);
  REQUIRE(a.bedrock.size() == 0);
  REQUIRE(a.biome.size() == 0);
  REQUIRE(a.heightmap.size() == 0);
  REQUIRE(a.water_depth.size() == 0);
  REQUIRE(a.flow.size() == 0);
  REQUIRE(a.sediment.size() == 0);
  REQUIRE(a.river_class.size() == 0);
  REQUIRE(a.river_graph.edges.empty());
}

TEST_CASE("generate_map: sim_resolution != resolution (resample/crop/units seam)") {
  MapGenParams p;
  p.resolution = 64;
  p.world_size_m = 256.0f;
  p.erosion.sim_resolution = 32;
  p.erosion.iterations = 8;
  const auto a = generate_map(p);

  REQUIRE(a.bedrock.width == 64);
  REQUIRE(a.bedrock.height == 64);
  REQUIRE(a.biome.width == 64);
  REQUIRE(a.biome.height == 64);
  REQUIRE(a.heightmap.width == 64);
  REQUIRE(a.heightmap.height == 64);
  REQUIRE(a.water_depth.width == 64);
  REQUIRE(a.water_depth.height == 64);
  REQUIRE(a.flow.width == 64);
  REQUIRE(a.flow.height == 64);
  REQUIRE(a.sediment.width == 64);
  REQUIRE(a.sediment.height == 64);
  REQUIRE(a.river_class.width == 64);
  REQUIRE(a.river_class.height == 64);

  for (float v : a.heightmap.data) REQUIRE(std::isfinite(v));
  for (float v : a.flow.data) REQUIRE(v > 0.0f);
  for (float v : a.sediment.data) REQUIRE(v >= 0.0f);
  for (uint8_t v : a.river_class.data) REQUIRE(v < badlands::mapgen::kRiverClassCount);
  for (float v : a.river_discharge_m3_s.data) REQUIRE(v >= 0.0f);
  // Lake covers exactly the water — no threshold band any more. The dry part
  // of a carved basin is left to classify_biomes (Plains, at bedrock minima),
  // which is what gives lakes a coast.
  for (size_t i = 0; i < a.biome.data.size(); ++i) {
    const bool is_lake = a.biome.data[i] == static_cast<uint8_t>(Biome::Lake);
    REQUIRE(is_lake == (a.water_depth.data[i] > 0.0f));
  }
}

TEST_CASE("generate_map: river artifact — output-res dims, some signal, "
          "values in [0,1]") {
  for (uint32_t seed : {1u, 2u}) {
    MapGenParams p;
    p.seed = seed;
    p.resolution = 128;
    p.world_size_m = 512.0f;
    p.erosion.sim_resolution = 128;
    // production erosion defaults otherwise (thresholds, iterations)
    const auto a = generate_map(p);
    REQUIRE(a.river_class.width == p.resolution);
    REQUIRE(a.river_class.height == p.resolution);
    bool any_positive = false;
    for (uint8_t v : a.river_class.data) {
      REQUIRE(v < badlands::mapgen::kRiverClassCount);
      if (v != 0) any_positive = true;
    }
    REQUIRE(any_positive);
  }
}

TEST_CASE("generate_map: the river survives a coarse output resolution") {
  // Replaces the old max-pool crispness test. That test guarded a real
  // property — a thin river must not be diluted away when the output grid is
  // coarser than the sim — but guarded it at the resample step, which no
  // longer exists: the network is now rasterized conservatively from
  // world-space geometry straight onto the output grid.
  //
  // Same seed / world / sim_resolution in both calls, so the sim and the
  // extracted GRAPH are identical and only rasterization differs.
  MapGenParams p;
  p.seed = 3;
  p.world_size_m = 256.0f;
  p.erosion.sim_resolution = 64;
  p.erosion.iterations = 8;
  p.erosion.min_channel_area_m2 = 20.0f;

  MapGenParams matched = p;
  matched.resolution = 64;
  const auto fine = generate_map(matched);
  MapGenParams coarse = p;
  coarse.resolution = 16;  // 4x coarser output
  const auto rough = generate_map(coarse);

  // The graph is resolution-independent by construction.
  REQUIRE(fine.river_graph.edges.size() == rough.river_graph.edges.size());
  REQUIRE(!fine.river_graph.edges.empty());

  auto top_class = [](const Field2D<uint8_t>& c) {
    uint8_t m = 0;
    for (uint8_t v : c.data) m = std::max(m, v);
    return m;
  };
  auto covered = [](const Field2D<uint8_t>& c) {
    int n = 0;
    for (uint8_t v : c.data) n += v != 0 ? 1 : 0;
    return n;
  };
  REQUIRE(top_class(fine.river_class) > 0);
  // The strongest reach keeps its tier at the coarser resolution — a diluting
  // resample would have washed it down a class or dropped it entirely.
  REQUIRE(top_class(rough.river_class) == top_class(fine.river_class));
  REQUIRE(covered(rough.river_class) > 0);
}

namespace {
// Brute-force oracle: the definition — min over all plains texels of the
// world-space Euclidean distance, double precision, O(n^2).
Field2D<float> brute_distance(const Field2D<uint8_t>& biome, glm::vec2 texel) {
  Field2D<float> out(biome.width, biome.height, 0.0f);
  for (int y = 0; y < biome.height; ++y) {
    for (int x = 0; x < biome.width; ++x) {
      double best = 1e30;
      for (int py = 0; py < biome.height; ++py) {
        for (int px = 0; px < biome.width; ++px) {
          if (biome.at(px, py) != static_cast<uint8_t>(Biome::Plains)) continue;
          const double dx = (x - px) * static_cast<double>(texel.x);
          const double dy = (y - py) * static_cast<double>(texel.y);
          best = std::min(best, dx * dx + dy * dy);
        }
      }
      out.at(x, y) = best < 1e30 ? static_cast<float>(std::sqrt(best)) : 0.0f;
    }
  }
  return out;
}
}  // namespace

TEST_CASE(
    "distance_to_plains: matches the brute-force oracle (incl. anisotropic "
    "texels)") {
  // Deterministic scattered plains pattern on a 17x11 grid.
  Field2D<uint8_t> biome(17, 11, static_cast<uint8_t>(Biome::Hills));
  for (int y = 0; y < 11; ++y)
    for (int x = 0; x < 17; ++x)
      if ((x * 7 + y * 13) % 9 == 0)
        biome.at(x, y) = static_cast<uint8_t>(Biome::Plains);
  for (glm::vec2 texel : {glm::vec2(1.0f, 1.0f), glm::vec2(2.0f, 0.5f)}) {
    const auto edt = distance_to_plains(biome, texel);
    const auto ref = brute_distance(biome, texel);
    // Power-of-two texels: every double op is exact, so exact float equality.
    REQUIRE(edt.data == ref.data);
  }
}

TEST_CASE("distance_to_plains: single plains texel gives the radial cone") {
  Field2D<uint8_t> biome(7, 7, static_cast<uint8_t>(Biome::Mountain));
  biome.at(3, 3) = static_cast<uint8_t>(Biome::Plains);
  const auto d = distance_to_plains(biome, {2.0f, 2.0f});
  for (int y = 0; y < 7; ++y) {
    for (int x = 0; x < 7; ++x) {
      const double dx = 2.0 * (x - 3), dy = 2.0 * (y - 3);
      REQUIRE(d.at(x, y) == static_cast<float>(std::sqrt(dx * dx + dy * dy)));
    }
  }
}

TEST_CASE("distance_to_plains: world-metric across resolutions (units guard)") {
  // Same world layout — plains where world_x < 128 m — sampled at 4 m and
  // 2 m texels. Distances at coinciding world points agree within one COARSE
  // texel (plains-boundary discretization); a texel-unit implementation would
  // be off by 2x and fail loudly.
  auto make = [](int w, int h, float texel) {
    Field2D<uint8_t> b(w, h, static_cast<uint8_t>(Biome::Hills));
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x)
        if (static_cast<float>(x) * texel < 128.0f)
          b.at(x, y) = static_cast<uint8_t>(Biome::Plains);
    return b;
  };
  const auto lo = distance_to_plains(make(64, 16, 4.0f), {4.0f, 4.0f});
  const auto hi = distance_to_plains(make(128, 32, 2.0f), {2.0f, 2.0f});
  for (int x = 40; x < 64; ++x) {  // well inside the non-plains half
    REQUIRE(lo.at(x, 8) == Catch::Approx(hi.at(2 * x, 16)).margin(4.0));
  }
}

TEST_CASE("distance_to_plains: no plains at all -> all zeros") {
  Field2D<uint8_t> biome(5, 4, static_cast<uint8_t>(Biome::Mountain));
  const auto d = distance_to_plains(biome, {1.0f, 1.0f});
  REQUIRE(d.data == std::vector<float>(20, 0.0f));
}

TEST_CASE("distance_to_mask: matches distance_to_plains on a plains mask") {
  Field2D<uint8_t> biome(17, 11, static_cast<uint8_t>(Biome::Hills));
  Field2D<uint8_t> mask(17, 11, 0);
  for (int y = 0; y < 11; ++y)
    for (int x = 0; x < 17; ++x)
      if ((x * 7 + y * 13) % 9 == 0) {
        biome.at(x, y) = static_cast<uint8_t>(Biome::Plains);
        mask.at(x, y) = 1;
      }
  REQUIRE(badlands::mapgen::distance_to_mask(mask, {1.0f, 1.0f}).data ==
          distance_to_plains(biome, {1.0f, 1.0f}).data);
}
