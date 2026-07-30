#include <catch_amalgamated.hpp>
#include <algorithm>
#include <cmath>
#include <iterator>  // std::size
#include <glm/gtc/constants.hpp>  // glm::pi
#include "game/geometry/tree_options.hpp"
#include "game/geometry/tree_generator.hpp"
#include "game/geometry/leaf_texture.hpp"
#include "engine/rendering/geometry/textured_mesh_builders.hpp"

using namespace badlands;

TEST_CASE("OakPreset and PinePreset carry the expected ez-tree values") {
  const TreeOptions oak = OakPreset();
  REQUIRE(oak.seed == 35729u);
  REQUIRE(oak.type == TreeType::Deciduous);
  REQUIRE(oak.levels == 3);
  REQUIRE(oak.children[0] == 6);
  REQUIRE(oak.sections[0] == 8);

  const TreeOptions pine = PinePreset();
  REQUIRE(pine.seed == 13977u);
  REQUIRE(pine.type == TreeType::Evergreen);
  REQUIRE(pine.levels == 1);
  REQUIRE(pine.children[0] == 82);
}

TEST_CASE("BuildTreeSkeleton: deterministic branch structure") {
  const auto oak = BuildTreeSkeleton(OakPreset());
  // Structure is fixed by recursion (1 continuation + children[level] per branch):
  // L0=1, L1=7, L2=35, L3=140 -> 183.
  REQUIRE(oak.size() == 183u);
  // Evergreen: trunk + children[0] radial, no continuation.
  const auto pine = BuildTreeSkeleton(PinePreset());
  REQUIRE(pine.size() == 83u);
}

TEST_CASE("BuildTreeSkeleton: trunk rooted at origin, tapers, deterministic") {
  const auto a = BuildTreeSkeleton(OakPreset());
  const auto b = BuildTreeSkeleton(OakPreset());
  REQUIRE(a.size() == b.size());

  const SkeletonBranch& trunk = a[0];
  REQUIRE(trunk.sections.size() == static_cast<size_t>(OakPreset().sections[0] + 1));
  REQUIRE(glm::length(trunk.sections.front().origin) == Catch::Approx(0.0f));
  // Base wider than tip.
  REQUIRE(trunk.sections.front().radius > trunk.sections.back().radius);
  // Run-twice identical (determinism): compare a mid branch's first origin.
  REQUIRE(a[10].sections.front().origin.x == Catch::Approx(b[10].sections.front().origin.x));
  REQUIRE(a[10].sections.front().origin.y == Catch::Approx(b[10].sections.front().origin.y));
  REQUIRE(a[10].sections.front().origin.z == Catch::Approx(b[10].sections.front().origin.z));
}

TEST_CASE("GenerateTreeMesh: well-formed indexed mesh") {
  const TexturedMeshResult r = GenerateTreeMesh(OakPreset());
  const auto& m = r.mesh;
  REQUIRE(m.vertex_count > 0u);
  REQUIRE(m.vertices.size() == m.vertex_count * kTexturedMeshFloatsPerVertex);
  REQUIRE_FALSE(m.indices.empty());
  REQUIRE(m.indices.size() % 3 == 0);
  for (uint32_t idx : m.indices) REQUIRE(idx < m.vertex_count);
  for (float f : m.vertices) REQUIRE(std::isfinite(f));
  // Base on floor, grows up.
  REQUIRE(r.local_bounds.min.y == Catch::Approx(0.0f).margin(0.05f));
  REQUIRE(r.local_bounds.max.y > 1.0f);
}

TEST_CASE("GenerateTreeMesh: deterministic, seed-sensitive") {
  const TexturedMeshResult a = GenerateTreeMesh(OakPreset());
  const TexturedMeshResult b = GenerateTreeMesh(OakPreset());
  REQUIRE(a.mesh.vertices == b.mesh.vertices);
  REQUIRE(a.mesh.indices == b.mesh.indices);

  TreeOptions other = OakPreset();
  other.seed = 999u;
  const TexturedMeshResult c = GenerateTreeMesh(other);
  REQUIRE(c.mesh.vertices != a.mesh.vertices);  // different tree
}

TEST_CASE("GenerateTreeMesh: exact counts for the (continuation-free) Pine") {
  // Pine is evergreen -> no stem continuation -> clean per-level counts.
  // Trunk: 13 rings * (8+1) = 117 verts; 82 branches * (11 rings * (6+1)) = 6314.
  // Indices: 12*8*6 + 82*(10*6*6) = 576 + 29520 = 30096.
  const TexturedMeshResult p = GenerateTreeMesh(PinePreset());
  REQUIRE(p.mesh.vertex_count == 6431u);
  REQUIRE(p.mesh.indices.size() == 30096u);
}

TEST_CASE("TreeCatalog: every predefined setup generates a well-formed mesh") {
  const std::vector<NamedTreeOptions> catalog = TreeCatalog();
  REQUIRE(catalog.size() == 15u);  // oak/pine/ash/aspen x3 sizes + 3 bushes

  for (const NamedTreeOptions& setup : catalog) {
    INFO("setup: " << setup.name);
    REQUIRE_FALSE(setup.name.empty());

    const TexturedMeshResult r = GenerateTreeMesh(setup.options);
    const auto& m = r.mesh;
    REQUIRE(m.vertex_count > 0u);
    REQUIRE(m.vertices.size() == m.vertex_count * kTexturedMeshFloatsPerVertex);
    REQUIRE_FALSE(m.indices.empty());
    REQUIRE(m.indices.size() % 3 == 0);

    bool indices_in_range = true;
    for (uint32_t idx : m.indices)
      if (idx >= m.vertex_count) indices_in_range = false;
    REQUIRE(indices_in_range);

    bool all_finite = true;
    for (float f : m.vertices)
      if (!std::isfinite(f)) all_finite = false;
    REQUIRE(all_finite);

    const float height = r.local_bounds.max.y - r.local_bounds.min.y;
    REQUIRE(std::isfinite(height));
    REQUIRE(height > 0.0f);
  }
}

TEST_CASE("TreeCatalog presets bake with LeafSilhouetteBakeCutoff's cutoff") {
  // Guards against the viewer's leaf-texture cache (model_viewer_view.cpp)
  // and TreeCatalog's per-preset LeafOptions::alpha_cutoff drifting apart --
  // both must agree with the single shared LeafSilhouetteBakeCutoff helper.
  auto check = [](const TreeOptions& o, const std::string& name) {
    INFO("setup: " << name);
    REQUIRE(o.leaves.alpha_cutoff ==
            Catch::Approx(LeafSilhouetteBakeCutoff(o.leaves.silhouette)));
  };
  check(OakPreset(), "OakPreset");
  check(PinePreset(), "PinePreset");

  const std::vector<NamedTreeOptions> catalog = TreeCatalog();
  for (const NamedTreeOptions& setup : catalog) check(setup.options, setup.name);
}

TEST_CASE("TreeCatalog: leaf world-size bands") {
  // Same preview-height scale the viewer applies to fit every catalog tree
  // into its orbit framing (model_viewer_view.cpp's kTreePreviewHeight) --
  // world_leaf_m is the on-screen leaf-card size once a tree is scaled to
  // that preview height, i.e. what a player actually sees.
  constexpr float kPreviewHeight = 8.0f;

  const std::vector<NamedTreeOptions> catalog = TreeCatalog();
  for (const NamedTreeOptions& setup : catalog) {
    INFO("setup: " << setup.name);
    const LeafOptions& lf = setup.options.leaves;
    CHECK(lf.arrangement != LeafArrangement::CrossedPair);

    const std::vector<SkeletonBranch> skeleton = BuildTreeSkeleton(setup.options);
    const TexturedMeshResult bark = GenerateTreeMesh(setup.options, skeleton);
    const float height = bark.local_bounds.max.y - bark.local_bounds.min.y;
    REQUIRE(height > 0.0f);

    const float world_leaf_m = lf.size * kPreviewHeight / height;
    const int quads_per_site = QuadsPerLeafSite(lf);

    INFO(setup.name << ": bark_height=" << height
                     << " world_leaf_m=" << world_leaf_m
                     << " count=" << lf.count
                     << " quads_per_site=" << quads_per_site);

    // Band keyed off silhouette: PineSprig is a small sprig cluster (bigger
    // card, coarser texture), Bush is a fine fat-oval leaf, everything else
    // (Oak/Ash/Aspen) is a deciduous single-leaflet card.
    float lo, hi;
    if (lf.silhouette == LeafSilhouette::PineSprig) {
      lo = 0.15f; hi = 0.35f;
    } else if (lf.silhouette == LeafSilhouette::Bush) {
      lo = 0.08f; hi = 0.20f;
    } else {
      lo = 0.10f; hi = 0.25f;
    }
    CHECK(world_leaf_m >= lo);
    CHECK(world_leaf_m <= hi);
  }
}

TEST_CASE("GenerateLeafMesh: deterministic") {
  const TexturedMeshResult a = GenerateLeafMesh(OakPreset());
  const TexturedMeshResult b = GenerateLeafMesh(OakPreset());
  REQUIRE(a.mesh.vertices == b.mesh.vertices);
  REQUIRE(a.mesh.indices == b.mesh.indices);
}

TEST_CASE("GenerateLeafMesh: well-formed indexed mesh (Oak, Pine)") {
  for (const TreeOptions& o : {OakPreset(), PinePreset()}) {
    const TexturedMeshResult r = GenerateLeafMesh(o);
    const auto& m = r.mesh;
    REQUIRE(m.vertex_count > 0u);
    REQUIRE(m.vertices.size() == m.vertex_count * kTexturedMeshFloatsPerVertex);
    REQUIRE(m.indices.size() % 3 == 0);
    for (uint32_t idx : m.indices) REQUIRE(idx < m.vertex_count);
    for (float f : m.vertices) REQUIRE(std::isfinite(f));
  }
}

TEST_CASE("GenerateLeafMesh: quad count scales with QuadsPerLeafSite") {
  TreeOptions single = OakPreset();
  single.leaves.arrangement = LeafArrangement::SingleQuad;
  REQUIRE(QuadsPerLeafSite(single.leaves) == 1);
  const TexturedMeshResult r1 = GenerateLeafMesh(single);
  REQUIRE(r1.mesh.vertex_count > 0u);
  // Each quad = 4 verts + 6 indices.
  REQUIRE(r1.mesh.indices.size() == r1.mesh.vertex_count / 4u * 6u);

  struct Case { LeafArrangement arrangement; int blade_count; int expected_quads; };
  const Case cases[] = {
      {LeafArrangement::CrossedPair, 2, 2},
      {LeafArrangement::FanFromStem, 3, 3},
      {LeafArrangement::AxialFins, 3, 3},
  };
  for (const Case& c : cases) {
    INFO("arrangement index " << static_cast<int>(c.arrangement));
    TreeOptions o = OakPreset();
    o.leaves.arrangement = c.arrangement;
    o.leaves.blade_count = c.blade_count;
    REQUIRE(QuadsPerLeafSite(o.leaves) == c.expected_quads);

    const TexturedMeshResult r = GenerateLeafMesh(o);
    REQUIRE(r.mesh.vertex_count ==
            r1.mesh.vertex_count * static_cast<uint32_t>(c.expected_quads));
    REQUIRE(r.mesh.indices.size() ==
            r1.mesh.indices.size() * static_cast<size_t>(c.expected_quads));
    REQUIRE(r.mesh.indices.size() == r.mesh.vertex_count / 4u * 6u);
  }
}

TEST_CASE("GenerateLeafMesh: disabled produces an empty mesh") {
  TreeOptions o = OakPreset();
  o.leaves.enabled = false;
  const TexturedMeshResult r = GenerateLeafMesh(o);
  REQUIRE(r.mesh.vertex_count == 0u);
  REQUIRE(r.mesh.indices.empty());
}

namespace {
constexpr LeafSilhouette kAllSilhouettes[] = {
    LeafSilhouette::Oak, LeafSilhouette::Ash, LeafSilhouette::Aspen,
    LeafSilhouette::Bush, LeafSilhouette::PineSprig,
};
}  // namespace

TEST_CASE("BuildLeafRgba8: leaf-shaped alpha card for every silhouette") {
  const int n = 128;
  const glm::vec3 color(0.30f, 0.55f, 0.18f);
  for (LeafSilhouette shape : kAllSilhouettes) {
    INFO("silhouette index " << static_cast<int>(shape));
    const std::vector<uint8_t> px = BuildLeafRgba8(n, color, shape);
    REQUIRE(px.size() == static_cast<size_t>(n) * static_cast<size_t>(n) * 4);
    auto alpha = [&](int x, int y) {
      return px[(static_cast<size_t>(y) * n + static_cast<size_t>(x)) * 4 + 3];
    };
    REQUIRE(alpha(0, 0) == 0);             // corners are outside
    REQUIRE(alpha(n - 1, 0) == 0);
    REQUIRE(alpha(0, n - 1) == 0);
    REQUIRE(alpha(n - 1, n - 1) == 0);

    // On-shape probe: center works for the sinusoidal-envelope shapes; the
    // needle strokes of PineSprig don't reliably cross the exact center, so
    // probe the always-present stem column near the base instead.
    const int probe_x = n / 2;
    const int probe_y = (shape == LeafSilhouette::PineSprig) ? n / 16 : n / 2;
    REQUIRE(alpha(probe_x, probe_y) == 255);

    // RGB carries the passed color (green > red at the probe texel).
    const size_t c = (static_cast<size_t>(probe_y) * n + static_cast<size_t>(probe_x)) * 4;
    REQUIRE(px[c + 1] > px[c + 0]);
  }
}

TEST_CASE("BuildLeafRgba8: opaque-texel counts are pairwise distinct across silhouettes") {
  const int n = 128;
  const glm::vec3 color(0.30f, 0.55f, 0.18f);
  size_t counts[std::size(kAllSilhouettes)];
  for (size_t i = 0; i < std::size(kAllSilhouettes); ++i) {
    const std::vector<uint8_t> px = BuildLeafRgba8(n, color, kAllSilhouettes[i]);
    size_t count = 0;
    for (size_t p = 0; p < static_cast<size_t>(n) * static_cast<size_t>(n); ++p)
      if (px[p * 4 + 3] >= 128) ++count;
    counts[i] = count;
  }
  for (size_t i = 0; i < std::size(kAllSilhouettes); ++i)
    for (size_t j = i + 1; j < std::size(kAllSilhouettes); ++j)
      REQUIRE(counts[i] != counts[j]);
}

TEST_CASE("BuildLeafRgba8: Bush matches the pre-species oval") {
  const int n = 64;
  const glm::vec3 color(0.30f, 0.55f, 0.18f);
  const std::vector<uint8_t> px = BuildLeafRgba8(n, color, LeafSilhouette::Bush);
  REQUIRE(px.size() == static_cast<size_t>(n) * static_cast<size_t>(n) * 4);
  auto alpha = [&](int x, int y) {
    return px[(static_cast<size_t>(y) * n + static_cast<size_t>(x)) * 4 + 3];
  };
  REQUIRE(alpha(n / 2, n / 2) == 255);   // center is inside the leaf
  REQUIRE(alpha(0, 0) == 0);             // corners are outside
  REQUIRE(alpha(n - 1, 0) == 0);
  REQUIRE(alpha(0, n - 1) == 0);
  REQUIRE(alpha(n - 1, n - 1) == 0);
  // RGB carries the leaf color (green > red at the center texel).
  const size_t c = (static_cast<size_t>(n / 2) * n + static_cast<size_t>(n / 2)) * 4;
  REQUIRE(px[c + 1] > px[c + 0]);

  // Analytic half-width W*sin(pi*t) at two known rows: 3 texels inside the
  // computed edge is opaque, 3 texels outside is transparent.
  const float W = 0.60f;
  const float texel_u = 2.0f / static_cast<float>(n);
  auto x_for_u = [&](float u) {
    return static_cast<int>(std::lround((u + 1.0f) * 0.5f * n - 0.5f));
  };
  for (int y : {n / 4, 3 * n / 4}) {
    const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(n) * 2.0f - 1.0f;
    const float t = (v + 1.0f) * 0.5f;
    const float half_w = W * std::sin(glm::pi<float>() * t);
    REQUIRE(alpha(x_for_u(half_w - 3.0f * texel_u), y) == 255);
    REQUIRE(alpha(x_for_u(half_w + 3.0f * texel_u), y) == 0);
  }
}

TEST_CASE("BuildLeafMipChainRgba8: coverage-preserving mip chain") {
  const int n = 128;
  const glm::vec3 color(0.30f, 0.55f, 0.18f);
  struct Case { LeafSilhouette shape; float cutoff; };
  const Case cases[] = {{LeafSilhouette::Oak, 0.5f}, {LeafSilhouette::PineSprig, 0.35f}};

  for (const Case& c : cases) {
    INFO("silhouette index " << static_cast<int>(c.shape));
    const std::vector<std::vector<uint8_t>> mips = BuildLeafMipChainRgba8(n, color, c.shape, c.cutoff);
    REQUIRE(mips.size() == 8u);  // 128 -> 64 -> 32 -> 16 -> 8 -> 4 -> 2 -> 1

    auto coverage = [&](const std::vector<uint8_t>& px, int size) {
      const uint8_t thresh = static_cast<uint8_t>(std::lround(c.cutoff * 255.0f));
      size_t count = 0;
      const size_t total = static_cast<size_t>(size) * static_cast<size_t>(size);
      for (size_t i = 0; i < total; ++i)
        if (px[i * 4 + 3] >= thresh) ++count;
      return static_cast<float>(count) / static_cast<float>(total);
    };

    int size = n;
    for (const std::vector<uint8_t>& level : mips) {
      REQUIRE(level.size() == static_cast<size_t>(size) * static_cast<size_t>(size) * 4);
      size = std::max(1, size / 2);
    }

    const float level0_coverage = coverage(mips[0], n);
    REQUIRE(level0_coverage > 0.0f);

    size = n;
    for (const std::vector<uint8_t>& level : mips) {
      if (size >= 8) {
        const float cov = coverage(level, size);
        REQUIRE(std::fabs(cov - level0_coverage) <= 0.30f * level0_coverage);
      }
      size = std::max(1, size / 2);
    }
  }
}

TEST_CASE("GenerateLeafMesh: terminal-tip leaf adds one leaf per leaf-bearing branch") {
  auto count_terminal = [](const TreeOptions& o) {
    int n = 0;
    for (const SkeletonBranch& b : BuildTreeSkeleton(o))
      if (b.level == o.levels && static_cast<int>(b.sections.size()) - 1 >= 1) ++n;
    return n;
  };
  TreeOptions on = OakPreset();  on.leaves.tip_leaf = true;
  TreeOptions off = OakPreset(); off.leaves.tip_leaf = false;
  const uint32_t vn = GenerateLeafMesh(on).mesh.vertex_count;
  const uint32_t vf = GenerateLeafMesh(off).mesh.vertex_count;
  const int quads = QuadsPerLeafSite(OakPreset().leaves);
  REQUIRE(vn > vf);
  // Each tip leaf = quads * 4 verts; one per leaf-bearing terminal branch.
  REQUIRE(vn - vf == static_cast<uint32_t>(count_terminal(OakPreset()) * quads * 4));
}

TEST_CASE("GenerateLeafMesh: FanFromStem blades never weld (full-stride distinct)") {
  TreeOptions o = OakPreset();
  o.leaves.arrangement = LeafArrangement::FanFromStem;
  o.leaves.blade_count = 3;
  const int n = QuadsPerLeafSite(o.leaves);
  REQUIRE(n == 3);

  const TexturedMeshResult r = GenerateLeafMesh(o);
  const size_t stride = kTexturedMeshFloatsPerVertex;
  const size_t verts_per_site = static_cast<size_t>(n) * 4u;
  REQUIRE(r.mesh.vertices.size() >= verts_per_site * stride);

  // First site (first n*4 verts). No two verts from DIFFERENT blades of this
  // site may be bit-identical across the full 11-float stride (pos+uv+normal+
  // tangent) -- shared full-stride verts are exactly what the LOD simplifier
  // would weld, collapsing separate blades into one.
  for (size_t i = 0; i < verts_per_site; ++i) {
    for (size_t j = i + 1; j < verts_per_site; ++j) {
      if (i / 4u == j / 4u) continue;  // same blade -- not the case under test
      bool identical = true;
      for (size_t k = 0; k < stride; ++k) {
        if (r.mesh.vertices[i * stride + k] != r.mesh.vertices[j * stride + k]) {
          identical = false;
          break;
        }
      }
      INFO("vertex " << i << " vs " << j);
      REQUIRE_FALSE(identical);
    }
  }
}

TEST_CASE("GenerateLeafMesh: AxialFins blades share one long axis") {
  TreeOptions o = OakPreset();
  o.leaves.arrangement = LeafArrangement::AxialFins;
  o.leaves.blade_count = 3;
  const int n = QuadsPerLeafSite(o.leaves);
  REQUIRE(n == 3);

  const TexturedMeshResult r = GenerateLeafMesh(o);
  const size_t stride = kTexturedMeshFloatsPerVertex;
  auto pos = [&](size_t v) {
    const size_t off = v * stride;
    return glm::vec3(r.mesh.vertices[off + 0], r.mesh.vertices[off + 1],
                     r.mesh.vertices[off + 2]);
  };

  // Quad corners are {top-left, bottom-left, bottom-right, top-right}; the
  // card's long axis is top-left minus bottom-left (== top-right minus
  // bottom-right, same rotated edge).
  glm::vec3 axis[3];
  for (int b = 0; b < n; ++b) {
    const size_t base = static_cast<size_t>(b) * 4u;
    axis[b] = glm::normalize(pos(base + 0) - pos(base + 1));
  }
  for (int i = 0; i < n; ++i)
    for (int j = i + 1; j < n; ++j)
      REQUIRE(std::fabs(glm::dot(axis[i], axis[j])) == Catch::Approx(1.0f).margin(1e-4f));
}
