#pragma once

// SimClock — generic simulation clock (game-agnostic time accounting).
//
// Decouples three notions of time so rendering, the day/night cycle, and the
// fixed-rate game logic all stay consistent and framerate-independent:
//   * real time  — wall-clock dt fed in each frame
//   * speed      — 0 (paused), 1, 2, 4x, ... (default 1)
//   * sim time   — accumulated real_dt * speed (the game/day clock)
//
// The render loop advances the clock from real dt (Advance). Both the day/night
// cycle (TimeOfDay / DayCounter) and the fixed-interval game logic (TickTarget)
// read from `sim_seconds`, so they advance together at `speed`x and neither is
// tied to the render framerate. No game types — just time math.
#include <algorithm>
#include <cmath>

#include "engine/app/fixed_timestep.hpp"  // kTickDt

namespace badlands {

struct SimClock {
  float speed = 1.0f;                    // 0 = paused; 1 / 2 / 4x, ...
  float real_seconds_per_day = 300.0f;   // real seconds per in-game day at 1x speed
  double sim_seconds = 0.0;              // total elapsed sim time

  // A single real frame is clamped to this before scaling, so a stall or a
  // debugger breakpoint cannot jump the day (or demand a huge tick catch-up).
  static constexpr double kMaxRealFrameDt = 0.25;

  // Advance by a real-time delta (clamped), scaled by speed. Returns the
  // sim-time delta added this frame.
  double Advance(double real_dt) {
    const double d =
        std::min(real_dt, kMaxRealFrameDt) * static_cast<double>(speed);
    sim_seconds += d;
    return d;
  }

  double Days() const {
    return sim_seconds / static_cast<double>(real_seconds_per_day);
  }
  int DayCounter() const { return static_cast<int>(std::floor(Days())); }
  float TimeOfDay() const {  // normalized t01 in [0,1)
    const double days = Days();
    return static_cast<float>(days - std::floor(days));
  }

  // Total number of fixed sim ticks that should have run by now — game logic
  // runs kSimHz ticks per sim-second, so this scales with speed automatically.
  unsigned long long TickTarget() const {
    return static_cast<unsigned long long>(sim_seconds / kTickDt);
  }

  // Jump to an absolute time-of-day within the current day (headless capture /
  // editor scrub). Wraps t01 into [0,1) so any input is valid. This is a
  // discontinuous jump — callers should NOT simulate the skipped time.
  void SeekTimeOfDay(float t01) {
    t01 -= std::floor(t01);  // wrap into [0,1); handles negatives too
    const double whole = std::floor(Days());
    sim_seconds = (whole + static_cast<double>(t01)) *
                  static_cast<double>(real_seconds_per_day);
  }
};

}  // namespace badlands
