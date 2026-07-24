#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>
#include <glm/glm.hpp>

namespace badlands {

enum class TreeType { Deciduous, Evergreen };

// Leaf-card generation parameters (ported from ez-tree TreeOptions.leaves).
// Consumed by GenerateLeafMesh (tree_generator.hpp); leaf placement uses a
// separate RNG stream so it never perturbs the branch skeleton.
struct LeafOptions {
  bool  enabled = true;
  int   billboard = 2;        // 1 = single quad, 2 = double (perpendicular cross)
  int   count = 18;           // leaves per leaf-bearing branch (ez-tree oak_medium)
  float start = 0.16f;        // fractional start along the branch (ez-tree)
  float size = 2.5f;          // leaf card size (native ez-tree units)
  float size_variance = 0.7f;
  float angle = 42.0f;        // tilt from the branch, degrees (ez-tree oak_medium)
  float alpha_cutoff = 0.5f;  // discard threshold (consumed by a later task's material)
  glm::vec3 tint{0.30f, 0.55f, 0.18f};  // green; per-preset overridable
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
  o.leaves = {.count=18, .start=0.16f, .size=2.5f, .size_variance=0.7f, .angle=42.0f, .tint={0.32f,0.52f,0.18f}};
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
  o.leaves = {.count=30, .start=0.09f, .size=1.435f, .size_variance=0.201f, .angle=39.0f, .tint={0.16f,0.40f,0.24f}, .tip_leaf=false};
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
