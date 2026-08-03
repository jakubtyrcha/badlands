#include "game/visual/skeleton_debug_overlay.hpp"

#include <algorithm>
#include <array>
#include <cmath>

#include <glm/gtc/matrix_transform.hpp>
#include <imgui.h>
#include <spdlog/spdlog.h>

#include "engine/animation/skeleton_lines.hpp"

namespace badlands {
namespace {

// Bone colour and thickness. Yellow reads over both the grey arena and the
// town's terrain, and over the capsules the skeleton is drawn on top of.
constexpr glm::vec3 kBoneColor{1.0f, 0.85f, 0.1f};
constexpr float kBoneThickness = 2.0f;

// Every logical clip, so Initialize can resolve each to a manifest index once.
constexpr std::array<LogicalClip, 7> kAllLogicalClips{
    LogicalClip::Idle,     LogicalClip::Walk,     LogicalClip::Jog,
    LogicalClip::Sprint,   LogicalClip::Attack,   LogicalClip::CastIdle,
    LogicalClip::HitBody,
};

}  // namespace

bool SkeletonDebugOverlay::Initialize(const std::string& manifest_path) {
  assets_ = AnimationSet::Load(manifest_path);
  if (!assets_) {
    // AnimationSet::Load already logged the specific failure.
    spdlog::warn("SkeletonDebugOverlay: no character assets at {} -- overlay disabled",
                 manifest_path);
    ready_ = false;
    return false;
  }

  // Resolve every logical clip to an index once. Nothing may look a clip up by
  // string per character per frame.
  clip_index_.assign(kAllLogicalClips.size(), -1);
  for (LogicalClip clip : kAllLogicalClips) {
    const int index = assets_->FindClip(LogicalClipName(clip));
    clip_index_[static_cast<size_t>(clip)] = index;
    if (index < 0) {
      spdlog::warn("SkeletonDebugOverlay: manifest has no \"{}\" clip",
                   LogicalClipName(clip));
    }
  }

  // Size the pooled scratch once for this skeleton.
  scratch_.current.emplace(assets_->skeleton());
  scratch_.fade_from.emplace(assets_->skeleton());
  scratch_.blended.emplace(assets_->skeleton());
  scratch_.sampler_a.Reset(assets_->skeleton());
  scratch_.sampler_b.Reset(assets_->skeleton());

  ready_ = true;
  return true;
}

void SkeletonDebugOverlay::Rebuild(Sim& sim, std::span<const CharacterState> rows,
                                   DebugLineBuffer& out,
                                   const GroundHeightFn& ground_y, float anim_dt) {
  drawn_ = 0;
  if (!show_ || !ready_) return;

  const Skeleton& skeleton = assets_->skeleton();
  const int64_t world_ticks = sim.World().world_ticks;

  for (const CharacterState& row : rows) {
    if (row.inside_building_id >= 0) continue;  // hidden: don't draw

    // The row is a COPY and may name an entity that died since it was taken, so
    // the handle is checked rather than assumed (see Sim::HandleForSlot).
    entt::handle character = sim.HandleForSlot(row.id);
    if (!character) continue;

    const auto* anim = character.try_get<CharacterAnim>();
    if (anim == nullptr) continue;  // projected only for live characters

    // The render layer's own state, attached on first sight and dropped with the
    // entity. Nothing under game/ may read this.
    auto& animator = character.get_or_emplace<CharacterAnimator>();

    const LogicalClip logical = ClipFor(*anim);
    const int clip = IndexOf(logical);
    if (clip < 0) continue;  // manifest lacks it; nothing sensible to draw

    // A NEW action -- or a new clip within the same action, e.g. walk -> jog as
    // speed rises -- starts a cross-fade from wherever the old clip was.
    const bool new_action = anim->action != animator.seen_action ||
                            anim->action_start_ticks != animator.seen_action_start_ticks;
    if (clip != animator.clip) {
      if (animator.clip >= 0) {
        animator.fade_from_clip = animator.clip;
        animator.fade_remaining = kAnimFadeSeconds;
      }
      animator.clip = clip;
      animator.loop_seconds = 0.0f;
    } else if (new_action) {
      animator.loop_seconds = 0.0f;  // same clip, fresh action: restart its phase
    }
    animator.seen_action = anim->action;
    animator.seen_action_start_ticks = anim->action_start_ticks;

    // Where in the clip we are. A BOUNDED action is re-derived from ticks every
    // frame and keeps no memory, so it cannot drift from the gameplay window it
    // depicts. A loop advances on presentation time.
    float ratio = 0.0f;
    if (IsBoundedAction(anim->action)) {
      ratio = PhaseRatio(*anim, world_ticks, assets_->clip_pivot(clip));
    } else {
      // Scale a locomotion loop by speed so feet do not slide; other loops run
      // at their authored rate.
      const float rate = anim->action == AnimAction::Locomotion
                             ? std::max(0.1f, anim->speed / kJogSpeed)
                             : 1.0f;
      animator.loop_seconds += anim_dt * rate;
      ratio = LoopRatioAt(assets_->clip(clip), animator.loop_seconds);
    }

    Pose& current = *scratch_.current;
    if (!scratch_.sampler_a.Sample(assets_->clip(clip), ratio, current)) continue;

    // Cross-fade out of the previous clip, frozen where it was. Blending only
    // smooths the transition -- it never decides what plays.
    Pose* posed = &current;
    if (animator.fade_remaining > 0.0f && animator.fade_from_clip >= 0) {
      animator.fade_remaining = std::max(0.0f, animator.fade_remaining - anim_dt);
      const float weight = animator.fade_remaining / kAnimFadeSeconds;
      Pose& from = *scratch_.fade_from;
      if (scratch_.sampler_b.Sample(assets_->clip(animator.fade_from_clip),
                                    animator.fade_from_ratio, from)) {
        const std::array<BlendLayer, 2> layers{
            BlendLayer{&from, weight},
            BlendLayer{&current, 1.0f - weight},
        };
        if (BlendPoses(skeleton, layers, *scratch_.blended)) {
          posed = &*scratch_.blended;
        }
      }
      if (animator.fade_remaining <= 0.0f) animator.fade_from_clip = -1;
    } else {
      // Remember where this clip is, so the NEXT change fades out of it.
      animator.fade_from_ratio = ratio;
    }

    if (!LocalToModel(skeleton, *posed)) continue;

    // The same transform the capsule pass builds: stand on the ground and face
    // the direction of travel (kCharacterForward is +Z).
    const float ground = ground_y(row.pos_x, row.pos_z);
    const float yaw = std::atan2(row.facing_x, row.facing_z);
    const glm::mat4 world =
        glm::translate(glm::mat4(1.0f), glm::vec3(row.pos_x, ground, row.pos_z)) *
        glm::rotate(glm::mat4(1.0f), yaw, glm::vec3(0.0f, 1.0f, 0.0f));

    EmitSkeletonLines(skeleton, *posed, world, out, kBoneColor, kBoneThickness);
    ++drawn_;
  }
}

void SkeletonDebugOverlay::DrawControls() {
  if (!ready_) {
    ImGui::TextUnformatted("Skeletons: assets failed to load");
    return;
  }
  ImGui::Checkbox("Show skeletons", &show_);
  if (show_) {
    ImGui::Text("%d drawn, %d joints", drawn_, assets_->skeleton().num_joints());
  }
}

}  // namespace badlands
