#include "engine/animation/pose.hpp"

#include <algorithm>

#include "engine/animation/skeleton.hpp"

namespace badlands {
namespace {

// The reason pose.hpp insists on ozz::vector. These types carry SIMD payloads
// that are over-aligned relative to max_align_t on the platforms we build for;
// ozz::StdAllocator honours that. If someone later moves the buffers to inline
// storage or a plain std::vector with a hand-rolled allocator, this fires at
// compile time instead of producing a misaligned load at runtime.
static_assert(alignof(ozz::math::SoaTransform) >= 16,
              "SoaTransform must stay 16-byte aligned for ozz's SIMD paths");
static_assert(alignof(ozz::math::Float4x4) >= 16,
              "Float4x4 must stay 16-byte aligned for ozz's SIMD paths");

}  // namespace

Pose::Pose(const Skeleton& skeleton) { Reset(skeleton); }

void Pose::Reset(const Skeleton& skeleton) {
  num_joints_ = skeleton.num_joints();
  locals_.resize(static_cast<size_t>(skeleton.num_soa_joints()));
  models_.resize(static_cast<size_t>(num_joints_));

  // Seed with the rest pose rather than leaving indeterminate transforms: a
  // Pose that is converted before anything samples into it then draws the
  // skeleton standing, which is a legible failure instead of a garbage one.
  const ozz::span<const ozz::math::SoaTransform> rest =
      skeleton.raw().joint_rest_poses();
  std::copy(rest.begin(), rest.end(), locals_.begin());
}

}  // namespace badlands
