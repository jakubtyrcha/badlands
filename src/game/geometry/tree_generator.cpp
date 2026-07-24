#include "game/geometry/tree_generator.hpp"
#include <random>
#include <algorithm>
#include <cmath>
#include <deque>
#include <glm/gtc/constants.hpp>   // two_pi
#include "engine/rendering/geometry/mesh_builder_utils.hpp"  // PushVertex
namespace badlands {
namespace {

// One seeded PRNG stream per generate call. range() draws uniformly from the
// [min(lo,hi), max(lo,hi)] interval -- callers pass symmetric bounds like
// range(-g, g) where g can be negative (negative gnarliness), so the arguments
// are NOT guaranteed ordered; std::uniform_real_distribution requires a <= b, so
// normalise here (this mirrors ez-tree's order-agnostic (max-min)*r+min).
class TreeRng {
 public:
  explicit TreeRng(uint32_t seed) : gen_(seed) {}
  float range(float lo, float hi) {
    return std::uniform_real_distribution<float>(std::min(lo, hi),
                                                 std::max(lo, hi))(gen_);
  }
  float unit() { return range(0.0f, 1.0f); }
  int index(int n) { return std::uniform_int_distribution<int>(0, n - 1)(gen_); }
 private:
  std::mt19937 gen_;
};

// Work item for the growth queue (ez-tree's Branch).
struct GrowBranch {
  glm::vec3 origin;
  glm::quat orientation;
  float length;
  float radius;
  int level;
  int section_count;
  int segment_count;
};

std::vector<int> ShuffledIndices(int count, TreeRng& rng) {
  std::vector<int> idx(static_cast<size_t>(count));
  for (int i = 0; i < count; ++i) idx[static_cast<size_t>(i)] = i;
  for (int i = count - 1; i > 0; --i) std::swap(idx[i], idx[rng.index(i + 1)]);
  return idx;
}

void GenerateChildBranches(const TreeOptions& o, TreeRng& rng, int count, int level,
                           const std::vector<BranchSection>& sections,
                           std::deque<GrowBranch>& queue) {
  if (count <= 0) return;
  const int last = static_cast<int>(sections.size()) - 1;
  if (last < 1) return;

  const float radial_offset = rng.unit();
  const float start_min = o.start[static_cast<size_t>(level)];
  const float height_step = (1.0f - start_min) / static_cast<float>(count);
  const std::vector<int> slots = ShuffledIndices(count, rng);

  for (int i = 0; i < count; ++i) {
    const float child_start = start_min + (static_cast<float>(i) + rng.unit()) * height_step;
    int si = static_cast<int>(std::floor(child_start * static_cast<float>(last)));
    si = std::clamp(si, 0, last);
    const BranchSection& a = sections[static_cast<size_t>(si)];
    const BranchSection& b = (si == last) ? a : sections[static_cast<size_t>(si + 1)];

    float alpha = (child_start - static_cast<float>(si) / static_cast<float>(last)) /
                  (1.0f / static_cast<float>(last));
    alpha = std::clamp(alpha, 0.0f, 1.0f);

    const glm::vec3 origin = glm::mix(a.origin, b.origin, alpha);
    const float radius = o.radius[static_cast<size_t>(level)] *
                         ((1.0f - alpha) * a.radius + alpha * b.radius);
    const glm::quat parent = glm::slerp(b.orientation, a.orientation, alpha);

    const float jitter = rng.range(-0.5f, 0.5f);
    const float radial_angle = glm::two_pi<float>() *
        (radial_offset + (static_cast<float>(slots[static_cast<size_t>(i)]) + jitter) /
                          static_cast<float>(count));
    const glm::quat q1 = glm::angleAxis(glm::radians(o.angle[static_cast<size_t>(level)]),
                                        glm::vec3(1, 0, 0));
    const glm::quat q2 = glm::angleAxis(radial_angle, glm::vec3(0, 1, 0));
    const glm::quat child_orient = parent * (q2 * q1);

    const float child_length = o.length[static_cast<size_t>(level)] *
        (o.type == TreeType::Evergreen ? (1.0f - child_start) : 1.0f);

    queue.push_back({origin, child_orient, child_length, radius, level,
                     o.sections[static_cast<size_t>(level)],
                     o.segments[static_cast<size_t>(level)]});
  }
}

}  // namespace

std::vector<SkeletonBranch> BuildTreeSkeleton(const TreeOptions& o) {
  TreeRng rng(o.seed);
  std::vector<SkeletonBranch> skeleton;
  std::deque<GrowBranch> queue;
  constexpr float kTipRadius = 0.001f;

  queue.push_back({glm::vec3(0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f),
                   o.length[0], o.radius[0], 0, o.sections[0], o.segments[0]});

  while (!queue.empty()) {
    const GrowBranch br = queue.front();
    queue.pop_front();

    glm::quat orient = br.orientation;
    glm::vec3 origin = br.origin;
    const int decay_div = (o.type == TreeType::Deciduous) ? std::max(1, o.levels - 1) : 1;
    const float section_len = br.length / static_cast<float>(br.section_count) /
                              static_cast<float>(decay_div);

    std::vector<BranchSection> sections;
    sections.reserve(static_cast<size_t>(br.section_count) + 1);

    for (int i = 0; i <= br.section_count; ++i) {
      float radius = br.radius;
      if (i == br.section_count && br.level == o.levels) {
        radius = kTipRadius;
      } else if (o.type == TreeType::Deciduous) {
        radius *= 1.0f - o.taper[static_cast<size_t>(br.level)] *
                          (static_cast<float>(i) / static_cast<float>(br.section_count));
      } else {
        radius *= 1.0f - (static_cast<float>(i) / static_cast<float>(br.section_count));
      }

      sections.push_back({origin, orient, radius});

      origin += orient * glm::vec3(0.0f, section_len, 0.0f);

      const float g = std::max(1.0f, 1.0f / std::sqrt(std::max(radius, 1e-6f))) *
                      o.gnarliness[static_cast<size_t>(br.level)];
      orient = orient * glm::angleAxis(rng.range(-g, g), glm::vec3(1, 0, 0));
      orient = orient * glm::angleAxis(rng.range(-g, g), glm::vec3(0, 0, 1));
      orient = orient * glm::angleAxis(o.twist[static_cast<size_t>(br.level)], glm::vec3(0, 1, 0));

      const glm::vec3 up = glm::normalize(orient * glm::vec3(0, 1, 0));
      const glm::vec3 target = glm::normalize(o.force_dir);
      glm::vec3 axis = glm::cross(up, target);
      const float s = glm::length(axis);
      if (s > 1e-6f) {
        axis /= s;
        const float full = std::atan2(s, glm::dot(up, target));
        const float step = o.force_strength / std::max(radius, 1e-6f);
        orient = glm::angleAxis(std::clamp(step, -full, full), axis) * orient;  // premultiply
      }
    }

    skeleton.push_back({sections, br.segment_count, br.radius});
    const BranchSection& last = sections.back();

    // Deciduous stem continuation (inherits parent section/segment counts).
    if (o.type == TreeType::Deciduous && br.level < o.levels) {
      queue.push_back({last.origin, last.orientation, o.length[br.level + 1],
                       last.radius, br.level + 1, br.section_count, br.segment_count});
    }
    // Radial children (both types). Leaves at the terminal level are SP2.
    if (br.level < o.levels) {
      GenerateChildBranches(o, rng, o.children[static_cast<size_t>(br.level)],
                            br.level + 1, sections, queue);
    }
  }
  return skeleton;
}

TexturedMeshResult GenerateTreeMesh(const TreeOptions& o) {
  const std::vector<SkeletonBranch> skeleton = BuildTreeSkeleton(o);

  StaticTexturedMeshComponent mesh;

  for (const SkeletonBranch& br : skeleton) {
    const int segments = std::max(3, br.segment_count);
    const int wraps = std::max(1, static_cast<int>(std::lround(br.base_radius * o.bark_uv_scale_x)));
    const int n = segments + 1;  // ring verts incl. duplicated seam
    const uint32_t index_offset = mesh.vertex_count;

    float cum_len = 0.0f;
    for (size_t k = 0; k < br.sections.size(); ++k) {
      const BranchSection& sec = br.sections[k];
      if (k > 0) cum_len += glm::length(sec.origin - br.sections[k - 1].origin);
      const float v = cum_len / o.bark_uv_scale_y;  // arc-length V (SP1 improvement)

      for (int j = 0; j <= segments; ++j) {
        const int jj = (j == segments) ? 0 : j;  // wrap position; U still reaches `wraps`
        const float angle = glm::two_pi<float>() * static_cast<float>(jj) /
                            static_cast<float>(segments);
        const glm::vec3 dir(std::cos(angle), 0.0f, std::sin(angle));
        const glm::vec3 tan_dir(-std::sin(angle), 0.0f, std::cos(angle));
        const glm::vec3 pos = sec.origin + sec.orientation * (dir * sec.radius);
        const glm::vec3 nrm = glm::normalize(sec.orientation * dir);
        const glm::vec3 tng = glm::normalize(sec.orientation * tan_dir);
        const float u = (static_cast<float>(j) / static_cast<float>(segments)) *
                        static_cast<float>(wraps);
        PushVertex(mesh.vertices, pos, glm::vec2(u, v), nrm, tng);
      }
    }
    mesh.vertex_count = static_cast<uint32_t>(mesh.vertices.size() / kTexturedMeshFloatsPerVertex);

    const int rings = static_cast<int>(br.sections.size());
    for (int i = 0; i < rings - 1; ++i) {
      for (int j = 0; j < segments; ++j) {
        const uint32_t v1 = index_offset + static_cast<uint32_t>(i * n + j);
        const uint32_t v2 = index_offset + static_cast<uint32_t>(i * n + j + 1);
        const uint32_t v3 = v1 + static_cast<uint32_t>(n);
        const uint32_t v4 = v2 + static_cast<uint32_t>(n);
        mesh.indices.insert(mesh.indices.end(), {v1, v3, v2, v2, v3, v4});
      }
    }
  }

  mesh.dirty = true;
  const Aabb bounds = ComputeLocalAabb(mesh);
  return {.mesh = std::move(mesh), .local_bounds = bounds};
}

std::vector<NamedTreeOptions> TreeCatalog() {
  // Build a TreeOptions from ez-tree preset values. Parameter order matches the
  // per-level arrays below: angle, children, gnarliness, length, radius,
  // sections, segments, start, taper, twist. force_dir is (0,1,0) for every
  // ez-tree preset; bark_uv_scale is left at the 1/1 default (untextured here).
  auto make = [](uint32_t seed, TreeType type, int levels,
                 std::array<float, 4> angle, std::array<int, 4> children,
                 std::array<float, 4> gnarliness, std::array<float, 4> length,
                 std::array<float, 4> radius, std::array<int, 4> sections,
                 std::array<int, 4> segments, std::array<float, 4> start,
                 std::array<float, 4> taper, std::array<float, 4> twist,
                 float force_strength) {
    TreeOptions o;
    o.seed = seed; o.type = type; o.levels = levels;
    o.angle = angle; o.children = children; o.gnarliness = gnarliness;
    o.length = length; o.radius = radius; o.sections = sections;
    o.segments = segments; o.start = start; o.taper = taper; o.twist = twist;
    o.force_strength = force_strength;
    return o;
  };

  std::vector<NamedTreeOptions> catalog;
  catalog.push_back({"Oak (small)", make(30895, TreeType::Deciduous, 3,
      {0, 54, 58, 32}, {4, 2, 3, 0}, {0.07f, -0.08f, 0.11f, 0.09f},
      {28.08f, 4.55f, 9.78f, 7.16f}, {1.0f, 1.02f, 0.69f, 1.19f}, {16, 9, 8, 1},
      {7, 5, 3, 3}, {0, 0.49f, 0.06f, 0.12f}, {0.73f, 0.42f, 0.69f, 0.75f},
      {-0.23f, 0.42f, 0, 0}, 0.01f)});
  catalog.push_back({"Oak (medium)", OakPreset()});
  catalog.push_back({"Oak (large)", make(23399, TreeType::Deciduous, 3,
      {0, 54, 43, 32}, {9, 5, 3, 0}, {-0.04f, 0.16f, -0.06f, 0.09f},
      {47.7f, 29.39f, 17.62f, 7.16f}, {3.0f, 0.69f, 0.69f, 1.19f}, {16, 9, 8, 3},
      {12, 5, 3, 3}, {0, 0.35f, 0.1f, 0}, {0.73f, 0.42f, 0.69f, 0.75f},
      {-0.23f, 0.42f, 0, 0}, 0.02f)});
  catalog.push_back({"Pine (small)", make(11744, TreeType::Evergreen, 1,
      {0, 117, 60, 60}, {91, 7, 5, 0}, {0.05f, 0.08f, 0, 0},
      {39.55f, 12.12f, 10.0f, 1.0f}, {0.55f, 0.41f, 0.7f, 0.7f}, {12, 10, 8, 6},
      {8, 6, 4, 3}, {0, 0.16f, 0.3f, 0.3f}, {0.7f, 0.7f, 0.7f, 0.7f},
      {0, 0, 0, 0}, 0.0f)});
  catalog.push_back({"Pine (medium)", PinePreset()});
  catalog.push_back({"Pine (large)", make(44166, TreeType::Evergreen, 1,
      {0, 129.13f, 16, 60}, {100, 3, 0, 0}, {0.05f, 0.08f, 0, 0},
      {65.25f, 34.85f, 27.25f, 1.0f}, {1.27f, 0.37f, 0.7f, 0.7f}, {12, 10, 8, 6},
      {8, 6, 4, 3}, {0, 0.29f, 0.14f, 0.3f}, {0.7f, 0.7f, 0.7f, 0.7f},
      {0, 0, 0, 0}, 0.009f)});
  catalog.push_back({"Ash (small)", make(26867, TreeType::Deciduous, 2,
      {0, 48, 75, 60}, {10, 3, 3, 0}, {0.11f, 0.09f, 0.05f, 0.09f},
      {23.87f, 18.0f, 5.59f, 4.6f}, {0.81f, 0.56f, 0.76f, 0.7f}, {12, 10, 10, 10},
      {8, 6, 4, 3}, {0, 0.53f, 0.33f, 0}, {0.7f, 0.7f, 0.7f, 0.7f},
      {0.3f, -0.07f, 0, 0}, 0.01f)});
  catalog.push_back({"Ash (medium)", make(36330, TreeType::Deciduous, 3,
      {0, 48, 75, 60}, {7, 4, 3, 0}, {0.03f, 0.25f, 0.2f, 0.09f},
      {43.47f, 27.14f, 9.51f, 4.6f}, {2.0f, 0.63f, 0.76f, 0.7f}, {12, 8, 6, 4},
      {12, 6, 4, 3}, {0, 0.23f, 0.33f, 0}, {0.7f, 0.7f, 0.7f, 0.7f},
      {0.09f, -0.07f, 0, 0}, 0.01f)});
  catalog.push_back({"Ash (large)", make(29919, TreeType::Deciduous, 3,
      {0, 39, 39, 51}, {10, 4, 3, 0}, {-0.05f, 0.2f, 0.16f, 0.05f},
      {45.0f, 29.42f, 15.3f, 4.6f}, {3.03f, 0.53f, 0.79f, 1.11f}, {12, 8, 6, 4},
      {8, 6, 4, 3}, {0, 0.32f, 0.34f, 0}, {0.7f, 0.62f, 0.76f, 0},
      {0.09f, -0.07f, 0, 0}, 0.01f)});
  catalog.push_back({"Aspen (small)", make(36330, TreeType::Deciduous, 2,
      {0, 70, 35, 7}, {4, 3, 3, 0}, {0.04f, -0.01f, 0.12f, 0.02f},
      {23.99f, 3.36f, 7.7f, 1.0f}, {0.37f, 0.41f, 0.7f, 0.7f}, {12, 10, 8, 6},
      {8, 6, 4, 3}, {0, 0.45f, 0.33f, 0}, {0.37f, 0.13f, 0.7f, 0.7f},
      {0, 0, 0, 0}, 0.0109f)});
  catalog.push_back({"Aspen (medium)", make(18020, TreeType::Deciduous, 2,
      {0, 75, 32, 7}, {10, 3, 3, 0}, {0.05f, 0.12f, 0.12f, 0.02f},
      {50.0f, 6.07f, 11.19f, 1.0f}, {0.72f, 0.41f, 0.7f, 0.7f}, {12, 10, 8, 6},
      {8, 6, 4, 3}, {0, 0.59f, 0.35f, 0}, {0.37f, 0.13f, 0.7f, 0.7f},
      {0, 0, 0, 0}, 0.0148f)});
  catalog.push_back({"Aspen (large)", make(30631, TreeType::Deciduous, 2,
      {0, 47, 63, 7}, {10, 6, 0, 0}, {0.05f, -0.03f, 0.12f, 0.02f},
      {69.6f, 18.56f, 11.19f, 1.0f}, {1.11f, 0.58f, 0.7f, 0.7f}, {12, 10, 8, 6},
      {8, 6, 4, 3}, {0, 0.62f, 0.05f, 0}, {0.7f, 0.13f, 0.7f, 0.7f},
      {0, 0, 0, 0}, 0.0217f)});
  catalog.push_back({"Bush 1", make(45590, TreeType::Deciduous, 3,
      {0, 21.52f, 62.61f, 60}, {7, 3, 2, 0}, {0.11f, 0.09f, 0.05f, 0.09f},
      {0.1f, 15.3f, 5.59f, 4.6f}, {0.58f, 0.95f, 0.76f, 0.7f}, {6, 6, 10, 10},
      {4, 4, 4, 3}, {0, 0.53f, 0.33f, 0}, {0.7f, 0.7f, 0.7f, 0.7f},
      {0.3f, -0.07f, 0, 0}, 0.0f)});
  catalog.push_back({"Bush 2", make(45590, TreeType::Deciduous, 2,
      {0, 19.57f, 27.39f, 60}, {10, 3, 2, 0}, {0.02f, 0.11f, 0.05f, 0.09f},
      {0.1f, 19.65f, 7.7f, 4.6f}, {0.58f, 0.95f, 0.76f, 0.7f}, {3, 4, 10, 10},
      {4, 4, 4, 3}, {0, 0.64f, 0.71f, 0}, {0.7f, 0.7f, 0.7f, 0.7f},
      {0.36f, -0.04f, 0, 0}, 0.0f)});
  catalog.push_back({"Bush 3", make(31343, TreeType::Evergreen, 3,
      {0, 66.52f, 52.83f, 0}, {13, 4, 4, 0}, {0.05f, 0.07f, 0.05f, 0.09f},
      {10.96f, 21.82f, 13.13f, 5.53f}, {0.58f, 0.95f, 0.69f, 0.74f}, {4, 3, 3, 10},
      {3, 3, 3, 3}, {0, 0.14f, 0.29f, 0}, {0.7f, 0.7f, 0.7f, 0.7f},
      {0.3f, -0.03f, 0, 0}, 0.0f)});
  return catalog;
}

}  // namespace badlands
