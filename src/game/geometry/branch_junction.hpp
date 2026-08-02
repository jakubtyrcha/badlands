#pragma once

// The geometry a branch junction needs, separated from the tree generator that
// orchestrates it. Everything here is pure math over section polylines and
// vertex loops -- no mesh building, no TreeOptions, no RNG, no allocation
// beyond the vectors it returns. `BranchSection` is the only tree type it
// borrows (from tree_generator.hpp; that header does NOT include this one, so
// there is no cycle).
//
// Why any of this exists: GenerateTreeMesh used to emit one independent tube
// per branch, so a tree was ~180 disconnected shells. meshoptimizer's edge
// collapse cannot merge separate components, which floored bark decimation at
// a few triangles per shell (see mesh_lod.hpp). These primitives let a child's
// buried base be replaced by a collar that SHARES the parent's vertices, so
// the tree becomes one component.

#include <cstdint>
#include <utility>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "game/geometry/tree_generator.hpp"  // BranchSection

namespace badlands {

// A socket may never span more than this much of its parent's circumference.
//
// Not a style choice -- a correctness bound. `TreeOptions::radius[level]` is a
// child-to-parent radius MULTIPLIER and several ez-tree presets exceed 1 (Oak's
// level-3 twigs are 1.19x their parent, Ash (large) 1.11x, the bushes 0.95x).
// A child at least as thick as its parent has no well-defined footprint on the
// parent's side -- unclamped, its socket would swallow the whole ring and sever
// the tube. Clamping to 60 degrees of half-angle (120 total) keeps at least a
// third of every ring intact and turns the `r > R` case into an outward-opening
// funnel, which is what that geometry honestly is.
inline constexpr float kMaxSocketHalfAngleDeg = 60.0f;

// ---------------------------------------------------------------------------
// Tube queries
// ---------------------------------------------------------------------------

// Where a point projects onto a branch's centerline polyline.
struct TubeHit {
  int   section = 0;       // index of the segment's start ring
  float alpha = 0.0f;      // position within [section, section + 1]
  float radius = 0.0f;     // tube radius interpolated at that point
  float distance = 0.0f;   // distance from the query point to the centerline
  // The query point lies BEYOND one of the polyline's ends, so the nearest
  // centerline point is an endpoint rather than a genuine perpendicular foot.
  //
  // Worth distinguishing because a branch's tube is open: it stops at its last
  // ring and there is no cap. Treating a clamped projection as "inside" turns
  // every branch into a capsule, and on a squat parent that phantom cap covers
  // real estate no surface occupies -- Bush 1's trunk is 0.05 long against a
  // 0.58 radius, so its cap swallowed a child ring that was genuinely in the
  // open and shrank the tree's own bounding box.
  bool  beyond_end = false;
};

// Closest point on `sections`' centerline. O(sections); the polylines here are
// 4-17 rings, so a linear scan beats any acceleration structure.
TubeHit ClosestOnTube(const std::vector<BranchSection>& sections,
                      const glm::vec3& p);

// True when `p` lies outside the swept tube. `slack` is a fraction of the local
// radius: positive values push the test outward, so a point must clear the
// surface by a margin before it counts as free.
bool IsOutsideTube(const std::vector<BranchSection>& sections,
                   const glm::vec3& p, float slack);

// ---------------------------------------------------------------------------
// Socket footprint
// ---------------------------------------------------------------------------

// The patch of a parent's surface a child covers, expressed in the parent's own
// parameter space: an angle about the parent's axis, and a span in fractional
// section index. Kept in parameter space (not world space) because that is what
// the parent's quad grid is indexed by, so marking the hole is arithmetic.
struct SocketFootprint {
  float centre_angle = 0.0f;  // radians in the parent's ring frame, atan2(z, x)
  float half_angle = 0.0f;    // always <= kMaxSocketHalfAngleDeg in radians
  float axial_min = 0.0f;     // fractional section index
  float axial_max = 0.0f;
  bool  valid = false;        // false when the parent is degenerate there
  // A child running (near) parallel to its parent's axis rather than out of its
  // side. It has no footprint to cut -- there is no side to cut into -- so it
  // joins at the parent's END RING instead, like a stem continuation with a
  // change of radius.
  //
  // Not an edge case: Bush 3's `angle` is {0, 66.52, 52.83, 0}, so all 208 of
  // its level-3 children are coaxial with their parents. Rejecting them outright
  // left three quarters of that preset as loose tubes.
  bool  axial = false;
};

// Sine of the smallest angle a child may leave at and still be socketed into
// its parent's side. Below this the socket degenerates -- its axial extent is
// 2r/sin(theta), which runs away -- and the axial join is the honest answer.
inline constexpr float kAxialJoinSinThreshold = 0.26f;  // ~15 degrees

// `child_orientation` is the child's base frame (its +Y is the growth axis);
// `child_radius` its base radius. Both come straight off the skeleton.
SocketFootprint ComputeSocketFootprint(const std::vector<BranchSection>& parent,
                                       int attach_section, float attach_alpha,
                                       const glm::quat& child_orientation,
                                       float child_radius);

// Two footprints overlap when they intersect in BOTH axes -- angularly (mod
// 2*pi) and axially. Neither alone is a collision.
bool FootprintsOverlap(const SocketFootprint& a, const SocketFootprint& b);

// Whether a point of the parent's surface, given as (fractional section index,
// ring angle), lies under the socket. This is how a parent's quads are marked
// as hole: test each quad's centre. Angle wrapping lives here so no caller has
// to repeat it.
bool FootprintContains(const SocketFootprint& f, float axial, float angle);

// Shrinks colliding footprints until they are pairwise disjoint, and returns
// how many were shrunk at all.
//
// Overlap is structural, not rare: Oak's level-2 branches carry `segments = 3`,
// so a clamped footprint is a whole 120-degree segment wide, and their three
// children sit closer together axially than their own footprints are long.
// Discarding those junctions would throw away most of what this feature exists
// to merge -- so they shrink instead. The child's own ring never moves, so a
// shrunk socket only makes its collar steeper.
//
// Deterministic: pairs are walked in index order and the step is fixed. A
// footprint that would have to go below `min_half_angle` or `min_axial` is left
// at the floor and marked invalid, which is the caller's signal to fall back.
int ShrinkToDisjoint(std::vector<SocketFootprint>& footprints,
                     float min_half_angle, float min_axial);

// ---------------------------------------------------------------------------
// Ring refinement
// ---------------------------------------------------------------------------

// Merges a branch's authored ring parameters with extra rings bracketing each
// socket, so a footprint always has quads to cut.
//
// Needed because the binding constraint is AXIAL. A parent's quads are very
// elongated -- Pine's mid-trunk quad is ~0.41 wide by ~4.17 tall against a
// ~0.38 by ~0.40 footprint -- so a socket otherwise falls between two rings and
// marks no quads at all.
//
// `spans` are (min, max) fractional section indices. A span already covering
// `min_rings_per_span` authored rings contributes nothing, which is why Oak's
// trunk pays no extra triangles. Output is sorted ascending and deduplicated
// within `tol`.
//
// Sorting is by a TOTAL order with dedup as a separate pass; a tolerance-based
// comparator is not transitive and is undefined behaviour inside std::sort.
std::vector<float> MergeRingParams(const std::vector<float>& authored,
                                   const std::vector<std::pair<float, float>>& spans,
                                   int min_rings_per_span, float tol);

// ---------------------------------------------------------------------------
// Loop stitching
// ---------------------------------------------------------------------------

// Bridges two closed vertex loops with a triangle strip, allocating NO new
// vertices -- the returned indices are drawn strictly from `a_ids` and `b_ids`.
//
// The loops may have completely different vertex counts. That is the whole
// point: it means a branch's ring resolution never has to agree with its
// parent's, which is what removes the all-quad "pants" counting constraint
// (waist + 2 = legA + legB) that would otherwise force reduction rings
// everywhere. Advance is by normalized arc length, so the strip follows both
// loops evenly instead of bunching.
//
// Emits exactly `a_ids.size() + b_ids.size()` triangles. Winding is chosen so
// the strip faces outward from the axis joining the two loops' centroids.
// Returns empty if either loop has fewer than 3 vertices.
std::vector<uint32_t> StitchLoops(const std::vector<uint32_t>& a_ids,
                                  const std::vector<glm::vec3>& a_pos,
                                  const std::vector<uint32_t>& b_ids,
                                  const std::vector<glm::vec3>& b_pos);

// ---------------------------------------------------------------------------
// Collar normals
// ---------------------------------------------------------------------------

// Re-averages the normals of `vertex_ids` only, over the triangles in
// `indices`, weighted by triangle area. Tangents are re-orthogonalized against
// the new normal; POSITIONS AND UVs ARE NEVER TOUCHED.
//
// Sharing vertices is what makes the collar connect, but it also leaves the
// parent's boundary normals pointing radially off the PARENT while the child's
// ring normals point radially off the CHILD -- up to 90 degrees apart across a
// single band of triangles, which reads as a hard lighting seam and is worst at
// the clamped `r > R` funnels. Positions staying put is what preserves the bark
// AABB, and with it CrownRadiusM and the whole forest's spacing.
//
// `vertices` is interleaved at `floats_per_vertex`, laid out
// pos(3) uv(2) normal(3) tangent(3) -- kTexturedMeshFloatsPerVertex.
void SmoothVertexNormals(std::vector<float>& vertices, size_t floats_per_vertex,
                         const std::vector<uint32_t>& indices,
                         const std::vector<uint32_t>& vertex_ids);

}  // namespace badlands
