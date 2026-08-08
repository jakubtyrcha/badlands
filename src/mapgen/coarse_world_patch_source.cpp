#include "mapgen/coarse_world_patch_source.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <utility>
#include <vector>

#include "mapgen/biomes.hpp"
#include "mapgen/biome_cover.hpp"
#include "mapgen/cover.hpp"
#include "mapgen/cubic_sample.hpp"
#include "mapgen/patch_io.hpp"
#include "mapgen/relief_filter.hpp"
#include "mapgen/river_clip.hpp"
#include "mapgen/river_io.hpp"
#include "mapgen/river_prune.hpp"

namespace badlands::mapgen {

namespace {

// Shortest headwater branch worth drawing, same physical threshold
// file_patch_source.cpp used to apply via the now-deleted window_rivers.cpp.
// Leaf-only, so the network cannot be severed; river_prune.hpp applies it
// repeatedly, since removing one leaf exposes the next.
constexpr float kMinRiverBranchM = 32.0f;

// --- resampling --------------------------------------------------------
//
// One output texel's contributing source taps along ONE axis: an (index,
// weight) list, weights summing to 1.
using Tap = std::pair<int, double>;

// CATMULL-ROM (B=0, C=0.5), NOT LANCZOS-3 -- the default kernel, and its
// reasoning is ported verbatim from tools/protogen/window.cpp (measured
// there): Lanczos-3 does not reproduce a linear ramp -- its weights sum to 1
// only to 5.7e-3 and its first moment is off by 6.5e-2, a ~6 cm
// reconstruction error. A planar hillside IS a linear ramp, so that error is
// PERIODIC WITH THE SOURCE GRID -- the corduroy ripple visible in an
// upscaled hillshade, because hillshade is a DERIVATIVE and amplifies what
// looks negligible in the height field (Lanczos overshoots 1.93 m on 160 m
// of relief in window.cpp's measured window). Every cubic reproduces a
// linear ramp to machine epsilon; Catmull-Rom is the interpolating member of
// the Mitchell-Netravali family and roughly halves Lanczos's overshoot at
// the same support.
//
// The kernel itself (CatmullRom/CatmullRomDeriv/kCubicSupport) lives in
// mapgen/cubic_sample.hpp, shared with the relief filter's point sampler --
// two copies of the weights would let the raster and point paths drift.

// Builds one output texel's cubic taps directly from WORLD METRES, unlike
// tools/protogen/window.cpp's BuildTaps, which built one shared src_n/out_n
// table because it always resampled the WHOLE source into the whole output
// on an integral number of source cells (window.cpp's WindowCellsIntegral
// check exists only to keep that one table valid). A patch request's origin
// need not fall on a source-cell boundary and its extent need not be a whole
// number of source cells -- alignment is this provider's problem, not the
// caller's -- so taps are computed per output texel instead.
std::vector<Tap> CubicTaps(double origin_axis_m, float out_texel_m, int j,
                           float src_texel_m, int src_n) {
  // NODE registration, NOT pixel centres. Texel i IS world i*texel_m on both
  // sides -- patch_data.hpp states it for the output, river_graph.cpp emits
  // node coordinates into rivers.bin, river_carve.cpp rounds world->texel
  // against the same lattice, and synthetic_patch_source samples that way.
  //
  // An earlier revision used the pixel-centre convention (j+0.5 out, -0.5 in),
  // copied from window.cpp's BuildTaps. That is self-consistent but disagrees
  // with everything else here by 0.5*(out_texel_m - src_texel_m): 7.5 m at a
  // 16 m source and a 1 m patch, so every carved channel landed 7.5 m off its
  // valley while the heightfield itself still looked perfectly sensible. See
  // the ramp registration test.
  const double world_pos =
      origin_axis_m + static_cast<double>(j) * static_cast<double>(out_texel_m);
  const double u = world_pos / static_cast<double>(src_texel_m);
  const int base = static_cast<int>(std::floor(u)) - kCubicSupport + 1;
  std::vector<Tap> taps;
  taps.reserve(2 * kCubicSupport);
  double sum = 0.0;
  for (int c = 0; c < 2 * kCubicSupport; ++c) {
    const int i = base + c;
    const double w = CatmullRom(u - i);
    const int ci = std::clamp(i, 0, src_n - 1);
    taps.emplace_back(ci, w);
    sum += w;
  }
  // Border renormalisation: without it, taps clamped at the source edge lose
  // mass and the resample dims toward the boundary.
  if (sum != 0.0)
    for (Tap& t : taps) t.second /= sum;
  return taps;
}

// AREA-AVERAGE taps. DOWNSAMPLING must never run a reconstruction filter
// backwards: evaluating a kernel with negative lobes (Catmull-Rom included)
// at a spacing coarser than the source aliases. Every source cell overlapping
// the output texel's world-space footprint contributes in proportion to how
// much of it the footprint covers.
std::vector<Tap> BoxTaps(double origin_axis_m, float out_texel_m, int j,
                         float src_texel_m, int src_n) {
  // NODE registration, matching CubicTaps: output node j sits AT
  // origin + j*out_texel_m, so its averaging footprint is CENTRED there rather
  // than starting there. Source node i likewise owns [(i-0.5), (i+0.5)] cells,
  // not [i, i+1]. Both halves matter -- getting only one right reintroduces
  // half the shift the cubic path just lost.
  const double center =
      origin_axis_m + static_cast<double>(j) * static_cast<double>(out_texel_m);
  const double half = 0.5 * static_cast<double>(out_texel_m);
  const double lo_i = (center - half) / src_texel_m + 0.5;
  const double hi_i = (center + half) / src_texel_m + 0.5;
  const int i0 = static_cast<int>(std::floor(lo_i));
  const int i1 = static_cast<int>(std::ceil(hi_i)) - 1;
  std::vector<Tap> taps;
  double sum = 0.0;
  for (int i = i0; i <= i1; ++i) {
    const double overlap = std::min(hi_i, static_cast<double>(i + 1)) -
                           std::max(lo_i, static_cast<double>(i));
    if (overlap <= 0.0) continue;
    const int ci = std::clamp(i, 0, src_n - 1);
    // A footprint running off the source edge clamps several source indices
    // onto the same edge cell; merge rather than list it twice.
    bool merged = false;
    for (Tap& t : taps)
      if (t.first == ci) {
        t.second += overlap;
        merged = true;
        break;
      }
    if (!merged) taps.emplace_back(ci, overlap);
    sum += overlap;
  }
  if (sum != 0.0)
    for (Tap& t : taps) t.second /= sum;
  else if (!taps.empty())
    taps[0].second = 1.0;
  return taps;
}

enum class ResampleKind { Cubic, Box };

// THE one biome rule: wet is Lake, dry classifies by soil against the
// whole-world manifest cutoffs. Shared by Fetch (per patch texel) and the
// loader (the whole-world raster the relief filter styles from), so the two
// cannot drift when the rule grows.
Biome ClassifyBiome(float soil_m, bool wet, const CoarseManifest& man) {
  if (wet) return Biome::Lake;
  if (soil_m < man.soil_cut_mountain_m) return Biome::Mountain;
  if (soil_m < man.soil_cut_hills_m) return Biome::Hills;
  return Biome::Plains;
}

// Biome stays INTERNAL here: the relief filter's per-class erosion strengths
// are tuned against it, and the coarse world has no other classification. What
// leaves through the contract is Cover -- translated by mapgen/biome_cover.hpp,
// the one place the two vocabularies are allowed to meet.

std::vector<Tap> BuildAxisTaps(ResampleKind kind, double origin_axis_m,
                               float out_texel_m, int j, float src_texel_m,
                               int src_n) {
  return kind == ResampleKind::Cubic
             ? CubicTaps(origin_axis_m, out_texel_m, j, src_texel_m, src_n)
             : BoxTaps(origin_axis_m, out_texel_m, j, src_texel_m, src_n);
}

// Separable 2D resample of `src` into an n x n output whose texel (i, j) is
// centred at world (origin_m.x + (i+0.5)*out_texel_m, origin_m.y +
// (j+0.5)*out_texel_m). Same horizontal-then-vertical structure as
// tools/protogen/window.cpp's Resample(), generalized to per-axis,
// per-output-texel taps (window.cpp shared one table across both axes
// because it always resampled the whole square source at a uniform scale
// from origin 0; a patch request's origin can differ per axis).
Field2D<float> ResampleField(const Field2D<float>& src, float src_texel_m,
                             glm::dvec2 origin_m, float out_texel_m, int n,
                             ResampleKind kind) {
  std::vector<std::vector<Tap>> taps_x(n), taps_y(n);
  for (int j = 0; j < n; ++j) {
    taps_x[j] = BuildAxisTaps(kind, origin_m.x, out_texel_m, j, src_texel_m, src.width);
    taps_y[j] = BuildAxisTaps(kind, origin_m.y, out_texel_m, j, src_texel_m, src.height);
  }
  std::vector<double> mid(static_cast<size_t>(src.height) * n, 0.0);
  for (int y = 0; y < src.height; ++y) {
    for (int j = 0; j < n; ++j) {
      double a = 0.0;
      for (const Tap& t : taps_x[j]) a += t.second * src.at(t.first, y);
      mid[static_cast<size_t>(y) * n + j] = a;
    }
  }
  Field2D<float> out(n, n);
  for (int i = 0; i < n; ++i) {
    for (int j = 0; j < n; ++j) {
      double a = 0.0;
      for (const Tap& t : taps_y[i])
        a += t.second * mid[static_cast<size_t>(t.first) * n + j];
      out.at(j, i) = static_cast<float>(a);
    }
  }
  return out;
}

// Direct block copy: no kernel at all. Valid only when the request's texel
// size matches the source's AND the origin lands on a source-cell boundary --
// the common case of fetching at native density from an aligned origin. A
// ratio of 1 at a FRACTIONAL origin still needs ResampleField(Cubic), which
// reproduces this path's result exactly anyway (Catmull-Rom is interpolating:
// weight 1 on the exact sample, 0 on every other integer offset) -- this path
// exists purely to skip that arithmetic in the aligned case.
Field2D<float> CropPassThrough(const Field2D<float>& src, glm::dvec2 origin_m,
                               float texel_m, int n) {
  const int bx = static_cast<int>(std::lround(origin_m.x / texel_m));
  const int by = static_cast<int>(std::lround(origin_m.y / texel_m));
  Field2D<float> out(n, n);
  for (int j = 0; j < n; ++j) {
    const int sy = std::clamp(by + j, 0, src.height - 1);
    for (int i = 0; i < n; ++i) {
      const int sx = std::clamp(bx + i, 0, src.width - 1);
      out.at(i, j) = src.at(sx, sy);
    }
  }
  return out;
}

bool IsAligned(glm::dvec2 origin_m, float texel_m) {
  const double fx = origin_m.x / static_cast<double>(texel_m);
  const double fy = origin_m.y / static_cast<double>(texel_m);
  return std::abs(fx - std::round(fx)) < 1e-4 && std::abs(fy - std::round(fy)) < 1e-4;
}

// --- water: rebuild, never resample -------------------------------------
//
// Ported from tools/protogen/window.cpp's ReconstructWater (its comments
// carry the measurements cited below): resampling the DEPTH field directly
// rings across the shoreline's hard step -- measured -2.01 m with Lanczos-3.
// Instead the bed is resampled (above) and each lake's surface elevation is
// carried as the constant it physically is; depth at output resolution is
// then surface - bed, flood-filled outward from the source footprint but
// never more than kFloodMarginCells beyond it. The sim's water field is not
// hydrostatically consistent -- it ponds perched puddles -- and one measured
// case (a 19-cell pond at 425.62 m sitting over 2682 connected cells below
// it) drowned 66% of a window when the flood ran unbounded. The sim decides a
// lake's EXTENT; the resample may only refine its BOUNDARY.
constexpr int kFloodMarginCells = 2;

// 4-connected components over `mask` (row-major, w x h). Labels are 1-based;
// 0 is background. `count` receives the number of components.
std::vector<int32_t> LabelComponents(const std::vector<uint8_t>& mask, int w,
                                     int h, int& count) {
  std::vector<int32_t> lab(static_cast<size_t>(w) * h, 0);
  count = 0;
  std::deque<int> q;
  static const int dx[4] = {1, -1, 0, 0}, dy[4] = {0, 0, 1, -1};
  for (int s = 0; s < w * h; ++s) {
    if (!mask[s] || lab[s]) continue;
    const int32_t id = ++count;
    lab[s] = id;
    q.clear();
    q.push_back(s);
    while (!q.empty()) {
      const int c = q.front();
      q.pop_front();
      const int cx = c % w, cy = c / w;
      for (int k = 0; k < 4; ++k) {
        const int nx = cx + dx[k], ny = cy + dy[k];
        if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
        const int t = ny * w + nx;
        if (mask[t] && !lab[t]) {
          lab[t] = id;
          q.push_back(t);
        }
      }
    }
  }
  return lab;
}

Field2D<float> ReconstructWater(const Field2D<float>& bed_src,
                                const Field2D<float>& depth_src,
                                const Field2D<float>& bed_out,
                                glm::dvec2 origin_m, float src_texel_m,
                                float out_texel_m, int n) {
  const int sw = bed_src.width, sh = bed_src.height;
  std::vector<uint8_t> wet(static_cast<size_t>(sw) * sh);
  for (size_t i = 0; i < wet.size(); ++i) wet[i] = depth_src.data[i] > 0.0f ? 1 : 0;

  int lake_count = 0;
  const std::vector<int32_t> lab = LabelComponents(wet, sw, sh, lake_count);

  // Each lake's surface is an equipotential; average bed+depth over its
  // source cells (already exactly constant by construction, but averaged for
  // safety rather than trusting one arbitrary member).
  std::vector<double> surf_sum(static_cast<size_t>(lake_count) + 1, 0.0);
  std::vector<int> surf_n(static_cast<size_t>(lake_count) + 1, 0);
  for (int i = 0; i < sw * sh; ++i) {
    const int32_t id = lab[i];
    if (!id) continue;
    surf_sum[id] += static_cast<double>(bed_src.data[i]) + static_cast<double>(depth_src.data[i]);
    surf_n[id]++;
  }
  std::vector<double> surface(static_cast<size_t>(lake_count) + 1, 0.0);
  for (int id = 1; id <= lake_count; ++id)
    surface[id] = surf_n[id] ? surf_sum[id] / surf_n[id] : 0.0;

  // BOUNDED FLOOD: BFS distance (not radius) out to kFloodMarginCells from
  // each lake's own source footprint, so an irregular shoreline is hugged
  // rather than circled.
  std::vector<int32_t> allow(static_cast<size_t>(sw) * sh, 0);
  std::vector<int> mdist(static_cast<size_t>(sw) * sh, INT32_MAX);
  {
    std::deque<int> q;
    static const int dx[4] = {1, -1, 0, 0}, dy[4] = {0, 0, 1, -1};
    for (int i = 0; i < sw * sh; ++i)
      if (lab[i]) {
        allow[i] = lab[i];
        mdist[i] = 0;
        q.push_back(i);
      }
    while (!q.empty()) {
      const int c = q.front();
      q.pop_front();
      if (mdist[c] >= kFloodMarginCells) continue;
      const int cx = c % sw, cy = c / sw;
      for (int k = 0; k < 4; ++k) {
        const int nx = cx + dx[k], ny = cy + dy[k];
        if (nx < 0 || ny < 0 || nx >= sw || ny >= sh) continue;
        const int t = ny * sw + nx;
        if (mdist[t] <= mdist[c] + 1) continue;
        mdist[t] = mdist[c] + 1;
        allow[t] = allow[c];
        q.push_back(t);
      }
    }
  }

  // Maps an OUTPUT texel to the SOURCE cell nominally beneath it -- in NODE
  // registration, like everything else here: output texel i IS world
  // origin + i*out_texel_m, and source node s owns [(s-0.5), (s+0.5)] cells.
  // An earlier revision used pixel centres ((i+0.5)*out, floor(wx/src)),
  // which displaced every lake seed and flood cap ~0.5*src_texel_m to one
  // side -- the same half-texel disagreement CubicTaps' comment documents.
  const auto source_cell = [&](int i, int j) {
    const double wx = origin_m.x + static_cast<double>(i) * out_texel_m;
    const double wy = origin_m.y + static_cast<double>(j) * out_texel_m;
    const int sx = std::clamp(
        static_cast<int>(std::floor(wx / src_texel_m + 0.5)), 0, sw - 1);
    const int sy = std::clamp(
        static_cast<int>(std::floor(wy / src_texel_m + 0.5)), 0, sh - 1);
    return sy * sw + sx;
  };

  Field2D<float> depth(n, n, 0.0f);
  std::vector<int32_t> owner(static_cast<size_t>(n) * n, 0);
  std::deque<int> q;
  for (int j = 0; j < n; ++j) {
    for (int i = 0; i < n; ++i) {
      const int32_t id = lab[source_cell(i, j)];
      if (!id) continue;
      const int t = j * n + i;
      // A seed texel can sit above its own lake surface once the bed is
      // resampled (the source cell was wet on average, this texel is a
      // shoal). Leave it dry; it simply does not propagate.
      if (static_cast<double>(bed_out.at(i, j)) >= surface[id]) continue;
      owner[t] = id;
      q.push_back(t);
    }
  }
  static const int dx[4] = {1, -1, 0, 0}, dy[4] = {0, 0, 1, -1};
  while (!q.empty()) {
    const int c = q.front();
    q.pop_front();
    const int cx = c % n, cy = c / n;
    const int32_t id = owner[c];
    const double surf = surface[id];
    for (int k = 0; k < 4; ++k) {
      const int nx = cx + dx[k], ny = cy + dy[k];
      if (nx < 0 || ny < 0 || nx >= n || ny >= n) continue;
      const int t = ny * n + nx;
      if (owner[t]) continue;
      if (static_cast<double>(bed_out.at(nx, ny)) >= surf) continue;
      // An output texel may only ever be claimed by the lake whose margin
      // covers it -- two lakes resampled close together must not bleed into
      // one another.
      if (allow[source_cell(nx, ny)] != id) continue;
      owner[t] = id;
      q.push_back(t);
    }
  }
  for (int t = 0; t < n * n; ++t) {
    if (!owner[t]) continue;
    const double d = surface[owner[t]] - static_cast<double>(bed_out.data[t]);
    if (d > 0.0) depth.data[t] = static_cast<float>(d);
  }
  return depth;
}

// --- raster I/O ----------------------------------------------------------

// Headerless float32 raster, same size-check discipline as patch_io.cpp's
// read_raster (duplicated rather than shared -- that one is private to its
// own TU, and this file's on-disk form is protogen's, not patch_io's).
bool ReadRaster(const std::string& path, size_t count, std::vector<float>& out,
                std::string* error) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) {
    if (error) *error = "cannot open " + path;
    return false;
  }
  const std::streamsize bytes = f.tellg();
  const size_t want = count * sizeof(float);
  if (bytes < 0 || static_cast<size_t>(bytes) != want) {
    if (error) {
      std::ostringstream os;
      os << path << " is " << bytes << " bytes, expected " << want << " ("
         << count << " x " << sizeof(float) << ") -- does it match world.txt?";
      *error = os.str();
    }
    return false;
  }
  out.resize(count);
  f.seekg(0);
  if (!f.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(want))) {
    if (error) *error = "short read on " + path;
    return false;
  }
  return true;
}

// Picks the lexicographically LAST "*-height.f32" in `dir` and strips the
// suffix to recover the tag. Tags are zero-padded step counts ("0060-step",
// see tools/protogen/protogen.cpp's Dump/snprintf("%04d-step", step)), so
// lexicographic order is numeric order and the last one names the final step.
std::string FindLatestTag(const std::string& dir, std::string* error) {
  constexpr const char* kSuffix = "-height.f32";
  const size_t suffix_len = std::char_traits<char>::length(kSuffix);
  std::string best;
  std::error_code ec;
  std::filesystem::directory_iterator it(dir, ec);
  if (ec) {
    if (error) *error = "cannot list " + dir + ": " + ec.message();
    return {};
  }
  for (const auto& entry : it) {
    if (!entry.is_regular_file()) continue;
    const std::string name = entry.path().filename().string();
    if (name.size() <= suffix_len) continue;
    if (name.compare(name.size() - suffix_len, suffix_len, kSuffix) != 0) continue;
    const std::string tag = name.substr(0, name.size() - suffix_len);
    // Compare the leading STEP NUMBER numerically, not the tag as a string.
    // protogen pads to four digits ("%04d-step"), so a run past 9999 steps
    // makes "10000-step" sort BELOW "9750-step" and this would silently select
    // an earlier snapshot -- a wrong render with no diagnostic. Tags with no
    // leading number fall back to lexicographic order among themselves.
    auto step_of = [](const std::string& t) -> long long {
      size_t k = 0;
      while (k < t.size() && t[k] >= '0' && t[k] <= '9') ++k;
      if (k == 0) return -1;
      return std::stoll(t.substr(0, k));
    };
    if (best.empty()) {
      best = tag;
      continue;
    }
    const long long a = step_of(tag), b = step_of(best);
    if (a != b ? a > b : tag > best) best = tag;
  }
  if (best.empty() && error) *error = dir + ": no *-height.f32 snapshot found";
  return best;
}

}  // namespace

PatchData CoarseWorldPatchSource::Fetch(const PatchRequest& req) const {
  PatchData out;
  const int n = req.resolution;
  if (n <= 0 || !(req.world_size_m > 0.0f) || !(manifest_.texel_m > 0.0f))
    return out;

  const float out_texel_m = patch_texel_m(req);
  out.texel_m = out_texel_m;
  out.origin_m = req.origin_m;

  const float src_texel_m = manifest_.texel_m;
  const double ratio = static_cast<double>(out_texel_m) / static_cast<double>(src_texel_m);

  Field2D<float> bed, soil;
  if (std::abs(ratio - 1.0) < 1e-6 && IsAligned(req.origin_m, src_texel_m)) {
    // RATIO == 1, ALIGNED: pass through -- no kernel needed at all.
    bed = CropPassThrough(height_, req.origin_m, src_texel_m, n);
    soil = CropPassThrough(soil_, req.origin_m, src_texel_m, n);
  } else if (ratio > 1.0) {
    // DOWNSAMPLE: the request is coarser than the source (one output texel
    // spans multiple source cells) -- area-average, never a reconstruction
    // filter, which would alias run backwards.
    bed = ResampleField(height_, src_texel_m, req.origin_m, out_texel_m, n, ResampleKind::Box);
    soil = ResampleField(soil_, src_texel_m, req.origin_m, out_texel_m, n, ResampleKind::Box);
  } else {
    // UPSAMPLE: the common case -- a fine patch cut out of a coarse world.
    bed = ResampleField(height_, src_texel_m, req.origin_m, out_texel_m, n, ResampleKind::Cubic);
    soil = ResampleField(soil_, src_texel_m, req.origin_m, out_texel_m, n, ResampleKind::Cubic);
  }
  out.height = std::move(bed);
  out.soil = std::move(soil);

  // --- relief detail (2026-08-06 spec): step 2 of the §3.4 chain. The
  // filter's delta rides on the resampled bed BEFORE the water rebuild, so
  // lake margins and levels see the detailed ground; the filter itself is
  // masked off standing water, so a lake bed is never touched. At or below
  // source density the octave band sits under the output Nyquist and the
  // delta is exactly zero -- Box/Crop requests stay bit-identical.
  const ReliefContext relief_ctx{&height_,       &soil_,      &biome_,
                                 &water_depth_,  src_texel_m, manifest_.seed};
  apply_relief(relief_ctx, req.origin_m, out_texel_m, out.height);

  // --- water -----------------------------------------------------------
  const Field2D<float> depth =
      ReconstructWater(height_, water_depth_, out.height, req.origin_m, src_texel_m, out_texel_m, n);
  out.level = Field2D<float>(n, n);
  for (int i = 0; i < n * n; ++i)
    out.level.data[i] =
        depth.data[i] > 0.0f ? out.height.data[i] + depth.data[i] : out.height.data[i];
  // Re-derive water_depth/lake_id/lakes canonically off height+level
  // (patch_io.hpp's derive_water) rather than hand-rolling a second
  // computation that could disagree -- `depth` above already IS level -
  // height by construction, so this is a relabel over the output grid, not a
  // second truth.
  derive_water(out.height, out.level, out_texel_m, out.water_depth, out.lake_id, out.lakes);

  // --- cover, via the biome rule, from the WHOLE-WORLD manifest cutoffs and
  // never a per-patch quantile -- see coarse_io.hpp on why the cutoffs live on
  // the manifest.
  out.cover = Field2D<uint8_t>(n, n);
  for (int i = 0; i < n * n; ++i)
    out.cover.data[i] = static_cast<uint8_t>(CoverForBiome(ClassifyBiome(
        out.soil.data[i], out.water_depth.data[i] > 0.0f, manifest_)));

  // The coarse world carries no morphology label; it is a substrate simulation,
  // not a survey of anywhere in particular.
  out.terrain_class = TerrainClass::Unknown;

  // --- rivers: clip to the request rect, THEN rebase to patch-local, THEN
  // cull -- order matters (see river_clip.hpp / river_prune.hpp).
  RiverGraph rg = rivers_;
  const glm::vec2 lo(static_cast<float>(req.origin_m.x), static_cast<float>(req.origin_m.y));
  const glm::vec2 hi(static_cast<float>(req.origin_m.x + req.world_size_m),
                     static_cast<float>(req.origin_m.y + req.world_size_m));
  clip_river_graph_to_rect(rg, lo, hi);
  // Texel (0, 0) sits at world (0, 0) in a patch (patch_data.hpp) -- rebase
  // by subtracting the request's origin from every node and edge point.
  for (RiverNode& node : rg.nodes) node.pos_m -= lo;
  for (RiverEdge& edge : rg.edges)
    for (glm::vec2& p : edge.points_m) p -= lo;
  prune_river_graph_by_length(rg, kMinRiverBranchM);
  out.rivers = std::move(rg);

  out.elevation_range = compute_elevation_range(out.height);
  return out;
}

std::unique_ptr<CoarseWorldPatchSource> LoadCoarseWorldPatchSource(
    const std::string& dir, const std::string& tag, std::string* error) {
  const std::optional<CoarseManifest> man = load_coarse_manifest(dir, error);
  if (!man) return nullptr;
  // The manifest tolerates absent keys (forward compat), but THIS consumer
  // classifies biomes against the soil cutoffs -- a manifest without them
  // would silently style every dry cell Plains. Reject it loudly instead.
  if (!(man->soil_cut_mountain_m > 0.0f) || !(man->soil_cut_hills_m > 0.0f)) {
    if (error)
      *error = dir + "/world.txt: missing or non-positive soil cutoffs "
                     "(soil_cut_mountain_m/soil_cut_hills_m) -- biome "
                     "classification needs them";
    return nullptr;
  }

  std::string use_tag = tag;
  if (use_tag.empty()) {
    use_tag = FindLatestTag(dir, error);
    if (use_tag.empty()) return nullptr;
  }

  const int n = man->resolution;
  const size_t count = static_cast<size_t>(n) * n;
  const std::string base = dir + "/" + use_tag + "-";

  std::vector<float> height, water, soil;
  if (!ReadRaster(base + "height.f32", count, height, error)) return nullptr;
  if (!ReadRaster(base + "water.f32", count, water, error)) return nullptr;
  if (!ReadRaster(base + "soil.f32", count, soil, error)) return nullptr;

  for (size_t i = 0; i < count; ++i) {
    if (!std::isfinite(height[i]) || !std::isfinite(water[i]) || !std::isfinite(soil[i])) {
      if (error) *error = dir + "/" + use_tag + ": non-finite sample";
      return nullptr;
    }
  }

  RiverGraph rivers;
  const std::string rivers_path = dir + "/rivers.bin";
  if (std::filesystem::exists(rivers_path)) {
    std::optional<RiverGraph> loaded = read_river_graph(rivers_path, error);
    if (!loaded) return nullptr;
    rivers = std::move(*loaded);
  }

  auto src = std::unique_ptr<CoarseWorldPatchSource>(new CoarseWorldPatchSource());
  src->manifest_ = *man;
  src->height_ = Field2D<float>(n, n);
  src->height_.data = std::move(height);
  src->water_depth_ = Field2D<float>(n, n);
  src->water_depth_.data = std::move(water);
  src->soil_ = Field2D<float>(n, n);
  src->soil_.data = std::move(soil);
  src->biome_ = Field2D<uint8_t>(n, n);
  for (size_t i = 0; i < count; ++i)
    src->biome_.data[i] = static_cast<uint8_t>(ClassifyBiome(
        src->soil_.data[i], src->water_depth_.data[i] > 0.0f, *man));
  src->rivers_ = std::move(rivers);
  return src;
}

}  // namespace badlands::mapgen
