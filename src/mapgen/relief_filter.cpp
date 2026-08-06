#include "mapgen/relief_filter.hpp"

#include <algorithm>
#include <cmath>

#include "mapgen/biomes.hpp"
#include "mapgen/cubic_sample.hpp"
#include "mapgen/parallel.hpp"

namespace badlands::mapgen {

namespace {

// --- the fixed style ---------------------------------------------------
//
// Everything here is a constant in world METRES. The octave band sits below
// the coarse source's Nyquist (2 x 16 m): stage 1 owns every wavelength it
// can represent, this filter only ever adds what the coarse grid could not
// carry. Per-biome depth ratios express "gully depth as a fraction of gully
// wavelength" -- the one number that survives across octaves.
constexpr int kOctaves = 4;
constexpr float kWavelengthM[kOctaves] = {24.0f, 12.0f, 6.0f, 3.0f};
constexpr float kCellPerWavelength = 4.0f;  // orientation-cell edge, in lambdas

// depth_ratio by Biome value (Lake, Swamp, Forest, Plains, Hills, Mountain).
// Lake is masked by water anyway; Swamp/Forest do not exist procedurally yet
// and get conservative values rather than zero so a hand-authored patch
// still reads as ground.
constexpr float kDepthRatio[kBiomeCount] = {0.0f,   0.010f, 0.020f,
                                            0.015f, 0.035f, 0.055f};

// The physical fade. Slope gate in tan(angle): gullies live on slopes the
// stage-1 measurement separated (bare/thin soil 31.6 deg vs deep soil 7.1).
// Soil gate in metres of erodible cover: thin soil is dissected rock, deep
// soil smooths out. Water gate on the bilinear wet fraction: any standing
// water nearby kills the filter well before the shoreline texel itself.
constexpr float kSlopeGateLo = 0.10f;   // tan ~6 deg: detail starts
constexpr float kSlopeGateHi = 0.25f;   // tan ~14 deg: full strength
constexpr float kSoilGateLoM = 0.5f;    // below: full strength
constexpr float kSoilGateHiM = 3.0f;    // above: faded out
constexpr float kWetGateHi = 0.45f;     // wet fraction where strength hits 0

float smoothstep01(float e0, float e1, float x) {
  const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

// --- deterministic hashing ---------------------------------------------
//
// All randomness is a pure function of (seed, octave, cell) -- no state, no
// order dependence, so any two patches agree about any cell.
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

// --- point sampling of the coarse context rasters -----------------------
//
// EVERY gate input is sampled C1-SMOOTHLY (per-axis smoothstep weights over
// the 2x2 coarse cells, node registration, clamped edges). The gates
// multiply the whole delta, so any kink in them prints an axis-aligned
// crease along every coarse-cell border once hillshade differentiates the
// field -- plain bilinear did exactly that, and nearest-biome sampling was
// worse: a hard amplitude STEP at each biome boundary.

// The 2x2 smooth-blend stencil at world pos p over an n_w x n_h raster.
struct SmoothTaps {
  int x0, x1, y0, y1;
  float wx1, wy1;  // smoothstep weight of the +1 corner per axis
};

SmoothTaps smooth_taps(int n_w, int n_h, float texel_m, glm::dvec2 p) {
  const double sx = std::clamp(p.x / texel_m, 0.0,
                               static_cast<double>(n_w - 1));
  const double sy = std::clamp(p.y / texel_m, 0.0,
                               static_cast<double>(n_h - 1));
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

// The WET indicator (depth > 0), not depth itself: a 300 m-deep lake must
// not feather 40x further than a pond. At a wet node all four corners are 1,
// so the exact-zero mask contract survives the smoothing.
float wet_fraction(const Field2D<float>& depth, float texel_m, glm::dvec2 p) {
  const SmoothTaps t = smooth_taps(depth.width, depth.height, texel_m, p);
  return smooth_blend(
      t, [&](int x, int y) { return depth.at(x, y) > 0.0f ? 1.0f : 0.0f; });
}

// Per-biome style, interpolated in RATIO space: each coarse cell contributes
// its biome's depth ratio and the blend is C1, so a Mountain/Hills border
// grades over a cell instead of stepping 57% in amplitude.
float depth_ratio_at(const Field2D<uint8_t>& biome, float texel_m,
                     glm::dvec2 p) {
  const SmoothTaps t = smooth_taps(biome.width, biome.height, texel_m, p);
  return smooth_blend(t, [&](int x, int y) {
    return kDepthRatio[std::min<uint8_t>(biome.at(x, y), kBiomeCount - 1)];
  });
}

// --- the gully profile ---------------------------------------------------
//
// Periodic in t (wavelength units): raised-cosine squared concentrates the
// carve into a groove around integer t, minus its period mean (3/8) so the
// octave is zero-mean and detail never lowers a hillside wholesale.
struct Profile {
  float value;  // dimensionless, in [-3/8, 5/8]
  float ddt;    // d(value)/dt
};

Profile gully_profile(float t) {
  const float c = std::cos(2.0f * float(M_PI) * t);
  const float s = std::sin(2.0f * float(M_PI) * t);
  const float base = 0.5f + 0.5f * c;
  return {base * base - 0.375f,
          -2.0f * float(M_PI) * base * s};
}

// One octave's oriented-stripe field at p: the 2x2 surrounding orientation
// cells each carve their own stripe train (downhill direction frozen at the
// cell's hashed pivot), blended C1 across cell borders (see below).
struct OctaveSample {
  float value = 0.0f;   // dimensionless profile height
  glm::vec2 grad{0.0f}; // d(value)/d(world metres)
};

OctaveSample octave_stripes(const ReliefContext& ctx, int octave,
                            glm::dvec2 p) {
  const float lambda = kWavelengthM[octave];
  const float cell = kCellPerWavelength * lambda;
  const double gx = p.x / cell - 0.5, gy = p.y / cell - 0.5;
  const int cx0 = static_cast<int>(std::floor(gx));
  const int cy0 = static_cast<int>(std::floor(gy));

  // Per-axis SMOOTHSTEP blend between the two cells, not a linear hat: the
  // pair (1-s, s) still sums to 1, but the blend is C1 across cell borders.
  // A linear hat left visible axis-aligned creases at every cell edge of
  // every octave once hillshade (a derivative) got hold of the field.
  const float ux = static_cast<float>(gx - cx0);
  const float uy = static_cast<float>(gy - cy0);
  const float sx = ux * ux * (3.0f - 2.0f * ux);
  const float sy = uy * uy * (3.0f - 2.0f * uy);
  const float dsx = 6.0f * ux * (1.0f - ux) / cell;
  const float dsy = 6.0f * uy * (1.0f - uy) / cell;

  OctaveSample out;
  for (int j = 0; j < 2; ++j) {
    for (int i = 0; i < 2; ++i) {
      const int cxi = cx0 + i, cyi = cy0 + j;
      const uint32_t ucx = static_cast<uint32_t>(cxi);
      const uint32_t ucy = static_cast<uint32_t>(cyi);

      const float wx = i ? sx : 1.0f - sx;
      const float wy = j ? sy : 1.0f - sy;
      if (wx <= 0.0f || wy <= 0.0f) continue;
      const float dwx = i ? dsx : -dsx;
      const float dwy = j ? dsy : -dsy;

      // The cell's pivot, and the downhill direction frozen there. On flat
      // ground the direction is the pivot's hash -- stripes still form, they
      // just stop pretending to follow drainage.
      const glm::dvec2 pivot{
          (cxi + hash01(ctx.seed, octave, ucx, ucy * 3u + 1u)) * double(cell),
          (cyi + hash01(ctx.seed, octave, ucx, ucy * 3u + 2u)) * double(cell)};
      const CubicSample base = cubic_sample(*ctx.bed, ctx.src_texel_m, pivot);
      glm::vec2 down;
      const float g2 = base.grad.x * base.grad.x + base.grad.y * base.grad.y;
      if (g2 > 1e-8f) {
        down = base.grad / std::sqrt(g2);
      } else {
        const float a =
            2.0f * float(M_PI) * hash01(ctx.seed, octave, ucx, ucy * 3u);
        down = {std::cos(a), std::sin(a)};
      }
      const glm::vec2 across{-down.y, down.x};

      // Stripe phase varies ACROSS the flow, so grooves run along it.
      const float t = static_cast<float>(
          (static_cast<float>(p.x - pivot.x) * across.x +
           static_cast<float>(p.y - pivot.y) * across.y) /
          lambda);
      const Profile pr = gully_profile(t);

      const float w = wx * wy;
      out.value += w * pr.value;
      out.grad.x += dwx * wy * pr.value + w * pr.ddt * across.x / lambda;
      out.grad.y += wx * dwy * pr.value + w * pr.ddt * across.y / lambda;
    }
  }
  return out;
}

}  // namespace

ReliefSample sample_relief_delta(const ReliefContext& ctx,
                                 glm::dvec2 world_pos_m, float out_texel_m) {
  if (!ctx.bed || !ctx.soil || !ctx.biome || !ctx.water_depth ||
      ctx.src_texel_m <= 0.0f || out_texel_m <= 0.0f)
    return {};

  // Gates that do not depend on the octave stack. A zero here must yield an
  // EXACT zero delta -- the water-mask contract -- so bail before any float
  // dust can accumulate.
  const float wet = wet_fraction(*ctx.water_depth, ctx.src_texel_m,
                                 world_pos_m);
  const float water_gate = 1.0f - smoothstep01(0.0f, kWetGateHi, wet);
  if (water_gate <= 0.0f) return {};

  const float soil = smooth_point(*ctx.soil, ctx.src_texel_m, world_pos_m);
  const float soil_gate = 1.0f - smoothstep01(kSoilGateLoM, kSoilGateHiM, soil);
  if (soil_gate <= 0.0f) return {};

  const float depth_ratio =
      depth_ratio_at(*ctx.biome, ctx.src_texel_m, world_pos_m);
  if (depth_ratio <= 0.0f) return {};

  const CubicSample base =
      cubic_sample(*ctx.bed, ctx.src_texel_m, world_pos_m);

  ReliefSample out;
  glm::vec2 acc_grad = base.grad;  // stacked fade: each octave sees the pile
  for (int k = 0; k < kOctaves; ++k) {
    const float lambda = kWavelengthM[k];
    if (lambda < 2.0f * out_texel_m) break;  // below the output Nyquist

    const float slope = std::sqrt(acc_grad.x * acc_grad.x +
                                  acc_grad.y * acc_grad.y);
    const float slope_gate = smoothstep01(kSlopeGateLo, kSlopeGateHi, slope);
    const float strength = water_gate * soil_gate * slope_gate * depth_ratio;

    const OctaveSample oct = octave_stripes(ctx, k, world_pos_m);
    // Amplitude = ratio * wavelength; the carve is NEGATIVE profile (grooves
    // down, inter-groove ridges slightly up around the zero mean).
    const float amp = -strength * lambda;
    const glm::vec2 octave_grad{amp * oct.grad.x, amp * oct.grad.y};
    out.delta_m += amp * oct.value;
    // Fade-gate derivatives are deliberately not part of the gradient (v1):
    // the gates vary over coarse-cell scales, an order below the stripe
    // terms. The FD test pins the terms we do carry.
    out.grad += octave_grad;
    acc_grad += octave_grad;
  }
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
