#include "game/visual/world_labels.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace badlands {

LabelProjection ProjectLabel(const glm::mat4& view_proj, const glm::vec3& world,
                             float viewport_w_px, float viewport_h_px) {
  LabelProjection p;
  const glm::vec4 clip = view_proj * glm::vec4(world, 1.0f);
  if (clip.w <= 1e-4f) {
    return p;  // on or behind the camera plane -> not visible
  }
  const glm::vec3 ndc = glm::vec3(clip) / clip.w;
  // Cull off-screen anchors, but with a margin (NDC units) so a label whose
  // anchor sits just past the edge -- yet whose text would still poke in -- is
  // kept rather than popping out at the border.
  constexpr float kMargin = 1.2f;
  if (ndc.x < -kMargin || ndc.x > kMargin || ndc.y < -kMargin || ndc.y > kMargin) {
    return p;
  }
  p.visible = true;
  p.x = (ndc.x * 0.5f + 0.5f) * viewport_w_px;
  p.y = (0.5f - ndc.y * 0.5f) * viewport_h_px;  // NDC y is up; screen y is down
  p.depth = clip.w;                             // view-forward distance
  p.scale = LabelDepthScale(clip.w);
  return p;
}

float LabelDepthScale(float depth) {
  using namespace label_scale;
  const float d = std::max(depth, 1e-3f);
  // pow(ref/d, exp<1) dampens the shrink: at exp==1 this is true perspective
  // (label size ~ 1/depth); exp<1 shrinks slower with distance.
  const float s = std::pow(kRefDepth / d, kDampExponent);
  return std::clamp(s, kMinScale, kMaxScale);
}

LabelVisual SampleLabelAnimation(LabelAnimation anim, float t01) {
  t01 = std::clamp(t01, 0.0f, 1.0f);  // 1 at spawn -> 0 at expiry
  switch (anim) {
    case LabelAnimation::RiseFade: {
      // Stay fully opaque for the first stretch of life, then ease the alpha to
      // zero over the final kFadeFraction of it; drift upward the whole time.
      constexpr float kFadeFraction = 0.4f;  // fade over the last 40% of life
      constexpr float kRiseWorld = 0.9f;     // total upward drift (world units)
      const float opacity = std::clamp(t01 / kFadeFraction, 0.0f, 1.0f);
      const float rise = (1.0f - t01) * kRiseWorld;
      return {opacity, rise};
    }
  }
  return {1.0f, 0.0f};
}

void WorldLabelPool::Spawn(uint32_t anchor_slot, const glm::vec3& anchor_pos,
                           float base_offset, std::string text, uint32_t color,
                           float lifetime, LabelAnimation anim) {
  labels_.push_back(TimedLabel{.anchor_slot = anchor_slot,
                               .anchor_pos = anchor_pos,
                               .base_offset = base_offset,
                               .text = std::move(text),
                               .color = color,
                               .timer = lifetime,
                               .lifetime = lifetime,
                               .anim = anim});
}

void WorldLabelPool::Advance(float dt) {
  for (TimedLabel& l : labels_) {
    l.timer -= dt;
  }
  std::erase_if(labels_, [](const TimedLabel& l) { return l.timer <= 0.0f; });
}

std::vector<ResolvedLabel> WorldLabelPool::Resolve(const AnchorLookup& live_pos) const {
  // Vertical spacing between stacked labels sharing an anchor (world units).
  constexpr float kStackGap = 0.5f;
  std::vector<ResolvedLabel> out;
  out.reserve(labels_.size());

  // Stack rank = number of NEWER (later-appended) labels on the same anchor. The
  // newest sits lowest (rank 0, just above the head); each older one is pushed up
  // a slot. Computed in one backward pass with a per-anchor running count, so
  // resolution stays linear rather than O(n^2) in the live-label count.
  std::vector<int> rank(labels_.size(), 0);
  std::unordered_map<uint32_t, int> seen;
  for (size_t k = labels_.size(); k-- > 0;) {
    rank[k] = seen[labels_[k].anchor_slot]++;
  }

  for (size_t i = 0; i < labels_.size(); ++i) {
    const TimedLabel& l = labels_[i];
    // Follow the live anchor if it still exists, else freeze at the spawn pos.
    const std::optional<glm::vec3> live = live_pos(l.anchor_slot);
    glm::vec3 pos = live.value_or(l.anchor_pos);
    const float t01 = l.lifetime > 0.0f ? l.timer / l.lifetime : 0.0f;
    const LabelVisual v = SampleLabelAnimation(l.anim, t01);
    pos.y += l.base_offset + v.rise + static_cast<float>(rank[i]) * kStackGap;
    out.push_back(ResolvedLabel{.world_pos = pos,
                                .text = l.text,
                                .color = l.color,
                                .opacity = v.opacity});
  }
  return out;
}

}  // namespace badlands
