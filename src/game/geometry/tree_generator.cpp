#include "game/geometry/tree_generator.hpp"
#include <random>
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

}  // namespace

std::vector<SkeletonBranch> BuildTreeSkeleton(const TreeOptions&) { return {}; }
TexturedMeshResult GenerateTreeMesh(const TreeOptions&) { return {}; }

}  // namespace badlands
