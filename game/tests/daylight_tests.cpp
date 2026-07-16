// Day/night system tests (Task: daylight system). Pure CPU — no GPU/Dawn.
//
//   * Hosek-Wilkie RGB port vs golden values from the reference
//     ArHosekSkyModel.cpp (ground truth), plus physical sanity.
//   * ComputeDaylight sun/moon geometry + the sun-precedence / moon-ease
//     handover.
//   * The fixed-timestep accumulator (AccumulateFixedTicks), incl. the
//     spiral-of-death clamp.
#include <cmath>

#include <catch_amalgamated.hpp>
#include <glm/glm.hpp>

#include "engine/app/sim_clock.hpp"
#include "engine/rendering/daylight.hpp"
#include "engine/rendering/hosek_wilkie.hpp"

using namespace badlands;

namespace {
constexpr double kPi = 3.141592653589793;
constexpr double kDeg = kPi / 180.0;
}  // namespace

// ---------------------------------------------------------------------------
// Hosek-Wilkie RGB port
// ---------------------------------------------------------------------------

TEST_CASE("Hosek-Wilkie RGB matches reference golden values", "[hosek]") {
  // {turbidity, albedo, elevation, theta, gamma, {r, g, b}} — the RGB values
  // are produced by the verbatim reference ArHosekSkyModel.cpp (RGB path).
  struct Golden {
    double turb, alb, elev, theta, gamma;
    float rgb[3];
  };
  const Golden cases[] = {
      {2.0, 0.10, 60 * kDeg, 30 * kDeg, 20 * kDeg, {4.134830049f, 6.391716416f, 12.189500470f}},
      {3.0, 0.15, 10 * kDeg, 80 * kDeg, 5 * kDeg, {47.375575030f, 38.833138433f, 22.528361175f}},
      {5.0, 0.30, 45 * kDeg, 45 * kDeg, 90 * kDeg, {3.352547212f, 5.991529984f, 10.875755373f}},
      {1.0, 0.00, 89 * kDeg, 10 * kDeg, 45 * kDeg, {1.013339043f, 2.900192415f, 7.068885981f}},
      {8.5, 0.50, 25 * kDeg, 60 * kDeg, 120 * kDeg, {5.301485252f, 7.242203573f, 10.138047485f}},
  };
  for (const Golden& c : cases) {
    HosekWilkieState st = HosekWilkieInit(c.turb, c.alb, c.elev);
    glm::vec3 r = HosekWilkieRadiance(st, c.theta, c.gamma);
    for (int ch = 0; ch < 3; ++ch) {
      REQUIRE(r[ch] == Catch::Approx(c.rgb[ch]).epsilon(1e-4));
    }
  }
}

TEST_CASE("Hosek-Wilkie radiance is finite, positive, brighter toward the sun",
          "[hosek]") {
  // Clear-ish sky, sun 40deg up.
  HosekWilkieState st = HosekWilkieInit(2.5, 0.2, 40 * kDeg);

  // Finite & non-negative across a spread of view directions.
  for (double theta = 0.0; theta < 89 * kDeg; theta += 10 * kDeg) {
    for (double gamma = 0.0; gamma <= kPi; gamma += 15 * kDeg) {
      glm::vec3 r = HosekWilkieRadiance(st, theta, gamma);
      for (int ch = 0; ch < 3; ++ch) {
        REQUIRE(std::isfinite(r[ch]));
        REQUIRE(r[ch] >= 0.0f);
      }
    }
  }

  // At a fixed zenith angle, the sky is brighter looking toward the sun
  // (small gamma) than away from it (large gamma).
  const double theta = 45 * kDeg;
  glm::vec3 near_sun = HosekWilkieRadiance(st, theta, 5 * kDeg);
  glm::vec3 away = HosekWilkieRadiance(st, theta, 120 * kDeg);
  REQUIRE(near_sun.r + near_sun.g + near_sun.b >
          away.r + away.g + away.b);
}

TEST_CASE("Hosek-Wilkie clamps out-of-range inputs without NaNs", "[hosek]") {
  // Below-horizon elevation, out-of-range turbidity/albedo — must clamp, not
  // blow up.
  HosekWilkieState st = HosekWilkieInit(/*turb=*/50.0, /*alb=*/2.0,
                                        /*elev=*/-0.5);
  glm::vec3 r = HosekWilkieRadiance(st, 45 * kDeg, 45 * kDeg);
  for (int ch = 0; ch < 3; ++ch) REQUIRE(std::isfinite(r[ch]));
}

// ---------------------------------------------------------------------------
// ComputeDaylight — sun/moon geometry + handover
// ---------------------------------------------------------------------------

TEST_CASE("Sun follows the day arc", "[daylight]") {
  DaylightConfig cfg;  // defaults: noon_elevation_deg = 80
  const float e_max = glm::radians(cfg.noon_elevation_deg);

  auto st = [&](float t) { return ComputeDaylight(cfg, t); };

  // Sunrise/sunset at the horizon; noon at peak; midnight below.
  REQUIRE(st(0.25f).sun_elevation_rad == Catch::Approx(0.0).margin(1e-4));
  REQUIRE(st(0.75f).sun_elevation_rad == Catch::Approx(0.0).margin(1e-4));
  REQUIRE(st(0.5f).sun_elevation_rad == Catch::Approx(e_max).margin(1e-4));
  REQUIRE(st(0.0f).sun_elevation_rad == Catch::Approx(-e_max).margin(1e-4));

  // sun_direction.y == sin(elevation); direction unit-length; moon anti-solar.
  for (float t = 0.0f; t < 1.0f; t += 0.05f) {
    DaylightState s = st(t);
    REQUIRE(s.sun_direction.y ==
            Catch::Approx(std::sin(s.sun_elevation_rad)).margin(1e-5));
    REQUIRE(glm::length(s.sun_direction) == Catch::Approx(1.0).margin(1e-5));
    REQUIRE(glm::length(s.light_direction) == Catch::Approx(1.0).margin(1e-5));
    REQUIRE(glm::dot(s.sun_direction, s.moon_direction) ==
            Catch::Approx(-1.0).margin(1e-5));
  }
}

TEST_CASE("Sun takes precedence while above the horizon", "[daylight]") {
  DaylightConfig cfg;
  // Sample the daylight half (sun above horizon, excluding the exact horizon).
  for (float t = 0.26f; t < 0.74f; t += 0.02f) {
    DaylightState s = ComputeDaylight(cfg, t);
    REQUIRE(s.sun_elevation_rad > 0.0f);
    // Directional light is the sun (points at the sun), never the moon.
    REQUIRE(glm::dot(s.light_direction, s.sun_direction) ==
            Catch::Approx(1.0).margin(1e-5));
    REQUIRE(s.light_intensity > 0.0f);
  }

  // Full daylight at noon.
  DaylightState noon = ComputeDaylight(cfg, 0.5f);
  REQUIRE(noon.light_intensity == Catch::Approx(cfg.sun_intensity_max).margin(1e-4));
}

TEST_CASE("Moon eases in only after the sun sets", "[daylight]") {
  DaylightConfig cfg;

  // Deep night: the light is the (dim) moon, at full moon intensity.
  DaylightState midnight = ComputeDaylight(cfg, 0.0f);
  REQUIRE(midnight.sun_elevation_rad < 0.0f);
  REQUIRE(glm::dot(midnight.light_direction, midnight.moon_direction) ==
          Catch::Approx(1.0).margin(1e-5));
  REQUIRE(midnight.light_intensity == Catch::Approx(cfg.moon_intensity).margin(1e-4));

  // Dynamic-range check: the moon sits ~2-3 orders of magnitude below the sun
  // (compressed from the physical ~6 so it fits the renderer's range).
  const double orders = std::log10(cfg.sun_intensity_max / cfg.moon_intensity);
  REQUIRE(orders >= 2.0);
  REQUIRE(orders <= 3.0);
}

TEST_CASE("Directional light is continuous through the horizon crossing",
          "[daylight]") {
  DaylightConfig cfg;
  // Just before, at, and just after sunset (t = 0.75).
  const float dt = 0.001f;
  float prev = ComputeDaylight(cfg, 0.75f - dt).light_intensity;
  float at = ComputeDaylight(cfg, 0.75f).light_intensity;
  float next = ComputeDaylight(cfg, 0.75f + dt).light_intensity;

  // The directional term is ~0 at the crossing (sun faded out, moon not yet
  // eased in) — so no drastic jump; the sky carries the frame.
  REQUIRE(at < 0.05f * cfg.sun_intensity_max);
  REQUIRE(std::abs(prev - at) < 0.05f * cfg.sun_intensity_max);
  REQUIRE(std::abs(next - at) < 0.05f * cfg.sun_intensity_max);
}

TEST_CASE("Night sky is dimmed relative to day", "[daylight]") {
  DaylightConfig cfg;
  DaylightState noon = ComputeDaylight(cfg, 0.5f);
  DaylightState midnight = ComputeDaylight(cfg, 0.0f);
  REQUIRE(midnight.sky_scale < noon.sky_scale);
  REQUIRE(noon.sky_scale == Catch::Approx(cfg.sky_exposure).margin(1e-6));
  // HW elevation is clamped to >= 0 (undefined below the horizon).
  REQUIRE(midnight.hw_solar_elevation_rad == Catch::Approx(0.0).margin(1e-6));
}

TEST_CASE("ComputeDaylight is pure/deterministic", "[daylight]") {
  DaylightConfig cfg;
  for (float t : {0.0f, 0.2f, 0.5f, 0.77f, 0.99f}) {
    DaylightState a = ComputeDaylight(cfg, t);
    DaylightState b = ComputeDaylight(cfg, t);
    REQUIRE(a.light_intensity == b.light_intensity);
    REQUIRE(a.sun_direction == b.sun_direction);
    REQUIRE(a.sky_scale == b.sky_scale);
  }
}

// ---------------------------------------------------------------------------
// SimClock — real time / sim speed / sim time
// ---------------------------------------------------------------------------

TEST_CASE("SimClock advances sim time at the current speed", "[simclock]") {
  SimClock c;
  c.speed = 1.0f;
  c.Advance(0.1);
  REQUIRE(c.sim_seconds == Catch::Approx(0.1));

  // Paused (0x) -> no advance.
  c.speed = 0.0f;
  c.Advance(0.1);
  REQUIRE(c.sim_seconds == Catch::Approx(0.1));

  // 2x -> double the sim-time delta.
  c.speed = 2.0f;
  c.Advance(0.1);
  REQUIRE(c.sim_seconds == Catch::Approx(0.3));
}

TEST_CASE("SimClock maps sim time to day fraction + day counter", "[simclock]") {
  SimClock c;
  c.real_seconds_per_day = 16.0f;
  c.speed = 1.0f;
  // Step by 0.25 (exactly representable, == the clamp) so the sum is exact.
  for (int i = 0; i < 32; ++i) c.Advance(0.25);  // exactly 8 s = half a day
  REQUIRE(c.sim_seconds == Catch::Approx(8.0));
  REQUIRE(c.TimeOfDay() == Catch::Approx(0.5).margin(1e-4));
  REQUIRE(c.DayCounter() == 0);

  // Another half day rolls the counter and wraps time-of-day.
  for (int i = 0; i < 32; ++i) c.Advance(0.25);  // total 16 s = one full day
  REQUIRE(c.sim_seconds == Catch::Approx(16.0));
  REQUIRE(c.DayCounter() == 1);
  REQUIRE(c.TimeOfDay() == Catch::Approx(0.0).margin(1e-4));
}

TEST_CASE("SimClock clamps a huge frame (no day jump on a stall)", "[simclock]") {
  SimClock c;
  c.speed = 1.0f;
  const double added = c.Advance(100.0);  // stall / breakpoint
  REQUIRE(added == Catch::Approx(SimClock::kMaxRealFrameDt));
  REQUIRE(c.sim_seconds == Catch::Approx(SimClock::kMaxRealFrameDt));
}

TEST_CASE("SimClock TickTarget tracks sim time and scales with speed",
          "[simclock]") {
  // 1.0 sim-second -> ~kSimHz ticks. Tolerate the integer-boundary rounding of
  // sim_seconds / kTickDt (kTickDt = 1/30 is not exact in binary).
  SimClock c;
  c.speed = 1.0f;
  for (int i = 0; i < 4; ++i) c.Advance(0.25);  // exactly 1.0 sim-second
  const unsigned long long t1 = c.TickTarget();
  REQUIRE(t1 >= static_cast<unsigned long long>(kSimHz) - 1);
  REQUIRE(t1 <= static_cast<unsigned long long>(kSimHz));

  // At 2x, the same real time yields ~twice the ticks.
  SimClock c2;
  c2.speed = 2.0f;
  for (int i = 0; i < 4; ++i) c2.Advance(0.25);  // 2.0 sim-seconds
  const unsigned long long t2 = c2.TickTarget();
  REQUIRE(t2 >= static_cast<unsigned long long>(2 * kSimHz) - 1);
  REQUIRE(t2 <= static_cast<unsigned long long>(2 * kSimHz));
}

TEST_CASE("SimClock SeekTimeOfDay wraps out-of-range input", "[simclock]") {
  SimClock c;
  c.real_seconds_per_day = 16.0f;
  c.SeekTimeOfDay(0.25f);
  REQUIRE(c.TimeOfDay() == Catch::Approx(0.25).margin(1e-4));
  c.SeekTimeOfDay(1.2f);  // wraps to 0.2
  REQUIRE(c.TimeOfDay() == Catch::Approx(0.2).margin(1e-4));
  c.SeekTimeOfDay(-0.1f);  // wraps to 0.9
  REQUIRE(c.TimeOfDay() == Catch::Approx(0.9).margin(1e-4));
}
