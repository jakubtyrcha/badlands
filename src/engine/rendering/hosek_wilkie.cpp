// Hosek-Wilkie RGB sky model — see hosek_wilkie.hpp. Port of the reference
// ArHosekSkyModel.cpp RGB path (cook config/radiance + evaluate), BSD-3.
#include "engine/rendering/hosek_wilkie.hpp"

#include <algorithm>
#include <cmath>

namespace badlands {
namespace {

// Vendored BSD-3 coefficient tables (Hosek & Wilkie). Included inside this
// anonymous namespace so the large arrays get internal linkage — they are used
// only here, and this keeps the reference's global `datasetsRGB` /
// `datasetsRGBRad` names out of the badlands global namespace.
//   datasetsRGB[channel]    -> 1080 doubles: 2 albedo x 10 turbidity x
//                              6 elevation control points x 9 coefficients
//   datasetsRGBRad[channel] -> 120 doubles:  2 albedo x 10 turbidity x
//                              6 control points x 1 radiance value
#include "engine/rendering/hosek_wilkie_data.hpp"

constexpr double kHalfPi = 3.141592653589793 / 2.0;

// 5th-order Bezier over the 6 solar-elevation control points at stride `stride`
// starting at m[0] (`t` = the reparameterised elevation, precomputed by caller).
double ElevationBezier(const double* m, int stride, double t) {
  const double it = 1.0 - t;
  return std::pow(it, 5.0) * m[0] +
         5.0 * std::pow(it, 4.0) * t * m[stride] +
         10.0 * std::pow(it, 3.0) * t * t * m[2 * stride] +
         10.0 * std::pow(it, 2.0) * std::pow(t, 3.0) * m[3 * stride] +
         5.0 * it * std::pow(t, 4.0) * m[4 * stride] +
         std::pow(t, 5.0) * m[5 * stride];
}

// Port of ArHosekSkyModel_CookConfiguration (RGB): fills config[9] by blending
// the Bezier fits across the two albedo endpoints and the two turbidity
// brackets that surround the requested turbidity.
void CookConfiguration(const double* dataset, double* config, double turbidity,
                       double albedo, double solar_elevation) {
  const int int_turbidity = static_cast<int>(turbidity);
  const double turbidity_rem = turbidity - static_cast<double>(int_turbidity);
  const double se = std::pow(solar_elevation / kHalfPi, 1.0 / 3.0);

  // alb 0, low turbidity
  const double* m = dataset + (9 * 6 * (int_turbidity - 1));
  for (int i = 0; i < 9; ++i)
    config[i] = (1.0 - albedo) * (1.0 - turbidity_rem) *
                ElevationBezier(m + i, 9, se);
  // alb 1, low turbidity
  m = dataset + (9 * 6 * 10 + 9 * 6 * (int_turbidity - 1));
  for (int i = 0; i < 9; ++i)
    config[i] += albedo * (1.0 - turbidity_rem) * ElevationBezier(m + i, 9, se);
  if (int_turbidity == 10) return;  // no higher bracket to interpolate toward
  // alb 0, high turbidity
  m = dataset + (9 * 6 * int_turbidity);
  for (int i = 0; i < 9; ++i)
    config[i] += (1.0 - albedo) * turbidity_rem * ElevationBezier(m + i, 9, se);
  // alb 1, high turbidity
  m = dataset + (9 * 6 * 10 + 9 * 6 * int_turbidity);
  for (int i = 0; i < 9; ++i)
    config[i] += albedo * turbidity_rem * ElevationBezier(m + i, 9, se);
}

// Port of ArHosekSkyModel_CookRadianceConfiguration (RGB): the single per-
// channel radiance scale, same albedo/turbidity blend as CookConfiguration.
double CookRadiance(const double* dataset, double turbidity, double albedo,
                    double solar_elevation) {
  const int int_turbidity = static_cast<int>(turbidity);
  const double turbidity_rem = turbidity - static_cast<double>(int_turbidity);
  const double se = std::pow(solar_elevation / kHalfPi, 1.0 / 3.0);

  double res = (1.0 - albedo) * (1.0 - turbidity_rem) *
               ElevationBezier(dataset + 6 * (int_turbidity - 1), 1, se);
  res += albedo * (1.0 - turbidity_rem) *
         ElevationBezier(dataset + 6 * 10 + 6 * (int_turbidity - 1), 1, se);
  if (int_turbidity == 10) return res;
  res += (1.0 - albedo) * turbidity_rem *
         ElevationBezier(dataset + 6 * int_turbidity, 1, se);
  res += albedo * turbidity_rem *
         ElevationBezier(dataset + 6 * 10 + 6 * int_turbidity, 1, se);
  return res;
}

// Port of ArHosekSkyModel_GetRadianceInternal: the closed-form Perez-like
// evaluation for one channel's cooked config.
double GetRadianceInternal(const double* c, double theta, double gamma) {
  const double cos_gamma = std::cos(gamma);
  const double cos_theta = std::cos(theta);
  const double expM = std::exp(c[4] * gamma);
  const double rayM = cos_gamma * cos_gamma;
  const double mieM = (1.0 + cos_gamma * cos_gamma) /
                      std::pow(1.0 + c[8] * c[8] - 2.0 * c[8] * cos_gamma, 1.5);
  const double zenith = std::sqrt(cos_theta);
  return (1.0 + c[0] * std::exp(c[1] / (cos_theta + 0.01))) *
         (c[2] + c[3] * expM + c[5] * rayM + c[6] * mieM + c[7] * zenith);
}

}  // namespace

HosekWilkieState HosekWilkieInit(double turbidity, double albedo,
                                 double solar_elevation_rad) {
  turbidity = std::clamp(turbidity, 1.0, 10.0);
  albedo = std::clamp(albedo, 0.0, 1.0);
  solar_elevation_rad = std::clamp(solar_elevation_rad, 0.0, kHalfPi);

  HosekWilkieState state;
  for (int c = 0; c < 3; ++c) {
    CookConfiguration(datasetsRGB[c], state.configs[c].data(), turbidity,
                      albedo, solar_elevation_rad);
    state.radiances[c] =
        CookRadiance(datasetsRGBRad[c], turbidity, albedo, solar_elevation_rad);
  }
  return state;
}

glm::vec3 HosekWilkieRadiance(const HosekWilkieState& state, double theta,
                              double gamma) {
  glm::vec3 rgb;
  for (int c = 0; c < 3; ++c) {
    rgb[c] = static_cast<float>(
        GetRadianceInternal(state.configs[c].data(), theta, gamma) *
        state.radiances[c]);
  }
  return rgb;
}

}  // namespace badlands
