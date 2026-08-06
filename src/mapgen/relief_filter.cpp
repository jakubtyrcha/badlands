// PROTOTYPE, WILL BE REPLACED. The stripe noise and the stacked-faded-gully
// erosion loop below are derived from Rune Skovbo Johansen's "Advanced
// Terrain Erosion Filter" and its Phacelle Noise function
// (https://www.shadertoy.com/view/wXcfWn, MPL-2.0,
//  https://blog.runevision.com/2026/03/fast-and-gorgeous-erosion-filter.html),
// adapted to this codebase's world-metre conventions, coarse-context gates
// and analytic-delta composition. This Source Code Form is subject to the
// terms of the Mozilla Public License, v. 2.0 (https://mozilla.org/MPL/2.0/).
#include "mapgen/relief_filter.hpp"

#include <algorithm>
#include <cmath>

#include "mapgen/biomes.hpp"
#include "mapgen/cubic_sample.hpp"
#include "mapgen/parallel.hpp"

namespace badlands::mapgen {

namespace {

constexpr float kTau = 6.28318530717959f;

// --- the fixed style ---------------------------------------------------
//
// Everything is a constant in world METRES. The octave band sits below the
// coarse source's Nyquist (2 x 16 m): stage 1 owns every wavelength it can
// represent, this filter only adds what the coarse grid could not carry.
constexpr int kOctaves = 5;
constexpr float kWavelengthM[kOctaves] = {24.0f, 12.0f, 6.0f, 3.0f, 1.5f};

// Erosion strength per biome, as gully depth per metre of wavelength -- the
// per-octave amplitude is ratio * wavelength (halving per octave, like the
// reference's gain 0.5 at lacunarity 2). Lake is masked by water anyway;
// Swamp/Forest do not exist procedurally yet and get conservative values.
constexpr float kDepthRatio[kBiomeCount] = {0.0f,   0.008f, 0.018f,
                                            0.012f, 0.030f, 0.040f};

// Reference-derived style constants (the Shadertoy demo's values, which are
// the article's tuned look).
constexpr float kGullyWeight = 0.5f;     // gully magnitude vs peak sharpening
constexpr float kDetail = 1.5f;          // how much the mask re-opens per octave
constexpr float kRidgeRounding = 0.1f;
constexpr float kCreaseRounding = 0.0f;
constexpr float kRoundingInputMult = 0.1f;
constexpr float kRoundingOctaveMult = 2.0f;
constexpr float kOnsetInput = 1.25f;     // mask onset vs the input slope
constexpr float kOnsetOctave = 1.25f;    // mask onset vs each octave's slope
constexpr float kAssumedSlope = 0.7f;    // trusted gully-direction magnitude
constexpr float kCellScale = 0.7f;       // stripe cell edge, in wavelengths
constexpr float kNormalization = 0.5f;   // phasor renormalization floor

// Below this input slope (tan) the mask is EXACTLY zero and stays zero
// through the whole octave stack -- near-flat ground gullies not at all,
// and the pure-resample fixtures/invariance tests keep their meaning.
constexpr float kSlopeFloor = 0.07f;  // tan ~4 deg

// The fade target comes from LOCAL altitude: bed minus a sampled local mean,
// over this half-band. (The reference normalizes global altitude; a 16 km
// world needs it local.)
constexpr float kTargetHalfBandM = 12.0f;
constexpr float kTargetMeanRadiusM = 48.0f;

// The physical gates, unchanged from v1: soil fades the whole effect (thin
// soil is dissected rock, deep soil smooths out), water is an exact-zero
// mask with a wet-fraction feather. Soil is additionally sampled through a
// widened footprint -- the stage-1 soil raster is noisy per-cell, and gating
// on it raw turns the strength into a blotch mask.
constexpr float kSoilGateLoM = 0.5f;
constexpr float kSoilGateHiM = 3.0f;
constexpr float kWetGateHi = 0.45f;
constexpr float kSoilWidenM = 12.0f;

float clamp01(float x) { return std::clamp(x, 0.0f, 1.0f); }

float smoothstep01(float e0, float e1, float x) {
  const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

// Flip, raise, flip back: re-opens a mask (t in [0,1]) for power < 1 stays,
// power > 1 opens.
float pow_inv(float t, float power) {
  return 1.0f - std::pow(1.0f - clamp01(t), power);
}

// Quadratic ease-out, saturating at 1.
float ease_out(float t) {
  const float v = 1.0f - clamp01(t);
  return 1.0f - v * v;
}

// Linear ramp whose start is rounded over [0, smoothing].
float smooth_start(float t, float smoothing) {
  if (t >= smoothing) return t - 0.5f * smoothing;
  if (smoothing <= 0.0f) return t;
  return 0.5f * t * t / smoothing;
}

// --- deterministic hashing ---------------------------------------------
uint32_t hash_u32(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
  uint32_t h = a * 0x9E3779B9u ^ b * 0x85EBCA6Bu ^ c * 0xC2B2AE35u ^
               d * 0x27D4EB2Fu;
  h ^= h >> 16;
  h *= 0x7FEB352Du;
  h ^= h >> 15;
  h *= 0x846CA68Bu;
  h ^= h >> 16;
  return h;
}

float hash01(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
  return static_cast<float>(hash_u32(a, b, c, d) & 0xFFFFFFu) /
         static_cast<float>(0x1000000);
}

// Per-cell jitter in [-0.5, 0.5]^2.
glm::vec2 cell_jitter(uint32_t seed, int octave, int cx, int cy) {
  const uint32_t ux = static_cast<uint32_t>(cx);
  const uint32_t uy = static_cast<uint32_t>(cy);
  return {hash01(seed, static_cast<uint32_t>(octave) * 2u, ux, uy) - 0.5f,
          hash01(seed, static_cast<uint32_t>(octave) * 2u + 1u, ux, uy) -
              0.5f};
}

// --- point sampling of the coarse context rasters (C1) -------------------
struct SmoothTaps {
  int x0, x1, y0, y1;
  float wx1, wy1;
};

SmoothTaps smooth_taps(int n_w, int n_h, float texel_m, glm::dvec2 p) {
  const double sx =
      std::clamp(p.x / texel_m, 0.0, static_cast<double>(n_w - 1));
  const double sy =
      std::clamp(p.y / texel_m, 0.0, static_cast<double>(n_h - 1));
  SmoothTaps t;
  t.x0 = static_cast<int>(sx);
  t.y0 = static_cast<int>(sy);
  t.x1 = std::min(t.x0 + 1, n_w - 1);
  t.y1 = std::min(t.y0 + 1, n_h - 1);
  const float fx = static_cast<float>(sx - t.x0);
  const float fy = static_cast<float>(sy - t.y0);
  t.wx1 = fx * fx * (3.0f - 2.0f * fx);
  t.wy1 = fy * fy * (3.0f - 2.0f * fy);
  return t;
}

template <typename At>
float smooth_blend(const SmoothTaps& t, At&& at) {
  const float a = at(t.x0, t.y0) * (1 - t.wx1) + at(t.x1, t.y0) * t.wx1;
  const float b = at(t.x0, t.y1) * (1 - t.wx1) + at(t.x1, t.y1) * t.wx1;
  return a * (1 - t.wy1) + b * t.wy1;
}

float smooth_point(const Field2D<float>& f, float texel_m, glm::dvec2 p) {
  const SmoothTaps t = smooth_taps(f.width, f.height, texel_m, p);
  return smooth_blend(t, [&](int x, int y) { return f.at(x, y); });
}

float wet_fraction(const Field2D<float>& depth, float texel_m, glm::dvec2 p) {
  const SmoothTaps t = smooth_taps(depth.width, depth.height, texel_m, p);
  return smooth_blend(
      t, [&](int x, int y) { return depth.at(x, y) > 0.0f ? 1.0f : 0.0f; });
}

float depth_ratio_at(const Field2D<uint8_t>& biome, float texel_m,
                     glm::dvec2 p) {
  const SmoothTaps t = smooth_taps(biome.width, biome.height, texel_m, p);
  return smooth_blend(t, [&](int x, int y) {
    return kDepthRatio[std::min<uint8_t>(biome.at(x, y), kBiomeCount - 1)];
  });
}

// Soil through a widened 5-tap footprint: the raster's per-cell noise must
// not print through the strength.
float soil_widened(const Field2D<float>& soil, float texel_m, glm::dvec2 p) {
  const double r = kSoilWidenM;
  return 0.2f * (smooth_point(soil, texel_m, p) +
                 smooth_point(soil, texel_m, {p.x + r, p.y + r}) +
                 smooth_point(soil, texel_m, {p.x - r, p.y + r}) +
                 smooth_point(soil, texel_m, {p.x + r, p.y - r}) +
                 smooth_point(soil, texel_m, {p.x - r, p.y - r}));
}

// --- oriented stripe noise (Phacelle port) -------------------------------
//
// cos/sin of one wave whose phase is anchored at jittered lattice points,
// blended as a PHASOR with a bell kernel over the 4x4 surrounding cells,
// then partially renormalized. Blending two unaligned waves yields another
// wave, so the field is smooth everywhere -- no cell borders exist at all.
struct StripeWave {
  float c = 0.0f;         // normalized cosine: the height profile, [-1, 1]
  float s = 0.0f;         // normalized sine: the wave-space derivative
  glm::vec2 wavevec{0.0f};  // world-space wave vector (1/m), points across
};

StripeWave stripe_wave(uint32_t seed, int octave, glm::dvec2 p,
                       glm::vec2 dir, float lambda) {
  const float cell_m = lambda * kCellScale;
  // Phase advances across the flow. sidedir is in CELL units: moving one
  // cell along it advances the phase by kCellScale cycles.
  const glm::vec2 side{-dir.y * kCellScale * kTau, dir.x * kCellScale * kTau};

  const glm::dvec2 q{p.x / cell_m, p.y / cell_m};
  const double qxi = std::floor(q.x), qyi = std::floor(q.y);
  const glm::vec2 qf{static_cast<float>(q.x - qxi),
                     static_cast<float>(q.y - qyi)};
  const int cx = static_cast<int>(qxi), cy = static_cast<int>(qyi);

  glm::vec2 phasor{0.0f};
  float wsum = 0.0f;
  for (int j = -1; j <= 2; ++j) {
    for (int i = -1; i <= 2; ++i) {
      const glm::vec2 jitter = cell_jitter(seed, octave, cx + i, cy + j);
      const glm::vec2 v{qf.x - i - jitter.x, qf.y - j - jitter.y};
      const float d2 = v.x * v.x + v.y * v.y;
      // Bell that reaches 0 at the farthest possible nearest-cell distance
      // (1.5 cells), so no cell pops in or out of support.
      const float w = std::max(0.0f, std::exp(-d2 * 2.0f) - 0.01111f);
      if (w <= 0.0f) continue;
      const float phase = v.x * side.x + v.y * side.y + 0.25f * kTau;
      phasor.x += std::cos(phase) * w;
      phasor.y += std::sin(phase) * w;
      wsum += w;
    }
  }
  if (wsum > 0.0f) phasor /= wsum;

  // Partial renormalization: full unit amplitude wherever the blend kept at
  // least (1 - normalization) of it; weaker cancellation zones scale up
  // proportionally rather than looping.
  const float mag = std::max(1.0f - kNormalization,
                             std::sqrt(phasor.x * phasor.x +
                                       phasor.y * phasor.y));
  StripeWave out;
  out.c = phasor.x / mag;
  out.s = phasor.y / mag;
  // side is per cell; the world-space rate of change is 1/cell_m of it.
  out.wavevec = side / cell_m;
  return out;
}

}  // namespace

ReliefSample sample_relief_delta(const ReliefContext& ctx,
                                 glm::dvec2 world_pos_m, float out_texel_m) {
  if (!ctx.bed || !ctx.soil || !ctx.biome || !ctx.water_depth ||
      ctx.src_texel_m <= 0.0f || out_texel_m <= 0.0f)
    return {};
  if (kWavelengthM[0] < 2.0f * out_texel_m) return {};  // nothing audible

  // Gates that multiply the WHOLE delta. A zero here must yield an exact
  // zero -- the water-mask contract -- so bail before accumulating.
  const float wet =
      wet_fraction(*ctx.water_depth, ctx.src_texel_m, world_pos_m);
  const float water_gate = 1.0f - smoothstep01(0.0f, kWetGateHi, wet);
  if (water_gate <= 0.0f) return {};

  const float soil = soil_widened(*ctx.soil, ctx.src_texel_m, world_pos_m);
  const float soil_gate = 1.0f - smoothstep01(kSoilGateLoM, kSoilGateHiM, soil);
  if (soil_gate <= 0.0f) return {};

  const float ratio =
      depth_ratio_at(*ctx.biome, ctx.src_texel_m, world_pos_m);
  if (ratio <= 0.0f) return {};

  const CubicSample base =
      cubic_sample(*ctx.bed, ctx.src_texel_m, world_pos_m);
  const float slope_len = std::sqrt(base.grad.x * base.grad.x +
                                    base.grad.y * base.grad.y);
  // Below the floor the mask is exactly zero and, because the octave update
  // multiplies into it, stays zero -- near-flat ground is untouched.
  if (slope_len < kSlopeFloor) return {};

  // Fade target from LOCAL altitude: the bed against a sampled local mean.
  const double r = kTargetMeanRadiusM;
  const float local_mean =
      0.25f *
      (cubic_sample(*ctx.bed, ctx.src_texel_m, {world_pos_m.x + r,
                                                world_pos_m.y}).value +
       cubic_sample(*ctx.bed, ctx.src_texel_m, {world_pos_m.x - r,
                                                world_pos_m.y}).value +
       cubic_sample(*ctx.bed, ctx.src_texel_m, {world_pos_m.x,
                                                world_pos_m.y + r}).value +
       cubic_sample(*ctx.bed, ctx.src_texel_m, {world_pos_m.x,
                                                world_pos_m.y - r}).value);
  const float target0 =
      std::clamp((base.value - local_mean) / kTargetHalfBandM, -1.0f, 1.0f);
  float target = target0;

  // Mask from the input slope, with fade-target-dependent rounding.
  const float rounding_in =
      (kCreaseRounding +
       (kRidgeRounding - kCreaseRounding) * clamp01(target0 + 0.5f)) *
      kRoundingInputMult;
  // The ramp starts AT the floor, so the exact-zero region below it meets
  // the mask continuously instead of along a visible contour seam.
  float allow = ease_out(smooth_start((slope_len - kSlopeFloor) * kOnsetInput,
                                      rounding_in * kOnsetInput));

  // Gully direction: the input slope's DIRECTION at an assumed magnitude --
  // the coarse slope magnitude is not trusted to shape the output.
  glm::vec2 gully_slope = base.grad * (kAssumedSlope / slope_len);

  float acc = 0.0f;       // sum of A_k * faded gully height
  float mag = 0.0f;       // sum of A_k
  glm::vec2 grad{0.0f};   // approximate: faded wave terms only
  float rounding_mult = kRoundingOctaveMult;

  for (int k = 0; k < kOctaves; ++k) {
    const float lambda = kWavelengthM[k];
    if (lambda < 2.0f * out_texel_m) break;  // below the output Nyquist
    const float amp = ratio * lambda;

    const float gl = std::sqrt(gully_slope.x * gully_slope.x +
                               gully_slope.y * gully_slope.y);
    const glm::vec2 dir = gl > 1e-10f ? gully_slope / gl : glm::vec2{1, 0};
    const StripeWave w =
        stripe_wave(ctx.seed, k, world_pos_m, dir, lambda);
    const float sloping = std::fabs(w.s);

    // Straight-gully slope accumulation for SUBSEQUENT octaves: the sign of
    // the sine fakes triangle-wave slopes of constant magnitude, unfaded --
    // finer gullies orient off the walls of coarser ones, which is where
    // branching comes from.
    const float sgn = w.s < 0.0f ? -1.0f : 1.0f;
    gully_slope -= w.wavevec * (sgn * amp * kGullyWeight);

    // Fade the gully toward the accumulated target; masked regions pull to
    // the target height with zero slope, preserving ridges and creases.
    const float faded = target + (w.c * kGullyWeight - target) * allow;
    acc += amp * faded;
    mag += amp;
    grad -= w.wavevec * (w.s * kGullyWeight * allow * amp);
    target = faded;

    // Rebuild the mask from this octave's raw slope so the ridges and
    // creases it carved protect themselves from the next octave; the detail
    // power re-opens it.
    const float rounding_oct =
        (kCreaseRounding +
         (kRidgeRounding - kCreaseRounding) * clamp01(w.c + 0.5f)) *
        rounding_mult;
    const float new_mask = ease_out(
        smooth_start(sloping * kOnsetOctave, rounding_oct * kOnsetOctave));
    allow = pow_inv(allow, kDetail) * new_mask;
    rounding_mult *= kRoundingOctaveMult;
  }

  // Subtract the target's DC term: a fully masked point accumulated exactly
  // mag * target0 and must come out exactly zero.
  const float gate = water_gate * soil_gate;
  ReliefSample out;
  out.delta_m = gate * (acc - mag * target0);
  out.grad = grad * gate;
  return out;
}

void apply_relief(const ReliefContext& ctx, glm::dvec2 origin_m,
                  float out_texel_m, Field2D<float>& height_inout) {
  if (height_inout.width <= 0 || height_inout.height <= 0) return;
  parallel_tiles(
      height_inout.width, height_inout.height, 64, [] { return 0; },
      [&](int&, int x0, int y0, int x1, int y1) {
        for (int y = y0; y < y1; ++y) {
          for (int x = x0; x < x1; ++x) {
            const glm::dvec2 p{origin_m.x + double(x) * out_texel_m,
                               origin_m.y + double(y) * out_texel_m};
            height_inout.at(x, y) +=
                sample_relief_delta(ctx, p, out_texel_m).delta_m;
          }
        }
      });
}

}  // namespace badlands::mapgen
