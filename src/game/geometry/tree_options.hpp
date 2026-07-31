#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>
#include <glm/glm.hpp>

namespace badlands {

enum class TreeType { Deciduous, Evergreen };

// Per-species leaf card silhouette shape (leaf_texture.hpp). Bush is the
// generic fat-oval fallback; PineSprig alone keeps the pre-species texture's
// byte-identical legacy path (see PineSprigAlpha's DO NOT TOUCH note in
// leaf_texture.cpp) -- Bush routes through the sprig builder like every other
// silhouette.
//
// kLeafSilhouetteCount/kAllLeafSilhouettes below are the canonical size/
// enumeration for this enum -- consumers that need to iterate every value or
// size a per-silhouette array (model_viewer_view.hpp's leaf_textures_/
// leaf_views_, model_viewer_view.cpp's texture bake loop,
// tree_generator_tests.cpp) must use them instead of hand-listing the values.
// When adding a new LeafSilhouette, append it to the enum and to kAllLeafSilhouettes,
// then update kLeafSilhouetteCount to match the array size.
enum class LeafSilhouette { Oak, Ash, Aspen, Bush, PineSprig };

inline constexpr size_t kLeafSilhouetteCount = 5;
inline constexpr std::array<LeafSilhouette, kLeafSilhouetteCount> kAllLeafSilhouettes = {
    LeafSilhouette::Oak, LeafSilhouette::Ash, LeafSilhouette::Aspen,
    LeafSilhouette::Bush, LeafSilhouette::PineSprig};
static_assert(static_cast<size_t>(kAllLeafSilhouettes.back()) + 1 == kLeafSilhouetteCount,
              "append new LeafSilhouette values at the end and update kAllLeafSilhouettes + kLeafSilhouetteCount");

// The alpha-discard cutoff a per-silhouette leaf-card texture should be BAKED
// with (BuildLeafMipChainRgba8's coverage-preserving mip chain, viewer's
// LeafSilhouette-keyed texture cache). Single source of truth for the value
// TreeCatalog()'s presets also assign to LeafOptions::alpha_cutoff below --
// PineSprig's thin needle strokes need the lower cutoff so coverage
// preservation has something to preserve; every other silhouette uses the
// LeafOptions default (0.5).
constexpr float LeafSilhouetteBakeCutoff(LeafSilhouette shape) {
  return (shape == LeafSilhouette::PineSprig) ? 0.35f : 0.5f;
}

// Per-site leaf-card blade layout (tree_generator.cpp's emit_leaf):
// SingleQuad/CrossedPair keep ez-tree's billboard=1/2 behavior; FanFromStem
// and AxialFins are multi-blade arrangements sharing the site's stem point
// or long axis respectively.
enum class LeafArrangement { SingleQuad, CrossedPair, FanFromStem, AxialFins };

// Leaf-card generation parameters (ported from ez-tree TreeOptions.leaves).
// Consumed by GenerateLeafMesh (tree_generator.hpp); leaf placement uses a
// separate RNG stream so it never perturbs the branch skeleton.
struct LeafOptions {
  bool  enabled = true;
  LeafArrangement arrangement = LeafArrangement::CrossedPair;  // == old billboard=2
  int   blade_count = 2;      // quads per site, FanFromStem/AxialFins only (2..3)
  float card_aspect = 1.0f;   // card width = size * card_aspect (height = size)
  int   count = 18;           // leaves per leaf-bearing branch (ez-tree oak_medium)
  float start = 0.16f;        // fractional start along the branch (ez-tree)
  float size = 2.5f;          // leaf card size (native ez-tree units)
  float size_variance = 0.7f;
  float angle = 42.0f;        // tilt from the branch, degrees (ez-tree oak_medium)
  float alpha_cutoff = 0.5f;  // discard threshold for the leaf cutout material (MaterialLibrary::AlphaCutout)
  glm::vec3 tint{0.30f, 0.55f, 0.18f};  // green; per-preset overridable
  glm::vec3 transmission_tint{0.35f, 0.6f, 0.15f};  // transmitted (back-lit) colour
  float transmission_strength{0.6f};
  LeafSilhouette silhouette = LeafSilhouette::Bush;  // card texture shape (leaf_texture.hpp)
  bool tip_leaf = true;       // ez-tree deciduous terminal-tip leaf: one extra leaf at each
                              // leaf-bearing branch's endpoint (set false for evergreens)
};

// Per-level branch parameters (index = branch level, 0 = trunk). Ported from
// ez-tree TreeOptions.branch (github.com/dgreenheck/ez-tree). Angles in degrees.
struct TreeOptions {
  uint32_t seed = 0;
  TreeType type = TreeType::Deciduous;
  int levels = 3;                              // recursion depth (0..levels)

  std::array<float, 4> angle{};                // child pitch from parent (idx 1..3)
  std::array<int, 4>   children{};             // radial children per level (idx 0..2)
  std::array<float, 4> gnarliness{};
  std::array<float, 4> length{};
  std::array<float, 4> radius{};
  std::array<int, 4>   sections{};             // rings along a branch
  std::array<int, 4>   segments{};             // radial divisions
  std::array<float, 4> start{};                // fractional child start height (1..3)
  std::array<float, 4> taper{};
  std::array<float, 4> twist{};                // radians per section

  glm::vec3 force_dir{0.0f, 1.0f, 0.0f};
  float force_strength = 0.0f;
  float bark_uv_scale_x = 1.0f;                // wraps = round(base_radius * this)
  float bark_uv_scale_y = 1.0f;                // V = cumulative_length / this

  LeafOptions leaves;
};

// ez-tree presets/oak_medium.json (deciduous).
inline TreeOptions OakPreset() {
  TreeOptions o;
  o.seed = 35729; o.type = TreeType::Deciduous; o.levels = 3;
  o.angle      = {0.0f, 54.0f, 58.0f, 32.0f};
  o.children   = {6, 4, 3, 0};
  o.gnarliness = {0.0f, -0.1f, -0.15f, 0.09f};
  o.length     = {37.24f, 11.08f, 12.39f, 7.16f};
  o.radius     = {1.41f, 0.9f, 0.69f, 1.19f};
  o.sections   = {8, 6, 3, 1};
  o.segments   = {7, 5, 3, 3};
  o.start      = {0.0f, 0.49f, 0.06f, 0.12f};
  o.taper      = {0.73f, 0.42f, 0.69f, 0.75f};
  o.twist      = {-0.23f, 0.42f, 0.0f, 0.0f};
  o.force_dir = {0.0f, 1.0f, 0.0f}; o.force_strength = 0.02f;
  o.bark_uv_scale_x = 1.0f; o.bark_uv_scale_y = 10.0f;
  o.leaves = {.arrangement=LeafArrangement::FanFromStem, .blade_count=2, .card_aspect=0.95f,
              .count=30, .start=0.16f, .size=2.32f, .size_variance=0.7f, .angle=42.0f,
              .tint={0.32f,0.52f,0.18f}, .transmission_tint={0.55f,0.62f,0.10f},
              .transmission_strength=0.65f, .silhouette=LeafSilhouette::Oak};
  return o;
}

// ez-tree presets/pine_medium.json (evergreen).
inline TreeOptions PinePreset() {
  TreeOptions o;
  o.seed = 13977; o.type = TreeType::Evergreen; o.levels = 1;
  o.angle      = {0.0f, 110.0f, 16.0f, 60.0f};
  o.children   = {82, 3, 5, 0};
  o.gnarliness = {0.05f, 0.08f, 0.0f, 0.0f};
  o.length     = {50.0f, 23.87f, 14.08f, 1.0f};
  o.radius     = {1.05f, 0.36f, 0.7f, 0.7f};
  o.sections   = {12, 10, 8, 6};
  o.segments   = {8, 6, 4, 3};
  o.start      = {0.0f, 0.27f, 0.14f, 0.3f};
  o.taper      = {0.7f, 0.7f, 0.7f, 0.7f};
  o.twist      = {0.0f, 0.0f, 0.0f, 0.0f};
  o.force_dir = {0.0f, 1.0f, 0.0f}; o.force_strength = -0.003f;
  o.bark_uv_scale_x = 1.0f; o.bark_uv_scale_y = 1.0f;
  o.leaves = {.arrangement=LeafArrangement::AxialFins, .blade_count=3, .card_aspect=0.45f,
              .count=280, .start=0.09f, .size=2.043f, .size_variance=0.201f, .angle=39.0f,
              .alpha_cutoff=0.35f, .tint={0.16f,0.40f,0.24f}, .transmission_tint={0.32f,0.46f,0.12f},
              .transmission_strength=0.28f, .silhouette=LeafSilhouette::PineSprig, .tip_leaf=false};
  return o;
}

// A named tree setup for the model-viewer's predefined-tree list.
struct NamedTreeOptions {
  std::string name;
  TreeOptions options;
};

// The full ez-tree preset catalog (oak/pine/ash/aspen sizes + bushes), ported as
// TreeOptions. Order is the viewer's list order. Defined in tree_generator.cpp.
std::vector<NamedTreeOptions> TreeCatalog();

}  // namespace badlands
