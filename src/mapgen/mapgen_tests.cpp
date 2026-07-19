// Unit tests for the deterministic mapgen analysis functions (block reduction,
// section flood-fill + graph, biome classification, height composition). These
// run on TINY synthetic patches — never full resolution.

#include <catch_amalgamated.hpp>

#include <algorithm>

#include <vector>

#include "mapgen/biome_assign.hpp"
#include "mapgen/config.hpp"
#include "mapgen/field2d.hpp"
#include "mapgen/heightmap.hpp"
#include "mapgen/mapgen_constants.hpp"
#include "mapgen/sections.hpp"

using namespace badlands::mapgen;

namespace {

// Build a Block grid directly from a row-major list of heights (biome = Plains).
Field2D<Block> make_blocks(const std::vector<std::vector<float>>& rows) {
  const int R = static_cast<int>(rows.size());
  const int C = static_cast<int>(rows[0].size());
  Field2D<Block> g(C, R);
  for (int y = 0; y < R; ++y)
    for (int x = 0; x < C; ++x) {
      g.at(x, y).height = rows[y][x];
      g.at(x, y).biome = static_cast<uint8_t>(Biome::Plains);
    }
  return g;
}

}  // namespace

TEST_CASE("reduce_to_blocks: median is outlier-robust; biome is footprint majority") {
  // 2x1 blocks, whatever kSamplesPerBlock currently is.
  Field2D<float> height(2 * kSamplesPerBlock, kSamplesPerBlock);
  Field2D<uint8_t> biome(2 * kSamplesPerBlock, kSamplesPerBlock);
  // Spike ONE column, partially: the point is that the spikes stay a strict
  // minority of the block's footprint, so the median rejects them while a mean
  // would not. Derived from kSamplesPerBlock -- a literal row count silently
  // assumes a block big enough for that to still be a minority, which is
  // exactly the assumption that breaks when the block size changes.
  const int spike_rows = std::max(1, kSamplesPerBlock / 2);
  for (int y = 0; y < height.height; ++y) {
    for (int x = 0; x < height.width; ++x) {
      const bool left = x < kSamplesPerBlock;
      // Left block: flat 5.0, all Plains. Right block: 3.0 with a few 100.0
      // spikes, mostly Forest with a couple Lake.
      float h = left ? 5.0f : 3.0f;
      uint8_t b = left ? static_cast<uint8_t>(Biome::Plains)
                       : static_cast<uint8_t>(Biome::Forest);
      if (!left && x == kSamplesPerBlock && y < spike_rows) {
        h = 100.0f;                                    // outliers
        b = static_cast<uint8_t>(Biome::Lake);         // minority biome
      }
      height.at(x, y) = h;
      biome.at(x, y) = b;
    }
  }

  Field2D<Block> blocks = reduce_to_blocks(height, biome, /*median=*/true);
  REQUIRE(blocks.width == 2);
  REQUIRE(blocks.height == 1);
  CHECK(blocks.at(0, 0).height == Catch::Approx(5.0f));
  CHECK(blocks.at(1, 0).height == Catch::Approx(3.0f));  // median ignores spikes
  CHECK(blocks.at(0, 0).biome == static_cast<uint8_t>(Biome::Plains));
  CHECK(blocks.at(1, 0).biome == static_cast<uint8_t>(Biome::Forest));
}

TEST_CASE("extract_sections: a step above section_step splits into two sections") {
  Field2D<Block> g = make_blocks({{1.0f, 1.2f, 3.0f, 3.1f}});
  SectionGraph graph = extract_sections(g, /*section_step=*/0.5f,
                                        /*min_section_blocks=*/1);

  REQUIRE(graph.nodes.size() == 2);
  // Blocks 0,1 share a section; 2,3 share the other.
  CHECK(g.at(0, 0).section_id == g.at(1, 0).section_id);
  CHECK(g.at(2, 0).section_id == g.at(3, 0).section_id);
  CHECK(g.at(1, 0).section_id != g.at(2, 0).section_id);

  REQUIRE(graph.edges.size() == 1);
  CHECK(graph.edges[0].border_len == 1);
  CHECK(graph.edges[0].height_step == Catch::Approx(1.8f));  // |1.2 - 3.0|
}

TEST_CASE("extract_sections: gentle drift within section_step stays one section") {
  // Each adjacent step is 0.4 <= 0.5, so the whole run is one section even
  // though the ends differ by 1.2.
  Field2D<Block> g = make_blocks({{0.0f, 0.4f, 0.8f, 1.2f}});
  SectionGraph graph = extract_sections(g, 0.5f, 1);
  CHECK(graph.nodes.size() == 1);
  CHECK(graph.edges.empty());
  CHECK(graph.nodes[0].block_count == 4);
}

TEST_CASE("extract_sections: a sub-min-size speck merges into its neighbor") {
  // 3x3 flat plateau at 0 with a single spike block in the center. The spike is
  // its own 1-block section, below min_section_blocks, so it merges away.
  Field2D<Block> g = make_blocks({{0.0f, 0.0f, 0.0f},
                                  {0.0f, 5.0f, 0.0f},
                                  {0.0f, 0.0f, 0.0f}});
  SectionGraph graph = extract_sections(g, /*section_step=*/0.5f,
                                        /*min_section_blocks=*/2);
  REQUIRE(graph.nodes.size() == 1);
  CHECK(graph.edges.empty());
  CHECK(graph.nodes[0].block_count == 9);
}

TEST_CASE("classify_biome: Whittaker split by elevation then moisture") {
  MapgenConfig c;  // elev_lake 0.30, elev_high 0.62, moisture_wet 0.50
  CHECK(classify_biome(0.10f, 0.70f, c) == Biome::Lake);    // low + wet
  CHECK(classify_biome(0.10f, 0.20f, c) == Biome::Swamp);   // low + dry
  CHECK(classify_biome(0.50f, 0.70f, c) == Biome::Forest);  // mid + wet
  CHECK(classify_biome(0.50f, 0.20f, c) == Biome::Plains);  // mid + dry
  CHECK(classify_biome(0.90f, 0.20f, c) == Biome::Hills);   // high
  CHECK(classify_biome(0.90f, 0.90f, c) == Biome::Hills);   // high, moisture-agnostic
}

TEST_CASE("compose_height: relief is snapped to terrace levels") {
  MapgenConfig c;
  c.height_scale_m = 10.0f;
  c.terrace_step_m = 1.0f;
  c.cavity_depth_m = 0.0f;
  c.hills_ridge_m = 6.0f;
  for (auto& v : c.variation_amp_m) v = 0.0f;  // isolate the terracing

  // Plains, elevation 0.37 -> base 3.7 -> nearest terrace level 4.0.
  CHECK(compose_height(c, Biome::Plains, 0.37f, 0.0f, 0.0f) ==
        Catch::Approx(4.0f));
  // Hills fold ridged into the relief before quantizing: 3.7 + (1.0-0.5)*6 =
  // 6.7 -> 7.0.
  CHECK(compose_height(c, Biome::Hills, 0.37f, 1.0f, 0.0f) ==
        Catch::Approx(7.0f));
}
