#include "game/geometry/tree_generator.hpp"
#include <random>
#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>
#include <utility>
#include <glm/gtc/constants.hpp>   // two_pi
#include "engine/rendering/geometry/mesh_builder_utils.hpp"  // PushVertex
#include "game/geometry/branch_junction.hpp"  // socket footprints, loop stitching
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
  // Parentage, carried through the queue into SkeletonBranch. See the field
  // comments there: bookkeeping only, never fed back into the growth math.
  int   parent = -1;
  int   attach_section = 0;
  float attach_alpha = 0.0f;
  bool  is_continuation = false;
  float base_arc_len = 0.0f;
};

// Arc length from a branch's base to (section_index + alpha), walking the
// section polyline. The parent's own base_arc_len is added by the caller, so
// the result chains all the way to the root.
float ArcLengthAt(const std::vector<BranchSection>& sections, int section_index,
                  float alpha) {
  float arc = 0.0f;
  const int last = static_cast<int>(sections.size()) - 1;
  const int upto = std::clamp(section_index, 0, last);
  for (int i = 1; i <= upto; ++i) {
    arc += glm::length(sections[static_cast<size_t>(i)].origin -
                       sections[static_cast<size_t>(i - 1)].origin);
  }
  if (upto < last) {
    arc += alpha * glm::length(sections[static_cast<size_t>(upto + 1)].origin -
                               sections[static_cast<size_t>(upto)].origin);
  }
  return arc;
}

std::vector<int> ShuffledIndices(int count, TreeRng& rng) {
  std::vector<int> idx(static_cast<size_t>(count));
  for (int i = 0; i < count; ++i) idx[static_cast<size_t>(i)] = i;
  for (int i = count - 1; i > 0; --i) std::swap(idx[i], idx[rng.index(i + 1)]);
  return idx;
}

void GenerateChildBranches(const TreeOptions& o, TreeRng& rng, int count, int level,
                           const std::vector<BranchSection>& sections,
                           int parent_index, float parent_base_arc,
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
                     o.segments[static_cast<size_t>(level)],
                     parent_index, si, alpha, /*is_continuation=*/false,
                     parent_base_arc + ArcLengthAt(sections, si, alpha)});
  }
}

// Per-branch UV chart: how many times the texture wraps the circumference, and
// where U starts.
//
// `wraps` is proportional to CIRCUMFERENCE (not to the raw radius as it was
// before), so texel density is the same on a trunk and on a twig and does not
// jump across a joint. It shares bark_uv_scale_y's units, which makes U and V
// isotropic -- one authored knob instead of two, and bark_uv_scale_x is gone
// because a number the generator ignores is worse than a missing one.
//
// A stem CONTINUATION inherits its parent's chart outright. It has to: its
// ring 0 is positionally coincident with the parent's last ring, and the mesh
// only becomes one component if those vertices are bit-identical in ALL
// attributes -- meshopt_generateVertexRemap welds on the full stride
// (mesh_lod.cpp). Recomputing `wraps` from the continuation's own (smaller)
// base radius would differ by a texel or two and silently keep the stem split.
// Holding wraps constant along a stem is also what a tapering trunk wants:
// changing the wrap count mid-stem is not expressible without a seam.
struct BarkUvChart {
  std::vector<int> wraps;
  std::vector<float> u_offset;
};

float BarkUvScaleY(const TreeOptions& o) {
  return std::max(o.bark_uv_scale_y, 1e-6f);
}

BarkUvChart BuildBarkUvCharts(const TreeOptions& o,
                              const std::vector<SkeletonBranch>& skeleton) {
  BarkUvChart out;
  out.wraps.resize(skeleton.size());
  out.u_offset.resize(skeleton.size());
  const float scale_y = BarkUvScaleY(o);

  for (size_t i = 0; i < skeleton.size(); ++i) {
    const SkeletonBranch& br = skeleton[i];
    // Parents are always recorded before their children (FIFO growth queue),
    // asserted by tree_generator_tests' "parentage is well-formed".
    if (br.is_continuation && br.parent >= 0) {
      out.wraps[i] = out.wraps[static_cast<size_t>(br.parent)];
      out.u_offset[i] = out.u_offset[static_cast<size_t>(br.parent)];
      continue;
    }

    out.wraps[i] = std::max(
        1, static_cast<int>(std::lround(glm::two_pi<float>() * br.base_radius / scale_y)));

    if (br.parent < 0) {
      out.u_offset[i] = 0.0f;
      continue;
    }

    // A radial child starts its U at whatever U the parent carries at the
    // angle the child grows out of, so the collar band is continuous at that
    // one point. Wrap counts differ either side, so only the START aligns --
    // per-branch charts are the accepted target, and the residual shear lands
    // inside the collar.
    const SkeletonBranch& parent = skeleton[static_cast<size_t>(br.parent)];
    const int last = static_cast<int>(parent.sections.size()) - 1;
    const int si = std::clamp(br.attach_section, 0, last);
    const BranchSection& a = parent.sections[static_cast<size_t>(si)];
    const BranchSection& b =
        (si == last) ? a : parent.sections[static_cast<size_t>(si + 1)];
    // Same argument order GenerateChildBranches used to build the child frame.
    const glm::quat parent_q = glm::slerp(b.orientation, a.orientation, br.attach_alpha);

    const glm::vec3 axis_in_parent =
        glm::inverse(parent_q) * (br.sections.front().orientation * glm::vec3(0, 1, 0));
    // The ring generator places vertex j at (cos, 0, sin), so the matching
    // angle is atan2(z, x).
    const float phi = std::atan2(axis_in_parent.z, axis_in_parent.x);
    out.u_offset[i] = (phi / glm::two_pi<float>()) *
                      static_cast<float>(out.wraps[static_cast<size_t>(br.parent)]);
  }
  return out;
}

// --- Bark grafting -----------------------------------------------------------

// Floors for ShrinkToDisjoint. A socket smaller than these has no room left to
// cut a quad out of, so the caller falls back rather than emitting a sliver.
constexpr float kMinSocketHalfAngle = 0.14f;  // ~8 degrees
constexpr float kMinSocketAxial = 0.02f;      // fraction of a section
// A footprint spanning at least this many authored rings already has quads to
// cut and needs no extra ones -- which is why Oak's trunk pays nothing.
constexpr int kMinRingsPerSocket = 2;
constexpr float kRingMergeTol = 1e-3f;

struct Socket {
  size_t child = 0;
  SocketFootprint footprint;
  // The hole, as a rectangle in the parent's EMITTED grid: quads
  // [ring_lo, ring_hi] x the circular segment run starting at seg_lo.
  int ring_lo = 0, ring_hi = 0;
  int seg_lo = 0, seg_count = 0;
  // For an axial join: the parent section parameter at which the child actually
  // emerges. Not always the parent's tip -- see the comment where it is set.
  float axial_param = -1.0f;
};

struct BranchLayout {
  uint32_t offset = 0;   // first vertex of this branch
  int n = 0;             // vertices per ring (segments + 1)
  int rings = 0;         // rings actually emitted
  int segments = 0;
  int first_ring = 0;    // index into the branch's refined ring params
  // For a stem continuation: the parent whose LAST ring serves as this
  // branch's base row, so the two share vertices outright.
  //
  // The continuation's ring 0 is exactly coincident with its parent's last
  // ring, and it is tempting to emit both and let the weld merge them -- that
  // is what this did first, and it was quietly wrong. Attribute-based welding
  // is fragile: SmoothVertexNormals rewrites the collar's normals, and any
  // continuation ring that happens to sit on a collar boundary then differs
  // from its twin by a normal and stops merging. Oak (large) lost 44 of its 71
  // stem seams that way while every junction still reported success. Aliasing
  // makes the join index identity instead, which nothing downstream can undo.
  int alias_parent = -1;
};

// The vertex ids of one emitted ring of a branch, columns 0..segments (the last
// is the duplicated UV seam).
uint32_t RingVertex(const BranchLayout& l, int ring, int column) {
  return l.offset + static_cast<uint32_t>(ring * l.n + column);
}

// A section at a fractional ring index, so refinement can add rings anywhere
// along the polyline without disturbing the authored ones.
BranchSection LerpSection(const std::vector<BranchSection>& s, float t) {
  const int last = static_cast<int>(s.size()) - 1;
  if (last <= 0) return s.front();
  const float ct = std::clamp(t, 0.0f, static_cast<float>(last));
  const int i = std::clamp(static_cast<int>(std::floor(ct)), 0, last - 1);
  const float a = ct - static_cast<float>(i);
  const BranchSection& s0 = s[static_cast<size_t>(i)];
  const BranchSection& s1 = s[static_cast<size_t>(i + 1)];
  return {glm::mix(s0.origin, s1.origin, a),
          glm::slerp(s0.orientation, s1.orientation, a),
          glm::mix(s0.radius, s1.radius, a)};
}

// One ring's worth of surface points, in the branch's own local space.
void RingPositions(const BranchSection& sec, int segments,
                   std::vector<glm::vec3>& out) {
  out.clear();
  for (int j = 0; j < segments; ++j) {
    const float angle = glm::two_pi<float>() * static_cast<float>(j) /
                        static_cast<float>(segments);
    const glm::vec3 dir(std::cos(angle), 0.0f, std::sin(angle));
    out.push_back(sec.origin + sec.orientation * (dir * sec.radius));
  }
}

// The first of a child's rings that lies wholly outside its parent's tube.
// Everything below it is buried and is replaced by the collar. Returns -1 when
// the child never clears the parent at all (a stub swallowed by its own trunk),
// which is a fallback, not an error.
int FirstFreeRing(const SkeletonBranch& child, const std::vector<float>& params,
                  const std::vector<BranchSection>& parent_sections) {
  const int segments = std::max(3, child.segment_count);
  std::vector<glm::vec3> ring;
  // The LAST ring counts. Oak's level-3 twigs carry `sections[3] = 1`, i.e. two
  // rings, and ring 0 is buried by definition -- refusing the last one made
  // every one of those fall back. A branch reduced to a single ring is fine:
  // its collar becomes the whole twig, a cone from the parent's surface to the
  // 0.001-radius tip, which is exactly the shape it had anyway.
  for (size_t k = 0; k < params.size(); ++k) {
    RingPositions(LerpSection(child.sections, params[k]), segments, ring);
    bool all_free = true;
    for (const glm::vec3& p : ring) {
      if (!IsOutsideTube(parent_sections, p, 0.0f)) { all_free = false; break; }
    }
    if (all_free) return static_cast<int>(k);
  }
  return -1;
}

// Marks the parent quads a socket cuts out, and records the rectangle so the
// collar can walk its boundary. Returns false when no usable hole exists.
bool MarkHole(Socket& s, const std::vector<BranchSection>& parent_sections,
              const glm::quat& child_orientation, const std::vector<float>& params,
              int start, int emitted, int segments, std::vector<uint8_t>& hole) {
  if (emitted < 2) return false;
  const int quad_rows = emitted - 1;

  // The angle a child leaves at, measured in the frame of a GIVEN row rather
  // than once at the attach point.
  //
  // Ring frames rotate row to row by `twist` plus gnarliness, and twist is not
  // small: Oak's twist[1] is 0.42 rad -- 24 degrees per section -- with Bush 2
  // at 0.36 and Ash at 0.3. A hole three rows tall would otherwise be cut up to
  // 70 degrees away from the branch at its far end, comparable to the whole
  // socket's width, so the hole would miss and the collar would skew sideways
  // across the trunk.
  auto angle_in_row = [&](float row_param) {
    const glm::quat frame = LerpSection(parent_sections, row_param).orientation;
    const glm::vec3 axis =
        glm::inverse(frame) * (child_orientation * glm::vec3(0.0f, 1.0f, 0.0f));
    return std::atan2(axis.z, axis.x);
  };

  // Rows whose centre falls inside the footprint's axial span. Ring refinement
  // bracketed the span, so there is normally at least one.
  int row_lo = -1, row_hi = -1;
  for (int i = 0; i < quad_rows; ++i) {
    const float centre = 0.5f * (params[static_cast<size_t>(start + i)] +
                                 params[static_cast<size_t>(start + i + 1)]);
    if (centre < s.footprint.axial_min || centre > s.footprint.axial_max) continue;
    if (row_lo < 0) row_lo = i;
    row_hi = i;
  }
  if (row_lo < 0) {
    // The footprint fell between two ring centres. Take the nearest row rather
    // than dropping the junction -- an over-cut of one row reads as a slightly
    // deeper collar, where a fallback costs a whole mesh component.
    const float want = 0.5f * (s.footprint.axial_min + s.footprint.axial_max);
    float best = std::numeric_limits<float>::max();
    for (int i = 0; i < quad_rows; ++i) {
      const float centre = 0.5f * (params[static_cast<size_t>(start + i)] +
                                   params[static_cast<size_t>(start + i + 1)]);
      const float d = std::abs(centre - want);
      if (d < best) { best = d; row_lo = row_hi = i; }
    }
    if (row_lo < 0) return false;
  }

  auto row_centre = [&](int i) {
    return 0.5f * (params[static_cast<size_t>(start + i)] +
                   params[static_cast<size_t>(start + i + 1)]);
  };
  auto wrap = [](float d) { return std::atan2(std::sin(d), std::cos(d)); };

  // Columns the branch passes through, taking the UNION over the hole's rows so
  // the cut follows the twist instead of being fixed at the attach frame. The
  // rows' angles drift monotonically, so the union stays a contiguous run.
  // Over-cutting slightly is the right way to be wrong here: the collar simply
  // spans a little more of the parent, whereas a hole that misses leaves the
  // branch pushing through uncut bark.
  std::vector<uint8_t> marked(static_cast<size_t>(segments), 0u);
  int count = 0;
  for (int j = 0; j < segments; ++j) {
    const float angle = glm::two_pi<float>() * (static_cast<float>(j) + 0.5f) /
                        static_cast<float>(segments);
    for (int i = row_lo; i <= row_hi; ++i) {
      if (std::abs(wrap(angle - angle_in_row(row_centre(i)))) < s.footprint.half_angle) {
        marked[static_cast<size_t>(j)] = 1u;
        ++count;
        break;
      }
    }
  }
  if (count == 0) {
    // Narrower than one segment -- Pine's 42-degree footprint against 45-degree
    // segments lands here routinely. Cut the single nearest column; the collar
    // then funnels inward, which is the honest shape for that geometry.
    const float centre_angle = angle_in_row(row_centre((row_lo + row_hi) / 2));
    float best = std::numeric_limits<float>::max();
    int best_j = 0;
    for (int j = 0; j < segments; ++j) {
      const float angle = glm::two_pi<float>() * (static_cast<float>(j) + 0.5f) /
                          static_cast<float>(segments);
      const float d = std::abs(wrap(angle - centre_angle));
      if (d < best) { best = d; best_j = j; }
    }
    marked[static_cast<size_t>(best_j)] = 1u;
    count = 1;
  }
  // A hole that swallowed the whole ring would sever the parent. Reachable once
  // twist drift widens the union, so this is a live guard, not a formality.
  if (count >= segments) return false;

  // Start of the circular run.
  int seg_lo = 0;
  for (int j = 0; j < segments; ++j) {
    if (marked[static_cast<size_t>(j)] &&
        !marked[static_cast<size_t>((j - 1 + segments) % segments)]) {
      seg_lo = j;
      break;
    }
  }

  // ShrinkToDisjoint only separates footprints in CONTINUOUS space. Marking then
  // snaps to whole quads -- and both fallbacks above ("nearest row", "nearest
  // column") snap without consulting anyone else -- so two siblings that are
  // genuinely disjoint can still round onto the same quads. Overlapping
  // rectangles would remove the union but leave each collar bridging only its
  // own boundary, tearing an open crack in the bark between them; identical ones
  // would stitch two collars over one hole. Refuse instead: a buried tube is a
  // worse LOD than a collar, but it is not a hole in the tree.
  for (int i = row_lo; i <= row_hi; ++i) {
    for (int k = 0; k < count; ++k) {
      const int j = (seg_lo + k) % segments;
      if (hole[static_cast<size_t>(i) * static_cast<size_t>(segments) +
               static_cast<size_t>(j)]) {
        return false;
      }
    }
  }

  s.ring_lo = row_lo;
  s.ring_hi = row_hi;
  s.seg_lo = seg_lo;
  s.seg_count = count;
  for (int i = row_lo; i <= row_hi; ++i) {
    for (int k = 0; k < count; ++k) {
      const int j = (seg_lo + k) % segments;
      hole[static_cast<size_t>(i) * static_cast<size_t>(segments) +
           static_cast<size_t>(j)] = 1u;
    }
  }
  return true;
}

// Walks the hole's boundary as a closed loop of existing parent vertex ids.
//
// Columns are taken mod `segments`, so a hole straddling the ring's UV seam
// picks up column 0 where column `segments` would be. Those two are the SAME
// POSITION (the seam vertex is duplicated only to carry a second U), so the
// surface still meets exactly -- no crack -- and the collar stays connected to
// the parent through the rest of its boundary.
std::vector<uint32_t> HoleBoundary(const BranchLayout& parent, const Socket& s,
                                   const std::vector<float>& parent_params) {
  std::vector<uint32_t> loop;
  if (s.footprint.axial) {
    // No hole is cut: a coaxial child encloses its parent from wherever it
    // clears it, so a whole parent ring is the boundary. Which ring matters --
    // the emergence point is only the tip when the child is thick enough or
    // attaches high enough. Bush 3's coaxial children attach from `start[3]` = 0
    // upward, and a thinner one on a tapering evergreen emerges through the side
    // partway along; bridging to the tip regardless would run the collar
    // backwards down over the parent's own surface.
    int ring = parent.rings - 1;
    if (s.axial_param >= 0.0f) {
      float best = std::numeric_limits<float>::max();
      for (int i = 0; i < parent.rings; ++i) {
        const float d = std::abs(
            parent_params[static_cast<size_t>(parent.first_ring + i)] - s.axial_param);
        if (d < best) { best = d; ring = i; }
      }
    }
    for (int j = 0; j < parent.segments; ++j)
      loop.push_back(RingVertex(parent, ring, j));
    return loop;
  }
  const int r0 = s.ring_lo;
  const int r1 = s.ring_hi + 1;
  const int cols = s.seg_count;
  auto vid = [&](int ring, int col) {
    return parent.offset + static_cast<uint32_t>(ring * parent.n +
                                                 ((s.seg_lo + col) % parent.segments));
  };
  for (int k = 0; k <= cols; ++k) loop.push_back(vid(r0, k));
  for (int i = r0 + 1; i <= r1; ++i) loop.push_back(vid(i, cols));
  for (int k = cols - 1; k >= 0; --k) loop.push_back(vid(r1, k));
  for (int i = r1 - 1; i > r0; --i) loop.push_back(vid(i, 0));
  return loop;
}

glm::vec3 VertexPosition(const StaticTexturedMeshComponent& mesh, uint32_t id) {
  const size_t base = static_cast<size_t>(id) * kTexturedMeshFloatsPerVertex;
  return {mesh.vertices[base], mesh.vertices[base + 1], mesh.vertices[base + 2]};
}

// Bridges the hole's boundary to the child's first free ring. Appends the new
// triangles and records every vertex they touch, for the normal pass.
bool EmitCollar(StaticTexturedMeshComponent& mesh, const BranchLayout& parent,
                const BranchLayout& child, const Socket& s,
                const std::vector<float>& parent_params,
                std::vector<uint32_t>& touched) {
  const std::vector<uint32_t> parent_loop = HoleBoundary(parent, s, parent_params);
  std::vector<uint32_t> child_loop;
  for (int j = 0; j < child.segments; ++j)
    child_loop.push_back(child.offset + static_cast<uint32_t>(j));
  if (parent_loop.size() < 3 || child_loop.size() < 3) return false;

  std::vector<glm::vec3> parent_pos, child_pos;
  for (uint32_t id : parent_loop) parent_pos.push_back(VertexPosition(mesh, id));
  for (uint32_t id : child_loop) child_pos.push_back(VertexPosition(mesh, id));

  const std::vector<uint32_t> tris =
      StitchLoops(parent_loop, parent_pos, child_loop, child_pos);
  if (tris.empty()) return false;

  mesh.indices.insert(mesh.indices.end(), tris.begin(), tris.end());
  touched.insert(touched.end(), parent_loop.begin(), parent_loop.end());
  touched.insert(touched.end(), child_loop.begin(), child_loop.end());
  return true;
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

    skeleton.push_back({sections, br.segment_count, br.radius, br.level,
                        br.parent, br.attach_section, br.attach_alpha,
                        br.is_continuation, br.base_arc_len});
    // This branch's own index, for the children queued below.
    const int self_index = static_cast<int>(skeleton.size()) - 1;
    const int last_section = static_cast<int>(sections.size()) - 1;
    const BranchSection& last = sections.back();

    // Deciduous stem continuation (inherits parent section/segment counts).
    // Its ring 0 is coincident with the parent's last ring, so the bark
    // grafter never sockets it -- the UV field alone welds that seam.
    if (o.type == TreeType::Deciduous && br.level < o.levels) {
      queue.push_back({last.origin, last.orientation, o.length[br.level + 1],
                       last.radius, br.level + 1, br.section_count, br.segment_count,
                       self_index, last_section, 0.0f, /*is_continuation=*/true,
                       br.base_arc_len + ArcLengthAt(sections, last_section, 0.0f)});
    }
    // Radial children (both types). Leaves at the terminal level are SP2.
    if (br.level < o.levels) {
      GenerateChildBranches(o, rng, o.children[static_cast<size_t>(br.level)],
                            br.level + 1, sections, self_index, br.base_arc_len,
                            queue);
    }
  }
  return skeleton;
}

TexturedMeshResult GenerateTreeMesh(const TreeOptions& o) {
  return GenerateTreeMesh(o, BuildTreeSkeleton(o));
}

TexturedMeshResult GenerateTreeMesh(const TreeOptions& o,
                                    const std::vector<SkeletonBranch>& skeleton,
                                    BarkMeshStats* stats) {
  StaticTexturedMeshComponent mesh;
  const BarkUvChart charts = BuildBarkUvCharts(o, skeleton);
  const size_t branch_count = skeleton.size();
  BarkMeshStats tally;

  // --- One socket per radial child, with its footprint on the parent's surface.
  // Continuations are absent by design: their ring 0 is already coincident with
  // the parent's last ring, so the UV field welds them at no geometric cost.
  std::vector<std::vector<Socket>> sockets(branch_count);
  for (size_t i = 1; i < branch_count; ++i) {
    const SkeletonBranch& br = skeleton[i];
    if (br.parent < 0 || br.is_continuation) continue;
    const SkeletonBranch& parent = skeleton[static_cast<size_t>(br.parent)];
    Socket s;
    s.child = i;
    s.footprint = ComputeSocketFootprint(parent.sections, br.attach_section,
                                         br.attach_alpha,
                                         br.sections.front().orientation,
                                         br.base_radius);
    sockets[static_cast<size_t>(br.parent)].push_back(s);
    ++tally.junctions;
  }

  // --- Colliding siblings shrink rather than fall back. See ShrinkToDisjoint:
  // overlap is structural on the deciduous presets, so discarding it would
  // throw away most of what the graft exists to merge.
  for (size_t p = 0; p < branch_count; ++p) {
    if (sockets[p].size() < 2) continue;
    std::vector<SocketFootprint> fps;
    fps.reserve(sockets[p].size());
    for (const Socket& s : sockets[p]) fps.push_back(s.footprint);
    tally.shrunk += ShrinkToDisjoint(fps, kMinSocketHalfAngle, kMinSocketAxial);
    for (size_t k = 0; k < fps.size(); ++k) sockets[p][k].footprint = fps[k];
  }

  // --- Refined ring sets. Only sockets that would fall BETWEEN two authored
  // rings pay for extra ones, so Oak's trunk is untouched and Pine's gets the
  // resolution its 0.4-tall footprints need against 4.2-tall quads.
  std::vector<std::vector<float>> ring_params(branch_count);
  for (size_t b = 0; b < branch_count; ++b) {
    const int last = static_cast<int>(skeleton[b].sections.size()) - 1;
    std::vector<float> authored;
    authored.reserve(static_cast<size_t>(last) + 1);
    for (int i = 0; i <= last; ++i) authored.push_back(static_cast<float>(i));
    std::vector<std::pair<float, float>> spans;
    for (const Socket& s : sockets[b])
      if (s.footprint.valid) spans.emplace_back(s.footprint.axial_min, s.footprint.axial_max);
    ring_params[b] =
        MergeRingParams(authored, spans, kMinRingsPerSocket, kRingMergeTol);
  }

  // --- Where each socketed child's tube finally clears its parent. The rings
  // below that are buried and are not emitted at all; the collar replaces them.
  std::vector<int> first_ring(branch_count, 0);
  // A stem continuation skips its own ring 0 and reuses its parent's last ring
  // instead -- see BranchLayout::alias_parent.
  for (size_t i = 1; i < branch_count; ++i)
    if (skeleton[i].is_continuation && skeleton[i].parent >= 0) first_ring[i] = 1;

  for (size_t p = 0; p < branch_count; ++p) {
    for (Socket& s : sockets[p]) {
      if (!s.footprint.valid) continue;
      const int fr = FirstFreeRing(skeleton[s.child], ring_params[s.child],
                                   skeleton[p].sections);
      if (fr < 0) {
        s.footprint.valid = false;  // never clears its parent -- leave it a tube
        continue;
      }
      first_ring[s.child] = fr;
      // An axial child leaves through the parent's surface wherever it finally
      // clears it, which is NOT always the tip: Bush 3's `start[3]` is 0, so its
      // coaxial children attach all the way down and a thinner one emerges
      // through the side of a tapering parent well below the end. Bridging to
      // the last ring regardless would run the collar backwards down the parent.
      if (s.footprint.axial) {
        const BranchSection emerge =
            LerpSection(skeleton[s.child].sections, ring_params[s.child][static_cast<size_t>(fr)]);
        const TubeHit hit = ClosestOnTube(skeleton[p].sections, emerge.origin);
        s.axial_param = static_cast<float>(hit.section) + hit.alpha;
      }
    }
  }

  // --- Cut the holes BEFORE any ring is dropped.
  //
  // Marking has to happen ahead of the sweep, not during it: a socket can still
  // fail here (its parent may have been reduced to a single ring, or a sibling
  // may already own the quads), and by then the child's buried rings would have
  // been discarded. That would leave the branch truncated AND unstitched --
  // floating with a gap at its base, strictly worse than the buried tube the
  // fallback is meant to restore. Everything below is index-space arithmetic on
  // `ring_params`, so none of it needs vertices to exist yet.
  //
  // Parents always precede their children (FIFO growth queue), so resetting a
  // child's `first_ring` here still lands before that child is swept.
  std::vector<std::vector<uint8_t>> holes(branch_count);
  std::vector<int> emitted_rings(branch_count, 0);
  for (size_t b = 0; b < branch_count; ++b) {
    const int segments = std::max(3, skeleton[b].segment_count);
    const std::vector<float>& params = ring_params[b];
    const int start = std::min(first_ring[b], static_cast<int>(params.size()) - 1);
    const int emitted = static_cast<int>(params.size()) - start;
    emitted_rings[b] = emitted;
    holes[b].assign(static_cast<size_t>(std::max(emitted - 1, 0)) *
                        static_cast<size_t>(segments), 0u);

    for (Socket& s : sockets[b]) {
      if (!s.footprint.valid) continue;
      if (s.footprint.axial) continue;  // joins at a ring; cuts nothing
      if (MarkHole(s, skeleton[b].sections, skeleton[s.child].sections.front().orientation,
                   params, start, emitted, segments, holes[b])) {
        continue;
      }
      // Put the child back the way it was: a plain buried tube.
      s.footprint.valid = false;
      first_ring[s.child] = skeleton[s.child].is_continuation ? 1 : 0;
    }
  }

  // --- Sweep every branch.
  std::vector<BranchLayout> layout(branch_count);
  for (size_t b = 0; b < branch_count; ++b) {
    const SkeletonBranch& br = skeleton[b];
    const int segments = std::max(3, br.segment_count);
    const int n = segments + 1;  // ring verts incl. the duplicated UV seam
    const std::vector<float>& params = ring_params[b];
    const int start = std::min(first_ring[b], static_cast<int>(params.size()) - 1);
    const int emitted = static_cast<int>(params.size()) - start;

    const int alias_parent =
        (br.is_continuation && br.parent >= 0) ? br.parent : -1;
    layout[b] = {mesh.vertex_count, n, emitted, segments, start, alias_parent};

    // cum_len walks from the branch's own base even when the first rings are
    // dropped, so V stays anchored to the root rather than to whatever ring
    // happened to survive.
    float cum_len = 0.0f;
    glm::vec3 prev = LerpSection(br.sections, params[0]).origin;
    for (size_t k = 0; k < params.size(); ++k) {
      const BranchSection sec = LerpSection(br.sections, params[k]);
      cum_len += glm::length(sec.origin - prev);
      prev = sec.origin;
      if (static_cast<int>(k) < start) continue;
      const float v = (br.base_arc_len + cum_len) / BarkUvScaleY(o);

      for (int j = 0; j <= segments; ++j) {
        const int jj = (j == segments) ? 0 : j;  // wrap position; U still reaches `wraps`
        const float angle = glm::two_pi<float>() * static_cast<float>(jj) /
                            static_cast<float>(segments);
        const glm::vec3 dir(std::cos(angle), 0.0f, std::sin(angle));
        const glm::vec3 tan_dir(-std::sin(angle), 0.0f, std::cos(angle));
        const glm::vec3 pos = sec.origin + sec.orientation * (dir * sec.radius);
        const glm::vec3 nrm = glm::normalize(sec.orientation * dir);
        const glm::vec3 tng = glm::normalize(sec.orientation * tan_dir);
        const float u = charts.u_offset[b] +
                        (static_cast<float>(j) / static_cast<float>(segments)) *
                            static_cast<float>(charts.wraps[b]);
        PushVertex(mesh.vertices, pos, glm::vec2(u, v), nrm,
                   glm::vec4(tng, kDefaultTangentHandedness));
      }
    }
    mesh.vertex_count = static_cast<uint32_t>(mesh.vertices.size() / kTexturedMeshFloatsPerVertex);

    const std::vector<uint8_t>& hole = holes[b];

    // The stem's base row bridges the parent's last ring to this branch's first,
    // sharing the parent's vertices outright rather than emitting a coincident
    // copy and hoping a weld finds it.
    if (alias_parent >= 0) {
      const BranchLayout& pl = layout[static_cast<size_t>(alias_parent)];
      const int pr = pl.rings - 1;
      for (int j = 0; j < segments; ++j) {
        const uint32_t v1 = RingVertex(pl, pr, j);
        const uint32_t v2 = RingVertex(pl, pr, j + 1);
        const uint32_t v3 = RingVertex(layout[b], 0, j);
        const uint32_t v4 = RingVertex(layout[b], 0, j + 1);
        mesh.indices.insert(mesh.indices.end(), {v1, v3, v2, v2, v3, v4});
      }
    }

    for (int i = 0; i + 1 < emitted; ++i) {
      for (int j = 0; j < segments; ++j) {
        if (hole[static_cast<size_t>(i) * static_cast<size_t>(segments) +
                 static_cast<size_t>(j)]) continue;
        const uint32_t v1 = RingVertex(layout[b], i, j);
        const uint32_t v2 = RingVertex(layout[b], i, j + 1);
        const uint32_t v3 = RingVertex(layout[b], i + 1, j);
        const uint32_t v4 = RingVertex(layout[b], i + 1, j + 1);
        mesh.indices.insert(mesh.indices.end(), {v1, v3, v2, v2, v3, v4});
      }
    }
  }

  // --- Collars. Each bridges a hole's boundary (existing PARENT vertices) to
  // the child's first free ring (existing CHILD vertices), so the two shells
  // become one component by index identity -- nothing here relies on a later
  // weld pass finding coincident vertices.
  std::vector<uint32_t> collar_vertices;
  for (size_t p = 0; p < branch_count; ++p) {
    for (const Socket& s : sockets[p]) {
      if (!s.footprint.valid) continue;
      if (!EmitCollar(mesh, layout[p], layout[s.child], s, ring_params[p],
                      collar_vertices)) continue;
      ++tally.stitched;
    }
  }
  tally.fallback = tally.junctions - tally.stitched;

  // Sharing vertices is what connects the collar, but it also leaves the
  // parent's boundary normals pointing off the PARENT and the child's off the
  // CHILD -- up to 90 degrees apart across one band. Positions never move, so
  // the bark AABB (and with it forest spacing) is untouched.
  SmoothVertexNormals(mesh.vertices, kTexturedMeshFloatsPerVertex, mesh.indices,
                      collar_vertices);

  if (stats) *stats = tally;
  mesh.dirty = true;
  const Aabb bounds = ComputeLocalAabb(mesh);
  return {.mesh = std::move(mesh), .local_bounds = bounds};
}

int QuadsPerLeafSite(const LeafOptions& lf) {
  switch (lf.arrangement) {
    case LeafArrangement::SingleQuad:  return 1;
    case LeafArrangement::CrossedPair: return 2;
    case LeafArrangement::FanFromStem:
    case LeafArrangement::AxialFins:   return lf.blade_count;
  }
  return 1;  // unreachable
}

namespace {

// FanFromStem: angular step between adjacent blades (N=3 -> -35/0/+35 deg,
// N=2 -> +-17.5 deg) and the alternating out-of-plane tilt that keeps blades
// from ever going fully coplanar -- the LOD simplifier welds bit-identical
// full-stride vertices, so coplanar cards would invite cross-blade collapse.
constexpr float kFanSpreadDeg = 35.0f;
constexpr float kFanDihedralDeg = 20.0f;

// Per-blade local rotation, composed onto the site's placement orientation by
// the caller (rot_b = orient * BladeRotation(...)). RNG-free: b is the blade
// index in [0, n), n = QuadsPerLeafSite(lf).
glm::quat BladeRotation(LeafArrangement arrangement, int b, int n) {
  switch (arrangement) {
    case LeafArrangement::FanFromStem: {
      const float spread = glm::radians(
          (static_cast<float>(b) - (static_cast<float>(n) - 1.0f) * 0.5f) * kFanSpreadDeg);
      const float dihedral = glm::radians((b % 2 == 0) ? kFanDihedralDeg : -kFanDihedralDeg);
      return glm::angleAxis(spread, glm::vec3(0, 0, 1)) *
             glm::angleAxis(dihedral, glm::vec3(0, 1, 0));
    }
    case LeafArrangement::AxialFins:
      return glm::angleAxis(static_cast<float>(b) * glm::pi<float>() / static_cast<float>(n),
                            glm::vec3(0, 1, 0));
    case LeafArrangement::SingleQuad:
    case LeafArrangement::CrossedPair:
      break;
  }
  return glm::angleAxis((b == 1) ? glm::half_pi<float>() : 0.0f, glm::vec3(0, 1, 0));
}

}  // namespace

TexturedMeshResult GenerateLeafMesh(const TreeOptions& o) {
  return GenerateLeafMesh(o, BuildTreeSkeleton(o));
}

TexturedMeshResult GenerateLeafMesh(const TreeOptions& o,
                                    const std::vector<SkeletonBranch>& skeleton) {
  const LeafOptions& lf = o.leaves;
  StaticTexturedMeshComponent mesh;

  if (lf.enabled && lf.count > 0) {
    TreeRng rng(o.seed ^ 0x9E3779B9u);
    const int quads_per_leaf = QuadsPerLeafSite(lf);

    // One leaf (1+ disjoint blade quads) at a placement frame. Consumes one
    // rng draw (size variance) as its FIRST statement -- draw order/count per
    // site is determinism-critical (byte-compared by the "deterministic" test).
    // Shared by distributed leaves and the terminal-tip leaf.
    auto emit_leaf = [&](const glm::vec3& origin, const glm::quat& orient) {
      const float leaf_size = lf.size * (1.0f - lf.size_variance * rng.unit());
      glm::vec3 rnormal(origin.x, 0.0f, origin.z);
      rnormal = (glm::length(rnormal) > 1e-5f) ? glm::normalize(rnormal)
                                               : glm::vec3(0, 0, 1);
      const float half_w = leaf_size * lf.card_aspect * 0.5f;
      const glm::vec3 local[4] = {{-half_w, leaf_size, 0.0f},
                                  {-half_w, 0.0f, 0.0f},
                                  { half_w, 0.0f, 0.0f},
                                  { half_w, leaf_size, 0.0f}};
      const glm::vec2 uv[4] = {{0, 1}, {0, 0}, {1, 0}, {1, 1}};
      for (int q = 0; q < quads_per_leaf; ++q) {
        const glm::quat rot = orient * BladeRotation(lf.arrangement, q, quads_per_leaf);
        // Every blade is a fully disjoint 4-vert quad: never share/dedupe
        // vertices across blades (see the weld-safety note above).
        const glm::vec3 tangent = glm::normalize(rot * glm::vec3(1, 0, 0));
        const uint32_t base = mesh.vertex_count;
        for (int c = 0; c < 4; ++c) {
          PushVertex(mesh.vertices, origin + rot * local[c], uv[c], rnormal,
                     glm::vec4(tangent, kDefaultTangentHandedness));
        }
        mesh.vertex_count =
            static_cast<uint32_t>(mesh.vertices.size() / kTexturedMeshFloatsPerVertex);
        mesh.indices.insert(mesh.indices.end(),
                            {base + 0u, base + 1u, base + 2u, base + 0u, base + 2u, base + 3u});
      }
    };

    for (const SkeletonBranch& br : skeleton) {
      if (br.level != o.levels) continue;
      const int last = static_cast<int>(br.sections.size()) - 1;
      if (last < 1) continue;

      const float radial_offset = rng.unit();
      const float start_min = lf.start;
      const float height_step = (1.0f - start_min) / static_cast<float>(lf.count);
      const std::vector<int> slots = ShuffledIndices(lf.count, rng);

      for (int i = 0; i < lf.count; ++i) {
        const float leaf_start =
            start_min + (static_cast<float>(i) + rng.unit()) * height_step;
        int si = static_cast<int>(std::floor(leaf_start * static_cast<float>(last)));
        si = std::clamp(si, 0, last);
        const BranchSection& a = br.sections[static_cast<size_t>(si)];
        const BranchSection& b =
            (si == last) ? a : br.sections[static_cast<size_t>(si + 1)];
        float alpha = (leaf_start - static_cast<float>(si) / static_cast<float>(last)) /
                      (1.0f / static_cast<float>(last));
        alpha = std::clamp(alpha, 0.0f, 1.0f);

        const glm::vec3 origin = glm::mix(a.origin, b.origin, alpha);
        const glm::quat parent = glm::slerp(b.orientation, a.orientation, alpha);
        const float radial_angle =
            glm::two_pi<float>() *
            (radial_offset + (static_cast<float>(slots[static_cast<size_t>(i)]) +
                              rng.range(-0.5f, 0.5f)) / static_cast<float>(lf.count));
        const glm::quat leaf_orient =
            parent * glm::angleAxis(radial_angle, glm::vec3(0, 1, 0)) *
            glm::angleAxis(glm::radians(lf.angle), glm::vec3(1, 0, 0));
        emit_leaf(origin, leaf_orient);
      }

      // ez-tree deciduous terminal-tip leaf: one extra leaf at the branch endpoint.
      if (lf.tip_leaf) {
        const BranchSection& tip = br.sections[static_cast<size_t>(last)];
        emit_leaf(tip.origin, tip.orientation);
      }
    }
  }

  mesh.dirty = true;
  const Aabb bounds = ComputeLocalAabb(mesh);
  return {.mesh = std::move(mesh), .local_bounds = bounds};
}

std::vector<NamedTreeOptions> TreeCatalog() {
  // ez-tree presets, one struct per src/lib/presets/<name>.json. Designated
  // initializers name every field, so this stays auditable against the source
  // and a mis-ordered value can't compile silently. Fields left out
  // (force_dir = (0,1,0), bark_uv_scale_y = 1) keep their TreeOptions defaults.
  using T = TreeType;
  std::vector<NamedTreeOptions> catalog;
  catalog.push_back({"Oak (small)", {
      .seed = 30895, .type = T::Deciduous, .levels = 3,
      .angle = {0, 54, 58, 32}, .children = {4, 2, 3, 0},
      .gnarliness = {0.07f, -0.08f, 0.11f, 0.09f},
      .length = {28.08f, 4.55f, 9.78f, 7.16f}, .radius = {1.0f, 1.02f, 0.69f, 1.19f},
      .sections = {16, 9, 8, 1}, .segments = {7, 5, 3, 3},
      .start = {0, 0.49f, 0.06f, 0.12f}, .taper = {0.73f, 0.42f, 0.69f, 0.75f},
      .twist = {-0.23f, 0.42f, 0, 0}, .force_strength = 0.01f,
      .leaves = {.arrangement=LeafArrangement::FanFromStem, .blade_count=2, .card_aspect=0.95f,
                 .count=24, .start=0.16f, .size=1.46f, .size_variance=0.7f, .angle=42.0f,
                 .tint={0.32f,0.52f,0.18f}, .transmission_tint={0.55f,0.62f,0.10f},
                 .transmission_strength=0.65f, .silhouette=LeafSilhouette::Oak}}});
  catalog.push_back({"Oak (medium)", OakPreset()});
  catalog.push_back({"Oak (large)", {
      .seed = 23399, .type = T::Deciduous, .levels = 3,
      .angle = {0, 54, 43, 32}, .children = {9, 5, 3, 0},
      .gnarliness = {-0.04f, 0.16f, -0.06f, 0.09f},
      .length = {47.7f, 29.39f, 17.62f, 7.16f}, .radius = {3.0f, 0.69f, 0.69f, 1.19f},
      .sections = {16, 9, 8, 3}, .segments = {12, 5, 3, 3},
      .start = {0, 0.35f, 0.1f, 0}, .taper = {0.73f, 0.42f, 0.69f, 0.75f},
      .twist = {-0.23f, 0.42f, 0, 0}, .force_strength = 0.02f,
      .leaves = {.arrangement=LeafArrangement::FanFromStem, .blade_count=2, .card_aspect=0.95f,
                 .count=17, .start=0.16f, .size=3.94f, .size_variance=0.7f, .angle=36.0f,
                 .tint={0.32f,0.52f,0.18f}, .transmission_tint={0.55f,0.62f,0.10f},
                 .transmission_strength=0.65f, .silhouette=LeafSilhouette::Oak}}});
  catalog.push_back({"Pine (small)", {
      .seed = 11744, .type = T::Evergreen, .levels = 1,
      .angle = {0, 117, 60, 60}, .children = {91, 7, 5, 0},
      .gnarliness = {0.05f, 0.08f, 0, 0},
      .length = {39.55f, 12.12f, 10.0f, 1.0f}, .radius = {0.55f, 0.41f, 0.7f, 0.7f},
      .sections = {12, 10, 8, 6}, .segments = {8, 6, 4, 3},
      .start = {0, 0.16f, 0.3f, 0.3f}, .taper = {0.7f, 0.7f, 0.7f, 0.7f},
      .twist = {0, 0, 0, 0}, .force_strength = 0.0f,
      .leaves = {.arrangement=LeafArrangement::AxialFins, .blade_count=3, .card_aspect=0.45f,
                 .count=196, .start=0.0f, .size=1.610f, .size_variance=0.7f, .angle=10.0f,
                 .alpha_cutoff=0.35f, .tint={0.16f,0.40f,0.24f}, .transmission_tint={0.32f,0.46f,0.12f},
                 .transmission_strength=0.28f, .silhouette=LeafSilhouette::PineSprig, .tip_leaf=false}}});
  catalog.push_back({"Pine (medium)", PinePreset()});
  catalog.push_back({"Pine (large)", {
      .seed = 44166, .type = T::Evergreen, .levels = 1,
      .angle = {0, 129.13f, 16, 60}, .children = {100, 3, 0, 0},
      .gnarliness = {0.05f, 0.08f, 0, 0},
      .length = {65.25f, 34.85f, 27.25f, 1.0f}, .radius = {1.27f, 0.37f, 0.7f, 0.7f},
      .sections = {12, 10, 8, 6}, .segments = {8, 6, 4, 3},
      .start = {0, 0.29f, 0.14f, 0.3f}, .taper = {0.7f, 0.7f, 0.7f, 0.7f},
      .twist = {0, 0, 0, 0}, .force_strength = 0.009f,
      .leaves = {.arrangement=LeafArrangement::AxialFins, .blade_count=3, .card_aspect=0.45f,
                 .count=168, .start=0.076f, .size=2.687f, .size_variance=0.201f, .angle=17.0f,
                 .alpha_cutoff=0.35f, .tint={0.16f,0.40f,0.24f}, .transmission_tint={0.32f,0.46f,0.12f},
                 .transmission_strength=0.28f, .silhouette=LeafSilhouette::PineSprig, .tip_leaf=false}}});
  catalog.push_back({"Ash (small)", {
      .seed = 26867, .type = T::Deciduous, .levels = 2,
      .angle = {0, 48, 75, 60}, .children = {10, 3, 3, 0},
      .gnarliness = {0.11f, 0.09f, 0.05f, 0.09f},
      .length = {23.87f, 18.0f, 5.59f, 4.6f}, .radius = {0.81f, 0.56f, 0.76f, 0.7f},
      .sections = {12, 10, 10, 10}, .segments = {8, 6, 4, 3},
      .start = {0, 0.53f, 0.33f, 0}, .taper = {0.7f, 0.7f, 0.7f, 0.7f},
      .twist = {0.3f, -0.07f, 0, 0}, .force_strength = 0.01f,
      .leaves = {.arrangement=LeafArrangement::FanFromStem, .blade_count=2, .card_aspect=0.95f,
                 .count=45, .start=0.0f, .size=2.93f, .size_variance=0.717f, .angle=55.0f,
                 .tint={0.34f,0.56f,0.20f}, .transmission_tint={0.58f,0.64f,0.12f},
                 .transmission_strength=0.62f, .silhouette=LeafSilhouette::Ash}}});
  catalog.push_back({"Ash (medium)", {
      .seed = 36330, .type = T::Deciduous, .levels = 3,
      .angle = {0, 48, 75, 60}, .children = {7, 4, 3, 0},
      .gnarliness = {0.03f, 0.25f, 0.2f, 0.09f},
      .length = {43.47f, 27.14f, 9.51f, 4.6f}, .radius = {2.0f, 0.63f, 0.76f, 0.7f},
      .sections = {12, 8, 6, 4}, .segments = {12, 6, 4, 3},
      .start = {0, 0.23f, 0.33f, 0}, .taper = {0.7f, 0.7f, 0.7f, 0.7f},
      .twist = {0.09f, -0.07f, 0, 0}, .force_strength = 0.01f,
      .leaves = {.arrangement=LeafArrangement::FanFromStem, .blade_count=2, .card_aspect=0.95f,
                 .count=24, .start=0.0f, .size=2.49f, .size_variance=0.72f, .angle=55.0f,
                 .tint={0.34f,0.56f,0.20f}, .transmission_tint={0.58f,0.64f,0.12f},
                 .transmission_strength=0.62f, .silhouette=LeafSilhouette::Ash}}});
  catalog.push_back({"Ash (large)", {
      .seed = 29919, .type = T::Deciduous, .levels = 3,
      .angle = {0, 39, 39, 51}, .children = {10, 4, 3, 0},
      .gnarliness = {-0.05f, 0.2f, 0.16f, 0.05f},
      .length = {45.0f, 29.42f, 15.3f, 4.6f}, .radius = {3.03f, 0.53f, 0.79f, 1.11f},
      .sections = {12, 8, 6, 4}, .segments = {8, 6, 4, 3},
      .start = {0, 0.32f, 0.34f, 0}, .taper = {0.7f, 0.62f, 0.76f, 0},
      .twist = {0.09f, -0.07f, 0, 0}, .force_strength = 0.01f,
      .leaves = {.arrangement=LeafArrangement::FanFromStem, .blade_count=2, .card_aspect=0.95f,
                 .count=15, .start=0.01f, .size=3.51f, .size_variance=0.72f, .angle=30.0f,
                 .tint={0.34f,0.56f,0.20f}, .transmission_tint={0.58f,0.64f,0.12f},
                 .transmission_strength=0.62f, .silhouette=LeafSilhouette::Ash}}});
  catalog.push_back({"Aspen (small)", {
      .seed = 36330, .type = T::Deciduous, .levels = 2,
      .angle = {0, 70, 35, 7}, .children = {4, 3, 3, 0},
      .gnarliness = {0.04f, -0.01f, 0.12f, 0.02f},
      .length = {23.99f, 3.36f, 7.7f, 1.0f}, .radius = {0.37f, 0.41f, 0.7f, 0.7f},
      .sections = {12, 10, 8, 6}, .segments = {8, 6, 4, 3},
      .start = {0, 0.45f, 0.33f, 0}, .taper = {0.37f, 0.13f, 0.7f, 0.7f},
      .twist = {0, 0, 0, 0}, .force_strength = 0.0109f,
      .leaves = {.arrangement=LeafArrangement::FanFromStem, .blade_count=2, .card_aspect=0.95f,
                 .count=13, .start=0.2f, .size=2.07f, .size_variance=0.7f, .angle=30.0f,
                 .tint={0.52f,0.66f,0.22f}, .transmission_tint={0.75f,0.78f,0.14f},
                 .transmission_strength=0.78f, .silhouette=LeafSilhouette::Aspen}}});
  catalog.push_back({"Aspen (medium)", {
      .seed = 18020, .type = T::Deciduous, .levels = 2,
      .angle = {0, 75, 32, 7}, .children = {10, 3, 3, 0},
      .gnarliness = {0.05f, 0.12f, 0.12f, 0.02f},
      .length = {50.0f, 6.07f, 11.19f, 1.0f}, .radius = {0.72f, 0.41f, 0.7f, 0.7f},
      .sections = {12, 10, 8, 6}, .segments = {8, 6, 4, 3},
      .start = {0, 0.59f, 0.35f, 0}, .taper = {0.37f, 0.13f, 0.7f, 0.7f},
      .twist = {0, 0, 0, 0}, .force_strength = 0.0148f,
      .leaves = {.arrangement=LeafArrangement::FanFromStem, .blade_count=2, .card_aspect=0.95f,
                 .count=11, .start=0.124f, .size=4.58f, .size_variance=0.7f, .angle=30.0f,
                 .tint={0.52f,0.66f,0.22f}, .transmission_tint={0.75f,0.78f,0.14f},
                 .transmission_strength=0.78f, .silhouette=LeafSilhouette::Aspen}}});
  catalog.push_back({"Aspen (large)", {
      .seed = 30631, .type = T::Deciduous, .levels = 2,
      .angle = {0, 47, 63, 7}, .children = {10, 6, 0, 0},
      .gnarliness = {0.05f, -0.03f, 0.12f, 0.02f},
      .length = {69.6f, 18.56f, 11.19f, 1.0f}, .radius = {1.11f, 0.58f, 0.7f, 0.7f},
      .sections = {12, 10, 8, 6}, .segments = {8, 6, 4, 3},
      .start = {0, 0.62f, 0.05f, 0}, .taper = {0.7f, 0.13f, 0.7f, 0.7f},
      .twist = {0, 0, 0, 0}, .force_strength = 0.0217f,
      .leaves = {.arrangement=LeafArrangement::FanFromStem, .blade_count=2, .card_aspect=0.95f,
                 .count=20, .start=0.152f, .size=7.68f, .size_variance=0.7f, .angle=36.0f,
                 .tint={0.52f,0.66f,0.22f}, .transmission_tint={0.75f,0.78f,0.14f},
                 .transmission_strength=0.78f, .silhouette=LeafSilhouette::Aspen}}});
  catalog.push_back({"Bush 1", {
      .seed = 45590, .type = T::Deciduous, .levels = 3,
      .angle = {0, 21.52f, 62.61f, 60}, .children = {7, 3, 2, 0},
      .gnarliness = {0.11f, 0.09f, 0.05f, 0.09f},
      .length = {0.1f, 15.3f, 5.59f, 4.6f}, .radius = {0.58f, 0.95f, 0.76f, 0.7f},
      .sections = {6, 6, 10, 10}, .segments = {4, 4, 4, 3},
      .start = {0, 0.53f, 0.33f, 0}, .taper = {0.7f, 0.7f, 0.7f, 0.7f},
      .twist = {0.3f, -0.07f, 0, 0}, .force_strength = 0.0f,
      .leaves = {.arrangement=LeafArrangement::FanFromStem, .blade_count=2, .card_aspect=0.95f,
                 .count=27, .start=0.0f, .size=0.62f, .size_variance=0.717f, .angle=55.0f,
                 .tint={0.40f,0.62f,0.20f}, .transmission_tint={0.62f,0.68f,0.14f},
                 .transmission_strength=0.60f, .silhouette=LeafSilhouette::Bush}}});
  catalog.push_back({"Bush 2", {
      .seed = 45590, .type = T::Deciduous, .levels = 2,
      .angle = {0, 19.57f, 27.39f, 60}, .children = {10, 3, 2, 0},
      .gnarliness = {0.02f, 0.11f, 0.05f, 0.09f},
      .length = {0.1f, 19.65f, 7.7f, 4.6f}, .radius = {0.58f, 0.95f, 0.76f, 0.7f},
      .sections = {3, 4, 10, 10}, .segments = {4, 4, 4, 3},
      .start = {0, 0.64f, 0.71f, 0}, .taper = {0.7f, 0.7f, 0.7f, 0.7f},
      .twist = {0.36f, -0.04f, 0, 0}, .force_strength = 0.0f,
      .leaves = {.arrangement=LeafArrangement::FanFromStem, .blade_count=2, .card_aspect=0.95f,
                 .count=16, .start=0.0f, .size=1.49f, .size_variance=0.717f, .angle=55.0f,
                 .tint={0.40f,0.62f,0.20f}, .transmission_tint={0.62f,0.68f,0.14f},
                 .transmission_strength=0.60f, .silhouette=LeafSilhouette::Bush}}});
  catalog.push_back({"Bush 3", {
      .seed = 31343, .type = T::Evergreen, .levels = 3,
      .angle = {0, 66.52f, 52.83f, 0}, .children = {13, 4, 4, 0},
      .gnarliness = {0.05f, 0.07f, 0.05f, 0.09f},
      .length = {10.96f, 21.82f, 13.13f, 5.53f}, .radius = {0.58f, 0.95f, 0.69f, 0.74f},
      .sections = {4, 3, 3, 10}, .segments = {3, 3, 3, 3},
      .start = {0, 0.14f, 0.29f, 0}, .taper = {0.7f, 0.7f, 0.7f, 0.7f},
      .twist = {0.3f, -0.03f, 0, 0}, .force_strength = 0.0f,
      .leaves = {.arrangement=LeafArrangement::AxialFins, .blade_count=3, .card_aspect=0.45f,
                 .count=48, .start=0.152f, .size=0.728f, .size_variance=0.457f, .angle=54.0f,
                 .alpha_cutoff=0.35f, .tint={0.20f,0.46f,0.32f}, .transmission_tint={0.38f,0.52f,0.20f},
                 .transmission_strength=0.30f, .silhouette=LeafSilhouette::PineSprig, .tip_leaf=false}}});
  return catalog;
}

}  // namespace badlands
