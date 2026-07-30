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
// ends unless noted. Shared pi constant. These are the per-species SINGLE-
// LEAF silhouettes; the sprig builder below stamps them (small, many, at
// varied placement) rather than drawing one filling the whole card.
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
// DO NOT TOUCH: byte-for-byte output must stay unchanged.
float PineSprigAlpha(float u, float t, float v, float edge) {
  constexpr float kStemHalfWidth = 0.04f;  // (screenshot-tuned) slightly wider stem column
  const float stem_a = AlphaFromHalfWidth(u, kStemHalfWidth, edge);

  const float theta = 55.0f * kPi / 180.0f;
  const float sin_t = std::sin(theta);
  const float cos_t = std::cos(theta);
  const float cot_t = cos_t / sin_t;
  constexpr float kPeriod = 0.125f;             // ~8px at size 128 (screenshot-tuned, ~20% tighter than 10px)
  constexpr float kStrokeHalfWidth = 0.041f;    // ~5px full width at size 128 (screenshot-tuned, x1.5 the prior 3.5px)
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

// ---------------------------------------------------------------------------
// Sprig construction (Oak/Ash/Aspen/Bush): a main stem + a handful of side
// twigs bearing 20-40 small leaf stamps (the single-leaf profiles above,
// reused at ~12-25% of the card's scale), composited by max-alpha so the
// card reads as a full photographed branch sprig (ez-tree reference) rather
// than one leaf on a mostly-empty quad. Everything here is a pure function
// of `shape` -- a fixed integer hash stands in for RNG so the layout (and so
// the final pixels) is byte-stable run-to-run with no external state.

uint32_t HashU32(uint32_t x) {  // integer finalizer-style hash, no external state
  x ^= x >> 16; x *= 0x7feb352du;
  x ^= x >> 15; x *= 0x846ca68bu;
  x ^= x >> 16;
  return x;
}
float HashUnit(uint32_t seed) {  // deterministic pseudo-random value in [0,1)
  return static_cast<float>(HashU32(seed) & 0x00FFFFFFu) / static_cast<float>(0x01000000u);
}
float HashRange(uint32_t seed, float lo, float hi) {
  return lo + (hi - lo) * HashUnit(seed);
}

// One leaf stamp: a single-leaf silhouette evaluated in its own local frame
// (center, rotation, scale = the stamp's fractional card size) then
// composited into the card. `gray` is this stamp's constant RGB gain
// (leaf_color * gray) -- the per-stamp brightness variation that makes
// individual leaves read as distinct up close.
struct SprigStamp {
  glm::vec2 center;
  float rotation;
  float scale;  // fraction of card size; 0.22–0.38 across species (Ash to Bush)
  float gray;
};

// A tapered capsule stroke (stem/twig segment): straight line a->b, half-
// width lerp(w0,w1) along its length, rounded caps (plain point-to-segment
// distance). Stem/twigs share one low, constant gray so they read as thin
// dark-ish wood against the brighter leaf stamps.
struct SprigStroke {
  glm::vec2 a, b;
  float half_w0, half_w1;
  float gray;
};

struct SprigLayout {
  std::vector<SprigStroke> strokes;
  std::vector<SprigStamp> stamps;
};

constexpr float kTwigGray = 0.45f;
constexpr float kStemHalfW0 = 0.032f, kStemHalfW1 = 0.010f;
constexpr float kTwigHalfW0 = 0.020f, kTwigHalfW1 = 0.006f;

// Main stem centerline: a gentle single-bow curve pinned at u=0 at both the
// base (t=0) and tip (t=1) so the stem always crosses the card's vertical
// centerline near its base -- a stable, easy-to-probe landmark (mirrors
// PineSprig's own u=0 stem column).
float StemU(float t, float amp) { return amp * std::sin(kPi * t); }
glm::vec2 StemPoint(float t, float amp) { return {StemU(t, amp), 2.0f * t - 1.0f}; }
glm::vec2 StemTangent(float t, float amp) {
  const glm::vec2 d(amp * kPi * std::cos(kPi * t), 2.0f);
  const float len = glm::length(d);
  return (len > 1e-6f) ? d / len : glm::vec2(0.0f, 1.0f);
}

// Per-species sprig recipe (screenshot-tuned): twig count/spacing/angle and
// stamp placement/scale, chosen so overall alpha coverage lands ~30-50%
// (ez-tree-like density) while keeping species character -- oak dense &
// lobed, ash pinnate leaflet pairs, aspen rounder & airier, bush compact.
struct SprigRecipe {
  int twig_count;
  float twig_t_lo, twig_t_hi;          // fractional stem height twigs originate from
  float twig_angle_lo, twig_angle_hi;  // degrees off the stem tangent
  float twig_len_lo, twig_len_hi;      // card units (stem/card half-span is 1.0)
  int placement_count;                 // attachment points spread across twigs
                                        // (pinnate doubles this into leaflet pairs)
  float stamp_lo, stamp_hi;            // stamp scale, fraction of card size
  int tip_stamps;                      // extra stamps capping the stem's tip
  bool pinnate;                        // ash: mirrored leaflet pairs off each twig point
  float stem_curve_amp;
};

SprigRecipe MakeRecipe(LeafSilhouette shape) {
  // Stamp counts (28–53 per species incl. tip stamps) and scales (0.22–0.38 range) are tuned
  // to achieve ~0.25–0.55 alpha-coverage and closed-crown, ez-tree-like density at 512px—
  // deliberately above the design's initial 20–40 stamp / 12–25% coverage guideline.
  switch (shape) {
    case LeafSilhouette::Oak:  // dense, lobed
      return {7, 0.12f, 0.85f, 35.0f, 55.0f, 0.34f, 0.54f, 48, 0.25f, 0.33f, 5, false, 0.05f};
    case LeafSilhouette::Ash:  // pinnate leaflet pairs
      return {6, 0.10f, 0.85f, 42.0f, 60.0f, 0.38f, 0.62f, 18, 0.22f, 0.29f, 2, true, 0.04f};
    case LeafSilhouette::Aspen:  // rounder, airier
      return {5, 0.12f, 0.85f, 30.0f, 50.0f, 0.34f, 0.58f, 25, 0.24f, 0.33f, 3, false, 0.05f};
    case LeafSilhouette::Bush:  // compact
    default:
      return {5, 0.05f, 0.72f, 30.0f, 48.0f, 0.28f, 0.42f, 38, 0.30f, 0.38f, 3, false, 0.03f};
  }
}

SprigLayout BuildSprigLayout(LeafSilhouette shape) {
  const SprigRecipe rc = MakeRecipe(shape);
  SprigLayout out;

  // Distinguishes species so twig/stamp jitter doesn't correlate across
  // silhouettes despite sharing the same index space.
  const uint32_t species_seed = 0x9E3779B9u * (static_cast<uint32_t>(shape) + 1u);

  // Main stem: a handful of short segments tracing StemPoint's curve.
  constexpr int kStemSegs = 20;
  for (int i = 0; i < kStemSegs; ++i) {
    const float t0 = static_cast<float>(i) / static_cast<float>(kStemSegs);
    const float t1 = static_cast<float>(i + 1) / static_cast<float>(kStemSegs);
    out.strokes.push_back({StemPoint(t0, rc.stem_curve_amp), StemPoint(t1, rc.stem_curve_amp),
                           glm::mix(kStemHalfW0, kStemHalfW1, t0),
                           glm::mix(kStemHalfW0, kStemHalfW1, t1), kTwigGray});
  }

  // Distribute placements across twigs as evenly as possible.
  const int base_per_twig = rc.placement_count / rc.twig_count;
  const int extra = rc.placement_count - base_per_twig * rc.twig_count;

  for (int i = 0; i < rc.twig_count; ++i) {
    const uint32_t tseed = species_seed ^ (static_cast<uint32_t>(i) * 0x01000193u);
    const float t_start = HashRange(tseed ^ 1u, rc.twig_t_lo, rc.twig_t_hi);
    const float side = (i % 2 == 0) ? 1.0f : -1.0f;
    const float angle_deg = side * HashRange(tseed ^ 2u, rc.twig_angle_lo, rc.twig_angle_hi);
    const float length = HashRange(tseed ^ 3u, rc.twig_len_lo, rc.twig_len_hi);

    const glm::vec2 p0 = StemPoint(t_start, rc.stem_curve_amp);
    const glm::vec2 stem_dir = StemTangent(t_start, rc.stem_curve_amp);
    const float stem_angle = std::atan2(stem_dir.x, stem_dir.y);  // angle from +v (up)
    const float twig_angle = stem_angle + glm::radians(angle_deg);
    const glm::vec2 dir(std::sin(twig_angle), std::cos(twig_angle));
    const glm::vec2 p1 = p0 + length * dir;
    const glm::vec2 perp(dir.y, -dir.x);

    out.strokes.push_back({p0, p1, kTwigHalfW0, kTwigHalfW1, kTwigGray});

    const int n = base_per_twig + (i < extra ? 1 : 0);
    for (int k = 0; k < n; ++k) {
      const uint32_t sseed = tseed ^ (static_cast<uint32_t>(k) * 0x85ebca6bu) ^ 0x1000u;
      const float f = (n > 1)
          ? (static_cast<float>(k) + HashRange(sseed ^ 4u, -0.15f, 0.15f)) /
                static_cast<float>(n - 1)
          : 0.6f;
      const float fc = std::clamp(0.2f + f * 0.8f, 0.0f, 1.0f);  // keep off the very base
      const glm::vec2 base_pos = p0 + fc * length * dir;
      // Bigger near the middle/base of the sprig, smaller toward the tip.
      const float size_bias = 1.0f - 0.5f * fc;
      const float scale =
          HashRange(sseed ^ 5u, rc.stamp_lo, rc.stamp_hi) * (0.7f + 0.3f * size_bias);
      const float jitter_perp = HashRange(sseed ^ 6u, -0.05f, 0.05f);
      const float rot_jitter = HashRange(sseed ^ 7u, -0.6f, 0.6f);  // radians

      if (rc.pinnate) {
        const float gap = scale * 0.55f;
        const glm::vec2 c0 = base_pos + perp * (gap + jitter_perp);
        const glm::vec2 c1 = base_pos - perp * (gap - jitter_perp);
        const float base_rot = std::atan2(perp.x, perp.y);
        const float g0 = HashRange(sseed ^ 8u, 0.80f, 1.0f);
        const float g1 = HashRange(sseed ^ 9u, 0.80f, 1.0f);
        out.stamps.push_back({c0, base_rot + rot_jitter, scale, g0});
        out.stamps.push_back({c1, base_rot + glm::pi<float>() + rot_jitter, scale, g1});
      } else {
        const glm::vec2 c = base_pos + perp * jitter_perp;
        const float gray = HashRange(sseed ^ 8u, 0.80f, 1.0f);
        out.stamps.push_back({c, twig_angle + rot_jitter, scale, gray});
      }
    }
  }

  // Terminal cluster capping the main stem's tip.
  {
    const glm::vec2 tip = StemPoint(1.0f, rc.stem_curve_amp);
    const glm::vec2 tdir = StemTangent(0.97f, rc.stem_curve_amp);
    const float tangle = std::atan2(tdir.x, tdir.y);
    for (int k = 0; k < rc.tip_stamps; ++k) {
      const uint32_t sseed = species_seed ^ 0x2000u ^ (static_cast<uint32_t>(k) * 0x27d4eb2fu);
      const float spread = HashRange(sseed ^ 1u, -0.5f, 0.5f);
      const float scale = HashRange(sseed ^ 2u, rc.stamp_lo, rc.stamp_hi) * 0.85f;
      const glm::vec2 c = tip - tdir * (0.02f + HashRange(sseed ^ 3u, 0.0f, 0.06f));
      const float gray = HashRange(sseed ^ 4u, 0.80f, 1.0f);
      out.stamps.push_back({c, tangle + spread, scale, gray});
    }
  }

  return out;
}

// Evaluates one species' single-leaf silhouette (the profiles above) at a
// stamp-local (u,t); shared by every stamp of that species regardless of
// where/how it's placed on the sprig.
float StampAlpha(LeafSilhouette shape, float u, float t, float edge) {
  switch (shape) {
    case LeafSilhouette::Oak: {
      float a = AlphaFromHalfWidth(u, LobedHalfWidth(t), edge);
      if (t < 0.1f) a = std::max(a, AlphaFromHalfWidth(u, 0.04f, edge));  // petiole stem bar
      return a;
    }
    case LeafSilhouette::Ash:
      return AlphaFromHalfWidth(u, LanceolateHalfWidth(t), edge);
    case LeafSilhouette::Aspen:
      return AlphaFromHalfWidth(u, CordateHalfWidth(t), edge);
    case LeafSilhouette::Bush:
    case LeafSilhouette::PineSprig:
    default:
      return AlphaFromHalfWidth(u, OvalHalfWidth(t), edge);
  }
}

// Rasterizes a tapered capsule stroke into alpha_buf/gray_buf, restricted to
// its own bounding box. Ties (equal alpha) go to whichever draw call runs
// later -- see the "top-most wins" note on PaintStamp below.
void PaintStroke(std::vector<float>& alpha_buf, std::vector<float>& gray_buf, int size,
                  const SprigStroke& s) {
  const float edge = 2.0f / static_cast<float>(size);
  const float maxw = std::max(s.half_w0, s.half_w1) + edge;
  const float umin = std::min(s.a.x, s.b.x) - maxw, umax = std::max(s.a.x, s.b.x) + maxw;
  const float vmin = std::min(s.a.y, s.b.y) - maxw, vmax = std::max(s.a.y, s.b.y) + maxw;
  const int x0 = std::clamp(static_cast<int>(std::floor((umin + 1.0f) * 0.5f * size)), 0, size - 1);
  const int x1 = std::clamp(static_cast<int>(std::ceil((umax + 1.0f) * 0.5f * size)), 0, size - 1);
  const int y0 = std::clamp(static_cast<int>(std::floor((vmin + 1.0f) * 0.5f * size)), 0, size - 1);
  const int y1 = std::clamp(static_cast<int>(std::ceil((vmax + 1.0f) * 0.5f * size)), 0, size - 1);

  const glm::vec2 ab = s.b - s.a;
  const float len2 = glm::dot(ab, ab);

  for (int y = y0; y <= y1; ++y) {
    for (int x = x0; x <= x1; ++x) {
      const float u = (static_cast<float>(x) + 0.5f) / size * 2.0f - 1.0f;
      const float v = (static_cast<float>(y) + 0.5f) / size * 2.0f - 1.0f;
      const glm::vec2 p(u, v);
      float proj = (len2 > 1e-8f) ? glm::dot(p - s.a, ab) / len2 : 0.0f;
      proj = std::clamp(proj, 0.0f, 1.0f);
      const glm::vec2 closest = s.a + proj * ab;
      const float dist = glm::length(p - closest);
      const float hw = glm::mix(s.half_w0, s.half_w1, proj);
      const float a = std::clamp((hw - dist) / edge + 0.5f, 0.0f, 1.0f);
      if (a <= 0.0f) continue;
      const size_t idx = static_cast<size_t>(y) * static_cast<size_t>(size) + static_cast<size_t>(x);
      if (a >= alpha_buf[idx]) { alpha_buf[idx] = a; gray_buf[idx] = s.gray; }
    }
  }
}

// Rasterizes one leaf stamp, restricted to its own (rotated) bounding box --
// keeps a 512x512, ~20-40 stamp sprig fast (bbox-per-stamp, not per-texel-
// over-all-stamps). Compositing rule: alpha is the running max over every
// draw call (stem/twigs, then stamps in a fixed order); on ties the LATEST
// draw's gray wins, so later-drawn leaf stamps read as sitting on top of
// earlier twigs/leaves wherever their silhouettes exactly coincide.
void PaintStamp(std::vector<float>& alpha_buf, std::vector<float>& gray_buf, int size,
                LeafSilhouette shape, const SprigStamp& st) {
  const float cos_t = std::cos(st.rotation), sin_t = std::sin(st.rotation);
  const float s = st.scale;
  const float edge = 2.0f / (static_cast<float>(size) * s);

  float umin = 1e9f, umax = -1e9f, vmin = 1e9f, vmax = -1e9f;
  for (float lx : {-1.0f, 1.0f}) {
    for (float ly : {-1.0f, 1.0f}) {
      const float u = st.center.x + s * (cos_t * lx - sin_t * ly);
      const float v = st.center.y + s * (sin_t * lx + cos_t * ly);
      umin = std::min(umin, u); umax = std::max(umax, u);
      vmin = std::min(vmin, v); vmax = std::max(vmax, v);
    }
  }
  const int x0 = std::clamp(static_cast<int>(std::floor((umin + 1.0f) * 0.5f * size)) - 1, 0, size - 1);
  const int x1 = std::clamp(static_cast<int>(std::ceil((umax + 1.0f) * 0.5f * size)) + 1, 0, size - 1);
  const int y0 = std::clamp(static_cast<int>(std::floor((vmin + 1.0f) * 0.5f * size)) - 1, 0, size - 1);
  const int y1 = std::clamp(static_cast<int>(std::ceil((vmax + 1.0f) * 0.5f * size)) + 1, 0, size - 1);

  for (int y = y0; y <= y1; ++y) {
    for (int x = x0; x <= x1; ++x) {
      const float u = (static_cast<float>(x) + 0.5f) / size * 2.0f - 1.0f;
      const float v = (static_cast<float>(y) + 0.5f) / size * 2.0f - 1.0f;
      const float dx = u - st.center.x, dy = v - st.center.y;
      const float lu = (cos_t * dx + sin_t * dy) / s;
      const float lv = (-sin_t * dx + cos_t * dy) / s;
      if (lu < -1.3f || lu > 1.3f) continue;
      const float t_raw = (lv + 1.0f) * 0.5f;
      if (t_raw < -0.05f || t_raw > 1.05f) continue;
      const float t = std::clamp(t_raw, 0.0f, 1.0f);
      const float a = StampAlpha(shape, lu, t, edge);
      if (a <= 0.0f) continue;
      const size_t idx = static_cast<size_t>(y) * static_cast<size_t>(size) + static_cast<size_t>(x);
      if (a >= alpha_buf[idx]) { alpha_buf[idx] = a; gray_buf[idx] = st.gray; }
    }
  }
}

}  // namespace

std::vector<uint8_t> BuildLeafRgba8(int size, glm::vec3 leaf_color, LeafSilhouette shape) {
  std::vector<uint8_t> rgba(static_cast<size_t>(size) * static_cast<size_t>(size) * 4, 0);
  auto to_byte = [](float c) {
    return static_cast<uint8_t>(std::lround(std::clamp(c, 0.0f, 1.0f) * 255.0f));
  };

  if (shape == LeafSilhouette::PineSprig) {
    // Unchanged from before the sprig rework -- DO NOT TOUCH (byte-for-byte
    // stable output).
    const uint8_t r = to_byte(leaf_color.r);
    const uint8_t g = to_byte(leaf_color.g);
    const uint8_t b = to_byte(leaf_color.b);
    const float edge = 2.0f / static_cast<float>(size);  // ~2-texel soft edge

    for (int y = 0; y < size; ++y) {
      for (int x = 0; x < size; ++x) {
        const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(size) * 2.0f - 1.0f;
        const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(size) * 2.0f - 1.0f;
        const float t = (v + 1.0f) * 0.5f;  // 0 = base, 1 = tip

        const float a = PineSprigAlpha(u, t, v, edge);

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

  // Sprig path: Oak/Ash/Aspen/Bush all render as a procedural branch sprig
  // (main stem + twigs + 20-40 leaf stamps), not a single leaf silhouette.
  const size_t texel_count = static_cast<size_t>(size) * static_cast<size_t>(size);
  std::vector<float> alpha_buf(texel_count, 0.0f);
  std::vector<float> gray_buf(texel_count, 1.0f);  // background: full leaf_color (avoids mip halos)

  const SprigLayout layout = BuildSprigLayout(shape);
  for (const SprigStroke& s : layout.strokes) PaintStroke(alpha_buf, gray_buf, size, s);
  for (const SprigStamp& st : layout.stamps) PaintStamp(alpha_buf, gray_buf, size, shape, st);

  for (size_t i = 0; i < texel_count; ++i) {
    const glm::vec3 c = leaf_color * gray_buf[i];
    rgba[i * 4 + 0] = to_byte(c.r);
    rgba[i * 4 + 1] = to_byte(c.g);
    rgba[i * 4 + 2] = to_byte(c.b);
    rgba[i * 4 + 3] = to_byte(alpha_buf[i]);
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
        // RGB reset to flat leaf_color; this also flattens per-stamp brightness variation below mip 0 (accepted: distance rendering out of scope).
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
