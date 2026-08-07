// Phase-1: the Eulerian shallow-water fluid core -- virtual-pipes SWE over
// the grid phase-0 (protogen.cpp) hands off, GPU-shaped by construction.
//
// THE PASS LIST IS THE FROZEN FUTURE GPU DISPATCH ORDER:
//   SweFlux -> SweDepth -> SweVelocity, repeated `swe_substeps` times per
//   RunSweCycles cycle, with an (currently empty) morpho hook after the
//   substep loop.
// Each pass is a standalone function, gather-only Jacobi over the whole
// grid via badlands::ParallelFor -- every cell reads only FRONT-buffer state
// (its neighbours' depth/flux as they stood when the pass began) and writes
// only its own back-buffer/own slot. NO atomics, NO cross-cell write, NO
// serial ordering dependency between cells. The ParallelFor call itself is
// the barrier between passes (it blocks until every cell is done), which is
// what makes "SweDepth reads the flux SweFlux just finished writing" safe
// despite both running on a thread pool.
//
// Units are SI throughout this file: metres, seconds, m/s, m^3/s. `bed`
// (metres) is computed on the fly as `height[i] * relief_m` wherever needed
// -- `Grid::height` itself stays the existing dimensionless ~[0,1] raster,
// untouched by anything here.
//
// build: see the repo's tools/protogen/README.md and this file's own
// CLAUDE.md-documented build line; Taskflow is header-only so this stays a
// standalone TU, same as protogen.cpp.

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "protogen.hpp"

#include "core/parallel.hpp"

using namespace pg;

namespace {

// Real gravity -- an acceleration, matching protogen.cpp's own kGravityMS2.
// Duplicated rather than shared: both are the same literal physical constant
// with their own local WHY comment, not a magic number one file could drift
// out of sync with by editing only the other.
constexpr float kGravityMS2 = 9.81f;

// Face direction order, shared by every pass in this file so a face index
// means the same thing everywhere: 0 = +x, 1 = -x, 2 = +y, 3 = -y. Matches
// protogen.cpp's Diffuse/SettleSus dx4/dy4 convention.
constexpr int kDx[4] = {1, -1, 0, 0};
constexpr int kDy[4] = {0, 0, 1, -1};
// Opposite-face index: the face that carries flow back the other way across
// the same edge. Pairs (0,1) and (2,3) are each other's opposite, hence xor 1.
inline int Opposite(int k) { return k ^ 1; }

}  // namespace

// SweFlux/SweDepth/SweVelocity live in `namespace pg` (not the anonymous
// namespace the helpers above and below use) because protogen.hpp declares
// them there: protogen_tests.cpp's SweLakeMomentumDissipation drives the
// three passes directly, substep by substep, rather than only through
// RunSweCycles, so they need real cross-TU linkage.
namespace pg {

// ------------------------------------------------------------------ swe_flux

// Per-cell face outflow update: virtual-pipes acceleration from the `bed+h`
// head difference, implicit Manning drag, then the export clamp.
//
// READS (front buffer only): own `flux[i]` (previous value, used as the
// pipe's existing momentum), own `h[i]`/`height[i]`, and EACH NEIGHBOUR's
// `h[j]`/`height[j]` -- never a neighbour's `flux`. WRITES: only `flux[i]`,
// for the single `i` this thread owns. Because no cell ever reads another
// cell's `flux` here, and `h`/`height` are untouched by this pass (SweDepth
// writes `h_b`, not `h`; nothing here writes `height`), two cells running
// concurrently can never observe a partially-written neighbour -- the single
// `flux` buffer needs no ping-pong (see the Grid::flux comment in
// protogen.hpp for the same argument stated at the field).
//
// Wet-dry front: a cell with `h <= eps_wet` is treated as having no water to
// send -- ALL FOUR of its outflow faces are forced to exactly zero, not just
// left to decay via the depth factor already in the flux law. Without the
// explicit zero, a cell that was wet a moment ago could keep exporting on
// the momentum term (`flux[i][k]` carried over from the previous substep)
// after draining below eps_wet, which is exactly the synthetic pressure
// spike/negative-depth risk the wet-dry test (SweWetDryFrontStability)
// exists to catch.
//
// Border: a face pointing off the n x n array (x/y < 0 or >= n) uses a GHOST
// neighbour at the border cell's OWN bed with h = 0 -- an always-open drain
// to base level (design decision: the whole array border is open outflow,
// matching the map-edge escape PriorityFlood already assumes elsewhere in
// this codebase). This cell never receives an inflow FROM that ghost
// (SweDepth only gathers from real grid neighbours), so the water genuinely
// leaves the simulated domain; SweDepth books the volume to
// `swe_border_outflow_m3`.
void SweFlux(Grid& g, const Params& p, float dt) {
  const int n = g.n;
  const float cell_m = p.world_m / float(p.res);
  badlands::ParallelFor(size_t(n), [&](size_t yy) {
    const int y = int(yy);
    for (int x = 0; x < n; ++x) {
      const size_t i = g.idx(x, y);
      const float own_h = g.h[i];

      if (own_h <= p.eps_wet) {
        g.flux[i] = {0.f, 0.f, 0.f, 0.f};
        continue;
      }

      const float bed_i = g.height[i] * p.relief_m;
      const float head_i = bed_i + own_h;
      // A_pipe = cell_m * own_h (pipe cross-section: cell width x donor
      // depth), L = cell_m (distance between cell centres), so A_pipe/L
      // reduces to own_h exactly -- the donor's own depth is what scales the
      // acceleration term, which is what makes a near-empty cell's flux
      // vanish even before the eps_wet gate above bites.
      const float A_over_L = own_h;
      // Implicit Manning drag, same closed-form as Descend's (protogen.cpp,
      // "Explicit Euler diverges whenever dt exceeds the drag relaxation
      // time" comment) -- B = g*n^2/depth^(4/3), solved as the quadratic
      // B*dt*v^2 + v - v_free = 0 rather than integrated explicitly, because
      // the drag relaxation time here is routinely far shorter than a CFL
      // substep's dt (shallow, rough cells relax in milliseconds).
      const float B = kGravityMS2 * p.swe_manning_n * p.swe_manning_n /
                      std::pow(own_h, 4.0f / 3.0f);

      std::array<float, 4> f{};
      for (int k = 0; k < 4; ++k) {
        const int nx = x + kDx[k], ny = y + kDy[k];
        float bed_j, h_j;
        if (nx < 0 || ny < 0 || nx >= n || ny >= n) {
          bed_j = bed_i;  // ghost: this cell's own bed, h = 0 (open border)
          h_j = 0.f;
        } else {
          const size_t j = g.idx(nx, ny);
          bed_j = g.height[j] * p.relief_m;
          h_j = g.h[j];
        }
        const float head_j = bed_j + h_j;
        const float dhead = head_i - head_j;

        // Free (undamped) flux from existing momentum plus this substep's
        // acceleration; clamped at 0 because this face stores OUTFLOW only
        // -- a downhill neighbour (dhead < 0) sends water back to US via
        // ITS OWN opposite-facing entry, not by this face going negative.
        float f_free = g.flux[i][k] + dt * kGravityMS2 * A_over_L * dhead;
        if (f_free < 0.f) f_free = 0.f;

        float f_new = 0.f;
        if (f_free > 0.f) {
          const float A_pipe = cell_m * own_h;
          const float v_free = f_free / A_pipe;
          const float bd = B * dt;
          const float v = (bd > 1e-12f)
                              ? (std::sqrt(1.0f + 4.0f * bd * v_free) - 1.0f) /
                                    (2.0f * bd)
                              : v_free;
          f_new = v * A_pipe;
        }
        f[k] = f_new;
      }

      // Export clamp: this substep's total outflow may not exceed the water
      // the cell actually holds, or SweDepth would drive depth negative.
      // Scaling all four faces by the SAME factor preserves their relative
      // split (a cell draining mostly east keeps draining mostly east, just
      // slower) rather than favouring whichever face happened to be
      // computed... there is no "computed first" here since all four were
      // already resolved above; the scale is applied uniformly regardless.
      const float total_out = f[0] + f[1] + f[2] + f[3];
      const float avail_m3 = own_h * cell_m * cell_m;
      if (total_out > 0.f && total_out * dt > avail_m3) {
        const float scale = avail_m3 / (total_out * dt);
        for (int k = 0; k < 4; ++k) f[k] *= scale;
      }
      g.flux[i] = f;
    }
  });
}

// ----------------------------------------------------------------- swe_depth

// Depth update: divergence of the fluxes SweFlux just finished (own outflow
// vs. each real neighbour's matching inflow), plus rain, minus evaporation,
// with border outflow ledgered separately from the ordinary neighbour
// exchange.
//
// READS (front buffer only): own `flux[i]` (all 4 faces, just written by
// SweFlux) and EACH REAL NEIGHBOUR's `flux[j]` (specifically the face
// pointing back at this cell -- see `Opposite`); own `h[i]` for the
// wet-for-evaporation test. WRITES: only `h_b[i]`, for the single `i` this
// thread owns, until the swap at the very end. Two cells running
// concurrently only ever read each other's `flux` (untouched by this pass)
// and never each other's `h_b`, so there is no read/write hazard.
//
// Rain and evaporation both use REAL rates (`runoff_m_per_yr`,
// `evaporation_m_per_yr` converted to m/s), matching the design decision
// that phase-1 runs on the actual climate clock rather than a landscape-step
// abstraction. Evaporation is gated on `h[i] > eps_wet` at the START of this
// substep (the front-buffer depth, same gate SweFlux's donor check uses) --
// not on the provisional post-rain/inflow depth -- so the decision is a pure
// function of Jacobi-front state and cannot depend on this same pass's own
// in-flight result. It is then clamped so it cannot remove more water than
// is actually present after rain/inflow are applied, which is the ONLY
// place this pass can produce a negative depth (the export clamp in SweFlux
// already guarantees the OUTFLOW term alone cannot).
void SweDepth(Grid& g, const Params& p, float dt) {
  const int n = g.n;
  const float cell_m = p.world_m / float(p.res);
  const float cell_area = cell_m * cell_m;
  const float rain_rate = p.runoff_m_per_yr / float(kSecondsPerYear);
  const float evap_rate = p.evaporation_m_per_yr / float(kSecondsPerYear);

  // Per-row ledger partials, combined serially in row order after the
  // parallel pass -- same shape as SettleSus's border-loss accumulation, and
  // for the same reason: a fixed combine order is what makes the ledger
  // reproducible regardless of how ParallelFor happened to schedule rows.
  std::vector<double> row_rain(n, 0.0), row_evap(n, 0.0), row_border(n, 0.0);

  badlands::ParallelFor(size_t(n), [&](size_t yy) {
    const int y = int(yy);
    double rain_acc = 0.0, evap_acc = 0.0, border_acc = 0.0;
    for (int x = 0; x < n; ++x) {
      const size_t i = g.idx(x, y);
      const float own_out =
          g.flux[i][0] + g.flux[i][1] + g.flux[i][2] + g.flux[i][3];

      float inflow = 0.f;
      for (int k = 0; k < 4; ++k) {
        const int nx = x + kDx[k], ny = y + kDy[k];
        if (nx < 0 || ny < 0 || nx >= n || ny >= n) {
          // This face's share of own_out left the map -- no grid neighbour
          // absorbs it. Booked below, once per face rather than re-derived,
          // by walking the same four directions again for clarity.
          continue;
        }
        const size_t j = g.idx(nx, ny);
        inflow += g.flux[j][Opposite(k)];
      }
      // Border outflow: the faces above that pointed off-grid, summed
      // directly from this cell's own flux rather than inferred from
      // own_out - (grid-neighbour sum), so the ledger term is exact even if
      // a future change reorders the loop above.
      float border_leaving = 0.f;
      for (int k = 0; k < 4; ++k) {
        const int nx = x + kDx[k], ny = y + kDy[k];
        if (nx < 0 || ny < 0 || nx >= n || ny >= n) border_leaving += g.flux[i][k];
      }

      float h_prov = g.h[i] + dt * (inflow - own_out) / cell_area + rain_rate * dt;
      if (h_prov < 0.f) h_prov = 0.f;  // safety net; the export clamp should
                                       // already prevent this from the
                                       // outflow term alone (see file header)

      const bool wet = g.h[i] > p.eps_wet;
      const float evap_amt = wet ? std::min(evap_rate * dt, h_prov) : 0.f;
      g.h_b[i] = h_prov - evap_amt;

      rain_acc += double(rain_rate) * double(dt) * double(cell_area);
      evap_acc += double(evap_amt) * double(cell_area);
      border_acc += double(border_leaving) * double(dt);
    }
    row_rain[y] = rain_acc;
    row_evap[y] = evap_acc;
    row_border[y] = border_acc;
  });

  for (int y = 0; y < n; ++y) {
    g.swe_rain_in_m3 += row_rain[y];
    g.swe_evap_out_m3 += row_evap[y];
    g.swe_border_outflow_m3 += row_border[y];
  }

  g.h.swap(g.h_b);
}

// -------------------------------------------------------------- swe_velocity

// Cell-centred velocity from a cell's OWN pair of opposing face fluxes --
// e.g. velx from (outflow +x) - (outflow -x), divided by (depth * cell
// width) to undo exactly the A_pipe scaling SweFlux applied when it turned
// a face velocity into a flux (flux = v * cell_m * own_h), so this recovers
// v_(+x) - v_(-x) algebraically, not merely dimensionally.
//
// READS: only `flux[i]` and `h[i]` for the single `i` this thread owns --
// no neighbour access at all, so this pass has no cross-cell hazard to
// state beyond "distinct threads own distinct i". WRITES: only
// `velx[i]`/`vely[i]`.
//
// Guarded for h -> 0: below eps_wet, velocity is forced to exactly zero
// rather than computed and divided by a near-zero depth -- SweFlux's own
// eps_wet gate already keeps flux at 0 there, so this guard is redundant in
// theory but not something this pass may assume; it is its own defence
// against a divide-by-near-zero producing a spurious large "velocity" from
// float noise in an otherwise-zero flux.
void SweVelocity(Grid& g, const Params& p) {
  const int n = g.n;
  const float cell_m = p.world_m / float(p.res);
  badlands::ParallelFor(size_t(n), [&](size_t yy) {
    const int y = int(yy);
    for (int x = 0; x < n; ++x) {
      const size_t i = g.idx(x, y);
      const float hv = g.h[i];
      if (hv <= p.eps_wet) {
        g.velx[i] = 0.f;
        g.vely[i] = 0.f;
        continue;
      }
      const float denom = hv * cell_m;
      g.velx[i] = (g.flux[i][0] - g.flux[i][1]) / denom;
      g.vely[i] = (g.flux[i][2] - g.flux[i][3]) / denom;
    }
  });
}

}  // namespace pg

namespace {

// ------------------------------------------------------- deterministic CFL

// Result of the once-per-cycle max-depth reduction: the value the CFL dt is
// based on, and whether any non-finite `h`/`height` was found along the way.
struct ReduceResult {
  float h_max = 0.f;
  bool nonfinite = false;
};

// Deterministic, chunked max-of-depth reduction that doubles as the NaN/Inf
// tripwire.
//
// Chunk boundaries are a FIXED function of `g.cells` alone (not of thread
// count or runtime scheduling), each chunk is scanned serially by whichever
// worker picks it up, and the per-chunk results are combined in a plain
// serial loop over chunk INDEX 0..kChunks-1 -- never by racing writers into
// a shared accumulator. Max is associative/commutative for ordinary floats,
// so a fixed chunking would already be enough for the h_max value alone; the
// fixed order matters for the OTHER half of this function's job, the
// non-finite check: std::max's behaviour when one operand is NaN depends on
// argument order (`std::max(NaN, x)` and `std::max(x, NaN)` disagree), so
// this does not lean on max()'s NaN handling at all -- it checks
// `std::isfinite` explicitly on every element and ORs the flag, which has no
// such ordering hazard, and is why the chunk order still needs to be fixed:
// bit-exact reproducibility (SweDeterminism) must not depend on which worker
// happened to grab which chunk first.
ReduceResult DeterministicMaxDepth(const Grid& g, const Params& p) {
  constexpr int kChunks = 64;
  const int chunks = int(std::min<size_t>(kChunks, std::max<size_t>(1, g.cells)));
  std::vector<float> chunk_max(chunks, -1e30f);
  std::vector<uint8_t> chunk_bad(chunks, 0);
  const size_t cells = g.cells;
  badlands::ParallelFor(size_t(chunks), [&](size_t c) {
    const size_t begin = cells * c / size_t(chunks);
    const size_t end = cells * (c + 1) / size_t(chunks);
    float m = -1e30f;
    bool bad = false;
    for (size_t i = begin; i < end; ++i) {
      const float hv = g.h[i];
      const float bedv = g.height[i] * p.relief_m;
      if (!std::isfinite(hv) || !std::isfinite(bedv)) {
        bad = true;
      } else if (hv > m) {
        m = hv;
      }
    }
    chunk_max[c] = m;
    chunk_bad[c] = bad ? 1 : 0;
  });
  ReduceResult r;
  for (int c = 0; c < chunks; ++c) {
    if (chunk_bad[c]) r.nonfinite = true;
    if (chunk_max[c] > r.h_max) r.h_max = chunk_max[c];
  }
  return r;
}

// --------------------------------------------------------- abort snapshot

// Minimal raw-.f32 dump used ONLY by RunSweCycles's abort path, so an
// operator can look at the state a tripwire fired on. Deliberately not
// protogen.cpp's `Dump` (anonymous-namespace, not reachable from this TU,
// and scoped to the full phase-0+phase-1 raster set Task 7 defines) --
// this writes just the fields this file owns.  No-op when `p.out` is empty,
// which is how every test in this file runs (see protogen_tests.cpp's SWE
// fixtures), so a tripwire test never touches the filesystem.
void WriteAbortSnapshot(const Grid& g, const Params& p, int cycle) {
  if (p.out.empty()) return;
  std::error_code ec;
  std::filesystem::create_directories(p.out, ec);
  if (ec) {
    std::fprintf(stderr, "protogen-swe: cannot create out dir '%s': %s\n",
                p.out.c_str(), ec.message().c_str());
    return;
  }
  auto write = [&](const char* name, const std::vector<float>& f) {
    char path[512];
    std::snprintf(path, sizeof(path), "%s/abort-%04d-%s.f32", p.out.c_str(),
                  cycle, name);
    FILE* fp = std::fopen(path, "wb");
    if (!fp) {
      std::fprintf(stderr, "protogen-swe: cannot write %s\n", path);
      return;
    }
    std::fwrite(f.data(), sizeof(float), f.size(), fp);
    std::fclose(fp);
  };
  write("h", g.h);
  write("velx", g.velx);
  write("vely", g.vely);
}

}  // namespace

namespace pg {

SweRunResult RunSweCycles(Grid& g, const Params& p, int cycles,
                          SimStats* stats) {
  const float cell_m = p.world_m / float(p.res);
  SweRunResult result;

  for (int cycle = 0; cycle < cycles; ++cycle) {
    const auto t0 = std::chrono::steady_clock::now();
    const ReduceResult red = DeterministicMaxDepth(g, p);
    const auto t1 = std::chrono::steady_clock::now();
    if (stats)
      stats->t_swe_reduce += std::chrono::duration<double>(t1 - t0).count();

    if (red.nonfinite) {
      result.ok = false;
      result.aborted_cycle = cycle;
      result.reason = "cycle " + std::to_string(cycle) +
                      ": non-finite h or height detected by the CFL reduction";
      WriteAbortSnapshot(g, p, cycle);
      return result;
    }

    // h_max floored at eps_wet: a bone-dry grid has zero wave speed, which
    // would divide by zero below. Flooring at the wet threshold gives a
    // finite, bounded dt instead of an arbitrary one -- physically, nothing
    // is moving fast enough to constrain dt tighter than "a barely-wet cell
    // would."
    const float h_for_cfl = std::max(red.h_max, p.eps_wet);
    const float dt =
        p.cfl_number * cell_m / std::sqrt(kGravityMS2 * h_for_cfl);

    if (dt < p.dt_floor_s) {
      result.ok = false;
      result.aborted_cycle = cycle;
      char buf[192];
      std::snprintf(buf, sizeof(buf),
                    "cycle %d: dt-floor -- CFL dt %.3e s below dt_floor_s %.3e s",
                    cycle, double(dt), double(p.dt_floor_s));
      result.reason = buf;
      WriteAbortSnapshot(g, p, cycle);
      return result;
    }

    for (int s = 0; s < p.swe_substeps; ++s) {
      auto ta = std::chrono::steady_clock::now();
      SweFlux(g, p, dt);
      auto tb = std::chrono::steady_clock::now();
      SweDepth(g, p, dt);
      auto tc = std::chrono::steady_clock::now();
      SweVelocity(g, p);
      auto td = std::chrono::steady_clock::now();
      if (stats) {
        stats->t_swe_flux += std::chrono::duration<double>(tb - ta).count();
        stats->t_swe_depth += std::chrono::duration<double>(tc - tb).count();
        stats->t_swe_velocity += std::chrono::duration<double>(td - tc).count();
      }
    }

    // Morpho hook -- EMPTY. Task 6 fills this in with SedExchange/SedAdvect/
    // TalusFlux+TalusApply, run once per cycle (not per substep) after the
    // fluid has advanced `swe_substeps` times. MORFAC clamp scaffolding:
    // when that lands, the bed delta it produces is scaled by `morfac` in
    // exactly one place and clamped to a bounded fraction of local depth
    // before it is applied -- inert here because nothing above writes bed.
  }

  return result;
}

}  // namespace pg
