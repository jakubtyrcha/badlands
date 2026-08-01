// THROWAWAY PROTOTYPE -- phase 2a: pick a gameplay window out of a protogen
// world and resample it to the gameplay grid.
//
// Reads the .f32 rasters protogen dumped (height/water/Q/discharge), scans every
// candidate window, keeps the ones that satisfy all four gameplay criteria, and
// emits the best as a bed heightmap + water depth at the fine grid.
//
// THE UPSCALE INVENTS NOTHING. At the default 16 m source cell and 0.5 m output
// texel this is a 32x resample of a 64x64 patch: the output is a smooth
// interpolant with no detail below 16 m, by construction. It is the LOW-FREQUENCY
// BASE for a later detail pass, not finished terrain.
//
// KERNEL DEFAULTS TO CATMULL-ROM, NOT LANCZOS-3. Hillshade is a DERIVATIVE, so
// ringing that looks negligible in the height field (Lanczos overshot the source
// range by 1.93 m on 160 m of relief) is plainly visible as periodic ripple in the
// shading -- and normals are what the terrain renderer consumes. Catmull-Rom
// roughly halves the overshoot at the same support. --kernel-compare prints the
// trade on the selected window; --kernel bspline is the zero-ringing option
// (all weights positive) at the cost of smoothing.
//
// THE BED KEEPS THE DEN. `height` is the lake BED, never the water surface, so a
// basin stays a basin through the resample. Water rides as its own depth field.
//
// WATER IS NOT RESAMPLED. Depth has a hard shoreline step and a ringing kernel on
// it goes NEGATIVE (measured with Lanczos-3: -2.01 m). Instead each lake's surface elevation is
// carried as the constant it physically is and depth is re-derived against the
// resampled bed, so the shoreline lands on the interpolated bed contour rather
// than on a 32-texel staircase.
//
// build (standalone, no CMake, no deps beyond the stdlib):
//   c++ -O3 -std=c++20 window.cpp -o protogen_window
//
// use:
//   protogen_window --in <dump-dir> --tag 3000-step --res 1024 --world 16384 \
//                   --out <window-dir>
//   protogen_window --test

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <string>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;

// Resampling kernels. See the weight functions further down for what each is;
// declared here because WinParams carries the choice.
enum class Kernel { Lanczos3, CatmullRom, BSpline, Mitchell, Bilinear };

const char* KernelName(Kernel k) {
  switch (k) {
    case Kernel::Lanczos3: return "lanczos3";
    case Kernel::CatmullRom: return "catmull";
    case Kernel::BSpline: return "bspline";
    case Kernel::Mitchell: return "mitchell";
    case Kernel::Bilinear: return "bilinear";
  }
  return "?";
}

bool ParseKernel(const std::string& s, Kernel& out) {
  if (s == "lanczos3" || s == "lanczos") { out = Kernel::Lanczos3; return true; }
  if (s == "catmull" || s == "catmull-rom") { out = Kernel::CatmullRom; return true; }
  if (s == "bspline") { out = Kernel::BSpline; return true; }
  if (s == "mitchell") { out = Kernel::Mitchell; return true; }
  if (s == "bilinear") { out = Kernel::Bilinear; return true; }
  return false;
}

// ---------------------------------------------------------------- parameters

struct WinParams {
  // source world
  std::string in;
  std::string tag = "3000-step";
  int src_res = 1024;
  float src_world_m = 16384.0f;

  // output window. window_m is DERIVED (out_res * out_texel_m) so the two can
  // never disagree -- 2048 x 0.5 m is 1024 m, which is exactly 64 source cells
  // at 16 m. A non-integer cell count is rejected rather than rounded.
  int out_res = 2048;
  float out_texel_m = 0.5f;

  int stride = 4;  // window scan stride, source cells

  // criteria. Defaults measured against the M16b world: 464 of 58081 windows
  // pass all four, so the gates bind without being unsatisfiable.
  float wet_depth_m = 0.5f;        // counts as standing water
  int min_lake_cells = 16;         // ~0.4 ha at 16 m cells
  float channel_q_m3s = 0.02f;     // counts as a channel
  int min_channel_cells = 40;
  float min_relief_m = 150.0f;     // "mountains" = local relief
  float plain_slope_deg = 5.0f;
  float min_plain_frac = 0.5f;
  float centre_frac = 0.5f;        // central 512 m of a 1024 m window
  float max_lake_depth_m = 0.0f;   // 0 = off; guard on the known-bad bathymetry
  // Must match the run that produced the dump, or inflow discharge and the
  // window's own rain will be on different scales.
  float runoff_m_per_yr = 1.0f;

  // Bed resampling kernel. Catmull-Rom over Lanczos-3 because hillshade is a
  // DERIVATIVE and amplifies ringing far beyond what the height overshoot
  // suggests; measured on the real window, Catmull-Rom roughly halves the
  // overshoot for the same sharpness. --kernel-compare prints the trade.
  Kernel kernel = Kernel::CatmullRom;

  // WHICH WINDOW TO EMIT. Prefer --origin-cell: `rank` is an ordinal into a list
  // that RESHUFFLES whenever the source map or any gate changes, so the same
  // --rank silently points at a different place after a re-run. Ranking is for
  // DISCOVERY; once a location is chosen, pin it by coordinates.
  int rank = 0;
  int origin_x = -1, origin_y = -1;  // >= 0 => emit exactly this window
  int report = 10;
  bool kernel_compare = false;
  std::string out = "window_out";
};

// ------------------------------------------------------------------- fields

struct Field {
  int n = 0;
  std::vector<float> v;
  Field() = default;
  Field(int n_, float fill = 0.f) : n(n_), v(size_t(n_) * n_, fill) {}
  float& at(int x, int y) { return v[size_t(y) * n + x]; }
  float at(int x, int y) const { return v[size_t(y) * n + x]; }
};

bool LoadField(const std::string& path, int n, Field& out) {
  FILE* fp = std::fopen(path.c_str(), "rb");
  if (!fp) {
    std::fprintf(stderr, "window: cannot open %s\n", path.c_str());
    return false;
  }
  out = Field(n);
  const size_t want = size_t(n) * n;
  const size_t got = std::fread(out.v.data(), sizeof(float), want, fp);
  std::fclose(fp);
  if (got != want) {
    std::fprintf(stderr, "window: %s is %zu floats, expected %zu (wrong --res?)\n",
                 path.c_str(), got, want);
    return false;
  }
  return true;
}

bool WriteField(const std::string& path, const Field& f) {
  FILE* fp = std::fopen(path.c_str(), "wb");
  if (!fp) {
    std::fprintf(stderr, "window: cannot write %s\n", path.c_str());
    return false;
  }
  std::fwrite(f.v.data(), sizeof(float), f.v.size(), fp);
  std::fclose(fp);
  return true;
}

// Slope magnitude in DEGREES, central differences inside and one-sided on the
// border -- matching numpy.gradient, so the C++ scan and the Python analysis
// agree cell for cell.
Field SlopeDeg(const Field& h, float cell_m) {
  const int n = h.n;
  Field s(n);
  for (int y = 0; y < n; ++y) {
    for (int x = 0; x < n; ++x) {
      const int x0 = std::max(0, x - 1), x1 = std::min(n - 1, x + 1);
      const int y0 = std::max(0, y - 1), y1 = std::min(n - 1, y + 1);
      const float gx = (h.at(x1, y) - h.at(x0, y)) / (float(x1 - x0) * cell_m);
      const float gy = (h.at(x, y1) - h.at(x, y0)) / (float(y1 - y0) * cell_m);
      s.at(x, y) = float(std::atan(std::hypot(gx, gy)) * 180.0 / kPi);
    }
  }
  return s;
}

// 4-connected components over `mask`. Labels are 1-based; 0 is background.
std::vector<int32_t> Label(const std::vector<uint8_t>& mask, int n,
                           std::vector<int>& sizes) {
  std::vector<int32_t> lab(size_t(n) * n, 0);
  sizes.clear();
  std::deque<int> q;
  for (int s = 0; s < n * n; ++s) {
    if (!mask[s] || lab[s]) continue;
    const int32_t id = int32_t(sizes.size()) + 1;
    lab[s] = id;
    q.clear();
    q.push_back(s);
    int count = 0;
    while (!q.empty()) {
      const int c = q.front();
      q.pop_front();
      ++count;
      const int cx = c % n, cy = c / n;
      const int dx[4] = {1, -1, 0, 0}, dy[4] = {0, 0, 1, -1};
      for (int k = 0; k < 4; ++k) {
        const int nx = cx + dx[k], ny = cy + dy[k];
        if (nx < 0 || ny < 0 || nx >= n || ny >= n) continue;
        const int t = ny * n + nx;
        if (mask[t] && !lab[t]) {
          lab[t] = id;
          q.push_back(t);
        }
      }
    }
    sizes.push_back(count);
  }
  return lab;
}

// -------------------------------------------------------------- summed area

// Inclusive-exclusive summed-area table so a window sum is 4 lookups. Sized
// (n+1)^2 with a zero first row/column.
struct Sat {
  int n = 0;
  std::vector<double> s;
  explicit Sat(const std::vector<uint8_t>& a, int n_) : n(n_), s(size_t(n_ + 1) * (n_ + 1), 0.0) {
    for (int y = 0; y < n; ++y) {
      double row = 0.0;
      for (int x = 0; x < n; ++x) {
        row += a[size_t(y) * n + x] ? 1.0 : 0.0;
        s[size_t(y + 1) * (n + 1) + (x + 1)] = s[size_t(y) * (n + 1) + (x + 1)] + row;
      }
    }
  }
  double Box(int x, int y, int k) const {
    const size_t w = size_t(n) + 1;
    return s[(y + k) * w + (x + k)] - s[size_t(y) * w + (x + k)] -
           s[(y + k) * w + size_t(x)] + s[size_t(y) * w + size_t(x)];
  }
};

// ---------------------------------------------------------------- candidates

struct Candidate {
  int cx = 0, cy = 0;  // window origin, source cells
  float relief_m = 0.f;
  float plain_frac = 0.f;   // of the CENTRE box, dry and gentle
  int lake_cells = 0;       // largest connected lake inside the window
  int channel_cells = 0;
  float max_depth_m = 0.f;
  float max_q_m3s = 0.f;
  float steep_frac = 0.f;
};

// Every window passing all four gates, ranked by plain fraction -- the scarcest
// resource in this world (map-wide median slope is 10.5 deg) and the one the
// gameplay area actually needs.
// Scores every window on the stride grid, or -- when `only_x/only_y` are >= 0 --
// exactly that one origin, ungated.
//
// A PINNED window must be scored HERE and not looked up among the strided
// candidates: the scan only visits origins that are multiples of `stride`, so a
// pinned origin off that grid is absent from the list even when it passes every
// gate. Reporting that absence as "does not pass the gates" was exactly wrong.
std::vector<Candidate> ScanImpl(const WinParams& p, const Field& h, const Field& w,
                                const Field& q, int win_cells, int only_x,
                                int only_y, bool* gates_pass) {
  const int n = h.n;
  const float cell_m = p.src_world_m / float(p.src_res);
  const Field slope = SlopeDeg(h, cell_m);

  std::vector<uint8_t> wet(size_t(n) * n), plain(size_t(n) * n),
      steep(size_t(n) * n), chan(size_t(n) * n);
  for (size_t i = 0; i < wet.size(); ++i) {
    wet[i] = w.v[i] > p.wet_depth_m;
    // A lake surface is FLAT, so a bare slope test scores standing water as
    // plains -- it put lakes at the top of the ranking. Plains must be dry.
    plain[i] = (!wet[i] && slope.v[i] < p.plain_slope_deg);
    steep[i] = slope.v[i] > 30.0f;
    chan[i] = (!wet[i] && q.v[i] >= p.channel_q_m3s);
  }

  std::vector<int> lake_sizes;
  const std::vector<int32_t> lake_lab = Label(wet, n, lake_sizes);

  const Sat s_plain(plain, n), s_steep(steep, n), s_chan(chan, n);

  const int cen = std::max(1, int(std::lround(win_cells * p.centre_frac)));
  const int cen_off = (win_cells - cen) / 2;

  std::vector<Candidate> out;
  std::vector<double> per_lake;  // scratch: cells of each lake inside this window
  const bool single = (only_x >= 0 && only_y >= 0);
  const int y_begin = single ? only_y : 0;
  const int y_end = single ? only_y + 1 : n - win_cells + 1;
  const int y_step = single ? 1 : p.stride;
  const int x_begin = single ? only_x : 0;
  const int x_end = single ? only_x + 1 : n - win_cells + 1;
  const int x_step = single ? 1 : p.stride;
  for (int y = y_begin; y < y_end; y += y_step) {
    for (int x = x_begin; x < x_end; x += x_step) {
      Candidate c;
      c.cx = x;
      c.cy = y;

      float lo = 1e30f, hi = -1e30f, dmax = 0.f, qmax = 0.f;
      per_lake.assign(lake_sizes.size() + 1, 0.0);
      for (int j = 0; j < win_cells; ++j) {
        for (int i = 0; i < win_cells; ++i) {
          const float z = h.at(x + i, y + j);
          lo = std::min(lo, z);
          hi = std::max(hi, z);
          dmax = std::max(dmax, w.at(x + i, y + j));
          qmax = std::max(qmax, q.at(x + i, y + j));
          const int32_t id = lake_lab[size_t(y + j) * n + (x + i)];
          if (id) per_lake[size_t(id)] += 1.0;
        }
      }
      c.relief_m = hi - lo;
      c.max_depth_m = dmax;
      c.max_q_m3s = qmax;
      // The gate is on the largest lake INSIDE the window: a dozen scattered
      // puddles are not "there's a lake".
      c.lake_cells = int(*std::max_element(per_lake.begin(), per_lake.end()));
      c.channel_cells = int(s_chan.Box(x, y, win_cells));
      c.plain_frac = float(s_plain.Box(x + cen_off, y + cen_off, cen) / double(cen * cen));
      c.steep_frac = float(s_steep.Box(x, y, win_cells) / double(win_cells * win_cells));

      const bool ok = c.relief_m >= p.min_relief_m &&
                      c.plain_frac >= p.min_plain_frac &&
                      c.lake_cells >= p.min_lake_cells &&
                      c.channel_cells >= p.min_channel_cells &&
                      (p.max_lake_depth_m <= 0.f ||
                       c.max_depth_m <= p.max_lake_depth_m);
      if (single) {
        // A named place is emitted whether or not it qualifies; the caller just
        // wants to know which.
        if (gates_pass) *gates_pass = ok;
        out.push_back(c);
        continue;
      }
      if (!ok) continue;
      out.push_back(c);
    }
  }
  std::sort(out.begin(), out.end(), [](const Candidate& a, const Candidate& b) {
    return a.plain_frac > b.plain_frac;
  });
  return out;
}

std::vector<Candidate> Scan(const WinParams& p, const Field& h, const Field& w,
                            const Field& q, int win_cells) {
  return ScanImpl(p, h, w, q, win_cells, -1, -1, nullptr);
}

// Scores exactly one origin with the same code the scan uses.
Candidate ScoreOne(const WinParams& p, const Field& h, const Field& w,
                   const Field& q, int win_cells, int x, int y,
                   bool* gates_pass) {
  const std::vector<Candidate> one =
      ScanImpl(p, h, w, q, win_cells, x, y, gates_pass);
  return one.empty() ? Candidate{} : one.front();
}

// Overlapping windows are the same place scored twice -- the raw ranking's top
// entries differ by one stride step. Keep a candidate only if its centre clears
// every kept one by half a window, so the report lists distinct locations.
std::vector<Candidate> Suppress(const std::vector<Candidate>& in, int win_cells) {
  std::vector<Candidate> keep;
  for (const Candidate& c : in) {
    bool clear = true;
    for (const Candidate& k : keep) {
      if (std::abs(c.cx - k.cx) < win_cells / 2 && std::abs(c.cy - k.cy) < win_cells / 2) {
        clear = false;
        break;
      }
    }
    if (clear) keep.push_back(c);
  }
  return keep;
}

// -------------------------------------------------------------------- resample

double Sinc(double x) { return x == 0.0 ? 1.0 : std::sin(kPi * x) / (kPi * x); }
double Lanczos3(double x) {
  x = std::fabs(x);
  return x >= 3.0 ? 0.0 : Sinc(x) * Sinc(x / 3.0);
}

// Mitchell-Netravali cubic family. (B,C) picks the member:
//   (0, 0.5)   Catmull-Rom -- interpolating, mild overshoot
//   (1, 0)     cubic B-spline -- ALL WEIGHTS POSITIVE, so ringing is
//              impossible by construction; approximating, so it smooths
//   (1/3, 1/3) Mitchell -- the standard compromise
double Cubic(double x, double B, double C) {
  x = std::fabs(x);
  const double x2 = x * x, x3 = x2 * x;
  if (x < 1.0)
    return ((12 - 9 * B - 6 * C) * x3 + (-18 + 12 * B + 6 * C) * x2 + (6 - 2 * B)) / 6.0;
  if (x < 2.0)
    return ((-B - 6 * C) * x3 + (6 * B + 30 * C) * x2 + (-12 * B - 48 * C) * x +
            (8 * B + 24 * C)) / 6.0;
  return 0.0;
}

// Kernel weight and half-support, in SOURCE samples.
double KernelWeight(Kernel k, double x) {
  switch (k) {
    case Kernel::Lanczos3: return Lanczos3(x);
    case Kernel::CatmullRom: return Cubic(x, 0.0, 0.5);
    case Kernel::BSpline: return Cubic(x, 1.0, 0.0);
    case Kernel::Mitchell: return Cubic(x, 1.0 / 3.0, 1.0 / 3.0);
    case Kernel::Bilinear: {
      x = std::fabs(x);
      return x < 1.0 ? 1.0 - x : 0.0;
    }
  }
  return 0.0;
}

int KernelSupport(Kernel k) {
  switch (k) {
    case Kernel::Lanczos3: return 3;
    case Kernel::CatmullRom:
    case Kernel::BSpline:
    case Kernel::Mitchell: return 2;
    case Kernel::Bilinear: return 1;
  }
  return 2;
}

// Per-output-texel taps for one axis, clamped at the border and renormalised so
// the weights always sum to 1. Without the renormalisation the border texels
// lose mass -- the clamped taps drop out of the sum.
struct Taps {
  int n = 0;  // taps per output texel
  std::vector<int> idx;
  std::vector<double> wgt;
};

Taps BuildTaps(int src_n, int out_n, Kernel k) {
  const int support = KernelSupport(k);
  Taps t;
  t.n = 2 * support;
  t.idx.resize(size_t(out_n) * t.n);
  t.wgt.resize(size_t(out_n) * t.n);
  const double scale = double(src_n) / double(out_n);
  for (int j = 0; j < out_n; ++j) {
    // Pixel CENTRES map to pixel centres; the -0.5/+0.5 is what keeps the
    // resample from drifting half an output texel.
    const double u = (j + 0.5) * scale - 0.5;
    const int base = int(std::floor(u)) - support + 1;
    double sum = 0.0;
    for (int c = 0; c < t.n; ++c) {
      const int i = base + c;
      const double wk = KernelWeight(k, u - i);
      t.idx[size_t(j) * t.n + c] = std::clamp(i, 0, src_n - 1);
      t.wgt[size_t(j) * t.n + c] = wk;
      sum += wk;
    }
    if (sum != 0.0)
      for (int c = 0; c < t.n; ++c) t.wgt[size_t(j) * t.n + c] /= sum;
  }
  return t;
}

Field Resample(const Field& src, int out_n, Kernel k) {
  const int sn = src.n;
  const Taps t = BuildTaps(sn, out_n, k);
  const int tn = t.n;
  // Horizontal into a sn-row x out_n-col intermediate, then vertical.
  std::vector<double> mid(size_t(sn) * out_n, 0.0);
  for (int y = 0; y < sn; ++y)
    for (int j = 0; j < out_n; ++j) {
      double a = 0.0;
      for (int c = 0; c < tn; ++c)
        a += t.wgt[size_t(j) * tn + c] * src.at(t.idx[size_t(j) * tn + c], y);
      mid[size_t(y) * out_n + j] = a;
    }
  Field out(out_n);
  for (int i = 0; i < out_n; ++i)
    for (int j = 0; j < out_n; ++j) {
      double a = 0.0;
      for (int c = 0; c < tn; ++c)
        a += t.wgt[size_t(i) * tn + c] * mid[size_t(t.idx[size_t(i) * tn + c]) * out_n + j];
      out.at(j, i) = float(a);
    }
  return out;
}

// ------------------------------------------------------------------- water

struct LakeSurface {
  int32_t id = 0;
  double surface_m = 0.0;
  double spread_m = 0.0;  // max-min of the member surfaces; a levelness check
  int cells = 0;
};

// Rebuilds water at the output grid: bed from Lanczos, surface per lake as the
// constant it physically is, depth = surface - bed clamped at zero. The lake
// mask is dilated by one SOURCE cell first so the waterline can settle on the
// interpolated bed contour instead of a 32-texel staircase.
Field ReconstructWater(const Field& bed_src, const Field& depth_src,
                       const Field& bed_out, float wet_depth_m,
                       std::vector<LakeSurface>& surfaces) {
  const int sn = bed_src.n, on = bed_out.n;
  std::vector<uint8_t> wet(size_t(sn) * sn);
  for (size_t i = 0; i < wet.size(); ++i) wet[i] = depth_src.v[i] > wet_depth_m;

  std::vector<int> sizes;
  std::vector<int32_t> lab = Label(wet, sn, sizes);

  surfaces.assign(sizes.size() + 1, LakeSurface{});
  std::vector<double> lo(sizes.size() + 1, 1e30), hi(sizes.size() + 1, -1e30),
      sum(sizes.size() + 1, 0.0);
  for (int i = 0; i < sn * sn; ++i) {
    const int32_t id = lab[i];
    if (!id) continue;
    const double s = double(bed_src.v[i]) + double(depth_src.v[i]);
    lo[id] = std::min(lo[id], s);
    hi[id] = std::max(hi[id], s);
    sum[id] += s;
    surfaces[id].cells++;
  }
  for (size_t id = 1; id < surfaces.size(); ++id) {
    surfaces[id].id = int32_t(id);
    if (surfaces[id].cells) {
      surfaces[id].surface_m = sum[id] / surfaces[id].cells;
      surfaces[id].spread_m = hi[id] - lo[id];
    }
  }

  // Flood the waterline at OUTPUT resolution. A nearest-neighbour lake mask
  // (even dilated) makes the shoreline a staircase of source cells -- plainly
  // visible as 32-texel notches around every lake and along narrow channels.
  // Instead each lake is seeded from its own source cells and grown over every
  // adjacent output texel lying below its surface, so the waterline lands
  // exactly on the resampled bed's contour.
  //
  // BOUNDED BY A MARGIN AROUND THE SOURCE FOOTPRINT. The sim's water field is
  // NOT hydrostatically consistent: it ponds puddles that sit above their own
  // surroundings. One measured case is a 19-cell pond perched at 425.62 m with
  // 2682 connected cells below that level -- an unbounded flood from it drowned
  // 66% of the window. The sim decides a lake's EXTENT; the resample is only
  // allowed to refine its BOUNDARY, so the flood may not stray more than
  // `margin` source cells outside the footprint it started from.
  const int margin = 2;
  std::vector<int32_t> allow(size_t(sn) * sn, 0);
  std::vector<int> mdist(size_t(sn) * sn, INT32_MAX);
  {
    std::deque<int> mq;
    for (int i = 0; i < sn * sn; ++i)
      if (lab[i]) {
        allow[i] = lab[i];
        mdist[i] = 0;
        mq.push_back(i);
      }
    while (!mq.empty()) {
      const int c = mq.front();
      mq.pop_front();
      if (mdist[c] >= margin) continue;
      const int cx = c % sn, cy = c / sn;
      const int dx[4] = {1, -1, 0, 0}, dy[4] = {0, 0, 1, -1};
      for (int k = 0; k < 4; ++k) {
        const int nx = cx + dx[k], ny = cy + dy[k];
        if (nx < 0 || ny < 0 || nx >= sn || ny >= sn) continue;
        const int t = ny * sn + nx;
        if (mdist[t] <= mdist[c] + 1) continue;
        mdist[t] = mdist[c] + 1;
        allow[t] = allow[c];
        mq.push_back(t);
      }
    }
  }

  Field depth(on, 0.f);
  std::vector<int32_t> owner(size_t(on) * on, 0);
  const double scale = double(sn) / double(on);
  std::deque<int> q;

  // An output texel may only ever be claimed by the lake whose margin covers it.
  auto eligible = [&](int i, int j, int32_t id) {
    const int sx = std::clamp(int(std::floor((i + 0.5) * scale)), 0, sn - 1);
    const int sy = std::clamp(int(std::floor((j + 0.5) * scale)), 0, sn - 1);
    return allow[size_t(sy) * sn + sx] == id;
  };

  for (int j = 0; j < on; ++j) {
    const int sy = std::clamp(int(std::floor((j + 0.5) * scale)), 0, sn - 1);
    for (int i = 0; i < on; ++i) {
      const int sx = std::clamp(int(std::floor((i + 0.5) * scale)), 0, sn - 1);
      const int32_t id = lab[size_t(sy) * sn + sx];
      if (!id) continue;
      const int t = j * on + i;
      if (owner[t]) continue;
      // A seed texel can sit above its own lake surface once the bed is
      // resampled (the source cell was wet on average, this texel is a shoal).
      // Those are left dry and simply do not propagate.
      if (double(bed_out.v[t]) >= surfaces[id].surface_m) continue;
      owner[t] = id;
      q.push_back(t);
    }
  }

  while (!q.empty()) {
    const int c = q.front();
    q.pop_front();
    const int cx = c % on, cy = c / on;
    const int32_t id = owner[c];
    const double surf = surfaces[id].surface_m;
    const int dx[4] = {1, -1, 0, 0}, dy[4] = {0, 0, 1, -1};
    for (int k = 0; k < 4; ++k) {
      const int nx = cx + dx[k], ny = cy + dy[k];
      if (nx < 0 || ny < 0 || nx >= on || ny >= on) continue;
      const int t = ny * on + nx;
      if (owner[t]) continue;
      if (double(bed_out.v[t]) >= surf) continue;
      if (!eligible(nx, ny, id)) continue;
      owner[t] = id;
      q.push_back(t);
    }
  }

  for (int t = 0; t < on * on; ++t) {
    if (!owner[t]) continue;
    const double d = surfaces[owner[t]].surface_m - double(bed_out.v[t]);
    if (d > 0.0) depth.v[t] = float(d);
  }
  return depth;
}

// ------------------------------------------------------------- mapview export

// mapgen::Biome values. Duplicated as plain constants rather than including
// mapgen/biomes.hpp, because this tool is a standalone stdlib-only TU; the
// loader validates the range on the way back in.
enum : uint8_t { kBiomeLake = 0, kBiomeSwamp = 1, kBiomeForest = 2,
                 kBiomePlains = 3, kBiomeHills = 4, kBiomeMountain = 5 };

// Area fractions from src/mapgen/generator.hpp. Quantile cuts make the split
// structural: it holds for every window, not on average.
constexpr float kMountainFrac = 0.12f;  // thinnest-soil fraction -> bare rock
constexpr float kHillsFrac = 0.33f;     // next band -> hills; the rest is plains

// Wet -> Lake. Dry ground is classified by SUBSTRATE: how much erodible cover
// sits over bedrock. Thin cover means the fluvial system has stripped the cell
// back to rock, which is what a mountain or a ridge physically IS -- so this
// reads the erosion's own result rather than re-deriving relief from elevation.
//
// Thresholds are QUANTILES of the dry soil distribution, not absolute depths, so
// the split is structural: it holds for every window rather than on average, and
// it does not need retuning when the sim's overall soil budget changes.
Field ClassifyBiomes(const Field& bed, const Field& depth, const Field& soil) {
  const int n = bed.n;
  Field out(n);
  std::vector<float> dry;
  dry.reserve(soil.v.size());
  for (size_t i = 0; i < soil.v.size(); ++i)
    if (depth.v[i] <= 0.f) dry.push_back(soil.v[i]);
  if (dry.empty()) {
    for (size_t i = 0; i < out.v.size(); ++i) out.v[i] = float(kBiomeLake);
    return out;
  }
  std::sort(dry.begin(), dry.end());
  const auto q = [&](float f) {
    return dry[std::min(dry.size() - 1, size_t(f * double(dry.size())))];
  };
  // Bare rock is the top of the mountain fraction; thin cover the hills.
  const float t_mountain = q(kMountainFrac);            // thinnest 12% -> rock
  const float t_hills = q(kMountainFrac + kHillsFrac);  // next 33% -> hills
  for (size_t i = 0; i < out.v.size(); ++i) {
    if (depth.v[i] > 0.f) { out.v[i] = float(kBiomeLake); continue; }
    const float sm = soil.v[i];
    out.v[i] = float(sm <= t_mountain ? kBiomeMountain
                                      : (sm <= t_hills ? kBiomeHills : kBiomePlains));
  }
  return out;
}

// --- boundary inflow ---------------------------------------------------------
//
// A window is a CUTOUT of a larger catchment, so rivers cross into it carrying
// discharge from ground the window cannot see. Routing the window alone would
// start every channel from zero at the edge and lose the entire upstream
// catchment -- the trunk river would arrive as a trickle.
//
// Found by steepest descent on the SOURCE grid: for each cell just outside the
// window border, if its D8 receiver lies inside the window and it carries
// Q >= `channel_q_m3s`, that is an inflow. Descent is recomputed here rather
// than read from a raster because direction is what decides IN vs OUT, and the
// dumps carry only magnitudes.
struct Inflow {
  int x = 0, y = 0;      // entry cell, WINDOW source-cell coordinates
  float q_m3_s = 0.0f;
};

std::vector<Inflow> FindInflows(const Field& h, const Field& q, int cx, int cy,
                                int win_cells, float cell_m, float min_q) {
  std::vector<Inflow> out;
  const int n = h.n;
  // One ring of cells just outside the window.
  for (int j = -1; j <= win_cells; ++j) {
    for (int i = -1; i <= win_cells; ++i) {
      const bool on_ring = (i == -1 || i == win_cells || j == -1 || j == win_cells);
      if (!on_ring) continue;
      const int sx = cx + i, sy = cy + j;
      if (sx < 0 || sy < 0 || sx >= n || sy >= n) continue;
      if (q.at(sx, sy) < min_q) continue;
      // Steepest descent, ranked by drop per unit distance so a diagonal is not
      // preferred merely for reaching further.
      int bx = -1, by = -1;
      float best = 0.0f;
      for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
          if (!dx && !dy) continue;
          const int nx = sx + dx, ny = sy + dy;
          if (nx < 0 || ny < 0 || nx >= n || ny >= n) continue;
          const float dist = (dx && dy) ? cell_m * 1.41421356f : cell_m;
          const float grad = (h.at(sx, sy) - h.at(nx, ny)) / dist;
          if (grad > best) { best = grad; bx = nx; by = ny; }
        }
      if (bx < 0) continue;
      const int wx = bx - cx, wy = by - cy;
      if (wx < 0 || wy < 0 || wx >= win_cells || wy >= win_cells) continue;
      out.push_back({wx, wy, q.at(sx, sy)});
    }
  }
  std::sort(out.begin(), out.end(),
            [](const Inflow& a, const Inflow& b) { return a.q_m3_s > b.q_m3_s; });
  return out;
}

// Entry points in OUTPUT texels, so the consumer never needs the source grid.
bool WriteInflows(const std::string& dir, const std::vector<Inflow>& in,
                  int win_cells, int out_res, float runoff_m_per_yr) {
  const std::string path = dir + "/inflows.txt";
  FILE* fp = std::fopen(path.c_str(), "w");
  if (!fp) {
    std::fprintf(stderr, "window: cannot write %s\n", path.c_str());
    return false;
  }
  const int f = out_res / win_cells;
  std::fprintf(fp, "# river inflow across the window boundary\n");
  std::fprintf(fp, "# runoff_m_per_yr %.6f\n", double(runoff_m_per_yr));
  std::fprintf(fp, "# texel_x texel_y discharge_m3_s\n");
  for (const Inflow& i : in)
    std::fprintf(fp, "%d %d %.9g\n", i.x * f + f / 2, i.y * f + f / 2,
                 double(i.q_m3_s));
  std::fclose(fp);
  return true;
}

// Writes the mapview load set: height.f32 + biome.u8 + level.f32 + map.txt.
// The LEVEL raster carries the water, not a depth field -- dry texels store
// `level == height`, so depth = max(0, level - height) needs no sentinel and a
// lake surface is exactly flat by construction.
bool WriteMapDir(const std::string& dir, const Field& bed, const Field& depth,
                 const Field& biome, const Field& soil, float world_size_m,
                 const std::string& source) {
  Field level(bed.n);
  for (size_t i = 0; i < level.v.size(); ++i)
    level.v[i] = depth.v[i] > 0.f ? bed.v[i] + depth.v[i] : bed.v[i];

  if (!WriteField(dir + "/height.f32", bed)) return false;
  if (!WriteField(dir + "/level.f32", level)) return false;
  if (!WriteField(dir + "/soil.f32", soil)) return false;

  std::vector<uint8_t> b(biome.v.size());
  for (size_t i = 0; i < b.size(); ++i) b[i] = uint8_t(biome.v[i]);
  const std::string bp = dir + "/biome.u8";
  FILE* fp = std::fopen(bp.c_str(), "wb");
  if (!fp) {
    std::fprintf(stderr, "window: cannot write %s\n", bp.c_str());
    return false;
  }
  std::fwrite(b.data(), 1, b.size(), fp);
  std::fclose(fp);

  const std::string mp = dir + "/map.txt";
  FILE* mf = std::fopen(mp.c_str(), "w");
  if (!mf) {
    std::fprintf(stderr, "window: cannot write %s\n", mp.c_str());
    return false;
  }
  std::fprintf(mf, "resolution %d\nworld_size_m %.6f\nsource %s\n", bed.n,
               double(world_size_m), source.c_str());
  std::fclose(mf);
  return true;
}

// -------------------------------------------------------------------- report

void PrintCandidates(const std::vector<Candidate>& c, const WinParams& p, int n) {
  const float cell_m = p.src_world_m / float(p.src_res);
  std::printf("  %4s %14s %11s %8s %7s %7s %9s %8s %7s\n", "rank", "origin cell",
              "world m", "relief", "plain", "steep", "lake", "depth", "maxQ");
  for (int i = 0; i < n && i < int(c.size()); ++i) {
    const Candidate& k = c[size_t(i)];
    std::printf("  %4d %6d,%-7d %5.0f,%-5.0f %6.0fm %6.0f%% %6.0f%% %7dc %7.1fm %7.3f\n",
                i, k.cx, k.cy, k.cx * cell_m, k.cy * cell_m, k.relief_m,
                k.plain_frac * 100.f, k.steep_frac * 100.f, k.lake_cells,
                k.max_depth_m, k.max_q_m3s);
  }
}

// ---------------------------------------------------------------------- tests

namespace test {

int g_pass = 0, g_fail = 0;

void Check(const char* name, bool ok, const std::string& detail) {
  (ok ? g_pass : g_fail)++;
  std::printf("  [%s] %-34s %s\n", ok ? "ok" : "FAIL", name, detail.c_str());
}

// Resampling a constant must give the constant. This is the partition-of-unity
// property, and it is what the border renormalisation in BuildTaps protects --
// without it the edge texels come out low.
void ConstantPreserved() {
  Field src(16, 42.0f);
  const Field out = Resample(src, 512, Kernel::Lanczos3);
  float lo = 1e30f, hi = -1e30f;
  for (float v : out.v) { lo = std::min(lo, v); hi = std::max(hi, v); }
  Check("lanczos: constant preserved", std::fabs(lo - 42.f) < 1e-3f && std::fabs(hi - 42.f) < 1e-3f,
        "range " + std::to_string(lo) + ".." + std::to_string(hi));
}

// Worst interior error reconstructing a linear ramp, for one kernel.
double LinearRampError(Kernel k) {
  const int sn = 32, on = 1024;
  Field src(sn);
  for (int y = 0; y < sn; ++y)
    for (int x = 0; x < sn; ++x) src.at(x, y) = 3.0f * x + 7.0f;
  const Field out = Resample(src, on, k);
  const double scale = double(sn) / double(on);
  double worst = 0.0;
  for (int j = on / 4; j < 3 * on / 4; ++j)
    for (int i = on / 4; i < 3 * on / 4; ++i) {
      const double u = (i + 0.5) * scale - 0.5;
      worst = std::max(worst, std::fabs(out.at(i, j) - (3.0 * u + 7.0)));
    }
  return worst;
}

// A planar hillside IS a linear ramp, and it is most of a terrain. The cubics
// reproduce one exactly, so a flat slope resamples to a flat slope.
void LinearReproducedByCubics() {
  for (Kernel k : {Kernel::CatmullRom, Kernel::BSpline, Kernel::Mitchell, Kernel::Bilinear}) {
    const double e = LinearRampError(k);
    Check((std::string(KernelName(k)) + ": linear ramp exact").c_str(), e < 1e-3,
          "max err " + std::to_string(e) + " m");
  }
}

// Lanczos-3 does NOT reproduce a linear ramp -- its weights sum to 1 only to
// 5.7e-3 and its first moment is off by 6.5e-2. The resulting ~6 cm error is
// PERIODIC WITH THE SOURCE GRID, which is the corduroy ripple visible in the
// hillshade of an upscaled Lanczos bed. Pinned as a known property so the
// default kernel choice does not get quietly reverted.
void LanczosDoesNotReproduceLinear() {
  const double e = LinearRampError(Kernel::Lanczos3);
  const double cat = LinearRampError(Kernel::CatmullRom);
  Check("lanczos: linear error is why not default", e > 1e-2 && cat < 1e-3,
        "lanczos " + std::to_string(e) + " m vs catmull " + std::to_string(cat) + " m");
}

// B-spline weights are all positive, so it cannot overshoot: the resampled bed
// must stay inside the source range. This is the zero-ringing escape hatch.
void BSplineNeverOvershoots() {
  const int sn = 16, on = 256;
  Field src(sn);
  for (int y = 0; y < sn; ++y)
    for (int x = 0; x < sn; ++x) src.at(x, y) = (x / 4 + y / 4) % 2 ? 100.f : 0.f;  // hard steps
  float slo = 1e30f, shi = -1e30f;
  for (float v : src.v) { slo = std::min(slo, v); shi = std::max(shi, v); }
  const Field bs = Resample(src, on, Kernel::BSpline);
  const Field lz = Resample(src, on, Kernel::Lanczos3);
  float blo = 1e30f, bhi = -1e30f, llo = 1e30f, lhi = -1e30f;
  for (float v : bs.v) { blo = std::min(blo, v); bhi = std::max(bhi, v); }
  for (float v : lz.v) { llo = std::min(llo, v); lhi = std::max(lhi, v); }
  Check("bspline: cannot overshoot", blo >= slo - 1e-3f && bhi <= shi + 1e-3f,
        "bspline " + std::to_string(blo) + ".." + std::to_string(bhi) + " vs lanczos " +
            std::to_string(llo) + ".." + std::to_string(lhi));
}

// A half-texel centring error shows up as an asymmetric result on a symmetric
// input. Cheap, and it catches the classic off-by-half in the tap mapping.
void SymmetryPreserved() {
  const int sn = 16, on = 256;
  Field src(sn);
  for (int y = 0; y < sn; ++y)
    for (int x = 0; x < sn; ++x) {
      const double dx = x - (sn - 1) / 2.0, dy = y - (sn - 1) / 2.0;
      src.at(x, y) = float(std::exp(-(dx * dx + dy * dy) / 8.0));
    }
  const Field out = Resample(src, on, Kernel::Lanczos3);
  double worst = 0.0;
  for (int j = 0; j < on; ++j)
    for (int i = 0; i < on; ++i)
      worst = std::max(worst, std::fabs(double(out.at(i, j)) - out.at(on - 1 - i, j)));
  Check("lanczos: symmetry preserved", worst < 1e-5, "max asym " + std::to_string(worst));
}

// The whole reason water is rebuilt instead of resampled. Lanczos on the depth
// field measured -2.01 m on the real world; the reconstruction must be >= 0
// everywhere by construction.
void DepthNeverNegative() {
  const int sn = 24, on = 384;
  Field bed(sn), dep(sn, 0.f);
  for (int y = 0; y < sn; ++y)
    for (int x = 0; x < sn; ++x) {
      const double dx = x - sn / 2.0, dy = y - sn / 2.0;
      bed.at(x, y) = float(100.0 + 0.4 * (dx * dx + dy * dy));  // a bowl
    }
  const float surface = 120.0f;
  for (int y = 0; y < sn; ++y)
    for (int x = 0; x < sn; ++x)
      dep.at(x, y) = std::max(0.f, surface - bed.at(x, y));
  const Field bed_out = Resample(bed, on, Kernel::CatmullRom);
  std::vector<LakeSurface> surf;
  const Field dep_out = ReconstructWater(bed, dep, bed_out, 0.5f, surf);
  float lo = 1e30f;
  for (float v : dep_out.v) lo = std::min(lo, v);
  Check("water: depth never negative", lo >= 0.f, "min depth " + std::to_string(lo) + " m");
}

// A lake surface is an equipotential. Bed + depth must come out level across the
// whole resampled lake, or the water plane will visibly tilt.
void LakeSurfaceLevel() {
  const int sn = 24, on = 384;
  Field bed(sn), dep(sn, 0.f);
  for (int y = 0; y < sn; ++y)
    for (int x = 0; x < sn; ++x) {
      const double dx = x - sn / 2.0, dy = y - sn / 2.0;
      bed.at(x, y) = float(100.0 + 0.4 * (dx * dx + dy * dy));
    }
  const float surface = 120.0f;
  for (int y = 0; y < sn; ++y)
    for (int x = 0; x < sn; ++x)
      dep.at(x, y) = std::max(0.f, surface - bed.at(x, y));
  const Field bed_out = Resample(bed, on, Kernel::CatmullRom);
  std::vector<LakeSurface> surf;
  const Field dep_out = ReconstructWater(bed, dep, bed_out, 0.5f, surf);
  float lo = 1e30f, hi = -1e30f;
  for (int i = 0; i < on * on; ++i) {
    if (dep_out.v[i] <= 0.f) continue;
    const float s = bed_out.v[i] + dep_out.v[i];
    lo = std::min(lo, s);
    hi = std::max(hi, s);
  }
  Check("water: lake surface level", hi - lo < 1e-2f,
        "spread " + std::to_string(hi - lo) + " m");
}

// The sim's water field is not hydrostatically consistent, so a small pond can
// sit ABOVE a large connected area. An unbounded flood from one measured case (19
// cells at 425.62 m, 2682 connected cells below it) drowned 66% of the window.
// The margin must hold the flood to the sim's footprint.
void PerchedPondBounded() {
  const int sn = 64, on = 512;
  Field bed(sn), dep(sn, 0.f);
  // A low plain everywhere, with a raised plateau carrying a small pond.
  for (int y = 0; y < sn; ++y)
    for (int x = 0; x < sn; ++x) bed.at(x, y) = 100.0f;
  for (int y = 24; y < 40; ++y)
    for (int x = 24; x < 40; ++x) bed.at(x, y) = 130.0f;   // plateau
  for (int y = 29; y < 35; ++y)
    for (int x = 29; x < 35; ++x) {                        // pond on top of it
      bed.at(x, y) = 126.0f;
      dep.at(x, y) = 4.0f;                                 // surface 130 m
    }
  const Field bed_out = Resample(bed, on, Kernel::CatmullRom);
  std::vector<LakeSurface> surf;
  const Field dep_out = ReconstructWater(bed, dep, bed_out, 0.5f, surf);
  size_t wet = 0;
  for (float v : dep_out.v) wet += (v > 0.f);
  const double frac = double(wet) / (double(on) * on);
  // 36 source cells of 4096 is 0.9%; the margin allows a couple of cells of
  // slack. Anything approaching the whole grid means the flood escaped.
  Check("water: perched pond stays bounded", frac < 0.05,
        "wet " + std::to_string(frac * 100.0) + "% of window");
}

// The shoreline must land on the resampled bed's contour, not on source-cell
// boundaries. On a cone the waterline is a circle: measure how much its radius
// wobbles. A nearest-neighbour mask gives ~1 SOURCE cell of wobble (8 output
// texels here); contour-following should be about one output texel.
void ShorelineFollowsContour() {
  const int sn = 64, on = 512;
  const int f = on / sn;
  Field bed(sn), dep(sn, 0.f);
  const double cx = (sn - 1) / 2.0, cy = (sn - 1) / 2.0;
  for (int y = 0; y < sn; ++y)
    for (int x = 0; x < sn; ++x)
      bed.at(x, y) = float(100.0 + 2.0 * std::hypot(x - cx, y - cy));  // cone
  const float surface = 140.0f;
  for (int y = 0; y < sn; ++y)
    for (int x = 0; x < sn; ++x)
      dep.at(x, y) = std::max(0.f, surface - bed.at(x, y));
  const Field bed_out = Resample(bed, on, Kernel::CatmullRom);
  std::vector<LakeSurface> surf;
  const Field dep_out = ReconstructWater(bed, dep, bed_out, 0.5f, surf);
  // Radii of the outermost wet texel along each of 4 axes and 4 diagonals.
  const double ocx = (on - 1) / 2.0, ocy = (on - 1) / 2.0;
  double rlo = 1e30, rhi = -1e30;
  for (int j = 0; j < on; ++j)
    for (int i = 0; i < on; ++i) {
      if (dep_out.at(i, j) <= 0.f) continue;
      const double r = std::hypot(i - ocx, j - ocy);
      rhi = std::max(rhi, r);
    }
  // Innermost DRY texel bounds the shoreline from the other side.
  for (int j = 0; j < on; ++j)
    for (int i = 0; i < on; ++i) {
      if (dep_out.at(i, j) > 0.f) continue;
      const double r = std::hypot(i - ocx, j - ocy);
      rlo = std::min(rlo, r);
    }
  const double wobble = rhi - rlo;
  Check("water: shoreline follows contour", wobble < 0.5 * f,
        "radius wobble " + std::to_string(wobble) + " texels (source cell = " +
            std::to_string(f) + ")");
}

// The bed must keep the basin. If the den ever got flattened -- by resampling the
// water surface instead of the bed -- the depth below the waterline would vanish.
void BedKeepsTheDen() {
  const int sn = 24, on = 384;
  Field bed(sn);
  for (int y = 0; y < sn; ++y)
    for (int x = 0; x < sn; ++x) {
      const double dx = x - sn / 2.0, dy = y - sn / 2.0;
      bed.at(x, y) = float(100.0 + 0.4 * (dx * dx + dy * dy));
    }
  const Field bed_out = Resample(bed, on, Kernel::CatmullRom);
  float lo = 1e30f, hi = -1e30f;
  for (float v : bed_out.v) { lo = std::min(lo, v); hi = std::max(hi, v); }
  const float src_lo = *std::min_element(bed.v.begin(), bed.v.end());
  Check("bed: den survives resample", std::fabs(lo - src_lo) < 1.0f && hi - lo > 50.f,
        "bed " + std::to_string(lo) + ".." + std::to_string(hi) + " m (src min " +
            std::to_string(src_lo) + ")");
}

// Plains must be DRY. A synthetic half-lake/half-flat window scores 100% plain
// under a bare slope test and 50% once water is excluded -- the bug that put
// lakes at the top of the real ranking.
void PlainsExcludeWater() {
  const int n = 64;
  WinParams p;
  p.src_res = n;
  p.src_world_m = n * 16.0f;
  p.min_relief_m = 0.f;
  p.min_lake_cells = 0;
  p.min_channel_cells = 0;
  p.min_plain_frac = 0.f;
  p.stride = n;  // exactly one window
  Field h(n, 100.f), w(n, 0.f), q(n, 0.f);
  for (int y = 0; y < n; ++y)
    for (int x = 0; x < n / 2; ++x) w.at(x, y) = 5.0f;  // left half flooded
  const std::vector<Candidate> c = Scan(p, h, w, q, n);
  const bool ok = !c.empty() && std::fabs(c[0].plain_frac - 0.5f) < 0.02f;
  Check("scan: plains exclude water", ok,
        c.empty() ? "no candidate" : "plain " + std::to_string(c[0].plain_frac * 100.f) + "%");
}

// The gates must actually gate. A flat dry world has no lake and no channel, so
// nothing may survive -- if it does, a criterion is not wired up.
void GatesReject() {
  const int n = 64;
  WinParams p;
  p.src_res = n;
  p.src_world_m = n * 16.0f;
  p.stride = n;
  Field h(n, 100.f), w(n, 0.f), q(n, 0.f);
  const std::vector<Candidate> c = Scan(p, h, w, q, n);
  Check("scan: gates reject empty world", c.empty(),
        std::to_string(c.size()) + " survivors");
}

// Overlapping windows are the same place. Suppression must collapse a dense
// cluster to one entry per location.
void SuppressionDedups() {
  std::vector<Candidate> in;
  for (int k = 0; k < 8; ++k) {
    Candidate c;
    c.cx = k * 2;  // all within half a window of each other
    c.cy = 0;
    c.plain_frac = 1.0f - 0.01f * k;
    in.push_back(c);
  }
  const std::vector<Candidate> out = Suppress(in, 64);
  Check("scan: overlap suppressed", out.size() == 1,
        std::to_string(out.size()) + " kept of " + std::to_string(in.size()));
}

// The window must be a whole number of source cells, or the resample silently
// samples between them and the gameplay grid stops aligning with the sim grid.
void WindowCellsIntegral() {
  WinParams p;
  const float cell_m = p.src_world_m / float(p.src_res);      // 16 m
  const float window_m = p.out_res * p.out_texel_m;           // 1024 m
  const double cells = window_m / cell_m;
  Check("geometry: window is whole cells", std::fabs(cells - std::lround(cells)) < 1e-9,
        std::to_string(window_m) + " m / " + std::to_string(cell_m) + " m = " +
            std::to_string(cells) + " cells");
}

int RunAll() {
  std::printf("protogen_window sanity\n");
  ConstantPreserved();
  LinearReproducedByCubics();
  LanczosDoesNotReproduceLinear();
  BSplineNeverOvershoots();
  SymmetryPreserved();
  DepthNeverNegative();
  LakeSurfaceLevel();
  PerchedPondBounded();
  ShorelineFollowsContour();
  BedKeepsTheDen();
  PlainsExcludeWater();
  GatesReject();
  SuppressionDedups();
  WindowCellsIntegral();
  std::printf("\n  %d passed, %d failed\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}

}  // namespace test

}  // namespace

int main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i)
    if (std::string(argv[i]) == "--test") return test::RunAll();

  WinParams p;
  try {
    for (int i = 1; i < argc; ++i) {
      const std::string a = argv[i];
      auto nxt = [&]() -> std::string { return (i + 1 < argc) ? argv[++i] : ""; };
      if (a == "--in") p.in = nxt();
      else if (a == "--tag") p.tag = nxt();
      else if (a == "--res") p.src_res = std::stoi(nxt());
      else if (a == "--world") p.src_world_m = std::stof(nxt());
      else if (a == "--out-res") p.out_res = std::stoi(nxt());
      else if (a == "--out-texel") p.out_texel_m = std::stof(nxt());
      else if (a == "--stride") p.stride = std::stoi(nxt());
      else if (a == "--wet-depth") p.wet_depth_m = std::stof(nxt());
      else if (a == "--min-lake-cells") p.min_lake_cells = std::stoi(nxt());
      else if (a == "--channel-q") p.channel_q_m3s = std::stof(nxt());
      else if (a == "--min-channel-cells") p.min_channel_cells = std::stoi(nxt());
      else if (a == "--min-relief") p.min_relief_m = std::stof(nxt());
      else if (a == "--plain-slope") p.plain_slope_deg = std::stof(nxt());
      else if (a == "--min-plain") p.min_plain_frac = std::stof(nxt());
      else if (a == "--centre-frac") p.centre_frac = std::stof(nxt());
      else if (a == "--max-lake-depth") p.max_lake_depth_m = std::stof(nxt());
      else if (a == "--runoff") p.runoff_m_per_yr = std::stof(nxt());
      else if (a == "--rank") p.rank = std::stoi(nxt());
      else if (a == "--origin-cell") {
        const std::string v = nxt();
        const size_t comma = v.find(',');
        if (comma == std::string::npos) {
          std::fprintf(stderr, "window: --origin-cell wants X,Y in source cells\n");
          return 2;
        }
        p.origin_x = std::stoi(v.substr(0, comma));
        p.origin_y = std::stoi(v.substr(comma + 1));
      }
      else if (a == "--report") p.report = std::stoi(nxt());
      else if (a == "--kernel-compare") p.kernel_compare = true;
      else if (a == "--kernel") {
        const std::string k = nxt();
        if (!ParseKernel(k, p.kernel)) {
          std::fprintf(stderr,
                       "window: unknown --kernel '%s' (lanczos3|catmull|bspline|"
                       "mitchell|bilinear)\n",
                       k.c_str());
          return 2;
        }
      }
      else if (a == "--out") p.out = nxt();
      else {
        std::fprintf(stderr, "window: unknown arg '%s'\n", a.c_str());
        return 2;
      }
    }
  } catch (const std::exception& e) {
    std::fprintf(stderr, "window: bad or missing argument value (%s)\n", e.what());
    return 2;
  }

  if (p.in.empty()) {
    std::fprintf(stderr, "window: --in <dump-dir> is required\n");
    return 2;
  }

  const float cell_m = p.src_world_m / float(p.src_res);
  const float window_m = p.out_res * p.out_texel_m;
  const double cells_f = window_m / cell_m;
  const int win_cells = int(std::lround(cells_f));
  if (std::fabs(cells_f - win_cells) > 1e-6) {
    std::fprintf(stderr,
                 "window: %.1f m window is %.3f source cells at %.2f m -- must be a "
                 "whole number (adjust --out-res/--out-texel)\n",
                 window_m, cells_f, cell_m);
    return 2;
  }
  if (win_cells > p.src_res) {
    std::fprintf(stderr, "window: %d-cell window exceeds the %d-cell source\n",
                 win_cells, p.src_res);
    return 2;
  }

  Field h, w, q, dis, soil;
  const std::string base = p.in + "/" + p.tag + "-";
  if (!LoadField(base + "height.f32", p.src_res, h)) return 1;
  if (!LoadField(base + "water.f32", p.src_res, w)) return 1;
  if (!LoadField(base + "Q.f32", p.src_res, q)) return 1;
  const bool have_dis = LoadField(base + "discharge.f32", p.src_res, dis);
  // Soil is what the substrate biomes are cut from. A dump predating the
  // two-layer substrate has none; fall back to a uniform layer so old runs still
  // load, and say so rather than silently classifying everything as plains.
  const bool have_soil = LoadField(base + "soil.f32", p.src_res, soil);
  if (!have_soil) {
    std::fprintf(stderr,
                 "window: no %ssoil.f32 -- this dump predates the substrate; "
                 "biomes will be uniform. Re-run protogen to get real soil.\n",
                 base.c_str());
    soil = Field(p.src_res, 1.0f);
  }

  std::printf("window: source %dx%d at %.1f m (%.0f m world)\n"
              "  window %.0f m = %d cells -> %dx%d at %.2f m (%.0fx upscale)\n",
              p.src_res, p.src_res, cell_m, p.src_world_m, window_m, win_cells,
              p.out_res, p.out_res, p.out_texel_m, double(p.out_res) / win_cells);
  std::printf("  gates: relief >= %.0f m, plain >= %.0f%% of centre %.0f m "
              "(dry & < %.0f deg),\n         lake >= %d cells, channel >= %d cells "
              "at Q >= %.3f m3/s\n",
              p.min_relief_m, p.min_plain_frac * 100.f, window_m * p.centre_frac,
              p.plain_slope_deg, p.min_lake_cells, p.min_channel_cells, p.channel_q_m3s);

  const std::vector<Candidate> all = Scan(p, h, w, q, win_cells);
  const std::vector<Candidate> distinct = Suppress(all, win_cells);
  const int total_windows = [&] {
    int c = 0;
    for (int y = 0; y + win_cells <= p.src_res; y += p.stride)
      for (int x = 0; x + win_cells <= p.src_res; x += p.stride) ++c;
    return c;
  }();
  std::printf("\n  %zu of %d windows pass all four; %zu distinct locations\n",
              all.size(), total_windows, distinct.size());
  if (distinct.empty()) {
    std::fprintf(stderr, "window: nothing passes -- relax a gate\n");
    return 1;
  }
  PrintCandidates(distinct, p, p.report);

  Candidate sel;
  if (p.origin_x >= 0 || p.origin_y >= 0) {
    if (p.origin_x < 0 || p.origin_y < 0 ||
        p.origin_x + win_cells > p.src_res || p.origin_y + win_cells > p.src_res) {
      std::fprintf(stderr,
                   "window: --origin-cell %d,%d does not fit a %d-cell window in "
                   "a %d-cell source\n",
                   p.origin_x, p.origin_y, win_cells, p.src_res);
      return 2;
    }
    // A named place is emitted whether or not it qualifies -- the gates are for
    // DISCOVERY. Scored directly rather than looked up: the scan only visits
    // stride-aligned origins, so a pinned one off that grid is simply absent.
    bool gates_pass = false;
    sel = ScoreOne(p, h, w, q, win_cells, p.origin_x, p.origin_y, &gates_pass);
    std::printf("\n  pinned to cell (%d,%d) -- %s\n", sel.cx, sel.cy,
                gates_pass ? "passes all four gates"
                           : "NOTE: does not pass the gates on this map");
    std::printf("    relief %.0f m | plain(centre) %.0f%% | lake %d cells, "
                "deepest %.1f m | channel %d cells\n",
                sel.relief_m, sel.plain_frac * 100.f, sel.lake_cells,
                sel.max_depth_m, sel.channel_cells);
  } else {
    if (p.rank < 0 || p.rank >= int(distinct.size())) {
      std::fprintf(stderr, "window: --rank %d out of range (0..%zu)\n", p.rank,
                   distinct.size() - 1);
      return 2;
    }
    sel = distinct[size_t(p.rank)];
    std::printf("\n  NOTE: --rank is an ordinal into a list that reshuffles when "
                "the map\n        or the gates change. Pin this window with "
                "--origin-cell %d,%d\n        to keep it across re-runs.\n",
                sel.cx, sel.cy);
  }

  // Crop, then resample.
  Field bed_src(win_cells), dep_src(win_cells), q_src(win_cells),
      dis_src(win_cells), soil_src(win_cells);
  for (int j = 0; j < win_cells; ++j)
    for (int i = 0; i < win_cells; ++i) {
      bed_src.at(i, j) = h.at(sel.cx + i, sel.cy + j);
      dep_src.at(i, j) = w.at(sel.cx + i, sel.cy + j);
      q_src.at(i, j) = q.at(sel.cx + i, sel.cy + j);
      dis_src.at(i, j) = have_dis ? dis.at(sel.cx + i, sel.cy + j) : 0.f;
      soil_src.at(i, j) = soil.at(sel.cx + i, sel.cy + j);
    }

  if (p.kernel_compare) {
    // Overshoot is the honest ringing measure on the HEIGHT; ripple is measured
    // in the SLOPE field over ground the source says is flat, because that is
    // what the shading actually shows and it is where Lanczos gives itself away.
    const Field src_slope = SlopeDeg(bed_src, cell_m);
    float slo2 = 1e30f, shi2 = -1e30f;
    for (float v : bed_src.v) { slo2 = std::min(slo2, v); shi2 = std::max(shi2, v); }
    std::printf("\n  kernel comparison on this window (%.1f m relief):\n",
                shi2 - slo2);
    std::printf("    %-10s %11s %12s %11s %10s\n", "kernel", "overshoot", "undershoot",
                "slope p99", "ripple");
    for (Kernel k : {Kernel::Bilinear, Kernel::BSpline, Kernel::Mitchell,
                     Kernel::CatmullRom, Kernel::Lanczos3}) {
      const Field t = Resample(bed_src, p.out_res, k);
      float lo2 = 1e30f, hi2 = -1e30f;
      for (float v : t.v) { lo2 = std::min(lo2, v); hi2 = std::max(hi2, v); }
      const Field ts = SlopeDeg(t, p.out_texel_m);
      // Ripple: spread of output slope over source cells that are nearly flat.
      // On a true interpolant of flat ground this is ~0; ringing shows up here.
      double sum = 0.0, sum2 = 0.0;
      size_t cnt = 0;
      const int f = p.out_res / win_cells;
      for (int j = 0; j < p.out_res; ++j)
        for (int i = 0; i < p.out_res; ++i)
          if (src_slope.at(std::min(win_cells - 1, i / f), std::min(win_cells - 1, j / f)) < 2.0f) {
            const double s = ts.at(i, j);
            sum += s;
            sum2 += s * s;
            ++cnt;
          }
      const double mean = cnt ? sum / cnt : 0.0;
      const double sd = cnt ? std::sqrt(std::max(0.0, sum2 / cnt - mean * mean)) : 0.0;
      std::vector<float> ss = ts.v;
      std::nth_element(ss.begin(), ss.begin() + (ss.size() * 99) / 100, ss.end());
      std::printf("    %-10s %10.2fm %11.2fm %10.1f %10.3f%s\n", KernelName(k),
                  std::max(0.f, hi2 - shi2), std::max(0.f, slo2 - lo2),
                  ss[(ss.size() * 99) / 100], sd,
                  k == p.kernel ? "   <- selected" : "");
    }
  }

  const Field bed_out = Resample(bed_src, p.out_res, p.kernel);
  std::vector<LakeSurface> surfaces;
  const Field dep_out =
      ReconstructWater(bed_src, dep_src, bed_out, p.wet_depth_m, surfaces);
  const Field q_out = Resample(q_src, p.out_res, Kernel::Bilinear);
  // Soil is a THICKNESS: bilinear so it cannot ring negative, then clamped.
  Field soil_out = Resample(soil_src, p.out_res, Kernel::Bilinear);
  for (float& v : soil_out.v) v = std::max(0.f, v);
  const Field dis_out = Resample(dis_src, p.out_res, Kernel::Bilinear);

  std::error_code ec;
  std::filesystem::create_directories(p.out, ec);
  if (ec) {
    std::fprintf(stderr, "window: cannot create out dir '%s': %s\n", p.out.c_str(),
                 ec.message().c_str());
    return 1;
  }
  // Named so tools/protogen/show.py picks them up unchanged:
  //   python3 tools/protogen/show.py <out> <out_res> <window_m>
  const std::string ob = p.out + "/window-";
  if (!WriteField(ob + "height.f32", bed_out)) return 1;
  if (!WriteField(ob + "water.f32", dep_out)) return 1;
  if (!WriteField(ob + "Q.f32", q_out)) return 1;
  if (!WriteField(ob + "discharge.f32", dis_out)) return 1;

  // The mapview load set, alongside the show.py rasters.
  const Field biome_out = ClassifyBiomes(bed_out, dep_out, soil_out);
  char src[256];
  std::snprintf(src, sizeof(src), "protogen %s tag=%s origin_cell=%d,%d kernel=%s",
                p.in.c_str(), p.tag.c_str(), sel.cx, sel.cy, KernelName(p.kernel));
  if (!WriteMapDir(p.out, bed_out, dep_out, biome_out, soil_out, window_m, src))
    return 1;

  // Where rivers cross INTO the window. Without these the fine routing would
  // start every channel at zero on the edge and lose the upstream catchment.
  const std::vector<Inflow> inflows =
      FindInflows(h, q, sel.cx, sel.cy, win_cells, cell_m, p.channel_q_m3s);
  if (!WriteInflows(p.out, inflows, win_cells, p.out_res, p.runoff_m_per_yr))
    return 1;
  double q_in = 0.0;
  for (const Inflow& i : inflows) q_in += i.q_m3_s;
  // Rain landing on the window itself, for scale: if inflow dwarfs it the
  // window is downstream of real catchment, which is what a trunk river needs.
  const double q_local =
      double(p.runoff_m_per_yr) * double(window_m) * double(window_m) / 31557600.0;
  std::printf("  inflows: %zu crossings, %.4f m3/s total (local rain %.4f m3/s)\n",
              inflows.size(), q_in, q_local);
  int bcount[6] = {0, 0, 0, 0, 0, 0};
  for (float v : biome_out.v) bcount[int(v)]++;

  float blo = 1e30f, bhi = -1e30f, dhi = 0.f;
  for (float v : bed_out.v) { blo = std::min(blo, v); bhi = std::max(bhi, v); }
  for (float v : dep_out.v) dhi = std::max(dhi, v);
  float slo = 1e30f, shi = -1e30f;
  for (float v : bed_src.v) { slo = std::min(slo, v); shi = std::max(shi, v); }
  size_t wet_out = 0;
  for (float v : dep_out.v) wet_out += (v > 0.f);

  std::printf("\n  emitting rank %d: origin cell (%d,%d) = world (%.0f, %.0f) m\n",
              p.rank, sel.cx, sel.cy, sel.cx * cell_m, sel.cy * cell_m);
  std::printf("  bed  src %.1f..%.1f m   out %.1f..%.1f m  (overshoot %+.2f / %+.2f)\n",
              slo, shi, blo, bhi, bhi - shi, blo - slo);
  std::printf("  water: %zu of %d texels wet (%.2f%%), max depth %.1f m\n",
              wet_out, p.out_res * p.out_res,
              100.0 * wet_out / (double(p.out_res) * p.out_res), dhi);
  for (const LakeSurface& s : surfaces) {
    if (s.cells < 4) continue;
    std::printf("    lake %2d: %4d src cells, surface %.2f m (levelness %.4f m)\n",
                s.id, s.cells, s.surface_m, s.spread_m);
  }
  std::printf("  biomes: lake %.1f%%  plains %.1f%%  hills %.1f%%  mountain %.1f%%\n",
              100.0 * bcount[0] / biome_out.v.size(),
              100.0 * bcount[3] / biome_out.v.size(),
              100.0 * bcount[4] / biome_out.v.size(),
              100.0 * bcount[5] / biome_out.v.size());
  std::printf("\n  wrote %s/window-{height,water,Q,discharge}.f32  (kernel: %s)\n"
              "        %s/{height.f32,level.f32,soil.f32,biome.u8,map.txt,inflows.txt}\n"
              "  render: python3 tools/protogen/show.py %s %d %.0f\n"
              "  view:   ./build/badlands_mapview --load %s\n",
              p.out.c_str(), KernelName(p.kernel), p.out.c_str(), p.out.c_str(),
              p.out_res, window_m, p.out.c_str());
  return 0;
}
