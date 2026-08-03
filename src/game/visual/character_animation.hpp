#pragma once

// badlands' half of the animation chain: which clip a gameplay state plays, and
// how far into it. The sim says what a character is DOING (CharacterAnim,
// badlands_sim.hpp); this says what that LOOKS like.
//
// The two functions here are pure and hold no state, which is what makes the
// timing promise checkable rather than merely visible -- see the continuity
// property in the tests.

#include <cstdint>

#include "badlands_sim.hpp"  // CharacterAnim, AnimAction

namespace badlands {

// Logical clip names, as authored in the character manifest
// (assets/characters/*/clips.json). Names rather than indices, because which
// index a clip lands on is a property of the manifest, not of the gameplay.
enum class LogicalClip {
  Idle,
  Walk,
  Jog,
  Sprint,
  Attack,
  CastIdle,
  HitBody,
};

// The manifest key for a logical clip.
const char* LogicalClipName(LogicalClip clip);

// Locomotion clip thresholds, in sim units/sec. A character below the first
// walks, below the second jogs, above it sprints. Fixed constants: the clip a
// speed picks is a content decision, and there is no tuning UI to feed.
inline constexpr float kJogSpeed = 3.0f;
inline constexpr float kSprintSpeed = 5.5f;

// Which clip a projected action plays.
LogicalClip ClipFor(const CharacterAnim& anim);

// True when this action drives its clip from its gameplay window (a swing)
// rather than looping at its own rate (a walk cycle).
bool IsBoundedAction(AnimAction action);

// How far into its clip a BOUNDED action is, in [0,1].
//
// `pivot` is the clip's authored culmination point (AnimationSet::clip_pivot).
// A wind-up maps its window onto [0, pivot] and a recovery maps its own onto
// [pivot, 1], so the two phases MEET at the pivot: at the tick the blow lands,
// both phases yield exactly `pivot` and the clip cannot jump. That continuity is
// the whole reason the sim owns the timing.
//
// A degenerate window (end <= start) yields the phase's starting ratio rather
// than dividing by zero.
float PhaseRatio(const CharacterAnim& anim, int64_t world_ticks, float pivot);

// The render layer's per-character animation state, attached to the SIM entity
// alongside CharacterAnim (see Sim::HandleForSlot). No sim system may read it.
//
// Deliberately holds NO ClipSampler: ozz's SamplingJob::Context is neither
// copyable nor movable, so a sampler inside a component could not live in EnTT
// storage. Samplers and poses are pooled by the overlay and reused across
// characters, which is also what keeps per-character cost to this struct alone.
//
// Nothing here DECIDES what plays -- that is ClipFor's job, re-derived every
// frame. These fields only smooth the transition into it.
struct CharacterAnimator {
  int clip = -1;                 // index into the AnimationSet, -1 = nothing yet
  float loop_seconds = 0.0f;     // phase of a looping clip, in presentation time
  int fade_from_clip = -1;       // the outgoing clip, -1 = not fading
  float fade_from_ratio = 0.0f;  // frozen where it was when the change happened
  float fade_remaining = 0.0f;   // seconds of cross-fade left
  // The last action window this animator saw, for edge detection. Comparing
  // start ticks is what tells a NEW action from a continuing one, with no event
  // stream -- see CharacterAnim's note in badlands_sim.hpp.
  int64_t seen_action_start_ticks = -1;
  AnimAction seen_action = AnimAction::Idle;
};

// How long a clip change takes to blend. One constant, not a knob.
inline constexpr float kAnimFadeSeconds = 0.15f;

}  // namespace badlands
