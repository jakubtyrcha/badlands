#pragma once

// Sampling, blending and local->model conversion: the three ozz jobs the
// animation runtime actually runs, wrapped so callers never touch a job struct.

#include <memory>
#include <span>

#include <glm/glm.hpp>
#include <ozz/animation/runtime/sampling_job.h>
#include <ozz/base/maths/simd_math.h>

namespace badlands {

class AnimationClip;
class Pose;
class Skeleton;

// Seconds -> the unit-interval ratio ozz samples with, for a ONE-SHOT: past the
// end it CLAMPS, holding the final frame. Returns 0 for a non-positive duration
// rather than dividing by it, so a clip authored with no length degrades to its
// first frame instead of producing NaN.
float RatioAt(const AnimationClip& clip, float seconds);

// Seconds -> ratio for a LOOP: past the end it wraps and keeps cycling.
//
// This exists as its own function because RatioAt(...) fed through WrapRatio
// does NOT loop -- the clamp pins the ratio at 1.0 and the wrap turns that into
// 0.0 forever, freezing the clip on its first frame after one cycle. Composing
// the two at a call site is exactly the bug this replaces.
float LoopRatioAt(const AnimationClip& clip, float seconds);

// Wraps an already-computed ratio into [0,1). Prefer LoopRatioAt when starting
// from seconds; this is for callers that already hold a ratio.
float WrapRatio(float ratio);

// Samples clips into a Pose's local transforms. Owns ozz's sampling context,
// which caches key positions between calls at nearby ratios.
//
// The context is neither copyable nor movable in ozz, so it is held by pointer
// and THIS type is movable -- otherwise a sampler could not live in any
// container, which every caller past the single-character viewer needs.
class ClipSampler {
 public:
  ClipSampler() = default;
  ClipSampler(ClipSampler&&) = default;
  ClipSampler& operator=(ClipSampler&&) = default;
  ClipSampler(const ClipSampler&) = delete;
  ClipSampler& operator=(const ClipSampler&) = delete;

  // Sizes the context for `skeleton`. Sampling before this is a no-op returning
  // false rather than a crash.
  void Reset(const Skeleton& skeleton);

  // Writes `clip` at `ratio` into pose.locals(). `ratio` is CLAMPED to [0,1]
  // (and a NaN is treated as 0), so no out-of-range value ever reaches ozz.
  // False when the sampler is unsized or the job rejects its inputs.
  bool Sample(const AnimationClip& clip, float ratio, Pose& pose);

 private:
  std::unique_ptr<ozz::animation::SamplingJob::Context> context_;
  int max_tracks_ = 0;
};

// One weighted input to BlendPoses.
struct BlendLayer {
  const Pose* pose = nullptr;
  float weight = 0.0f;
};

// Blends `layers` into pose.locals() using the skeleton's rest pose for joints
// no layer contributes to. False if any layer is malformed or the job rejects
// its inputs. Layers whose combined weight is below ozz's threshold fall back
// to the rest pose, which is ozz's behaviour and not something we paper over.
bool BlendPoses(const Skeleton& skeleton, std::span<const BlendLayer> layers,
                Pose& out);

// Runs the joint hierarchy, filling pose.models() from pose.locals().
bool LocalToModel(const Skeleton& skeleton, Pose& pose);

// ozz's column-major SIMD matrix to glm's. Used to place a joint in world
// space; kept here so callers never unpack SimdFloat4 by hand.
glm::mat4 ToMat4(const ozz::math::Float4x4& m);

}  // namespace badlands
