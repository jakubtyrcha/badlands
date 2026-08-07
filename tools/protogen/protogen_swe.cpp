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

    // Morpho hook -- EMPTY. Task 6 fills this in with SedExchange/SedAdvect/
    // TalusFlux+TalusApply, run once per cycle (not per substep) after the
    // fluid has advanced `swe_substeps` times, calling `ClampMorfacBedDelta`
    // (declared in protogen.hpp, defined below in this file) on every bed
    // delta it produces -- present and real today, just with no caller yet,
    // so Task 6 activates this path instead of inventing it.

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
