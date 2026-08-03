#include "engine/animation/sampler.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

#include <ozz/animation/runtime/blending_job.h>
#include <ozz/animation/runtime/local_to_model_job.h>

#include "engine/animation/animation_clip.hpp"
#include "engine/animation/pose.hpp"
#include "engine/animation/skeleton.hpp"

namespace badlands {
namespace {

// Clamp to [0,1], mapping NaN to 0. std::clamp would PROPAGATE a NaN here
// (every comparison against it is false), and a NaN ratio reaching ozz walks
// its key search off the end -- so the ordering of these tests is load-bearing.
float SafeRatio(float ratio) {
  if (!(ratio > 0.0f)) return 0.0f;  // false for NaN and for negatives alike
  return ratio < 1.0f ? ratio : 1.0f;
}

}  // namespace

float RatioAt(const AnimationClip& clip, float seconds) {
  const float duration = clip.duration_seconds();
  if (!(duration > 0.0f)) return 0.0f;  // zero-length or NaN: no divide
  return SafeRatio(seconds / duration);
}

float LoopRatioAt(const AnimationClip& clip, float seconds) {
  const float duration = clip.duration_seconds();
  if (!(duration > 0.0f)) return 0.0f;  // zero-length or NaN: no divide
  // Wrap the UNCLAMPED quotient. Clamping first would pin every time past the
  // end at 1.0, and wrapping that yields 0.0 for every subsequent frame.
  return WrapRatio(seconds / duration);
}

float WrapRatio(float ratio) {
  if (!std::isfinite(ratio)) return 0.0f;
  const float wrapped = std::fmod(ratio, 1.0f);
  return wrapped < 0.0f ? wrapped + 1.0f : wrapped;
}

void ClipSampler::Reset(const Skeleton& skeleton) {
  max_tracks_ = skeleton.num_joints();
  if (!context_) {
    context_ = std::make_unique<ozz::animation::SamplingJob::Context>();
  }
  context_->Resize(max_tracks_);
}

bool ClipSampler::Sample(const AnimationClip& clip, float ratio, Pose& pose) {
  if (!context_) return false;
  // A clip with more tracks than the context was sized for would sample past
  // the cache. Refuse rather than truncate: it means the clip does not belong
  // to this skeleton, which is a content error worth surfacing.
  if (clip.num_tracks() > max_tracks_) return false;

  ozz::animation::SamplingJob job;
  job.animation = &clip.raw();
  job.context = context_.get();
  job.ratio = SafeRatio(ratio);
  job.output = pose.locals();
  return job.Run();
}

bool BlendPoses(const Skeleton& skeleton, std::span<const BlendLayer> layers,
                Pose& out) {
  std::vector<ozz::animation::BlendingJob::Layer> ozz_layers;
  ozz_layers.reserve(layers.size());
  for (const BlendLayer& layer : layers) {
    if (layer.pose == nullptr) return false;
    ozz::animation::BlendingJob::Layer& dst = ozz_layers.emplace_back();
    dst.transform = layer.pose->locals();
    dst.weight = layer.weight;
  }

  ozz::animation::BlendingJob job;
  job.layers = ozz::make_span(ozz_layers);
  job.rest_pose = skeleton.raw().joint_rest_poses();
  job.output = out.locals();
  return job.Run();
}

bool LocalToModel(const Skeleton& skeleton, Pose& pose) {
  ozz::animation::LocalToModelJob job;
  job.skeleton = &skeleton.raw();
  job.input = pose.locals();
  job.output = pose.models();
  return job.Run();
}

glm::mat4 ToMat4(const ozz::math::Float4x4& m) {
  glm::mat4 out(1.0f);
  for (int col = 0; col < 4; ++col) {
    // Unaligned store: glm::mat4's columns carry no SIMD alignment guarantee.
    ozz::math::StorePtrU(m.cols[col], &out[col][0]);
  }
  return out;
}

}  // namespace badlands
