#pragma once

// Fixed-timestep simulation constants (Task: daylight system).
//
// The world simulation advances at a fixed rate in SIM time (see sim_clock.hpp)
// so it is deterministic and framerate-independent. The clock is driven by the
// render loop's real dt, scaled by the sim speed; game logic runs kSimHz ticks
// per sim-second (SimClock::TickTarget). Game-agnostic engine code.
namespace badlands {

inline constexpr int kSimHz = 30;                // fixed simulation rate (ticks / sim-second)
inline constexpr double kTickDt = 1.0 / kSimHz;  // sim-seconds per tick

// Upper bound on fixed sim ticks run in a single frame — spiral-of-death
// safety. Real dt is clamped in SimClock::Advance, so this is not normally
// reached; it only caps catch-up after a pathological stall.
inline constexpr int kMaxSimTicksPerFrame = 64;

// Fixed presentation dt used for deterministic headless capture (--record):
// bypasses the wall clock so the sim clock advances a fixed amount per captured
// frame. 60 fps => a 16 s day is 960 frames.
inline constexpr float kPresentationDt = 1.0f / 60.0f;

}  // namespace badlands
