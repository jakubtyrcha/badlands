#include "game/visual/character_animation.hpp"

#include <algorithm>

namespace badlands {

const char* LogicalClipName(LogicalClip clip) {
  switch (clip) {
    case LogicalClip::Idle: return "idle";
    case LogicalClip::Walk: return "walk";
    case LogicalClip::Jog: return "jog";
    case LogicalClip::Sprint: return "sprint";
    case LogicalClip::Attack: return "attack";
    case LogicalClip::CastIdle: return "cast_idle";
    case LogicalClip::HitBody: return "hit_body";
  }
  return "idle";
}

LogicalClip ClipFor(const CharacterAnim& anim) {
  switch (anim.action) {
    case AnimAction::Locomotion:
      if (anim.speed >= kSprintSpeed) return LogicalClip::Sprint;
      if (anim.speed >= kJogSpeed) return LogicalClip::Jog;
      return LogicalClip::Walk;
    // Both strike phases play the SAME clip, split at its pivot. That is what
    // makes the swing one continuous motion across the tick the blow lands.
    case AnimAction::AttackWindUp:
    case AnimAction::AttackRecovery:
      return LogicalClip::Attack;
    case AnimAction::CastFocus:
      return LogicalClip::CastIdle;
    case AnimAction::Stunned:
      return LogicalClip::HitBody;
    case AnimAction::Idle:
      break;
  }
  return LogicalClip::Idle;
}

bool IsBoundedAction(AnimAction action) {
  switch (action) {
    case AnimAction::AttackWindUp:
    case AnimAction::AttackRecovery:
    case AnimAction::CastFocus:
      return true;
    case AnimAction::Idle:
    case AnimAction::Locomotion:
    case AnimAction::Stunned:
      return false;
  }
  return false;
}

float PhaseRatio(const CharacterAnim& anim, int64_t world_ticks, float pivot) {
  const float clamped_pivot = std::clamp(pivot, 0.0f, 1.0f);

  // Where in its own window the action is. A degenerate window means the
  // mechanic gave no duration, so report the phase's beginning rather than
  // dividing by zero.
  float progress = 0.0f;
  if (anim.action_end_ticks > anim.action_start_ticks) {
    const double span =
        static_cast<double>(anim.action_end_ticks - anim.action_start_ticks);
    const double into = static_cast<double>(world_ticks - anim.action_start_ticks);
    progress = static_cast<float>(std::clamp(into / span, 0.0, 1.0));
  }

  switch (anim.action) {
    case AnimAction::AttackWindUp:
      // [0, pivot]. At the end of the wind-up this is exactly `pivot`.
      return clamped_pivot * progress;
    case AnimAction::AttackRecovery:
      // [pivot, 1]. At the START of the recovery this is also exactly `pivot`,
      // so the two phases meet with no jump on the tick damage resolves.
      return clamped_pivot + (1.0f - clamped_pivot) * progress;
    default:
      // Any other bounded action owns its clip whole.
      return progress;
  }
}

}  // namespace badlands
