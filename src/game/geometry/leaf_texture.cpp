// leaf_texture.cpp
#include "game/geometry/leaf_texture.hpp"
#include <algorithm>
#include <cmath>
#include <glm/gtc/constants.hpp>  // glm::pi
namespace badlands {
namespace {

// Alpha in [0,1] for a slab of half-width `half_w` (in u, fraction of
// half-size) around u=0, softened over `edge` (~2 texels). >0 half_w with
// |u| < half_w is fully inside; the old oval formula inlined this directly.
float AlphaFromHalfWidth(float u, float half_w, float edge) {
  const float d = half_w - std::fabs(u);
  return std::clamp(d / edge + 0.5f, 0.0f, 1.0f);
}

// Half-width profiles over t in [0,1] (0 = base, 1 = tip), all zero at both
// ends unless noted. Shared pi constant.
constexpr float kPi = glm::pi<float>();

float OvalHalfWidth(float t) {  // Bush: today's exact oval, unchanged.
  constexpr float W = 0.60f;
  return W * std::sin(kPi * t);
}

float LobedHalfWidth(float t) {  // Oak: broad body with lobe scalloping.
  constexpr float W = 0.55f;
  return W * std::sin(kPi * t) * (0.62f + 0.38f * std::fabs(std::sin(3.5f * kPi * t)));
}

float LanceolateHalfWidth(float t) {  // Ash: narrow, pointed leaflet.
  constexpr float W = 0.32f;
  return W * std::pow(std::sin(kPi * t), 0.75f);
}

float CordateHalfWidth(float t) {  // Aspen: round/broad, short pointed tip.
  constexpr float W = 0.72f;
  float half_w = W * std::sqrt(std::max(std::sin(kPi * t), 0.0f));
  // sqrt(sin) is blunt (vertical tangent) at t=1; replace the last 10% with a
  // linear taper so the tip comes to an actual point instead of a round cutoff.
  constexpr float kTipStart = 0.9f;
  if (t > kTipStart) {
    half_w *= std::clamp((1.0f - t) / (1.0f - kTipStart), 0.0f, 1.0f);
  }
  return half_w;
}

// PineSprig: central stem column union a periodic diagonal needle-stripe
// pattern, masked to a lateral band that tapers toward the tip. Analytic
// (distance to a periodic line family in (u,v) space -- u,v share the same
// per-texel scale, unlike t which is a half-rate reparam of v, so the
// family's angle is geometrically correct in the raster). The band (rather
// than per-needle finite segments) keeps the pattern a simple, spatially
// slowly-varying envelope around a uniform stripe field, so it downsamples
// predictably through the mip chain instead of moire-ing at coarse levels.
float PineSprigAlpha(float u, float t, float v, float edge) {
  constexpr float kStemHalfWidth = 0.03f;
  const float stem_a = AlphaFromHalfWidth(u, kStemHalfWidth, edge);

  const float theta = 55.0f * kPi / 180.0f;
  const float sin_t = std::sin(theta);
  const float cos_t = std::cos(theta);
  const float cot_t = cos_t / sin_t;
  constexpr float kPeriod = 0.15625f;           // ~10px at size 128 (10 * 2/128)
  constexpr float kStrokeHalfWidth = 0.02734f;  // ~3.5px full width at size 128 (>= the required 3px)
  constexpr float kReach = 0.20f;               // longest (base) lateral needle reach past the stem edge

  const float out = std::fabs(u) - kStemHalfWidth;       // lateral distance outside the stem edge
  const float reach = kReach * std::clamp(1.0f - t, 0.0f, 1.0f);  // shorter reach toward the tip
  const float d_band = std::min(out, reach - out);        // >0 inside the [0, reach] lateral band
  const float band_a = std::clamp(d_band / edge + 0.5f, 0.0f, 1.0f);

  const float b = v - out * cot_t;                        // this texel's line-family intercept at out=0
  const float vk = kPeriod * std::round(b / kPeriod);      // nearest stripe center
  const float dist_perp = std::fabs(b - vk) * sin_t;
  const float stripe_a = std::clamp((kStrokeHalfWidth - dist_perp) / edge + 0.5f, 0.0f, 1.0f);

  const float needle_a = std::min(band_a, stripe_a);
  return std::max(stem_a, needle_a);
}

}  // namespace

std::vector<uint8_t> BuildLeafRgba8(int size, glm::vec3 leaf_color, LeafSilhouette shape) {
  std::vector<uint8_t> rgba(static_cast<size_t>(size) * static_cast<size_t>(size) * 4, 0);
  auto to_byte = [](float c) {
    return static_cast<uint8_t>(std::lround(std::clamp(c, 0.0f, 1.0f) * 255.0f));
  };
  const uint8_t r = to_byte(leaf_color.r);
  const uint8_t g = to_byte(leaf_color.g);
  const uint8_t b = to_byte(leaf_color.b);
  const float edge = 2.0f / static_cast<float>(size);  // ~2-texel soft edge

  for (int y = 0; y < size; ++y) {
    for (int x = 0; x < size; ++x) {
      const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(size) * 2.0f - 1.0f;
      const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(size) * 2.0f - 1.0f;
      const float t = (v + 1.0f) * 0.5f;  // 0 = base, 1 = tip

      float a;
      switch (shape) {
        case LeafSilhouette::Oak: {
          a = AlphaFromHalfWidth(u, LobedHalfWidth(t), edge);
          if (t < 0.1f) a = std::max(a, AlphaFromHalfWidth(u, 0.04f, edge));  // petiole stem bar
          break;
        }
        case LeafSilhouette::Ash:
          a = AlphaFromHalfWidth(u, LanceolateHalfWidth(t), edge);
          break;
        case LeafSilhouette::Aspen:
          a = AlphaFromHalfWidth(u, CordateHalfWidth(t), edge);
          break;
        case LeafSilhouette::PineSprig:
          a = PineSprigAlpha(u, t, v, edge);
          break;
        case LeafSilhouette::Bush:
        default:
          a = AlphaFromHalfWidth(u, OvalHalfWidth(t), edge);
          break;
      }

      const size_t idx = (static_cast<size_t>(y) * static_cast<size_t>(size) +
                          static_cast<size_t>(x)) * 4;
      rgba[idx + 0] = r;
      rgba[idx + 1] = g;
      rgba[idx + 2] = b;
      rgba[idx + 3] = static_cast<uint8_t>(std::lround(a * 255.0f));
    }
  }
  return rgba;
}

std::vector<std::vector<uint8_t>> BuildLeafMipChainRgba8(int size, glm::vec3 leaf_color,
                                                           LeafSilhouette shape,
                                                           float alpha_cutoff) {
  auto to_byte = [](float c) {
    return static_cast<uint8_t>(std::lround(std::clamp(c, 0.0f, 1.0f) * 255.0f));
  };
  const uint8_t r = to_byte(leaf_color.r);
  const uint8_t g = to_byte(leaf_color.g);
  const uint8_t b = to_byte(leaf_color.b);
  const uint8_t cutoff_byte =
      static_cast<uint8_t>(std::lround(std::clamp(alpha_cutoff, 0.0f, 1.0f) * 255.0f));

  auto coverage = [&](const std::vector<uint8_t>& px, size_t texel_count) {
    if (texel_count == 0) return 0.0f;
    size_t count = 0;
    for (size_t i = 0; i < texel_count; ++i) {
      if (px[i * 4 + 3] >= cutoff_byte) ++count;
    }
    return static_cast<float>(count) / static_cast<float>(texel_count);
  };

  std::vector<std::vector<uint8_t>> mips;
  mips.push_back(BuildLeafRgba8(size, leaf_color, shape));
  const float level0_coverage = coverage(mips[0], static_cast<size_t>(size) * static_cast<size_t>(size));

  int w = size, h = size;
  while (w > 1 || h > 1) {
    const int nw = std::max(1, w / 2);
    const int nh = std::max(1, h / 2);
    const std::vector<uint8_t>& prev = mips.back();
    std::vector<uint8_t> next(static_cast<size_t>(nw) * static_cast<size_t>(nh) * 4, 0);

    for (int y = 0; y < nh; ++y) {
      for (int x = 0; x < nw; ++x) {
        // 2x2 box downsample of alpha; clamp to the previous level's last
        // row/col so odd w/h don't read out of bounds. RGB is reset to the
        // flat leaf_color below rather than filtered, so it never drifts.
        const int x0 = std::min(2 * x, w - 1), x1 = std::min(2 * x + 1, w - 1);
        const int y0 = std::min(2 * y, h - 1), y1 = std::min(2 * y + 1, h - 1);
        uint32_t a_sum = 0;
        for (int yy : {y0, y1}) {
          for (int xx : {x0, x1}) {
            const size_t idx = (static_cast<size_t>(yy) * static_cast<size_t>(w) +
                                static_cast<size_t>(xx)) * 4;
            a_sum += prev[idx + 3];
          }
        }
        const size_t oi = (static_cast<size_t>(y) * static_cast<size_t>(nw) +
                           static_cast<size_t>(x)) * 4;
        next[oi + 0] = r;
        next[oi + 1] = g;
        next[oi + 2] = b;
        next[oi + 3] = static_cast<uint8_t>(a_sum / 4);
      }
    }

    // Coverage preservation (Castano): rescale this level's alpha by s so its
    // coverage at alpha_cutoff matches level 0's. Skip trivially-small levels.
    const size_t texel_count = static_cast<size_t>(nw) * static_cast<size_t>(nh);
    if (texel_count > 4) {
      auto coverage_at_scale = [&](float s) {
        size_t count = 0;
        for (size_t i = 0; i < texel_count; ++i) {
          const float scaled = std::min(255.0f, std::round(static_cast<float>(next[i * 4 + 3]) * s));
          if (scaled >= static_cast<float>(cutoff_byte)) ++count;
        }
        return static_cast<float>(count) / static_cast<float>(texel_count);
      };
      // Coverage is monotonic non-decreasing in s, so bisect for the scale
      // whose coverage best matches level 0's.
      float lo = 1.0f, hi = 4.0f, best_s = 1.0f;
      float best_diff = std::fabs(coverage_at_scale(1.0f) - level0_coverage);
      for (int i = 0; i < 10; ++i) {
        const float mid = 0.5f * (lo + hi);
        const float cov = coverage_at_scale(mid);
        const float diff = std::fabs(cov - level0_coverage);
        if (diff < best_diff) { best_diff = diff; best_s = mid; }
        if (cov < level0_coverage) lo = mid; else hi = mid;
      }
      for (size_t i = 0; i < texel_count; ++i) {
        const float scaled = std::min(255.0f, std::round(static_cast<float>(next[i * 4 + 3]) * best_s));
        next[i * 4 + 3] = static_cast<uint8_t>(scaled);
      }
    }

    mips.push_back(std::move(next));
    w = nw;
    h = nh;
  }
  return mips;
}

}  // namespace badlands
