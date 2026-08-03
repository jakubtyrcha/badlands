// THROWAWAY PROTOTYPE -- selection: pick a gameplay window out of a protogen
// world.
//
// Reads the .f32 rasters protogen dumped (height/water/Q), scans every
// candidate window, keeps the ones that satisfy all four gameplay criteria,
// ranks the survivors, and prints an origin to pin -- in source cells and in
// world metres.
//
// EXTRACTION LIVES ELSEWHERE NOW. Resampling a picked origin into a gameplay
// patch (bed/water reconstruction, biome classification, the mapview load
// set) is src/mapgen/coarse_world_patch_source.hpp, used through
// `badlands_mapview --load <coarse-dir> --patch-size/--patch-res/--patch-origin`.
// This tool's whole job ends at printing the origin to pass to
// --patch-origin; it never resamples anything itself.
//
// Geometry (resolution, world size) is read from <dump-dir>/world.txt --
// see src/mapgen/coarse_io.hpp for the format -- not passed on argv. That
// keeps this tool honest about which world it is scanning; --res/--world
// remain as an explicit override for a dump that predates world.txt.
//
// build (standalone, no CMake, no deps beyond the stdlib):
//   c++ -O3 -std=c++23 select.cpp -o select
//
// use:
//   select --in <dump-dir> --tag 3000-step
//   select --test

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;

// ---------------------------------------------------------------- parameters

struct WinParams {
  // source world
  std::string in;
  std::string tag = "3000-step";
  int src_res = 1024;
  float src_world_m = 16384.0f;

  // gameplay window to scan for, in world metres. Alignment to the source
  // grid is no longer this tool's problem -- extraction resamples, so a
  // window need not land on a whole number of source cells here.
  float window_m = 1024.0f;

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

  // WHICH WINDOW TO EMIT. Prefer --origin-cell: `rank` is an ordinal into a list
  // that RESHUFFLES whenever the source map or any gate changes, so the same
  // --rank silently points at a different place after a re-run. Ranking is for
  // DISCOVERY; once a location is chosen, pin it by coordinates.
  int rank = 0;
  int origin_x = -1, origin_y = -1;  // >= 0 => report exactly this window
  int report = 10;
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
    std::fprintf(stderr, "select: cannot open %s\n", path.c_str());
    return false;
  }
  out = Field(n);
  const size_t want = size_t(n) * n;
  const size_t got = std::fread(out.v.data(), sizeof(float), want, fp);
  std::fclose(fp);
  if (got != want) {
    std::fprintf(stderr, "select: %s is %zu floats, expected %zu (wrong --res?)\n",
                 path.c_str(), got, want);
    return false;
  }
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

int RunAll() {
  std::printf("select sanity\n");
  PlainsExcludeWater();
  GatesReject();
  SuppressionDedups();
  std::printf("\n  %d passed, %d failed\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}

}  // namespace test

}  // namespace

int main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i)
    if (std::string(argv[i]) == "--test") return test::RunAll();

  WinParams p;
  int res_override = 0;
  float world_override = 0.0f;
  try {
    for (int i = 1; i < argc; ++i) {
      const std::string a = argv[i];
      auto nxt = [&]() -> std::string { return (i + 1 < argc) ? argv[++i] : ""; };
      if (a == "--in") p.in = nxt();
      else if (a == "--tag") p.tag = nxt();
      else if (a == "--res") res_override = std::stoi(nxt());
      else if (a == "--world") world_override = std::stof(nxt());
      else if (a == "--window-m") p.window_m = std::stof(nxt());
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
      else if (a == "--rank") p.rank = std::stoi(nxt());
      else if (a == "--origin-cell") {
        const std::string v = nxt();
        const size_t comma = v.find(',');
        if (comma == std::string::npos) {
          std::fprintf(stderr, "select: --origin-cell wants X,Y in source cells\n");
          return 2;
        }
        p.origin_x = std::stoi(v.substr(0, comma));
        p.origin_y = std::stoi(v.substr(comma + 1));
      }
      else if (a == "--report") p.report = std::stoi(nxt());
      else {
        std::fprintf(stderr, "select: unknown arg '%s'\n", a.c_str());
        return 2;
      }
    }
  } catch (const std::exception& e) {
    std::fprintf(stderr, "select: bad or missing argument value (%s)\n", e.what());
    return 2;
  }

  if (p.in.empty()) {
    std::fprintf(stderr, "select: --in <dump-dir> is required\n");
    return 2;
  }

  // Geometry comes from <in>/world.txt (src/mapgen/coarse_io.hpp's format:
  // "key value" per line, '#' comments, unknown keys ignored for forward
  // compatibility) unless --res/--world override it explicitly -- e.g. for a
  // dump that predates world.txt.
  int res = res_override;
  float world = world_override;
  if (res <= 0 || !(world > 0.0f)) {
    const std::string path = p.in + "/world.txt";
    std::ifstream f(path);
    if (!f) {
      std::fprintf(stderr,
                   "select: cannot open %s (pass --res/--world to override)\n",
                   path.c_str());
      return 1;
    }
    int file_res = 0;
    float file_world = 0.0f;
    std::string line;
    while (std::getline(f, line)) {
      std::istringstream ls(line);
      std::string key;
      if (!(ls >> key) || key.empty() || key[0] == '#') continue;
      if (key == "resolution") ls >> file_res;
      else if (key == "world_size_m") ls >> file_world;
      // Every other key (texel_m, seed, runoff_m_per_yr, ...) is ignored on
      // purpose -- forward compatible with world.txt gaining fields.
    }
    if (res <= 0) res = file_res;
    if (!(world > 0.0f)) world = file_world;
  }
  if (res <= 0 || !(world > 0.0f)) {
    std::fprintf(stderr,
                 "select: %s/world.txt missing resolution/world_size_m (or pass "
                 "--res/--world)\n",
                 p.in.c_str());
    return 2;
  }
  p.src_res = res;
  p.src_world_m = world;

  const float cell_m = p.src_world_m / float(p.src_res);
  const int win_cells = std::max(1, int(std::lround(double(p.window_m) / cell_m)));
  if (win_cells > p.src_res) {
    std::fprintf(stderr,
                 "select: %.0f m window is %d source cells, exceeds the %d-cell "
                 "source\n",
                 p.window_m, win_cells, p.src_res);
    return 2;
  }

  Field h, w, q;
  const std::string base = p.in + "/" + p.tag + "-";
  if (!LoadField(base + "height.f32", p.src_res, h)) return 1;
  if (!LoadField(base + "water.f32", p.src_res, w)) return 1;
  if (!LoadField(base + "Q.f32", p.src_res, q)) return 1;

  std::printf("select: source %dx%d at %.1f m (%.0f m world)\n"
              "  window %.0f m = %d source cells\n",
              p.src_res, p.src_res, cell_m, p.src_world_m, p.window_m, win_cells);
  std::printf("  gates: relief >= %.0f m, plain >= %.0f%% of centre %.0f m "
              "(dry & < %.0f deg),\n         lake >= %d cells, channel >= %d cells "
              "at Q >= %.3f m3/s\n",
              p.min_relief_m, p.min_plain_frac * 100.f, p.window_m * p.centre_frac,
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
  // Only fatal when we are RANKING. A pinned origin is reported whether or not
  // anything qualifies -- that is the whole contract of pinning, and bailing
  // here contradicted it the moment a gate was tightened.
  if (distinct.empty() && p.origin_x < 0 && p.origin_y < 0) {
    std::fprintf(stderr, "select: nothing passes -- relax a gate, or pin one "
                         "with --origin-cell X,Y\n");
    return 1;
  }
  PrintCandidates(distinct, p, p.report);

  Candidate sel;
  if (p.origin_x >= 0 || p.origin_y >= 0) {
    if (p.origin_x < 0 || p.origin_y < 0 ||
        p.origin_x + win_cells > p.src_res || p.origin_y + win_cells > p.src_res) {
      std::fprintf(stderr,
                   "select: --origin-cell %d,%d does not fit a %d-cell window in "
                   "a %d-cell source\n",
                   p.origin_x, p.origin_y, win_cells, p.src_res);
      return 2;
    }
    // A named place is reported whether or not it qualifies -- the gates are
    // for DISCOVERY. Scored directly rather than looked up: the scan only
    // visits stride-aligned origins, so a pinned one off that grid is simply
    // absent.
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
      std::fprintf(stderr, "select: --rank %d out of range (0..%zu)\n", p.rank,
                   distinct.size() - 1);
      return 2;
    }
    sel = distinct[size_t(p.rank)];
    std::printf("\n  NOTE: --rank is an ordinal into a list that reshuffles when "
                "the map\n        or the gates change. Pin this window with "
                "--origin-cell %d,%d\n        to keep it across re-runs.\n",
                sel.cx, sel.cy);
  }

  const float origin_x_m = sel.cx * cell_m;
  const float origin_y_m = sel.cy * cell_m;
  std::printf("\n  origin (%d,%d) source cells = world (%.0f, %.0f) m\n"
              "  extraction lives in src/mapgen/coarse_world_patch_source.hpp -- "
              "view it with:\n"
              "    ./build/badlands_mapview --load %s --patch-size %.0f "
              "--patch-origin %.0f,%.0f\n",
              sel.cx, sel.cy, origin_x_m, origin_y_m, p.in.c_str(), p.window_m,
              origin_x_m, origin_y_m);
  return 0;
}
