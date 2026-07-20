// Pure-CPU tests for the frozen MapData contract and the biome blending that
// turns its slices into per-vertex weights. Synthetic fixtures only -- never
// pinned to the production map, so these stay valid as generation changes.
//
// The headline regression: a STRAIGHT biome border must produce a smooth,
// monotonic, orientation-independent ramp -- not the staircase of triangular
// wedges the old nearest-neighbour one-hot sampling produced.

#include <algorithm>
#include <cmath>
#include <vector>

#include <glm/glm.hpp>

#include "catch_amalgamated.hpp"
#include "game/geometry/terrain_mesh.hpp"
#include "game/map/map_data.hpp"

using badlands::BiomeWeights;
using badlands::BuildTerrainMesh;
using badlands::kBiomeSliceCount;
using badlands::MapData;
using badlands::ResolveVertexBlend;
using badlands::TerrainMesh;
using badlands::VertexBlend;
using badlands::mapgen::Biome;

namespace {

constexpr float kSpacing = 4.0f;

// Unpack one kTerrainBlend vertex.
struct Vertex {
  glm::vec3 pos;
  glm::vec3 normal;
  uint8_t layers[4];
  uint8_t weights[4];
};

Vertex Unpack(const std::vector<float>& v, uint32_t i) {
  const float* p = v.data() + static_cast<size_t>(i) * TerrainMesh::kFloatsPerVertex;
  Vertex out;
  out.pos = {p[0], p[1], p[2]};
  out.normal = {p[3], p[4], p[5]};
  uint32_t li, wi;
  std::memcpy(&li, &p[6], 4);
  std::memcpy(&wi, &p[7], 4);
  for (int k = 0; k < 4; ++k) {
    out.layers[k] = static_cast<uint8_t>((li >> (8 * k)) & 0xFF);
    out.weights[k] = static_cast<uint8_t>((wi >> (8 * k)) & 0xFF);
  }
  return out;
}

// Weight of `biome` in an unpacked vertex, as a 0..1 fraction.
float WeightOf(const Vertex& v, Biome biome) {
  for (int k = 0; k < 4; ++k) {
    if (v.weights[k] > 0 && v.layers[k] == static_cast<uint8_t>(biome)) {
      return static_cast<float>(v.weights[k]) / 255.0f;
    }
  }
  return 0.0f;
}

// A map whose slices encode a soft border: biome `a` on the negative side of the
// line through the origin with normal `n`, biome `b` on the positive side, with a
// linear ramp of half-width `feather` metres across it. Heights are flat.
MapData MakeBorderMap(int nodes, Biome a, Biome b, glm::vec2 n, float feather) {
  n = glm::normalize(n);
  MapData map(nodes, nodes, kSpacing);
  const float mid = 0.5f * static_cast<float>(nodes - 1) * kSpacing;
  for (int j = 0; j < nodes; ++j) {
    for (int i = 0; i < nodes; ++i) {
      const glm::vec2 p(static_cast<float>(i) * kSpacing - mid,
                        static_cast<float>(j) * kSpacing - mid);
      const float d = glm::dot(p, n);                       // signed distance
      const float t = std::clamp(0.5f + 0.5f * d / feather, 0.0f, 1.0f);
      map.mutable_slice(static_cast<int>(a), i, j) =
          static_cast<uint8_t>(std::lround((1.0f - t) * 255.0f));
      map.mutable_slice(static_cast<int>(b), i, j) =
          static_cast<uint8_t>(std::lround(t * 255.0f));
    }
  }
  return map;
}

BiomeWeights MakeWeights(std::initializer_list<std::pair<Biome, float>> in) {
  BiomeWeights w;
  for (auto [b, v] : in) w.w[static_cast<int>(b)] = v;
  return w;
}

int NonZeroSlots(const VertexBlend& vb) {
  int n = 0;
  for (int k = 0; k < 4; ++k)
    if (vb.weights[k] > 0) ++n;
  return n;
}

int WeightSum(const VertexBlend& vb) {
  int s = 0;
  for (int k = 0; k < 4; ++k) s += vb.weights[k];
  return s;
}

}  // namespace

// ---------------------------------------------------------------- MapData ---

TEST_CASE("MapData: bilinear height query, edge clamped") {
  MapData map(3, 3, kSpacing);  // 8 m x 8 m
  for (int j = 0; j < 3; ++j)
    for (int i = 0; i < 3; ++i)
      map.mutable_height(i, j) = static_cast<float>(i);  // ramp along +x

  CHECK(map.size_x_m() == Catch::Approx(8.0f));
  CHECK(map.HeightAt(0.0f, 0.0f) == Catch::Approx(0.0f));
  CHECK(map.HeightAt(4.0f, 0.0f) == Catch::Approx(1.0f));
  CHECK(map.HeightAt(2.0f, 0.0f) == Catch::Approx(0.5f));   // interpolated
  CHECK(map.HeightAt(6.0f, 4.0f) == Catch::Approx(1.5f));
  // Off-map clamps to the edge rather than failing.
  CHECK(map.HeightAt(-10.0f, 0.0f) == Catch::Approx(0.0f));
  CHECK(map.HeightAt(999.0f, 0.0f) == Catch::Approx(2.0f));
}

TEST_CASE("MapData: biome query normalizes, interpolates, and reports dominant") {
  MapData map(2, 2, kSpacing);
  // Left nodes fully Forest, right nodes fully Swamp -- note the raw slice
  // values differ in scale, so this also proves the query normalizes.
  for (int j = 0; j < 2; ++j) {
    map.mutable_slice(static_cast<int>(Biome::Forest), 0, j) = 200;
    map.mutable_slice(static_cast<int>(Biome::Swamp), 1, j) = 100;
  }
  const BiomeWeights left = map.BiomesAt(0.0f, 0.0f);
  CHECK(left.w[static_cast<int>(Biome::Forest)] == Catch::Approx(1.0f));
  CHECK(left.Sum() == Catch::Approx(1.0f));
  CHECK(left.Dominant() == Biome::Forest);
  CHECK(map.DominantBiomeAt(4.0f, 0.0f) == Biome::Swamp);

  // Halfway the two raw slices are 100 vs 50 -> 2/3 Forest after normalizing.
  const BiomeWeights mid = map.BiomesAt(2.0f, 0.0f);
  CHECK(mid.Sum() == Catch::Approx(1.0f));
  CHECK(mid.w[static_cast<int>(Biome::Forest)] == Catch::Approx(2.0f / 3.0f));
  CHECK(mid.Dominant() == Biome::Forest);
}

TEST_CASE("MapData: no coverage yields all-zero weights, never NaN") {
  MapData map(2, 2, kSpacing);  // slices all zero
  const BiomeWeights w = map.BiomesAt(1.0f, 1.0f);
  CHECK(w.Sum() == Catch::Approx(0.0f));
  for (float v : w.w) CHECK(std::isfinite(v));
  CHECK(map.WeightsAtNode(0, 0).Sum() == Catch::Approx(0.0f));
}

// ------------------------------------------------- ResolveVertexBlend ------

TEST_CASE("blend: a single biome packs to one full-weight slot") {
  const VertexBlend vb = ResolveVertexBlend(MakeWeights({{Biome::Plains, 1.0f}}));
  CHECK(vb.layers[0] == static_cast<uint8_t>(Biome::Plains));
  CHECK(vb.weights[0] == 255);
  CHECK(NonZeroSlots(vb) == 1);
  // Padding discipline: slot index 0 is a REAL layer (Lake), so unused slots
  // must carry weight 0 or they would silently blend Lake in.
  for (int k = 1; k < 4; ++k) CHECK(vb.weights[k] == 0);
}

TEST_CASE("blend: weights are normalized and sum to exactly 255") {
  // Deliberately unnormalized input, and a 3-way split that does not divide
  // evenly into 255 -- the largest-remainder pass must still land on 255.
  const VertexBlend vb = ResolveVertexBlend(MakeWeights(
      {{Biome::Forest, 2.0f}, {Biome::Swamp, 2.0f}, {Biome::Plains, 2.0f}}));
  CHECK(NonZeroSlots(vb) == 3);
  CHECK(WeightSum(vb) == 255);
}

TEST_CASE("blend: keeps the 4 strongest when 5 or 6 biomes meet") {
  // Six biomes at one point, all distinct weights.
  const BiomeWeights six = MakeWeights({{Biome::Lake, 0.30f},
                                        {Biome::Swamp, 0.25f},
                                        {Biome::Forest, 0.20f},
                                        {Biome::Plains, 0.15f},
                                        {Biome::Hills, 0.07f},
                                        {Biome::Mountain, 0.03f}});
  const VertexBlend vb = ResolveVertexBlend(six);

  CHECK(NonZeroSlots(vb) == 4);          // capped by the vertex format
  CHECK(WeightSum(vb) == 255);           // still a normalized blend

  // Exactly the top four survived, and the two smallest were dropped.
  std::vector<uint8_t> kept;
  for (int k = 0; k < 4; ++k) kept.push_back(vb.layers[k]);
  auto has = [&](Biome b) {
    return std::find(kept.begin(), kept.end(),
                     static_cast<uint8_t>(b)) != kept.end();
  };
  CHECK(has(Biome::Lake));
  CHECK(has(Biome::Swamp));
  CHECK(has(Biome::Forest));
  CHECK(has(Biome::Plains));
  CHECK_FALSE(has(Biome::Hills));
  CHECK_FALSE(has(Biome::Mountain));

  // The five-biome case likewise drops exactly the smallest.
  const VertexBlend five = ResolveVertexBlend(MakeWeights({{Biome::Lake, 0.4f},
                                                           {Biome::Swamp, 0.3f},
                                                           {Biome::Forest, 0.2f},
                                                           {Biome::Plains, 0.09f},
                                                           {Biome::Hills, 0.01f}}));
  CHECK(NonZeroSlots(five) == 4);
  CHECK(WeightSum(five) == 255);
  for (int k = 0; k < 4; ++k)
    CHECK(five.layers[k] != static_cast<uint8_t>(Biome::Hills));
}

TEST_CASE("blend: ties resolve deterministically to the lower biome index") {
  const BiomeWeights tied = MakeWeights({{Biome::Lake, 0.25f},
                                         {Biome::Swamp, 0.25f},
                                         {Biome::Forest, 0.25f},
                                         {Biome::Plains, 0.25f},
                                         {Biome::Hills, 0.25f}});
  const VertexBlend a = ResolveVertexBlend(tied);
  const VertexBlend b = ResolveVertexBlend(tied);
  for (int k = 0; k < 4; ++k) {
    CHECK(a.layers[k] == b.layers[k]);      // stable across calls
    CHECK(a.weights[k] == b.weights[k]);
  }
  // Hills (index 4) is the highest index, so it is the one dropped.
  for (int k = 0; k < 4; ++k)
    CHECK(a.layers[k] != static_cast<uint8_t>(Biome::Hills));
}

TEST_CASE("blend: zero coverage produces all-zero weights") {
  const VertexBlend vb = ResolveVertexBlend(BiomeWeights{});
  CHECK(NonZeroSlots(vb) == 0);
  CHECK(WeightSum(vb) == 0);
}

// ---------------------------------------------- straight-border blending ---

TEST_CASE("a straight biome border blends smoothly at every angle") {
  // The regression test for the staircase: for borders at a range of angles,
  // the rendered weight must depend ONLY on perpendicular distance to the
  // border -- never on where a vertex happens to sit in the X-split lattice.
  const float kAngles[] = {0.0f, 30.0f, 45.0f, 63.4f, 90.0f};
  constexpr int kNodes = 21;
  // Wide feather so the ramp stays LINEAR across a big band around the border.
  // That matters: a centre vertex is the average of its 4 corners, which equals
  // a direct sample only where the underlying field is linear. Across the
  // clamp knee the two legitimately differ, so the smoothness assertions below
  // are restricted to the linear band (purity is checked outside it instead).
  const float feather = 6.0f * kSpacing;
  const float linear_band = feather - 1.5f * kSpacing;

  for (float deg : kAngles) {
    INFO("border angle " << deg << " degrees");
    const float rad = deg * 3.14159265f / 180.0f;
    const glm::vec2 n(std::cos(rad), std::sin(rad));
    const MapData map =
        MakeBorderMap(kNodes, Biome::Forest, Biome::Swamp, n, feather);
    const TerrainMesh mesh = BuildTerrainMesh(map);
    REQUIRE(mesh.vertex_count > 0);

    const float mid = 0.5f * static_cast<float>(kNodes - 1) * kSpacing;
    std::vector<std::pair<float, float>> samples;  // (signed distance, forest w)
    float most_forest = 0.0f, most_swamp = 0.0f;   // deep on either side

    for (uint32_t i = 0; i < mesh.vertex_count; ++i) {
      const Vertex v = Unpack(mesh.vertices, i);
      const float d = glm::dot(glm::vec2(v.pos.x - mid, v.pos.z - mid), n);
      const float fw = WeightOf(v, Biome::Forest);
      const float sw = WeightOf(v, Biome::Swamp);

      // Only the two biomes actually present ever appear.
      CHECK(fw + sw == Catch::Approx(1.0f).margin(2.0f / 255.0f));
      if (d <= -feather) most_forest = std::max(most_forest, fw);
      if (d >= feather) most_swamp = std::max(most_swamp, sw);
      if (std::abs(d) <= linear_band) samples.emplace_back(d, fw);
    }
    REQUIRE(samples.size() > 8);

    // Orientation independence: equal perpendicular distance => equal weight,
    // regardless of lattice position. Bucket by distance and check spread.
    std::sort(samples.begin(), samples.end(),
              [](auto& a, auto& b) { return a.first < b.first; });
    for (size_t i = 1; i < samples.size(); ++i) {
      if (std::abs(samples[i].first - samples[i - 1].first) < 1e-3f) {
        INFO("same distance " << samples[i].first << " gave different weights");
        REQUIRE(std::abs(samples[i].second - samples[i - 1].second) <=
                2.0f / 255.0f);
      }
    }

    // Monotonic: weight never rises as we move to the swamp side. A staircase
    // (or triangle-orientation bias) would break this.
    for (size_t i = 1; i < samples.size(); ++i) {
      INFO("non-monotonic at d=" << samples[i].first);
      REQUIRE(samples[i].second <= samples[i - 1].second + 2.0f / 255.0f);
    }

    // Beyond the feather the blend saturates to a pure biome on each side.
    CHECK(most_forest == Catch::Approx(1.0f).margin(0.02f));
    CHECK(most_swamp == Catch::Approx(1.0f).margin(0.02f));

    // And the ramp really does span the range across the band (it is a blend,
    // not a hard cut sitting at one value).
    CHECK(samples.front().second - samples.back().second > 0.5f);
  }
}

// ------------------------------------------------- junctions & centres -----

TEST_CASE("3- and 4-biome junctions keep every biome present") {
  // Quadrants meeting at the map centre.
  constexpr int kNodes = 13;
  const Biome quad[4] = {Biome::Forest, Biome::Swamp, Biome::Plains,
                         Biome::Hills};

  for (int biome_count : {3, 4}) {
    INFO("junction of " << biome_count << " biomes");
    MapData map(kNodes, kNodes, kSpacing);
    const float mid = 0.5f * static_cast<float>(kNodes - 1) * kSpacing;
    for (int j = 0; j < kNodes; ++j) {
      for (int i = 0; i < kNodes; ++i) {
        const float x = static_cast<float>(i) * kSpacing - mid;
        const float z = static_cast<float>(j) * kSpacing - mid;
        int q = (x >= 0 ? 1 : 0) + (z >= 0 ? 2 : 0);
        if (q >= biome_count) q = biome_count - 1;  // fold to 3 biomes
        map.mutable_slice(static_cast<int>(quad[q]), i, j) = 255;
      }
    }

    const TerrainMesh mesh = BuildTerrainMesh(map);
    REQUIRE(mesh.vertex_count > 0);

    std::vector<int> seen(kBiomeSliceCount, 0);
    for (uint32_t i = 0; i < mesh.vertex_count; ++i) {
      const Vertex v = Unpack(mesh.vertices, i);
      int sum = 0;
      for (int k = 0; k < 4; ++k) {
        sum += v.weights[k];
        if (v.weights[k] > 0) seen[v.layers[k]] = 1;
      }
      REQUIRE(sum == 255);  // every vertex is a fully normalized blend
    }
    for (int q = 0; q < biome_count; ++q) {
      INFO("biome " << static_cast<int>(quad[q]) << " missing");
      CHECK(seen[static_cast<int>(quad[q])] == 1);
    }
    // Each quadrant's interior is still pure.
    CHECK(map.DominantBiomeAt(mid * 0.5f + mid, mid * 0.5f + mid) ==
          quad[biome_count - 1]);
  }
}

TEST_CASE("cell centre vertex is the average of its 4 corners") {
  // The rule: the centre is DERIVED, so the 4 triangles of a cell agree and no
  // independent centre sample can disagree with its corners.
  constexpr int kNodes = 5;
  MapData map =
      MakeBorderMap(kNodes, Biome::Forest, Biome::Swamp, glm::vec2(1, 0), 8.0f);
  for (int j = 0; j < kNodes; ++j)
    for (int i = 0; i < kNodes; ++i)
      map.mutable_height(i, j) = static_cast<float>(i * 2 + j);

  const TerrainMesh mesh = BuildTerrainMesh(map);
  const int cells = kNodes - 1;
  const int corner_count = kNodes * kNodes;
  REQUIRE(mesh.vertex_count ==
          static_cast<uint32_t>(corner_count + cells * cells));

  for (int cz = 0; cz < cells; ++cz) {
    for (int cx = 0; cx < cells; ++cx) {
      const Vertex c = Unpack(mesh.vertices,
                              static_cast<uint32_t>(corner_count + cz * cells + cx));
      const float expect_h =
          0.25f * (map.height(cx, cz) + map.height(cx + 1, cz) +
                   map.height(cx + 1, cz + 1) + map.height(cx, cz + 1));
      INFO("cell " << cx << "," << cz);
      CHECK(c.pos.y == Catch::Approx(expect_h));
      CHECK(c.pos.x == Catch::Approx((cx + 0.5f) * kSpacing));
      CHECK(c.pos.z == Catch::Approx((cz + 0.5f) * kSpacing));

      // Biome weights are the average of the corners' normalized weights.
      float expect_forest = 0.0f;
      for (auto [dx, dz] : {std::pair{0, 0}, std::pair{1, 0}, std::pair{1, 1},
                            std::pair{0, 1}}) {
        expect_forest +=
            map.WeightsAtNode(cx + dx, cz + dz).w[static_cast<int>(Biome::Forest)];
      }
      expect_forest *= 0.25f;
      CHECK(WeightOf(c, Biome::Forest) ==
            Catch::Approx(expect_forest).margin(2.0f / 255.0f));
    }
  }
}

TEST_CASE("shared vertices carry one blend, so triangles cannot disagree") {
  const MapData map = MakeBorderMap(9, Biome::Forest, Biome::Swamp,
                                    glm::vec2(1, 1), 3.0f * kSpacing);
  const TerrainMesh mesh = BuildTerrainMesh(map);
  REQUIRE_FALSE(mesh.indices.empty());

  // The mesh is indexed: every triangle referencing a vertex reads the same
  // record, so a shared corner (up to 4 cells / 8 triangles) has exactly one
  // normalized blend. Assert every referenced index is in range and that no two
  // vertices share a position with differing weights.
  for (uint32_t idx : mesh.indices) REQUIRE(idx < mesh.vertex_count);

  std::vector<Vertex> verts;
  verts.reserve(mesh.vertex_count);
  for (uint32_t i = 0; i < mesh.vertex_count; ++i)
    verts.push_back(Unpack(mesh.vertices, i));
  for (size_t a = 0; a < verts.size(); ++a) {
    for (size_t b = a + 1; b < verts.size(); ++b) {
      if (glm::distance(verts[a].pos, verts[b].pos) > 1e-4f) continue;
      for (int k = 0; k < 4; ++k) {
        INFO("duplicated position with differing blend");
        REQUIRE(verts[a].weights[k] == verts[b].weights[k]);
        REQUIRE(verts[a].layers[k] == verts[b].layers[k]);
      }
    }
  }
}
