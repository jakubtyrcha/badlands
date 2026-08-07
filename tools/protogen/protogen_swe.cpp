// Phase-1: the Eulerian shallow-water fluid core -- virtual-pipes SWE over
// the grid phase-0 (protogen.cpp) hands off, GPU-shaped by construction.
//
// THE PASS LIST IS THE FROZEN FUTURE GPU DISPATCH ORDER:
//   SweFlux -> SweDepth -> SweVelocity, repeated `swe_substeps` times per
//   RunSweCycles cycle, then ONCE per cycle the morpho group
//   SedExchange -> SedAdvect -> TalusFlux -> TalusApply (Task 6; skipped
//   entirely when `morfac` is 0, which is what makes a fluid-only run).
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
// THE Flux -> Depth -> Velocity ORDER IS NOT JUST A CONVENTION, IT IS A
// DEPENDENCY: SweVelocity divides by `h_b`, which only holds "the depth
// SweFlux used" because SweDepth's `h`/`h_b` swap ran in between (see
// SweVelocity's own comment). Calling these three out of order, or calling
// SweVelocity on its own after some other sequence, reads stale or wrong
// state. RunSweCycles's substep loop is the one place this order is allowed
// to be assumed; tests that drive the passes directly (see
// protogen_tests.cpp) must replicate it exactly.
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
#include "mapgen/erosion.hpp"
#include "mapgen/field2d.hpp"
#include "mapgen/hydrology.hpp"

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

// ------------------------------------------------------------- units seam
//
// THIS FILE IS SI. `protogen.hpp`'s Grid is NOT: `height`, `soil` and `sus`
// are all stored in HEIGHT UNITS, where one unit is `relief_m` metres (see
// Grid::height's own comment). Every quantity in the morpho passes below --
// capacity, the exchange, talus transfers -- is a length of SOLID in metres,
// so every read of `height`/`soil`/`sus` and every write back to them crosses
// that seam.
//
// THE SEAM GOES THROUGH THESE TWO FUNCTIONS AND NOWHERE ELSE in the morpho
// code. That is a deliberate rule, not a stylistic preference: a stray
// `* p.relief_m` on one side of an exchange and a missing one on the other is
// a silent mass bug that no amount of staring at a raster reveals -- it looks
// exactly like "erosion is a bit stronger than expected". Keeping the
// conversion in one named place means the ledger can be traced by reading two
// lines instead of auditing every arithmetic expression.
//
// (The FLUID passes above predate this and spell `g.height[i] * p.relief_m`
// inline where they need a bed elevation. They only ever READ `height`, never
// write it, so they cannot produce an asymmetric conversion -- the failure
// mode this seam exists to prevent does not apply to them.)
inline float ToMetres(float height_units, const Params& p) {
  return height_units * p.relief_m;
}
inline float ToHeightUnits(float metres, const Params& p) {
  return (p.relief_m > 0.f) ? metres / p.relief_m : 0.f;
}

// ------------------------------------------------- talus neighbourhood (8)
//
// The talus pair uses the EIGHT-neighbourhood, matching phase-0's `Cascade`
// (protogen.cpp) so both express the same repose angle over the same
// directions -- a 4-neighbour avalanche relaxes to a visibly different,
// axis-aligned shape. Row-major from (-1,-1) to (+1,+1), which makes the
// opposite direction of index k exactly 7 - k (the ordering is symmetric
// under negation by construction).
constexpr int kTdx[8] = {-1, 0, 1, -1, 1, -1, 0, 1};
constexpr int kTdy[8] = {-1, -1, -1, 0, 0, 1, 1, 1};
inline int TalusOpposite(int k) { return 7 - k; }

// ------------------------------------------------------- deterministic sum
//
// Fixed-chunk parallel sum, combined in a plain serial loop over chunk INDEX
// -- the same construction (and the same reasons) as DeterministicMaxDepth
// further down: chunk boundaries are a function of `cells` alone, never of
// thread count or scheduling, so the result is bit-reproducible run to run.
// Floating-point addition is NOT associative, so unlike the max reduction
// this one genuinely needs the fixed order for its VALUE and not merely for
// its tripwire flag. Accumulates in double from float inputs.
template <typename F>
double DeterministicSum(size_t cells, F&& fn) {
  constexpr int kChunks = 64;
  const int chunks = int(std::min<size_t>(kChunks, std::max<size_t>(1, cells)));
  std::vector<double> part(size_t(chunks), 0.0);
  badlands::ParallelFor(size_t(chunks), [&](size_t c) {
    const size_t begin = cells * c / size_t(chunks);
    const size_t end = cells * (c + 1) / size_t(chunks);
    double s = 0.0;
    for (size_t i = begin; i < end; ++i) s += fn(i);
    part[c] = s;
  });
  double total = 0.0;
  for (int c = 0; c < chunks; ++c) total += part[size_t(c)];
  return total;
}

// Fixed-chunk parallel max, same construction and same reason.
template <typename F>
float DeterministicMax(size_t cells, F&& fn) {
  constexpr int kChunks = 64;
  const int chunks = int(std::min<size_t>(kChunks, std::max<size_t>(1, cells)));
  std::vector<float> part(size_t(chunks), 0.f);
  badlands::ParallelFor(size_t(chunks), [&](size_t c) {
    const size_t begin = cells * c / size_t(chunks);
    const size_t end = cells * (c + 1) / size_t(chunks);
    float m = 0.f;
    for (size_t i = begin; i < end; ++i) {
      const float v = fn(i);
      if (v > m) m = v;
    }
    part[c] = m;
  });
  float total = 0.f;
  for (int c = 0; c < chunks; ++c) total = std::max(total, part[size_t(c)]);
  return total;
}

// ------------------------------------------------------ Catmull-Rom bicubic
//
// One-dimensional Catmull-Rom (the uniform, tension-1/2 cubic through p1 and
// p2, with the interior slopes taken from p0/p3). Chosen over bilinear for
// SedAdvect's backtrace on purpose: a bilinear semi-Lagrangian step is
// strongly diffusive -- each substep convolves the field with a triangle
// kernel -- and the thing being advected here is a sediment concentration
// whose SHARP EDGES (a bank, a plume front) are the structure the whole
// morphodynamic model exists to produce. Bilinear turns banks into gradients
// and gradients into mud.
//
// The price is that it overshoots: a cubic through a step produces values
// outside the data range, and on a strictly non-negative field like `sus` a
// negative overshoot is nonsense. SedAdvect clips it and accounts for the
// clipped mass -- see there.
//
// EXACT AT t = 0: the expression collapses to 0.5 * 2 * p1 = p1 with no
// rounding, so a zero-displacement backtrace reproduces the source field
// bit-for-bit. StillLakeInert depends on that.
inline float CatmullRom1D(float p0, float p1, float p2, float p3, float t) {
  const float t2 = t * t, t3 = t2 * t;
  return 0.5f * ((2.0f * p1) + (-p0 + p2) * t +
                 (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t2 +
                 (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t3);
}

// Bicubic Catmull-Rom sample of a cell-centred field at a CONTINUOUS position
// (cell centres at (i+0.5, j+0.5), the same convention protogen.cpp's
// SampleField uses). OFF-GRID TAPS READ 0, not a clamped edge value: an
// off-grid tap means the parcel came from outside the simulated domain, and
// outside the domain there is no suspended sediment. Clamping to the edge
// instead would manufacture inflow from a border that has none.
inline float CatmullRomSample(const std::vector<float>& f, int n, float fx,
                              float fy) {
  const float px = fx - 0.5f, py = fy - 0.5f;
  const int x1 = int(std::floor(px)), y1 = int(std::floor(py));
  const float tx = px - float(x1), ty = py - float(y1);
  float col[4];
  for (int j = 0; j < 4; ++j) {
    const int sy = y1 - 1 + j;
    float row[4];
    for (int k = 0; k < 4; ++k) {
      const int sx = x1 - 1 + k;
      row[k] = (sx < 0 || sy < 0 || sx >= n || sy >= n)
                   ? 0.f
                   : f[size_t(sy) * size_t(n) + size_t(sx)];
    }
    col[j] = CatmullRom1D(row[0], row[1], row[2], row[3], tx);
  }
  return CatmullRom1D(col[0], col[1], col[2], col[3], ty);
}

// ------------------------------------------ sub-grid channelization factor
//
// THE STAGE-0 -> STAGE-1 RECONCILIATION (the brief's amendment). SweFlux's
// virtual pipe is `cell_m` wide by construction (its A_pipe is
// `cell_m * own_h`, see SweFlux), so the solver sees every watercourse as
// sheet flow spread across a whole cell. Reality is a channel NARROWER than
// one cell carrying the same discharge: deeper, faster, and applying its
// shear to only a fraction of the cell floor. A capacity built naively on the
// cell-averaged velocity therefore describes neither -- it under-states the
// stress inside the real channel and applies what stress it does compute to
// the entire cell width, which is a recipe for a river that erodes weakly and
// spreads sideways instead of incising. ChannelPersistence is the test that
// measures exactly that failure with this factor switched off.
//
// The factor is the fractional wet width, min(1, w_regime/cell_m), with the
// regime width w = channel_width_coeff*sqrt(Q) this repo already uses
// (Params::channel_width_coeff, src/mapgen/erosion.hpp). It is applied to the
// EROSION demand as an AREAL FRACTION: only that share of the cell floor is
// under a channel's shear, so only that share of the cell-averaged bed
// responds.
//
// Q_local IS THE MAGNITUDE OF THE CELL'S NET DISCHARGE VECTOR, sqrt((f+x -
// f-x)^2 + (f+y - f-y)^2), built from the four face fluxes. NOT the sum of
// the outflow magnitudes, and the difference is the whole point: the sum
// counts a cell that is merely spreading water in every direction -- a lake
// surface, a divergent sheet, a numerical sloshing pair -- as if it were
// conveying a large discharge, and would hand it a wide "channel" that does
// not exist. The net vector is what actually passes THROUGH the cell, which
// is the discharge a regime width is defined on: w = k*sqrt(Q) describes a
// channel conveying Q, not a puddle exchanging Q with itself.
//
// Setting `channel_width_coeff` absurdly large drives the factor to exactly 1
// everywhere, which is how ChannelPersistence runs its factor-disabled
// counterfactual without a second code path.
inline float ChannelizationFactor(const Grid& g, const Params& p, size_t i,
                                  float cell_m) {
  const float qx = g.flux[i][0] - g.flux[i][1];
  const float qy = g.flux[i][2] - g.flux[i][3];
  const float Q = std::sqrt(qx * qx + qy * qy);  // m^3/s through the cell
  if (!(cell_m > 0.f)) return 1.f;
  const float w_regime = p.channel_width_coeff * std::sqrt(Q);
  return std::min(1.0f, w_regime / cell_m);
}

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
      //
      // Solved as v = 2*v_free / (1 + sqrt(1 + 4*bd*v_free)), NOT Descend's
      // own (sqrt(1+4*bd*v_free) - 1) / (2*bd) -- the two are algebraically
      // identical (multiply the latter's numerator and denominator by the
      // conjugate sqrt(...)+1) but not numerically identical: Descend only
      // ever runs in the shallow sheet-flow regime, where that subtraction
      // is safe. Here the SWE state routinely sits at low face velocities in
      // deep water (a barely-perturbed lake), where 4*bd*v_free is small and
      // sqrt(1+x)-1 catastrophically cancels -- measured to collapse to
      // EXACTLY 0.0 (100% error) for v_free ~3e-4 m/s in 20 m water, and 10%
      // error still at ~3e-3 m/s. The 2*v_free/(1+sqrt(...)) form has no
      // subtraction of near-equal quantities at either extreme (bd -> 0 gives
      // v -> v_free exactly; bd large gives v -> sqrt(v_free/bd)), so it
      // needs no bd > 0 special case either -- the denominator is always
      // >= 2.
      const float B = kGravityMS2 * p.swe_manning_n * p.swe_manning_n /
                      std::pow(own_h, 4.0f / 3.0f);
      // Froude bound, PER FACE (see the loop below for why the /sqrt(2)):
      // `flux[i][k]` carries momentum forward from whatever substep set it,
      // and `A_pipe` above is built from `own_h` as it stands THIS substep
      // -- if depth collapsed faster than the carried-over flux did (a
      // fast-draining wet-dry front, exactly SweWetDryFrontStability's
      // fixture), reinterpreting a still-large flux through a now-small
      // A_pipe manufactures a velocity with no physical bound (measured
      // 113 m/s, Froude ~75, in 0.2 m water before this clamp existed).
      // Capping at kMaxFroude x the local wave speed sqrt(g*own_h) closes
      // that without touching ordinary subcritical flow -- see kMaxFroude's
      // own comment (protogen.hpp) for why 10x. Same `own_h` for all 4
      // directions of this cell, so computed once here rather than per face.
      //
      // The /sqrt(2) is load-bearing, not decoration: SweVelocity combines
      // TWO independently-clamped face velocities per axis (velx from the
      // +x/-x pair, vely from +y/-y), so if this per-face cap were exactly
      // kMaxFroude*sqrt(g*h) and both axes simultaneously sat at their own
      // cap, the COMBINED 2-D speed sqrt(velx^2+vely^2) could reach
      // kMaxFroude*sqrt(g*h)*sqrt(2) -- measured exactly (worst ratio
      // 1.4142, i.e. sqrt(2)) the first time this bound was tested without
      // the correction. Capping each face at the bound/sqrt(2) makes the
      // WORST-CASE combination land exactly back on kMaxFroude*sqrt(g*h),
      // which is the number SweWetDryFrontStability actually asserts
      // against.
      const float v_max_per_face =
          kMaxFroude * std::sqrt(kGravityMS2 * own_h) * 0.70710678f;  // /sqrt(2)

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
          float v = (2.0f * v_free) / (1.0f + std::sqrt(1.0f + 4.0f * bd * v_free));
          if (v > v_max_per_face) v = v_max_per_face;
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
// THE DEPTH THAT DIVISION MUST USE IS `h_b`, NOT `h`. SweFlux built
// `flux[i]` from the depth `h` held BEFORE this substep's SweDepth call;
// SweDepth's `g.h.swap(g.h_b)` (the last thing it does) then makes `h` the
// NEW depth and `h_b` the OLD one -- so calling SweVelocity after SweDepth
// (the mandated Flux->Depth->Velocity order) and dividing by `h` recovers
// v_(+x)-v_(-x) using the WRONG denominator, off by a factor of
// h_old/h_new, unbounded by anything but `eps_wet` (measured: a cell
// draining from 1 m to 1 mm between two substeps reported a 1000x-inflated
// velocity; 14% error even on the comparatively gentle wet-dry cliff
// fixture). Reading `h_b` instead recovers the SAME depth SweFlux actually
// used, exactly. THIS IS A REAL COUPLING: SweVelocity depending on `h_b`
// still holding "the depth as of the start of this substep" is only true
// because the pass order is always Flux -> Depth -> Velocity within one
// substep, never reordered and never called standalone after some other
// sequence -- if that dispatch order ever changes, this division must move
// with it.
//
// READS: `flux[i]` and `h_b[i]` for the single `i` this thread owns -- no
// neighbour access at all, so this pass has no cross-cell hazard to state
// beyond "distinct threads own distinct i". WRITES: only
// `velx[i]`/`vely[i]`.
//
// Guarded for h -> 0: below eps_wet (checked on `h_b`, the SAME depth
// SweFlux gated its own donor check on -- a cell SweFlux treated as dry
// produced all-zero flux, so this guard and that one agree by construction),
// velocity is forced to exactly zero rather than computed and divided by a
// near-zero depth.
void SweVelocity(Grid& g, const Params& p) {
  const int n = g.n;
  const float cell_m = p.world_m / float(p.res);
  badlands::ParallelFor(size_t(n), [&](size_t yy) {
    const int y = int(yy);
    for (int x = 0; x < n; ++x) {
      const size_t i = g.idx(x, y);
      const float h_at_flux_time = g.h_b[i];  // see header comment: post-swap,
                                              // h_b holds the PRE-substep depth
      if (h_at_flux_time <= p.eps_wet) {
        g.velx[i] = 0.f;
        g.vely[i] = 0.f;
        continue;
      }
      const float denom = h_at_flux_time * cell_m;
      g.velx[i] = (g.flux[i][0] - g.flux[i][1]) / denom;
      g.vely[i] = (g.flux[i][2] - g.flux[i][3]) / denom;
    }
  });
}

// ---------------------------------------------------------------- warm start

// The phase-0 -> phase-1 handoff (Task 5). ONE-SHOT plain CPU code, called
// EXACTLY ONCE between phase-0's finished bed and phase-1's first
// RunSweCycles call -- see the "why" in protogen.hpp's declaration. NOT a
// Jacobi pass and deliberately NOT on the SweFlux/SweDepth/SweVelocity
// dispatch list above: that list is the frozen future GPU dispatch order,
// this runs once on the CPU before it, using ordinary priority-flood/D8
// algorithms that have no GPU-shaped equivalent here.
void SweWarmStart(Grid& g, const Params& p) {
  const int n = g.n;
  const float cell_m = p.world_m / float(p.res);

  // --- step 1: lakes ---------------------------------------------------
  //
  // PriorityFlood (protogen.cpp) raises every cell to the lowest elevation
  // on any path to the map edge -- its spill level. `filled` and `g.height`
  // are the SAME dimensionless-times-relief_m units used everywhere else in
  // this file, so `(filled[i] - g.height[i]) * relief_m` is the depth a lake
  // sits at, at exact hydrostatic fill.
  //
  // filled[i] >= g.height[i] always (PriorityFlood only ever raises a cell),
  // so `max(0, ...)` is a no-op algebraically; it stays explicit only as a
  // guard against float rounding making the difference a few ULPs negative
  // at a cell PriorityFlood left untouched. Outside every depression
  // flood_level_m == bed_m exactly (PriorityFlood does not touch a cell it
  // does not flood), so this same expression already gives h = 0 there with
  // no separate "is this cell in a depression" branch needed.
  std::vector<float> filled;
  std::vector<int32_t> outlet_of;  // unused here; PriorityFlood's other output
  PriorityFlood(g, filled, outlet_of);

  for (size_t i = 0; i < g.cells; ++i) {
    const float bed_m = g.height[i] * p.relief_m;
    const float flood_level_m = filled[i] * p.relief_m;
    g.h[i] = std::max(0.0f, flood_level_m - bed_m);
  }

  // --- step 2: channels -------------------------------------------------
  //
  // Route D8 flow over the surface step 1 just produced (the FLOODED
  // surface, not the raw bed) and turn its per-cell drainage area into a
  // discharge, then a Manning channel depth. Routing on the flooded surface
  // rather than the raw bed is what makes a null lake_tag safe here: every
  // depression the raw bed had has already been leveled to its spill height
  // by step 1, so this surface has no depressions of its own left for
  // route_flow's internal priority-flood to invent an exit through (see
  // route_flow's own header comment on why an untagged shallow margin is
  // otherwise unsafe -- that risk is specifically about REAL depressions in
  // the input, and there are none left here by construction). The only
  // "flat" spots route_flow's internal flood can still find are the lake
  // tops step 1 already leveled correctly; falling back to its own in_lake
  // flags those and keeps them on the flood-tree receiver rather than
  // steepest descent, which is exactly correct on a flat lake top (there is
  // no real gradient to descend) and funnels each lake's accumulated area
  // toward its one real spill point -- reproducing step 1's own basin
  // structure rather than fighting it.
  badlands::mapgen::Field2D<float> surface(n, n);
  for (size_t i = 0; i < g.cells; ++i) surface.data[i] = filled[i] * p.relief_m;

  const badlands::mapgen::FlowRouting routing = badlands::mapgen::route_flow(
      surface, cell_m, badlands::mapgen::kEpsilonM, /*lake_tag=*/nullptr);
  const float cell_area_m2 = cell_m * cell_m;
  const badlands::mapgen::Field2D<float> drainage_area_m2 =
      badlands::mapgen::accumulate_drainage(routing, cell_area_m2);

  const float runoff_rate_mps = p.runoff_m_per_yr / float(kSecondsPerYear);
  const float diag_m = cell_m * 1.41421356f;  // sqrt(2)

  for (int y = 0; y < n; ++y) {
    for (int x = 0; x < n; ++x) {
      const size_t i = g.idx(x, y);
      // In-lake cells are excluded on purpose, not merely left to fall out
      // of the arithmetic below: on the flooded surface's flat lake tops the
      // only "slope" route_flow's receiver graph can report is the
      // epsilon-per-BFS-step tilt its OWN flood used to break ties, which is
      // bookkeeping, not a physical gradient. Feeding that into Manning's
      // S^-0.6 would manufacture an arbitrarily large "channel" depth in the
      // middle of a lake that already sits at the exactly-correct level from
      // step 1 -- precisely the epsilon-leakage step 1's exactness would
      // otherwise be undone by.
      if (routing.in_lake[i]) continue;

      const int32_t rcv = routing.receiver[i];
      if (rcv < 0) continue;  // border: base level, no channel to seed

      const float Q = drainage_area_m2.data[i] * runoff_rate_mps;
      if (!(Q > 0.0f)) continue;

      const int rx = int(rcv) % n, ry = int(rcv) / n;
      const bool diagonal = (rx != x) && (ry != y);
      const float dist_m = diagonal ? diag_m : cell_m;
      const float drop_m = routing.water_level[i] - routing.water_level[size_t(rcv)];
      // Floored exactly like Descend's S_h (protogen.cpp's Manning depth
      // block, "river_graph's kMinChannelSlope") and river_graph.hpp's own
      // kMinChannelSlope: Manning divides by sqrt(S), so an unfloored flat
      // reach (a receiver step whose flood levels tie, or float noise on an
      // otherwise-flat dry reach) would give an infinite depth rather than a
      // merely-large one. Same value, 1e-4, for the same reason.
      const float S = std::max(drop_m / dist_m, 1e-4f);

      // Manning's normal-depth closure, d = (n*q/sqrt(S))^0.6 with q the
      // UNIT discharge (m^2/s) -- the same closed form Descend uses
      // off-channel (protogen.cpp) and SweManningConvergence (this file's S8
      // test) validates SweFlux's OWN dynamics against. Two deliberate
      // departures from Descend's literal instance of that formula:
      //
      // 1. CONVEYANCE WIDTH is `cell_m`, NOT Descend's regime width
      //    `channel_width_coeff*sqrt(Q)`. Descend's regime width models a
      //    sub-grid channel narrower than one cell, appropriate for a
      //    particle walking a continuous position. SweFlux has no such
      //    concept: its own A_pipe is `cell_m * own_h` (see SweFlux's
      //    comment), i.e. it already treats a cell's full width as the
      //    conveyance -- so seeding a depth calibrated for a NARROWER
      //    channel systematically overshoots what SweFlux's own steady
      //    state needs, and every seeded cell downstream of any real
      //    confluence has to visibly relax back down before
      //    WarmStartProximity's water surface stops moving. Measured on that
      //    fixture (a lake spilling into a channelled valley, cell_m = 32 m):
      //    the regime-width formula measured 0.123 m of L-inf drift over 50
      //    fluid-only cycles, concentrated exactly at the first post-spill
      //    channel cell (seeded 0.121 m, relaxed to 0.020 m by cycle 300 --
      //    a 6x overshoot). Using `cell_m` as the width instead measured
      //    0.015 m over the same 50 cycles, an 8x improvement, because it is
      //    seeding the SAME quantity SweFlux's own dynamics converge to
      //    rather than a different physical model's answer.
      // 2. ROUGHNESS is the PHASE-1 `p.swe_manning_n`, not phase-0's
      //    `p.manning_n`: this depth seeds phase-1's `h`, and the fluid
      //    cycles that run after this warm start drag against swe_manning_n
      //    (SweFlux's B = g*swe_manning_n^2/h^(4/3)), so using that SAME
      //    roughness here is what makes the seeded depth close to what
      //    those cycles would themselves settle to at this discharge. The
      //    two Manning knobs are deliberately independent (see
      //    Params::swe_manning_n's own comment) and share a default value
      //    today only by coincidence.
      const float q = Q / cell_m;
      const float manning_depth_m =
          std::pow(p.swe_manning_n * q / std::sqrt(S), 0.6f);

      // Channel depth may raise h but never lower it: a channel cell right
      // at a lake's shoreline can compute a depth shallower than the lake's
      // own exact prefill there (step 1 already got that cell right), and
      // must not be allowed to undercut it.
      float& h = g.h[i];
      if (manning_depth_m > h) h = manning_depth_m;
    }
  }

  // Face fluxes start at exactly zero. Safe, not merely convenient, FROM a
  // well-balanced start: SweWellBalancedness (this file's own S1 test)
  // proves a flat lake with zero flux stays exactly flat and exactly still,
  // and step 1 above hands SweFlux precisely that inside every lake -- a
  // spatially uniform head with nothing to accelerate a spurious flux from
  // on the very first substep. Outside lakes, a channel cell's flux starts
  // at zero too; SweFlux's own acceleration term (dt*g*A_over_L*dhead) picks
  // it up from the head difference step 2 just seeded, which is what
  // WarmStartProximity checks stays small rather than assumed.
  std::fill(g.flux.begin(), g.flux.end(), std::array<float, 4>{0.f, 0.f, 0.f, 0.f});
}

// ============================================================ morphodynamics
//
// Four passes, run once per fluid cycle in this order by RunSweCycles:
//
//   SedExchange  bed <-> suspended load (Exner source term), MORFAC lives here
//   SedAdvect    the suspended load rides the flow (semi-Lagrangian)
//   TalusFlux    each cell's desired downslope shed over the repose angle
//   TalusApply   the gather half of the talus pair
//
// They extend the frozen GPU dispatch list, so each obeys the same rules as
// the fluid passes: a standalone function, gather-only Jacobi over the whole
// grid via badlands::ParallelFor, reading FRONT buffers and writing only its
// own cell's BACK buffer, no atomics and no cross-cell write. Where a pass
// mutates bed state that another cell reads in the same pass (SedExchange's
// energy-slope stencil reads neighbours' `height`/`h`; TalusApply gathers
// neighbours' talus) the update ping-pongs through `height_b`/`soil_b`/
// `sus_b` and swaps at the end -- phase-0's serial in-place Deposit/Erode
// would be a read-after-write race here, which is why these do not use them.
//
// PHASE-0'S `SettleSus` DOES NOT RUN IN PHASE 1. That pass (protogen.cpp) is
// the particle walk's own drain for `sus`: a fixed fraction settles per
// LANDSCAPE STEP plus an isotropic diffusion, neither of which has any
// meaning once there is a real velocity field. In phase 1 `sus` is owned
// entirely by SedExchange (which puts sediment in and takes it out, at rates
// derived from the flow) and SedAdvect (which moves it). Running both would
// double-count the settling and smear the plumes SedAdvect exists to keep
// sharp.

// ------------------------------------------------------------- capacity law

float SedCapacityM(const Grid& g, const Params& p, int x, int y) {
  const int n = g.n;
  const float cell_m = p.world_m / float(p.res);
  // ENERGY SLOPE, |grad(bed + h)|, by CENTRAL DIFFERENCE over the water
  // SURFACE -- not the bed. On a lake the surface is flat however the bed
  // rolls underneath, so this is zero and so is the capacity; on a flat bed
  // carrying flow the surface still tilts (that tilt is what drives the flow
  // at all), so this is NON-zero and the capacity is too. Those two
  // statements are FlatReachTransport and StillLakeInert, and a bed-slope
  // capacity would fail both.
  //
  // Stencil: (surf[x+1] - surf[x-1]) / (2*cell_m) in the interior, degrading
  // to a ONE-SIDED difference over the actual span at the border -- note the
  // divisor is `(xp - xm) * cell_m`, so the border case divides by one cell
  // and not two. Dividing by 2*cell_m regardless would halve every border
  // slope and quietly make the map edge a low-transport ring.
  auto surf = [&](int sx, int sy) -> float {
    const size_t j = g.idx(sx, sy);
    return ToMetres(g.height[j], p) + g.h[j];
  };
  const int xm = std::max(x - 1, 0), xp = std::min(x + 1, n - 1);
  const int ym = std::max(y - 1, 0), yp = std::min(y + 1, n - 1);
  const float span_x = float(xp - xm) * cell_m;
  const float span_y = float(yp - ym) * cell_m;
  const float sx = span_x > 0.f ? (surf(xp, y) - surf(xm, y)) / span_x : 0.f;
  const float sy = span_y > 0.f ? (surf(x, yp) - surf(x, ym)) / span_y : 0.f;
  const float s_energy = std::sqrt(sx * sx + sy * sy);

  const size_t i = g.idx(x, y);
  const float speed =
      std::sqrt(g.velx[i] * g.velx[i] + g.vely[i] * g.vely[i]);
  return p.capacity_Kc_s * s_energy * speed;
}

// ------------------------------------------------------------ substrate law

float SedSubstrateYieldM(float demand_m, float soil_m, const Params& p) {
  if (!(demand_m > 0.f)) return 0.f;
  const float from_soil = std::min(demand_m, std::max(soil_m, 0.f));
  // THE RESIDUAL DEMAND IS SCALED BEFORE IT ATTACKS BEDROCK, which is what
  // makes the soil -> bedrock transition mass-conservative BY CONSTRUCTION:
  // the cut takes soil at full rate, and only what the cut still WANTS after
  // the soil is gone gets multiplied by `bedrock_erodibility`. `sus` then
  // gains exactly (from_soil + from_rock) -- there is no separate
  // "yield-scaling" step that could disagree with what the bed lost. Same law
  // and same order as phase-0's `Erode` (protogen.cpp); pinned to exact
  // numbers by ExnerSoilBedrockConservation.
  const float from_rock = (demand_m - from_soil) * p.bedrock_erodibility;
  return from_soil + from_rock;
}

// --------------------------------------------------------------- sed_exchange

// The Exner source term: material moves between the bed and the suspended
// load at a rate set by how far the load is from the flow's capacity.
//
// READS (front buffers only): own `h`, `velx`, `vely`, `soil`, `sus`, `flux`;
// own and FOUR-NEIGHBOUR `height`/`h` (via SedCapacityM's energy-slope
// stencil). WRITES: `height_b[i]`, `soil_b[i]`, `sus_b[i]` for the single `i`
// this thread owns; the three swaps happen after the ParallelFor returns.
// Nothing this pass writes is read by any cell during the pass.
//
// WET GATE, with ONE deliberate exception. Erosion needs flowing water and is
// skipped entirely on a cell with `h <= eps_wet`. DEPOSITION IS NOT: a dry
// cell has zero capacity by definition, so any `sus` sitting on it is pure
// surplus and settles. That exception is load-bearing rather than tidy -- it
// is how the suspended load phase-0 hands over (Grid::sus) reaches the
// ground at all, and how sediment stranded by a retreating wet-dry front
// stops following the water around forever.
//
// MORFAC, and why the two sides of the exchange disagree on purpose: the bed
// delta goes through `ClampMorfacBedDelta` (the sole place `p.morfac` is ever
// applied, in this file or any other), so the BED moves M cycles' worth per
// fluid cycle. The `sus` side does not -- it gains or loses exactly
// (bed delta)/M, one fluid cycle's worth, because `sus` is advected by a
// fluid that really did only advance one cycle. THAT ASYMMETRY IS THE WHOLE
// STAGGERED SCHEME, not an oversight: it is what lets a landform-scale bed
// evolve without simulating landform-scale fluid time. Its cost is that
// Sigma(bed + sus) is genuinely not conserved at M != 1, and the difference
// is booked to `swe_sed_morfac_created_m3` so the mass audit can subtract a
// known term instead of tripping on it.
void SedExchange(Grid& g, const Params& p, float dt_morpho) {
  const int n = g.n;
  const float cell_m = p.world_m / float(p.res);
  const float cell_area = cell_m * cell_m;
  const float morfac = p.morfac;
  if (!(morfac > 0.f)) return;
  // Guarded only against a degenerate zero: `adaptation_length_m` is a real
  // physical length (Params) and a caller setting it to 0 means "reach
  // capacity immediately", which the min(1, ...) below already expresses.
  const float adapt_L = std::max(p.adaptation_length_m, 1e-9f);
  const float w_settle = std::max(p.sus_settling_velocity_m_per_s, 0.f);

  std::vector<double> row_created(size_t(n), 0.0);

  badlands::ParallelFor(size_t(n), [&](size_t yy) {
    const int y = int(yy);
    double created_acc = 0.0;
    for (int x = 0; x < n; ++x) {
      const size_t i = g.idx(x, y);
      const float h_m = g.h[i];
      const bool wet = h_m > p.eps_wet;
      const float sus_m = ToMetres(g.sus[i], p);
      const float soil_m = ToMetres(g.soil[i], p);

      float bed_delta_m = 0.f;   // signed, + is aggradation
      float soil_delta_m = 0.f;  // signed, tracks bed_delta except on bedrock
      float sus_delta_m = 0.f;   // signed, fluid-clock
      float created_m = 0.f;     // solid MORFAC conjured (+) or vanished (-)

      const float cap_m = wet ? SedCapacityM(g, p, x, y) : 0.f;

      if (wet && cap_m > sus_m) {
        // ---- DEFICIT: the flow can carry more than it holds -> erode ----
        const float speed =
            std::sqrt(g.velx[i] * g.velx[i] + g.vely[i] * g.vely[i]);
        // Relaxation over the ADAPTATION LENGTH: d(sus)/ds = (C - sus)/L
        // integrated over the distance |v|*dt_morpho this cycle covers, in
        // its saturating first-order form. `adaptation_length_m` is reused
        // (rather than a new knob) because it is literally the same physical
        // quantity: Params' own comment defines it as "the distance over
        // which flow reaches its equilibrium concentration", derived twice
        // (back-calibration from phase-0's law, and u*h/w_s for a silt-grade
        // load) and agreeing at ~215-300 m. Phase 0 applied it to a
        // particle's swept path; phase 1 applies it to a cell's flow path.
        // Same length, same meaning, one number.
        const float rate = std::min(1.0f, speed * dt_morpho / adapt_L);
        const float chan = ChannelizationFactor(g, p, i, cell_m);
        const float demand_fluid_m = rate * (cap_m - sus_m) * chan;
        // MORFAC + clamp, applied to the DEMAND rather than to the yield --
        // so the substrate split below sees M cycles' worth of demand at
        // once. THIS ORDER IS DELIBERATE AND RATIFIED; the argument is one
        // identity. The soil-first yield law is EXACTLY additive over
        // sequential demands on a monotonically DEPLETING soil column:
        //
        //   yield(D1) then yield(D2)  ==  yield(D1 + D2)
        //
        // for every case (both in soil; straddling the mantle; both in
        // bedrock) -- so asking once for M*D removes exactly what M separate
        // asks for D would, including the partial interval that crosses the
        // soil/bedrock boundary. MorfacAggregationIdentity pins the linear
        // half of that and MorfacMantleTransition the nonlinear half.
        //
        // The alternative, M*yield(D), is not merely a different convention:
        // with D < soil it demands M*D of SOIL, so at M = 300 on a 0.5 m
        // mantle it asks for 3 m of cover that does not exist, and the excess
        // comes out of bedrock at erodibility 1.0 instead of 0.1 -- a 10x
        // over-erosion of rock that no ledger would notice.
        //
        // TWO PRECONDITIONS, both real:
        //  1. The demand D must be the same across the M intervals. It is not
        //     -- the flow responds as the channel deepens -- and that is
        //     MORFAC's general quasi-steady caveat, sharpest exactly at the
        //     mantle transition (see MorfacMantleTransition's own comment).
        //  2. The column must only DEPLETE. Soil production violates this;
        //     RunSweCycles has a tripwire for it, and `ProduceSoil`
        //     (protogen.cpp) carries the matching landmine note.
        //
        // Clamping the demand rather than the yield is also what keeps this
        // simple: the yield is monotone in and never exceeds the demand, so
        // bounding the demand bounds the elevation change too (strictly
        // tighter on bedrock, which is the safe direction).
        const float demand_bed_m =
            -ClampMorfacBedDelta(-demand_fluid_m, h_m, p);
        const float yield_m = SedSubstrateYieldM(demand_bed_m, soil_m, p);
        bed_delta_m = -yield_m;
        soil_delta_m = -std::min(demand_bed_m, soil_m);
        sus_delta_m = yield_m / morfac;
        created_m = bed_delta_m + sus_delta_m;  // = -(1 - 1/M)*yield <= 0
      } else if (sus_m > cap_m && sus_m > 0.f) {
        // ---- SURPLUS: more load than the flow can hold -> deposit ----
        if (wet) {
          // Settling, not adaptation-length relaxation -- see
          // Params::sus_settling_velocity_m_per_s for the derivation and for
          // why the velocity drops out (w_s*dt/h is what |v|*dt/L collapses
          // to once L = u*h/w_s is substituted). Deep water deposits slowly,
          // a shallow film almost instantly, and NEITHER needs the flow to
          // be moving -- which is the property the still-lake and dry-cell
          // cases depend on.
          const float rate = std::min(1.0f, w_settle * dt_morpho / h_m);
          const float dep_fluid_m = std::min(rate * (sus_m - cap_m), sus_m);
          bed_delta_m = ClampMorfacBedDelta(dep_fluid_m, h_m, p);
          soil_delta_m = bed_delta_m;
          sus_delta_m = -bed_delta_m / morfac;
          created_m = bed_delta_m + sus_delta_m;  // = +(1 - 1/M)*dep >= 0
        } else {
          // DRY: a stranded load, and MORFAC DOES NOT APPLY. MORFAC
          // accelerates an ONGOING transport -- it credits the bed with M
          // cycles of a flux that a quasi-steady flow keeps supplying. There
          // is no flow here and no supply: this is a finite stock that
          // settles exactly once. Amplifying it would build a mountain out
          // of a puddle's worth of silt (at M = 10,000, 0.14 m of stranded
          // load would credit the bed with 1.4 km of it). So the transfer is
          // one-to-one and exactly conservative, and `created_m` stays 0.
          bed_delta_m = sus_m;
          soil_delta_m = sus_m;
          sus_delta_m = -sus_m;
        }
      }

      // Deposition always lands as SOIL (loose material by definition,
      // whatever it was eroded from -- the same rule phase-0's `Deposit`
      // states); erosion removes soil first and only then bedrock, which is
      // why soil_delta and bed_delta differ on the erosion side.
      //
      // THE BED DELTA IS WRITTEN, THEN READ BACK, AND THE OTHER TWO SIDES
      // ARE SCALED TO WHAT THE BED ACTUALLY DID. This is not defensive
      // programming, it is the only way the exchange is conservative in
      // float32: `height` stores a landform elevation, so at production
      // relief its representable step is ~1e-5 m -- the same order as one
      // cycle's bed delta, and SMALLER than it near a large elevation. A
      // delta under half a ULP does not merely lose precision, it is
      // DISCARDED ENTIRELY by the addition, and crediting `sus` with
      // material the bed never gave up would be plain mass creation, cell by
      // cell, every cycle. Reading back `height_b[i] - height[i]` recovers
      // the realised change exactly (the two operands are within a factor of
      // 2, so the subtraction is exact by Sterbenz's lemma), and scaling the
      // sediment and ledger sides by the same ratio keeps all three in step.
      // The physical reading is the honest one: if the bed could not move,
      // no sediment was exchanged.
      const float d_hu = ToHeightUnits(bed_delta_m, p);
      g.height_b[i] = g.height[i] + d_hu;
      const float realised_hu = g.height_b[i] - g.height[i];
      const float ratio = (d_hu != 0.f) ? (realised_hu / d_hu) : 0.f;
      soil_delta_m *= ratio;
      sus_delta_m *= ratio;
      created_m *= ratio;
      g.soil_b[i] = std::max(0.f, g.soil[i] + ToHeightUnits(soil_delta_m, p));
      g.sus_b[i] = std::max(0.f, g.sus[i] + ToHeightUnits(sus_delta_m, p));
      created_acc += double(created_m) * double(cell_area);
    }
    row_created[size_t(y)] = created_acc;
  });

  // Serial, fixed-order combine (row index 0..n-1), same reproducibility
  // argument as SweDepth's ledger partials.
  double created_total = 0.0;
  for (int y = 0; y < n; ++y) created_total += row_created[size_t(y)];
  g.swe_sed_morfac_created_m3 += created_total;

  g.height.swap(g.height_b);
  g.soil.swap(g.soil_b);
  g.sus.swap(g.sus_b);
}

// ---------------------------------------------------------------- sed_advect

// The suspended load rides the flow: a semi-Lagrangian backtrace of `sus`
// along the cell-centred velocity, sampled with Catmull-Rom bicubic.
//
// READS (front buffers only): `sus` over the 4x4 stencil around the
// backtraced position, own `velx`/`vely`/`h`/`flux`, own and four-neighbour
// `height` (the transverse bed slope). WRITES: `sus_b[i]`, then a second
// gather-only pass rescales `sus_b` in place (each cell touching only its own
// slot), then the swap. No cell reads another cell's `sus_b`.
//
// THE ADVECTION STEP IS THE CYCLE'S WHOLE FLUID TIME, `dt_morpho` =
// swe_substeps * dt. `sus` is not advected during the substeps themselves --
// this one pass stands in for all of them -- so it must cover the distance
// they covered. That distance is large (measured at ~6 cells per cycle on a
// production-shaped fixture), so the pass takes it as SEVERAL sub-steps of
// about a cell each rather than one long chord -- see kMaxAdvectSubsteps /
// kMaxBacktraceCells in protogen.hpp for the sizing and the reason.
//
// TRANSVERSE-SLOPE DEFLECTION lives here, and that is a considered choice
// rather than the obvious reading of the brief (which lists it under the
// exchange). The effect being modelled -- Ikeda's and Struiksma's closure --
// is a statement about the DIRECTION of the sediment flux: on a laterally
// tilted bed, transport deviates from the flow toward the downhill side, and
// that deviation is what builds a point bar, which steers the flow, which is
// how meandering starts. The only pass here that has a direction to deflect
// is this one; expressing it inside SedExchange would mean faking a
// directional effect through a scalar rate, which cannot move a grain
// sideways and so cannot seed a meander. (Honest caveat kept in the open:
// the classic closure is a BED-LOAD result, and this model has one sediment
// field rather than separate bed and suspended load, so it is applied to the
// only transport there is.)
//
// MASS. Semi-Lagrangian advection is not conservative -- a converging flow
// field lets two cells backtrace into the same source and duplicate it, a
// diverging one loses mass between the taps -- and Catmull-Rom's negative
// overshoot has to be clipped on a non-negative field, which adds mass of its
// own. Both are handled by ONE mechanism, a global multiplicative mass fixer:
// the field is rescaled so its total is exactly (total before) minus a border
// export computed INDEPENDENTLY from the border faces' own water flux times
// the cell's sediment concentration. Two things follow, and both are the
// point: the ledger closes on a physical export term rather than on
// "whatever went missing", and the size of the correction (recorded in
// `swe_sed_advect_fix_max`) becomes a diagnostic of how far the advection
// step is being pushed.
void SedAdvect(Grid& g, const Params& p, float dt_morpho) {
  const int n = g.n;
  const float cell_m = p.world_m / float(p.res);
  const float cell_area = cell_m * cell_m;
  if (!(cell_m > 0.f)) return;

  const double before_m3 = DeterministicSum(g.cells, [&](size_t i) {
    return double(ToMetres(g.sus[i], p)) * double(cell_area);
  });

  // Sub-step count, from the largest displacement anywhere on the grid: aim
  // for about one cell per sub-step. The bound uses the UNDEFLECTED speed
  // times sqrt(2), which is exactly the worst case the transverse deflection
  // below can produce (it is bounded at 45 degrees, so it can lengthen the
  // velocity vector by at most sqrt(2)) -- that way the deflection maths is
  // written once, in the loop, rather than duplicated here to measure it.
  const float max_speed = DeterministicMax(g.cells, [&](size_t i) {
    return std::sqrt(g.velx[i] * g.velx[i] + g.vely[i] * g.vely[i]);
  });
  const float max_disp_cells = max_speed * 1.41421356f * dt_morpho / cell_m;
  int substeps = 1;
  if (max_disp_cells > 1.f)
    substeps = std::min(kMaxAdvectSubsteps, int(std::ceil(max_disp_cells)));
  const float dt_sub = dt_morpho / float(substeps);

  // Border export accumulates across the sub-steps, per row inside each and
  // combined serially afterwards (SweDepth's ledger construction, and the
  // same reproducibility reason).
  std::vector<double> row_export(size_t(n), 0.0);
  double export_raw_m3 = 0.0;

  for (int sub = 0; sub < substeps; ++sub) {
    badlands::ParallelFor(size_t(n), [&](size_t yy) {
      const int y = int(yy);
      double export_acc = 0.0;
      for (int x = 0; x < n; ++x) {
        const size_t i = g.idx(x, y);
        float vx = g.velx[i], vy = g.vely[i];
        const float speed = std::sqrt(vx * vx + vy * vy);
        if (speed > 0.f) {
          // Transverse bed slope, central difference on the BED (not the
          // water surface: this is about which way a grain rolls, which is
          // gravity acting on the bed, not the pressure gradient driving the
          // water).
          const int xm = std::max(x - 1, 0), xp = std::min(x + 1, n - 1);
          const int ym = std::max(y - 1, 0), yp = std::min(y + 1, n - 1);
          const float span_x = float(xp - xm) * cell_m;
          const float span_y = float(yp - ym) * cell_m;
          const float bx =
              span_x > 0.f
                  ? (ToMetres(g.height[g.idx(xp, y)], p) -
                     ToMetres(g.height[g.idx(xm, y)], p)) / span_x
                  : 0.f;
          const float by =
              span_y > 0.f
                  ? (ToMetres(g.height[g.idx(x, yp)], p) -
                     ToMetres(g.height[g.idx(x, ym)], p)) / span_y
                  : 0.f;
          // Downslope direction, minus the part already along the flow --
          // what remains is the TRANSVERSE downslope component, a
          // dimensionless slope.
          const float ux = vx / speed, uy = vy / speed;
          const float dsx = -bx, dsy = -by;
          const float along = dsx * ux + dsy * uy;
          const float tx = dsx - along * ux, ty = dsy - along * uy;
          // tan(deviation) = coeff * transverse slope, so the lateral speed
          // is coeff * |v| * (transverse slope), in the transverse direction.
          float latx = p.transverse_slope_coeff * speed * tx;
          float laty = p.transverse_slope_coeff * speed * ty;
          const float latmag = std::sqrt(latx * latx + laty * laty);
          // Bounded at 45 degrees of deflection. A transverse slope steep
          // enough to demand more is a cliff, not a bank, and the
          // small-angle closure has nothing to say about it.
          if (latmag > speed) {
            const float s = speed / latmag;
            latx *= s;
            laty *= s;
          }
          vx += latx;
          vy += laty;
          // RENORMALISE BACK TO THE ORIGINAL SPEED. This closure is a
          // ROTATION of the sediment flux, not an amplification of it:
          // Ikeda's and Struiksma's result says transport DEVIATES from the
          // flow direction on a laterally tilted bed, and says nothing about
          // it going faster. Adding a perpendicular component without
          // rescaling lengthens the vector by sqrt(1 + (coeff*slope)^2) --
          // up to 1.414x at the 45-degree bound, which binds exactly at
          // steep banks, i.e. exactly where the meandering mechanism lives
          // and exactly where a spurious 41% transport boost would be least
          // visible and most damaging. One sqrt buys the semantics the
          // comment above already claimed.
          const float defl_mag = std::sqrt(vx * vx + vy * vy);
          if (defl_mag > 0.f) {
            const float renorm = speed / defl_mag;
            vx *= renorm;
            vy *= renorm;
          }
        }

        float dxc = -vx * dt_sub / cell_m;  // backtrace, in CELLS
        float dyc = -vy * dt_sub / cell_m;
        const float dist = std::sqrt(dxc * dxc + dyc * dyc);
        float step_scale = 1.f;
        if (dist > kMaxBacktraceCells) {
          step_scale = kMaxBacktraceCells / dist;
          dxc *= step_scale;
          dyc *= step_scale;
        }
        const float src_x = float(x) + 0.5f + dxc;
        const float src_y = float(y) + 0.5f + dyc;
        float v = CatmullRomSample(g.sus, n, src_x, src_y);
        if (!(v > 0.f)) v = 0.f;  // also catches NaN, which cannot be a mass
        g.sus_b[i] = v;

        // Border export: for a cell with a face pointing off the array, the
        // water leaving through it carries this cell's sediment at this
        // cell's concentration (`sus` is an equivalent solid DEPTH, so sus/h
        // is a dimensionless volumetric concentration).
        //
        // THE TIME USED IS THIS SUB-STEP'S OWN `dt_sub * step_scale`, the
        // same effective step the backtrace just took. Sharing one clock is
        // what keeps the export estimate and the field's implicit border
        // loss consistent; when they were allowed to differ (a nominal
        // whole-cycle export against a clamped backtrace) the mass fixer had
        // to absorb the gap and grew to |f-1| ~ 0.19.
        float border_flux = 0.f;
        for (int k = 0; k < 4; ++k) {
          const int nx = x + kDx[k], ny = y + kDy[k];
          if (nx < 0 || ny < 0 || nx >= n || ny >= n)
            border_flux += g.flux[i][k];
        }
        const float h_m = g.h[i];
        const float sus_m = ToMetres(g.sus[i], p);
        if (border_flux > 0.f && h_m > p.eps_wet && sus_m > 0.f) {
          const double vol = double(border_flux) *
                             double(dt_sub * step_scale) * double(sus_m / h_m);
          export_acc += std::min(vol, double(sus_m) * double(cell_area));
        }
      }
      row_export[size_t(y)] = export_acc;
    });
    for (int y = 0; y < n; ++y) export_raw_m3 += row_export[size_t(y)];
    g.sus.swap(g.sus_b);
  }

  const double export_m3 = std::min(export_raw_m3, std::max(before_m3, 0.0));
  // NOTE the buffers: the sub-step loop above already swapped, so the result
  // is in `sus` and the mass fixer works on it in place. Still gather-only --
  // each cell touches its own slot and reads no other.
  const double after_m3 = DeterministicSum(g.cells, [&](size_t i) {
    return double(ToMetres(g.sus[i], p)) * double(cell_area);
  });
  const double target_m3 = std::max(before_m3 - export_m3, 0.0);

  // THE FIXER'S GAIN IS BOUNDED, and what it cannot fix is ledgered rather
  // than quietly absorbed. Unbounded, `target/after` is a loaded gun: a
  // gather that collapses -- an expanding wet-dry front, where neighbouring
  // cells backtrace to wildly different places and most of them sample dry
  // ground -- drives `after` toward zero and the factor toward infinity,
  // multiplying the whole suspended field by an arbitrary number and dumping
  // the result into whichever cells happened to gather anything. That is not
  // a mass fix, it is mass teleportation, and because the fixer defines the
  // total the periodic audit then checks against, THE AUDIT CANNOT SEE IT.
  // Measured on a bone-dry-start fixture before this bound: a 3.0x factor on
  // cycle 0.
  //
  // So: the factor is clamped to [1/kMaxAdvectFixFactor, kMaxAdvectFixFactor],
  // and the mass the clamp refuses to move --
  // `target - (what the field actually ends up holding)` -- is booked to
  // `swe_sed_advect_fix_residual_m3`, which the audit subtracts as a known
  // term. A nonzero residual is a REPORTED failure of the advection step, not
  // a silently balanced book.
  double fix = 1.0;
  bool clamped = false;
  if (after_m3 > 0.0) {
    fix = target_m3 / after_m3;
    if (fix > kMaxAdvectFixFactor) { fix = kMaxAdvectFixFactor; clamped = true; }
    if (fix < 1.0 / kMaxAdvectFixFactor) {
      fix = 1.0 / kMaxAdvectFixFactor;
      clamped = true;
    }
  }
  if (fix != 1.0) {
    const float fixf = float(fix);
    badlands::ParallelFor(size_t(n), [&](size_t yy) {
      const int y = int(yy);
      for (int x = 0; x < n; ++x) g.sus[g.idx(x, y)] *= fixf;
    });
  }
  // Booked ONLY when the fixer could not do its job -- the factor hit its
  // bound, or the gather returned an empty field with mass still owed. Left
  // unbooked otherwise because `after * fix == target` identically there, so
  // the difference would be nothing but double round-off (~1e-14 m3) and
  // would turn a term that is supposed to mean "the advection failed here"
  // into permanent noise. The after_m3 == 0 case IS booked: the whole of
  // `target_m3` has genuinely vanished (the gather found nothing anywhere and
  // there is no field left to scale), and that branch used to destroy the
  // difference with no entry anywhere -- the silent hole this term closes.
  if (clamped || after_m3 == 0.0)
    g.swe_sed_advect_fix_residual_m3 += target_m3 - after_m3 * fix;
  g.swe_sed_advect_fix_max =
      std::max(g.swe_sed_advect_fix_max, std::fabs(fix - 1.0));
  g.swe_sed_border_export_m3 += export_m3;
}

// ------------------------------------------------------------- talus_flux

// Half one of the Jacobi twin of phase-0's `Cascade`: what each cell WANTS to
// shed downslope, per neighbour, with nothing applied yet.
//
// READS (front buffers only): own and eight-neighbour `height`, own `soil`.
// WRITES: `talus[i]` for the single `i` this thread owns. Nothing reads
// `talus` during this pass.
//
// SOIL ONLY, and donor-limited to the soil actually present. `Cascade` moves
// material through the substrate (bedrock sheds at `bedrock_erodibility`),
// which is right for a serial pass modelling long-run rock creep; this one is
// an avalanche, and BEDROCK DOES NOT AVALANCHE. A rock face stands at
// whatever angle it was cut at -- that is why cliffs exist -- and only the
// loose regolith on top of it flows to the repose angle. Making the transfer
// soil-only is also what keeps the pass exactly conservative with no
// substrate arithmetic at all: whatever leaves a donor as soil arrives as
// soil.
//
// Shed magnitude is `relax * 0.5 * (largest excess)`, split between the
// receiving neighbours in proportion to their own excess. Two separate
// factors doing two separate jobs:
//
//   `0.5 * largest excess` is the STABILITY CAP, not a rate. It is NOT
//   `Cascade`'s per-neighbour `settling * excess * 0.5` applied eight times:
//   Cascade re-reads `height` after every single transfer (its own comment
//   says so) and is self-limiting because of it, while a Jacobi pass commits
//   all eight at once from stale state and would shed up to ~3x the excess in
//   one step -- an oscillation, not a relaxation. Keying the total on the
//   LARGEST excess keeps one pass strictly under-relaxed whatever the rate
//   below says.
//
//   `relax = min(1, talus_relaxation_per_yr * dt_bed)` is the RATE, and
//   `dt_bed` is the BED-clock interval `morfac * dt_morpho`. See
//   Params::talus_relaxation_per_yr for why this pass needed a clock at all
//   (without one the hillslope/fluvial ratio tracked swe_substeps, morfac and
//   resolution) and why the clock is the bed's rather than the fluid's (talus
//   moves substrate, and the substrate is what MORFAC accelerates). Same
//   saturating first-order shape as SedExchange's own rate factors, so the
//   two passes relax on comparable terms.
void TalusFlux(Grid& g, const Params& p, float dt_bed_s) {
  const int n = g.n;
  const float cell_m = p.world_m / float(p.res);
  const float tan_repose = std::tan(p.repose_angle_deg * 3.14159265f / 180.0f);
  const float diag_m = cell_m * 1.41421356f;
  const float rate_per_s =
      std::max(0.f, p.talus_relaxation_per_yr) / float(kSecondsPerYear);
  const float relax = std::min(1.0f, rate_per_s * std::max(dt_bed_s, 0.f));
  if (!(relax > 0.f)) {
    std::fill(g.talus.begin(), g.talus.end(),
              std::array<float, 8>{0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f});
    return;
  }

  badlands::ParallelFor(size_t(n), [&](size_t yy) {
    const int y = int(yy);
    for (int x = 0; x < n; ++x) {
      const size_t i = g.idx(x, y);
      std::array<float, 8> f{};
      const float soil_m = ToMetres(g.soil[i], p);
      if (!(soil_m > 0.f)) {
        g.talus[i] = f;
        continue;
      }
      const float z_i = ToMetres(g.height[i], p);
      float excess[8] = {};
      float excess_sum = 0.f, excess_max = 0.f;
      for (int k = 0; k < 8; ++k) {
        const int nx = x + kTdx[k], ny = y + kTdy[k];
        if (nx < 0 || ny < 0 || nx >= n || ny >= n) continue;
        // Per-direction threshold using the ACTUAL centre-to-centre distance
        // (cell_m orthogonally, cell_m*sqrt(2) diagonally). Phase-0's
        // Cascade uses one `max_diff` for all eight, which makes its
        // effective diagonal repose angle ~35 deg when it asks for 40; the
        // angle is the shared quantity, and getting the distance right is
        // strictly more faithful to it.
        const bool diagonal = (kTdx[k] != 0) && (kTdy[k] != 0);
        const float dist_m = diagonal ? diag_m : cell_m;
        const float drop = z_i - ToMetres(g.height[g.idx(nx, ny)], p);
        const float e = drop - tan_repose * dist_m;
        if (e > 0.f) {
          excess[k] = e;
          excess_sum += e;
          excess_max = std::max(excess_max, e);
        }
      }
      if (excess_sum > 0.f) {
        float move_total = relax * 0.5f * excess_max;
        if (move_total > soil_m) move_total = soil_m;  // donor-limited
        for (int k = 0; k < 8; ++k)
          f[k] = move_total * (excess[k] / excess_sum);
      }
      g.talus[i] = f;
    }
  });
}

// ------------------------------------------------------------- talus_apply

// The gather half: a cell's new soil is its own minus what it shed plus what
// its neighbours sent it.
//
// READS (front buffers only): own and eight-neighbour `talus`, own `height`
// and `soil`. WRITES: `height_b[i]`, `soil_b[i]`, then the swaps.
//
// EXACTLY CONSERVATIVE, by pairing rather than by tolerance: every entry
// `talus[j][k]` is subtracted once (from j's own outflow sum) and added once
// (by the neighbour it points at, which finds it at index `TalusOpposite(k)`
// of j). TalusFlux never writes an entry pointing off the array, so nothing
// escapes the domain and there is no border term to ledger -- material piles
// against the map edge instead, which is the honest behaviour for a pass with
// no knowledge of what is beyond it.
void TalusApply(Grid& g, const Params& p) {
  const int n = g.n;
  badlands::ParallelFor(size_t(n), [&](size_t yy) {
    const int y = int(yy);
    for (int x = 0; x < n; ++x) {
      const size_t i = g.idx(x, y);
      float out_m = 0.f, in_m = 0.f;
      for (int k = 0; k < 8; ++k) {
        out_m += g.talus[i][size_t(k)];
        const int nx = x + kTdx[k], ny = y + kTdy[k];
        if (nx < 0 || ny < 0 || nx >= n || ny >= n) continue;
        in_m += g.talus[g.idx(nx, ny)][size_t(TalusOpposite(k))];
      }
      const float d_hu = ToHeightUnits(in_m - out_m, p);
      g.height_b[i] = g.height[i] + d_hu;
      // Cannot go negative: TalusFlux caps a cell's total shed at the soil it
      // holds, and `in_m` only adds. The max() is a float-rounding guard.
      g.soil_b[i] = std::max(0.f, g.soil[i] + d_hu);
    }
  });
  g.height.swap(g.height_b);
  g.soil.swap(g.soil_b);
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

// ------------------------------------------------------ sediment mass audit

// How often RunSweCycles audits the sediment ledger. Every 10 cycles: often
// enough that a leak is named within a few percent of a production run rather
// than at the end of it, rare enough that the two extra whole-grid reductions
// it costs stay in the noise against 10 cycles x swe_substeps fluid passes.
constexpr int kMassAuditEveryCycles = 10;
// WHAT THIS AUDIT IS, STATED BEFORE THE NUMBERS, because the numbers are
// easy to over-read: IT IS A BOOKKEEPING-CONSISTENCY CHECK, NOT AN
// INDEPENDENT CONSERVATION CHECK. `SedAdvect` renormalises the suspended
// field to a total it computes from its OWN border-export estimate, so the
// advection half of the chain closes here by construction and this audit
// cannot testify that the export estimate is physically right. What it does
// testify to is that everything else agrees with the books: the substrate
// split, the MORFAC staggering, the talus pairing, the float32 write-backs,
// and (via `swe_sed_advect_fix_residual_m3`) any mass the fixer refused to
// place. That is a genuine and useful check -- it catches a 2% yield-scaling
// bug within 40 cycles, measured -- but it is not proof of conservation, and
// anything that reads it as such is reading it wrong.
//
// Audit tolerance, as (relative term) + (absolute term):
//
//   RELATIVE, 1e-3 of everything the run has moved. This is what catches a
//   real leak, which is always a FRACTION of the transported mass (a missing
//   substrate term, a talus transfer that does not pair, a fixer pointed at
//   the wrong total). 0.1% is far below any such bug and far above the
//   double-precision summation noise of the reductions themselves (~1e-7
//   relative at 1e6 cells).
//
//   ABSOLUTE, 1e-8 of the starting solid volume: the float32 QUANTISATION
//   BUDGET, and sized to what that argument actually supports rather than to
//   a comfortable-looking round number. `height` is a single-precision
//   landform elevation, so at production settings (relief_m = 400, surfaces
//   around 300 m) its representable step is ~2.4e-5 m and each write rounds
//   by up to half of that. SedExchange reads its own write back and scales
//   the sediment side to the realised delta, so its roundings do NOT leak;
//   TalusApply's do, because the donor/receiver pairing is exact only on the
//   INTENDED transfers. Those roundings random-walk: sqrt(N*C) * 1.2e-5 m *
//   cell_area, which at N = 1e6 cells and C = 1000 cycles is ~1e2 m3 against
//   a ~7.7e10 m3 baseline -- 1.3e-9 relative. 1e-8 leaves roughly an order of
//   magnitude of headroom over that.
//
//   THIS TERM WAS 1e-6 AND THAT WAS WRONG BY ~1000x. Nothing failed because
//   of it, which is the point: on ExnerLedger's own fixture the absolute term
//   alone came to 1.3% of the transported mass, so the audit was carrying a
//   blind spot two orders wider than its stated justification. Tightening it
//   makes the audit strictly better and moved no test.
constexpr double kMassAuditRelTol = 1e-3;
constexpr double kMassAuditAbsFrac = 1e-8;

// Total solid on the grid, m^3: the bed (`height`, which already includes the
// soil layer -- bedrock is implicitly height - soil, see Grid) plus the
// suspended load, both crossing the units seam exactly once.
double SolidVolumeM3(const Grid& g, const Params& p) {
  const float cell_m = p.world_m / float(p.res);
  const double cell_area = double(cell_m) * double(cell_m);
  return DeterministicSum(g.cells, [&](size_t i) {
    return (double(ToMetres(g.height[i], p)) + double(ToMetres(g.sus[i], p))) *
           cell_area;
  });
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

// Times one DeterministicMaxDepth call into `stats->t_swe_reduce` (if
// `stats` is non-null) and returns its result -- shared by the initial
// pre-loop check and the post-substep check inside the loop below, so the
// two call sites can't drift into timing one and not the other.
ReduceResult TimedReduce(const Grid& g, const Params& p, SimStats* stats) {
  const auto t0 = std::chrono::steady_clock::now();
  const ReduceResult red = DeterministicMaxDepth(g, p);
  const auto t1 = std::chrono::steady_clock::now();
  if (stats)
    stats->t_swe_reduce += std::chrono::duration<double>(t1 - t0).count();
  return red;
}

SweRunResult RunSweCycles(Grid& g, const Params& p, int cycles,
                          SimStats* stats) {
  const float cell_m = p.world_m / float(p.res);
  SweRunResult result;

  // Validate the STARTING state before running anything -- the only check in
  // this function that runs before a cycle's substeps rather than after.
  // Everything below checks AFTER a cycle's own substeps, on purpose: an
  // earlier version checked at the TOP of the NEXT cycle instead, which (a)
  // left the FINAL cycle's own corruption completely unchecked (there is no
  // "next cycle" for it to be caught at the top of, so the function returned
  // ok=true regardless of what the last cycle's substeps produced), and (b)
  // mislabeled every other cycle's fault too (a NaN born during cycle k's
  // substeps was reported as "cycle k+1", the cycle that happened to notice
  // it, not the one that caused it). Checking after every cycle -- including
  // silently reusing that SAME check as the next cycle's CFL input, and
  // running one extra time up front for the initial state -- fixes both:
  // `aborted_cycle` now always names the cycle whose substeps actually
  // produced the fault (or, for a bad starting state, the cycle that fault
  // would have blocked), and no cycle is special-cased out of the check.
  ReduceResult red = TimedReduce(g, p, stats);
  if (red.nonfinite) {
    result.ok = false;
    result.aborted_cycle = 0;
    result.reason =
        "cycle 0: non-finite h or height detected in the starting state, "
        "before any substep ran";
    WriteAbortSnapshot(g, p, 0);
    return result;
  }

  // Sediment mass audit baseline, captured HERE -- at phase-1 entry, i.e. on
  // whatever bed and suspended load the caller hands over (in production,
  // phase-0's finished landscape after SweWarmStart). The three ledger
  // counters are snapshotted alongside it rather than assumed zero, so a
  // caller that runs RunSweCycles more than once on the same Grid audits its
  // own segment instead of re-auditing history.
  const bool morpho = p.morfac > 0.f;

  // SOIL-PRODUCTION TRIPWIRE. This guards a mistake nobody has made yet, and
  // that is deliberate -- it is here so the mistake cannot be made silently.
  //
  // The whole MORFAC-before-substrate-split order (see SedExchange) rests on
  // ONE identity: the soil-first yield law is EXACTLY additive over
  // sequential demands on a DEPLETING soil column, so asking it once for M
  // cycles' worth of demand removes exactly what M separate asks would.
  // WEATHERING BREAKS THAT IDENTITY, because the column stops being merely
  // depleting: between two real fluid intervals, soil production adds cover
  // back, so the soil/bedrock boundary moves BOTH ways and where it sits
  // after M intervals depends on the interleaving -- which a single
  // yield(M*D) call has no way to know. The error is not small either: it is
  // largest exactly at the transition, which is the boundary that decides
  // whether a slope is armoured rock or strippable regolith.
  //
  // `ProduceSoil` is phase-0-only today (protogen.cpp, called from RunSim),
  // so this cannot fire from any current path. It exists for the future
  // wiring -- someone running weathering inside or alongside the phase-1
  // cycle loop -- and it fires on the ONLY configuration where the identity
  // is actually load-bearing, morfac > 1. At morfac <= 1 there is no
  // aggregation happening and nothing to break. The fix, when someone needs
  // this combination, is to sub-step SedExchange across the mantle rather
  // than to widen the guard.
  if (morpho && p.enable_soil_production && p.morfac > 1.0f) {
    result.ok = false;
    result.aborted_cycle = 0;
    result.reason =
        "cycle 0: soil production is enabled with morfac > 1 -- the "
        "MORFAC-before-substrate-split aggregation in SedExchange is exact "
        "only over a monotonically DEPLETING soil column, and weathering "
        "adds cover back between fluid intervals. Sub-step the exchange "
        "across the mantle, or run this combination at morfac <= 1";
    return result;
  }

  const double audit_baseline_m3 = morpho ? SolidVolumeM3(g, p) : 0.0;
  const double audit_base_export = g.swe_sed_border_export_m3;
  const double audit_base_created = g.swe_sed_morfac_created_m3;
  const double audit_base_fix_residual = g.swe_sed_advect_fix_residual_m3;

  for (int cycle = 0; cycle < cycles; ++cycle) {
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

    // Morpho hook (Task 6). ONCE per cycle, after the fluid has advanced
    // `swe_substeps` times -- that is the staggering the whole design rests
    // on: the fluid runs at its CFL dt, the bed sees one update per cycle
    // covering all of it. `dt_morpho` is therefore the cycle's total fluid
    // time, and the passes are sequenced HERE and nowhere else (no pass
    // calls another).
    if (morpho) {
      const float dt_morpho = float(p.swe_substeps) * dt;
      auto m0 = std::chrono::steady_clock::now();
      SedExchange(g, p, dt_morpho);
      auto m1 = std::chrono::steady_clock::now();
      SedAdvect(g, p, dt_morpho);
      auto m2 = std::chrono::steady_clock::now();
      // BED-clock interval: `morfac` fluid cycles' worth. This is the second
      // and last place `morfac` enters the code (ClampMorfacBedDelta is the
      // other) and, like that one, it is a fluid-clock -> bed-clock
      // conversion. See Params::talus_relaxation_per_yr.
      const float dt_bed = p.morfac * dt_morpho;
      TalusFlux(g, p, dt_bed);
      TalusApply(g, p);
      auto m3 = std::chrono::steady_clock::now();
      if (stats) {
        stats->t_swe_sed_exchange += std::chrono::duration<double>(m1 - m0).count();
        stats->t_swe_sed_advect += std::chrono::duration<double>(m2 - m1).count();
        stats->t_swe_talus += std::chrono::duration<double>(m3 - m2).count();
      }

      // Periodic mass audit. The invariant, stated once here so the signs
      // are not left implicit anywhere else:
      //
      //   solid_now = solid_baseline + morfac_created - border_export
      //                              - advect_fix_residual
      //
      // `morfac_created` is signed (negative while the landscape is net
      // eroding, since MORFAC destroys the bed material it does not hand to
      // `sus`); `border_export` is the solid SedAdvect ledgered as leaving
      // through the open border; `advect_fix_residual` is mass the advection
      // mass fixer refused to place once its gain hit kMaxAdvectFixFactor,
      // and is exactly 0 on a healthy run. Everything else -- the substrate
      // split, deposition, the whole talus pair -- is conservative by
      // construction, so any residual here is a bug in one of them, not a
      // physical term this expression forgot. See kMassAuditRelTol for what
      // this check does and does not prove.
      if ((cycle + 1) % kMassAuditEveryCycles == 0) {
        const double solid_now = SolidVolumeM3(g, p);
        const double created = g.swe_sed_morfac_created_m3 - audit_base_created;
        const double exported = g.swe_sed_border_export_m3 - audit_base_export;
        const double unplaced =
            g.swe_sed_advect_fix_residual_m3 - audit_base_fix_residual;
        const double expected =
            audit_baseline_m3 + created - exported - unplaced;
        const double resid = solid_now - expected;
        const double moved = std::fabs(created) + std::fabs(exported) +
                             std::fabs(unplaced) +
                             std::fabs(solid_now - audit_baseline_m3);
        const double tol = kMassAuditRelTol * moved +
                           kMassAuditAbsFrac * std::fabs(audit_baseline_m3);
        if (!(std::fabs(resid) <= tol)) {
          result.ok = false;
          result.aborted_cycle = cycle;
          char buf[352];
          std::snprintf(buf, sizeof(buf),
                        "cycle %d: sediment-mass audit -- residual %.6e m3 "
                        "exceeds tolerance %.6e m3 (solid %.6e, baseline "
                        "%.6e, morfac-created %.6e, border-export %.6e, "
                        "advect-unplaced %.6e)",
                        cycle, resid, tol, solid_now, audit_baseline_m3,
                        created, exported, unplaced);
          result.reason = buf;
          WriteAbortSnapshot(g, p, cycle);
          return result;
        }
      }
    }

    // Post-substep validity check -- see this function's header comment for
    // why this runs after EVERY cycle, including the last, rather than only
    // at the top of a next one that might not exist. Its result also becomes
    // next iteration's CFL input, so this is not an extra pass over the grid
    // beyond what computing next cycle's dt already needed.
    red = TimedReduce(g, p, stats);
    if (red.nonfinite) {
      result.ok = false;
      result.aborted_cycle = cycle;
      result.reason = "cycle " + std::to_string(cycle) +
                      ": non-finite h or height detected after this cycle's "
                      "substeps";
      WriteAbortSnapshot(g, p, cycle);
      return result;
    }
  }

  return result;
}

// See the declaration (protogen.hpp) for the design intent -- this is
// scaffolding for Task 6, called by nothing today.
float ClampMorfacBedDelta(float raw_delta_m, float depth_m, const Params& p) {
  const float scaled = raw_delta_m * p.morfac;
  const float bound = kMaxBedDeltaFraction * std::max(depth_m, 0.0f);
  if (scaled > bound) return bound;
  if (scaled < -bound) return -bound;
  return scaled;
}

}  // namespace pg
