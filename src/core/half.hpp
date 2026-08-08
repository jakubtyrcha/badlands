#pragma once

// IEEE 754 binary16 -> float.
//
// ONE COPY. This existed three times before it lived here -- object_viewer's
// headless readback, gpu_instance_tests, and water_gpu_tests -- because every
// float render target hands back raw bytes and something has to decode them.
// Three copies of a bit-twiddling routine is three chances for the subnormal
// branch to be subtly different.

#include <bit>
#include <cmath>
#include <cstdint>

namespace badlands::core {

inline float HalfToFloat(uint16_t h) {
  const uint32_t sign = uint32_t(h & 0x8000u) << 16;
  const uint32_t exp = (h >> 10) & 0x1Fu;
  const uint32_t mant = h & 0x3FFu;
  if (exp == 0) {
    if (mant == 0) return std::bit_cast<float>(sign);  // +/-0
    // Subnormal: normalize by hand rather than trusting a shift chain.
    const float v = float(mant) / 1024.0f * std::ldexp(1.0f, -14);
    return sign ? -v : v;
  }
  if (exp == 31) {  // inf / nan
    return std::bit_cast<float>(sign | 0x7F800000u | (mant << 13));
  }
  return std::bit_cast<float>(sign | ((exp + 112u) << 23) | (mant << 13));
}

}  // namespace badlands::core
