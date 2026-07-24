#include "game/geometry/tree_generator.hpp"
#include <random>
#include <algorithm>
#include <cmath>
#include <deque>
#include <glm/gtc/constants.hpp>   // two_pi
#include "engine/rendering/geometry/mesh_builder_utils.hpp"  // PushVertex
namespace badlands {
namespace {

// One seeded PRNG stream per generate call. range() is [lo, hi).
class TreeRng {
 public:
  explicit TreeRng(uint32_t seed) : gen_(seed) {}
  float range(float lo, float hi) {
    return std::uniform_real_distribution<float>(lo, hi)(gen_);
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

}  // namespace badlands
