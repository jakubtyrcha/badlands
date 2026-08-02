#pragma once

// Alpha-coverage preservation (Castano) for alpha-cutout mip chains.
//
// A plain box downsample averages alpha, which drives partially-covered texels
// below the cutoff -- so cutout foliage THINS as it recedes and eventually
// dissolves. The fix is to rescale each level's alpha by the factor that makes
// its coverage at the cutoff match level 0's.
//
// Extracted from BuildLeafMipChainRgba8 (game/geometry/leaf_texture.cpp), which
// needed exactly this for leaf cards; that function now calls these rather than
// carrying its own copy, and the impostor atlas calls them too. One copy on
// purpose: it is thirty lines of numerics whose failure mode is a gradual,
// plausible-looking fade rather than an error, so two copies would drift
// without anyone noticing.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstddef>
#include <span>

namespace badlands {

// Fraction of texels whose alpha is at or above `cutoff_byte`.
inline float AlphaCoverage(std::span<const uint8_t> rgba, size_t texel_count,
                           uint8_t cutoff_byte) {
  if (texel_count == 0) return 0.0f;
  size_t count = 0;
  for (size_t i = 0; i < texel_count; ++i) {
    if (rgba[i * 4 + 3] >= cutoff_byte) ++count;
  }
  return static_cast<float>(count) / static_cast<float>(texel_count);
}

// The alpha scale whose resulting coverage best matches `target_coverage`.
//
// Bisected because coverage is monotonic non-decreasing in the scale. The
// search spans BOTH directions around 1.0: box-downsampling usually loses
// coverage, but thin sprig/needle content can also INFLATE it as more
// partially-covered texels average together, and a floor of 1.0 could never
// correct that.
inline float FitAlphaCoverageScale(std::span<const uint8_t> rgba,
                                   size_t texel_count, uint8_t cutoff_byte,
                                   float target_coverage) {
  if (texel_count == 0) return 1.0f;

  const auto coverage_at = [&](float s) {
    size_t count = 0;
    for (size_t i = 0; i < texel_count; ++i) {
      const float scaled = std::min(
          255.0f, std::round(static_cast<float>(rgba[i * 4 + 3]) * s));
      if (scaled >= static_cast<float>(cutoff_byte)) ++count;
    }
    return static_cast<float>(count) / static_cast<float>(texel_count);
  };

  float lo = 0.25f, hi = 4.0f, best_s = 1.0f;
  float best_diff = std::fabs(coverage_at(1.0f) - target_coverage);
  for (int i = 0; i < 10; ++i) {
    const float mid = 0.5f * (lo + hi);
    const float cov = coverage_at(mid);
    const float diff = std::fabs(cov - target_coverage);
    if (diff < best_diff) {
      best_diff = diff;
      best_s = mid;
    }
    if (cov < target_coverage) {
      lo = mid;
    } else {
      hi = mid;
    }
  }
  return best_s;
}

// Multiplies every texel's alpha by `scale`, saturating at 255.
inline void ApplyAlphaScale(std::span<uint8_t> rgba, size_t texel_count,
                            float scale) {
  for (size_t i = 0; i < texel_count; ++i) {
    const float scaled = std::min(
        255.0f, std::round(static_cast<float>(rgba[i * 4 + 3]) * scale));
    rgba[i * 4 + 3] = static_cast<uint8_t>(scaled);
  }
}

}  // namespace badlands
