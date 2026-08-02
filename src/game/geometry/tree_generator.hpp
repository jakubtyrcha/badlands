#pragma once
#include <vector>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include "engine/rendering/geometry/textured_mesh_builders.hpp"  // TexturedMeshResult
#include "game/geometry/tree_options.hpp"

namespace badlands {

// One ring of a branch: center, frame (local +Y = growth axis), radius.
struct BranchSection { glm::vec3 origin; glm::quat orientation; float radius; };
struct SkeletonBranch {
  std::vector<BranchSection> sections;
  int segment_count = 0;
  float base_radius = 0.0f;
  int level = 0;  // recursion depth (0 = trunk); used for debug-graph coloring

  // Where this branch hangs off its parent. Pure BOOKKEEPING -- the bark
  // grafter (branch_junction.hpp) needs to know which surface a child grows
  // out of, and the UV field needs the parent's arc length to continue V
  // across the joint. Adding these must not move a single vertex; that is
  // pinned by tree_generator_tests' "geometry is byte-stable" golden hashes.
  int   parent = -1;             // index into the skeleton; -1 for the trunk
  int   attach_section = 0;      // parent section index the child starts at
  float attach_alpha = 0.0f;     // lerp within [attach_section, +1]
  bool  is_continuation = false; // deciduous stem continuation, not a radial child
  float base_arc_len = 0.0f;     // arc length from the ROOT to this branch's base
};

// Quads emitted per leaf site for a given arrangement: SingleQuad->1,
// CrossedPair->2, FanFromStem/AxialFins->lf.blade_count. Shared by the
// generator and its tests so the quad-count arithmetic isn't duplicated.
int QuadsPerLeafSite(const LeafOptions& lf);

// Phase A: seeded recursive branch skeleton (quaternion growth, child spawning).
std::vector<SkeletonBranch> BuildTreeSkeleton(const TreeOptions& options);
// What the bark grafter did, for logging. `junctions` counts radial children
// (stem continuations are not socketed -- they weld on the UV field alone);
// `stitched` those whose collar merged into the parent; `shrunk` those that had
// to give ground to a sibling first; `fallback` those left as an independent
// buried tube, which is the pre-graft behaviour and costs one extra mesh
// component each.
struct BarkMeshStats {
  int junctions = 0;
  int stitched = 0;
  int shrunk = 0;
  int fallback = 0;
};

// Phase B: sweep tapered rings along each branch -> one opaque bark mesh,
// base at local y=0.
TexturedMeshResult GenerateTreeMesh(const TreeOptions& options);
// Phase B: alpha-cut leaf cards on the terminal-level branches -> one indexed
// kTexturedMesh mesh (quads), same local space as the bark mesh. Deterministic:
// uses a SEPARATE RNG stream so the branch skeleton is byte-identical to SP1.
TexturedMeshResult GenerateLeafMesh(const TreeOptions& options);

// Variants that reuse a prebuilt skeleton (must be BuildTreeSkeleton(options) for the same
// options) so a caller needing both bark and leaf meshes builds the skeleton once.
TexturedMeshResult GenerateTreeMesh(const TreeOptions& options,
                                    const std::vector<SkeletonBranch>& skeleton,
                                    BarkMeshStats* stats = nullptr);
TexturedMeshResult GenerateLeafMesh(const TreeOptions& options,
                                    const std::vector<SkeletonBranch>& skeleton);

}  // namespace badlands
