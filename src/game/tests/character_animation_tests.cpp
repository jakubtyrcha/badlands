// The badlands half of the animation chain (src/game/visual/character_animation):
// which clip a projected gameplay state plays, and how far into it.
//
// Both functions under test are pure, which is deliberate -- it is what makes
// the timing promise ("the blow and its animation cannot drift") an assertion
// rather than something you have to watch for.

#include "game/visual/character_animation.hpp"

#include <catch_amalgamated.hpp>

#include <cmath>

using namespace badlands;

namespace {

// A wind-up spanning [start, resolve], as the projector writes it.
CharacterAnim WindUp(int64_t start, int64_t resolve) {
  CharacterAnim a;
  a.action = AnimAction::AttackWindUp;
  a.action_start_ticks = start;
  a.action_end_ticks = resolve;
  a.action_param = 0;
  return a;
}

// The recovery that follows it, spanning [resolve, free].
CharacterAnim Recovery(int64_t resolve, int64_t free) {
  CharacterAnim a;
  a.action = AnimAction::AttackRecovery;
  a.action_start_ticks = resolve;
  a.action_end_ticks = free;
  a.action_param = 0;
  return a;
}

CharacterAnim Moving(float speed) {
  CharacterAnim a;
  a.action = AnimAction::Locomotion;
  a.speed = speed;
  return a;
}

}  // namespace

TEST_CASE("the wind-up and the recovery meet exactly at the pivot", "[anim]") {
  // THE property this milestone exists to establish. A swing is two gameplay
  // phases over one clip; if they did not meet, the sword would jump on the very
  // frame the blow lands -- the most visible moment there is.
  constexpr float kPivot = 0.45f;
  constexpr int64_t kDeclared = 1000;
  constexpr int64_t kResolve = 1060;
  constexpr int64_t kFree = 1090;

  const CharacterAnim wind_up = WindUp(kDeclared, kResolve);
  const CharacterAnim recovery = Recovery(kResolve, kFree);

  // Last instant of the wind-up and first instant of the recovery are the SAME
  // tick, and both must land on the pivot.
  const float end_of_windup = PhaseRatio(wind_up, kResolve, kPivot);
  const float start_of_recovery = PhaseRatio(recovery, kResolve, kPivot);

  CHECK(end_of_windup == Catch::Approx(kPivot));
  CHECK(start_of_recovery == Catch::Approx(kPivot));
  CHECK(end_of_windup == Catch::Approx(start_of_recovery));
}

TEST_CASE("each strike phase spans its half of the clip", "[anim]") {
  constexpr float kPivot = 0.45f;
  const CharacterAnim wind_up = WindUp(1000, 1060);
  const CharacterAnim recovery = Recovery(1060, 1090);

  // Wind-up runs 0 -> pivot across its own window.
  CHECK(PhaseRatio(wind_up, 1000, kPivot) == Catch::Approx(0.0f));
  CHECK(PhaseRatio(wind_up, 1030, kPivot) == Catch::Approx(kPivot * 0.5f));

  // Recovery runs pivot -> 1 across its own, which is a DIFFERENT length: the
  // clip is stretched to each phase independently, which is the whole point of
  // the sim owning the timing.
  CHECK(PhaseRatio(recovery, 1075, kPivot) == Catch::Approx(kPivot + (1.0f - kPivot) * 0.5f));
  CHECK(PhaseRatio(recovery, 1090, kPivot) == Catch::Approx(1.0f));
}

TEST_CASE("phase ratios stay in range outside their window", "[anim]") {
  constexpr float kPivot = 0.45f;
  const CharacterAnim wind_up = WindUp(1000, 1060);

  // A frame taken before the window opened or after it closed still samples a
  // legal ratio -- the renderer runs on presentation time and can land either
  // side of a tick boundary.
  CHECK(PhaseRatio(wind_up, 900, kPivot) == Catch::Approx(0.0f));
  CHECK(PhaseRatio(wind_up, 5000, kPivot) == Catch::Approx(kPivot));
  for (int64_t t = 990; t <= 1070; ++t) {
    const float r = PhaseRatio(wind_up, t, kPivot);
    CHECK(r >= 0.0f);
    CHECK(r <= 1.0f);
    CHECK(std::isfinite(r));
  }
}

TEST_CASE("a degenerate window does not divide by zero", "[anim]") {
  // end == start is the projector's "unbounded" encoding; a bounded action
  // should never carry it, but a renderer must not produce NaN if one does.
  CharacterAnim degenerate = WindUp(1000, 1000);
  const float r = PhaseRatio(degenerate, 1000, 0.45f);
  CHECK(std::isfinite(r));
  CHECK(r == Catch::Approx(0.0f));

  CharacterAnim degenerate_recovery = Recovery(1000, 1000);
  const float rr = PhaseRatio(degenerate_recovery, 1000, 0.45f);
  CHECK(std::isfinite(rr));
  CHECK(rr == Catch::Approx(0.45f));  // recovery still begins at the pivot
}

TEST_CASE("a pivot outside [0,1] is clamped rather than trusted", "[anim]") {
  const CharacterAnim wind_up = WindUp(1000, 1060);
  CHECK(PhaseRatio(wind_up, 1060, 5.0f) == Catch::Approx(1.0f));
  CHECK(PhaseRatio(wind_up, 1060, -1.0f) == Catch::Approx(0.0f));
}

TEST_CASE("a pivot of 1 degrades to wind-up-plays-all", "[anim]") {
  // The default for a clip whose manifest declares no pivot. The wind-up owns
  // the whole clip and the recovery holds its last frame -- visibly plain, but
  // never broken.
  const CharacterAnim wind_up = WindUp(1000, 1060);
  const CharacterAnim recovery = Recovery(1060, 1090);
  CHECK(PhaseRatio(wind_up, 1060, 1.0f) == Catch::Approx(1.0f));
  CHECK(PhaseRatio(recovery, 1060, 1.0f) == Catch::Approx(1.0f));
  CHECK(PhaseRatio(recovery, 1090, 1.0f) == Catch::Approx(1.0f));
}

TEST_CASE("every action maps to a clip", "[anim]") {
  CharacterAnim idle;
  CHECK(ClipFor(idle) == LogicalClip::Idle);

  CHECK(ClipFor(WindUp(0, 10)) == LogicalClip::Attack);
  CHECK(ClipFor(Recovery(10, 20)) == LogicalClip::Attack);

  CharacterAnim cast;
  cast.action = AnimAction::CastFocus;
  CHECK(ClipFor(cast) == LogicalClip::CastIdle);

  CharacterAnim stunned;
  stunned.action = AnimAction::Stunned;
  CHECK(ClipFor(stunned) == LogicalClip::HitBody);
}

TEST_CASE("locomotion picks its clip by speed", "[anim]") {
  CHECK(ClipFor(Moving(0.5f)) == LogicalClip::Walk);
  // Either side of each threshold, so a retune cannot silently invert them.
  CHECK(ClipFor(Moving(kJogSpeed - 0.01f)) == LogicalClip::Walk);
  CHECK(ClipFor(Moving(kJogSpeed)) == LogicalClip::Jog);
  CHECK(ClipFor(Moving(kSprintSpeed - 0.01f)) == LogicalClip::Jog);
  CHECK(ClipFor(Moving(kSprintSpeed)) == LogicalClip::Sprint);
  CHECK(ClipFor(Moving(100.0f)) == LogicalClip::Sprint);
}

TEST_CASE("only the strike phases are driven by their window", "[anim]") {
  // Window-driven actions are re-derived from ticks and keep no presentation
  // memory; the rest loop on presentation time. Getting this wrong makes a swing
  // free-run instead of tracking the blow it depicts.
  CHECK(DrivenByWindow(AnimAction::AttackWindUp));
  CHECK(DrivenByWindow(AnimAction::AttackRecovery));
  CHECK_FALSE(DrivenByWindow(AnimAction::Idle));
  CHECK_FALSE(DrivenByWindow(AnimAction::Locomotion));
  CHECK_FALSE(DrivenByWindow(AnimAction::Stunned));

  // A cast HAS a window but its clip is a channel loop, so stretching it would
  // play a 1s loop once in slow motion across a 5s cast. It loops instead.
  CHECK_FALSE(DrivenByWindow(AnimAction::CastFocus));
}

TEST_CASE("locomotion playback tracks the speed its clip was authored for",
          "[anim]") {
  // Every clip is scaled toward its OWN reference, not one shared anchor: a slow
  // walk anchored to the jog speed crawled at a third rate, feet dragging.
  CHECK(LocomotionRate(Moving(kWalkReferenceSpeed)) == Catch::Approx(1.0f));
  CHECK(LocomotionRate(Moving(kJogReferenceSpeed)) == Catch::Approx(1.0f));
  CHECK(LocomotionRate(Moving(kSprintReferenceSpeed)) == Catch::Approx(1.0f));

  // Within a band, rate rises with speed.
  CHECK(LocomotionRate(Moving(0.5f)) < LocomotionRate(Moving(1.4f)));

  // Clamped at both ends: a near-still character does not freeze its clip, and
  // an absurd speed does not blur it.
  CHECK(LocomotionRate(Moving(0.0f)) == Catch::Approx(0.25f));
  CHECK(LocomotionRate(Moving(1000.0f)) == Catch::Approx(2.0f));
  for (float speed : {0.0f, 0.1f, 1.0f, 3.0f, 5.5f, 50.0f}) {
    const float rate = LocomotionRate(Moving(speed));
    CHECK(rate >= 0.25f);
    CHECK(rate <= 2.0f);
    CHECK(std::isfinite(rate));
  }
}

TEST_CASE("every logical clip names a manifest key", "[anim]") {
  // A typo here shows as a clip that silently never plays, so pin the names.
  CHECK(std::string(LogicalClipName(LogicalClip::Idle)) == "idle");
  CHECK(std::string(LogicalClipName(LogicalClip::Walk)) == "walk");
  CHECK(std::string(LogicalClipName(LogicalClip::Jog)) == "jog");
  CHECK(std::string(LogicalClipName(LogicalClip::Sprint)) == "sprint");
  CHECK(std::string(LogicalClipName(LogicalClip::Attack)) == "attack");
  CHECK(std::string(LogicalClipName(LogicalClip::CastIdle)) == "cast_idle");
  CHECK(std::string(LogicalClipName(LogicalClip::HitBody)) == "hit_body");
}
