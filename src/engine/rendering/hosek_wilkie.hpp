#pragma once

// Hosek-Wilkie (2012) analytic sky-dome radiance — RGB path.
//
// A clean C++ reimplementation of the RGB model from the reference
// "An Analytic Model for Full Spectral Sky-Dome Radiance" (Hosek & Wilkie,
// SIGGRAPH 2012 / IEEE CG&A 2013). The fitted coefficient tables live in
// hosek_wilkie_data.hpp (both are BSD-3, © 2012-2013 Lukas Hosek & Alexander
// Wilkie). CPU-only, no Dawn/GPU dependency — used to bake the sky environment
// cube (see daylight.cpp). Game-agnostic engine code.
#include <array>

#include <glm/glm.hpp>

namespace badlands {

// Cooked, per-RGB-channel sky configuration (9 coefficients A..I) plus a
// per-channel radiance scale. Stack-allocated (the reference mallocs; we do
// not, so it is cheap to rebuild whenever the sun moves).
struct HosekWilkieState {
  std::array<std::array<double, 9>, 3> configs{};
  std::array<double, 3> radiances{};
};

// Builds the state for a given atmosphere. Inputs are clamped internally:
//   turbidity           in [1, 10]   (clear sky ~2-3)
//   albedo              in [0, 1]    (ground reflectance)
//   solar_elevation_rad in [0, pi/2] (measured UP from the horizon; the model
//                                     is undefined below the horizon)
HosekWilkieState HosekWilkieInit(double turbidity, double albedo,
                                 double solar_elevation_rad);

// Sky radiance toward a view direction, linear RGB in the model's units.
//   theta = angle from the zenith of the view direction, in [0, pi/2)
//   gamma = angle between the view direction and the sun
// (cos(theta) must stay > 0; callers clamp theta just under pi/2 at the
// horizon.)
glm::vec3 HosekWilkieRadiance(const HosekWilkieState& state, double theta,
                              double gamma);

}  // namespace badlands
