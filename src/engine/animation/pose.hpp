#pragma once

// A skeleton-sized sampling buffer: SoA local transforms (what a clip samples
// into) plus the model-space matrices LocalToModel derives from them.
//
// TRANSIENT SCRATCH BY DESIGN, not per-character state. A pose only has to live
// long enough to be consumed (emit debug lines, later upload skinning matrices),
// so callers keep a small pool and reuse it across characters within a frame.
// That is what keeps crowd scaling flat: the only persistent per-character
// animation state is which clip is playing and how far in, which is tiny.
//
// The buffers use ozz::vector (ozz::StdAllocator), so the over-aligned SIMD
// payload is heap-allocated by an allocator that honours its alignment and a
// Pose held in a container is just pointers. Do not switch these to inline
// storage without revisiting that -- see the static_asserts in pose.cpp.

#include <ozz/base/containers/vector.h>
#include <ozz/base/maths/simd_math.h>
#include <ozz/base/maths/soa_transform.h>
#include <ozz/base/span.h>

namespace badlands {

class Skeleton;

class Pose {
 public:
  // Sizes both buffers for `skeleton` and fills locals with its rest pose, so a
  // Pose is valid to convert even before anything is sampled into it.
  explicit Pose(const Skeleton& skeleton);

  // Resizes to a different skeleton and re-seeds the rest pose. Cheap when the
  // size is unchanged, which is the pooled-reuse path.
  void Reset(const Skeleton& skeleton);

  int num_joints() const { return num_joints_; }

  ozz::span<ozz::math::SoaTransform> locals() { return ozz::make_span(locals_); }
  ozz::span<const ozz::math::SoaTransform> locals() const { return ozz::make_span(locals_); }

  ozz::span<ozz::math::Float4x4> models() { return ozz::make_span(models_); }
  ozz::span<const ozz::math::Float4x4> models() const { return ozz::make_span(models_); }

 private:
  ozz::vector<ozz::math::SoaTransform> locals_;
  ozz::vector<ozz::math::Float4x4> models_;
  int num_joints_ = 0;
};

}  // namespace badlands
