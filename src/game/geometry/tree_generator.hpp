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
};

// Phase A: seeded recursive branch skeleton (quaternion growth, child spawning).
std::vector<SkeletonBranch> BuildTreeSkeleton(const TreeOptions& options);
// Phase B: sweep tapered rings along each branch -> one opaque bark mesh,
// base at local y=0.
TexturedMeshResult GenerateTreeMesh(const TreeOptions& options);

}  // namespace badlands
