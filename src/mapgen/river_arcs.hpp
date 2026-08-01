#pragma once

// CIRCULAR-ARC representation of the river network: every reach polyline is
// refitted as a chain of circular arcs that is G1-continuous end to end.
//
// Why arcs and not the polyline. The polyline out of extract_river_graph is
// already de-latticed (Douglas-Peucker + resample), so it no longer staircases
// -- but it is still a sequence of straight segments at ~3 texels' spacing, and
// every consumer that wants a SMOOTH river has to invent its own smoothing:
// the ribbon mesh, a future carve pass, boat/flow-field motion along the
// channel, a bank spline. An arc chain gives all of them the same curve.
//
// Arcs specifically, rather than a cubic spline:
//   - A meander IS an arc. Curvature is the quantity river geometry is written
//     in (radius of curvature ~ 2-3 channel widths at the apex), so it is a
//     value to READ off the representation rather than a byproduct of a basis.
//   - Offsetting is exact and closed-form. The bank of a channel of width w is
//     the same arc at radius r -/+ w/2 -- no subdivision, no self-intersection
//     surprises. A cubic's offset is not a cubic.
//   - Arc length is exact (s = r*theta), so parameterising by DISTANCE ALONG
//     THE RIVER is free. On a cubic it is an integral nobody wants to evaluate.
//
// The fit is a BIARC chain. A biarc joins (p0, t0) to (p1, t1) with exactly two
// circular arcs meeting tangentially at a joint; picking the equal-chord joint
// makes tangent continuity fall out algebraically rather than being solved for
// (see fit_biarc). Chaining biarcs between polyline knots, with each knot's
// tangent shared by the spans on either side, makes the whole reach G1.
//
// Pure functions of their inputs -- no I/O, no failure path.

#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

#include "mapgen/river_graph.hpp"

namespace badlands::mapgen {

// One circular arc, in world METRES, directed DOWNSTREAM (p0 -> p1).
//
// Stored as (point, tangent, curvature, length) rather than (centre, radius,
// angles) because that form degrades gracefully: a straight arc is exactly
// curvature 0 and needs no special case at the call site, whereas a centre
// runs off to infinity. Centre and radius are recoverable (arc_centre,
// arc_radius_m) when a caller actually wants them.
struct RiverArc {
  glm::vec2 p0{0.0f}, p1{0.0f};
  glm::vec2 t0{1.0f, 0.0f};    // unit tangent at p0, pointing downstream
  float curvature_1_m = 0.0f;  // signed, LEFT-positive; exactly 0 == straight
  float length_m = 0.0f;       // arc length, not chord length

  // Normalised arc-length position of the endpoints along the SOURCE reach
  // polyline, in [0, 1]. This is how per-point reach attributes (width, depth,
  // speed, discharge) reach a consumer without the arc carrying a copy of each
  // -- see sample_at_param. Keeping the arc purely geometric is what lets the
  // same chain serve the mesh, a carve pass and a flow field.
  float param0 = 0.0f, param1 = 1.0f;
};

// Position / unit tangent at arc length `s` metres from p0. `s` outside
// [0, length_m] extrapolates along the same circle, which is what an offset or
// a cap query wants; callers that need clamping should clamp.
glm::vec2 arc_point(const RiverArc& a, float s);
glm::vec2 arc_tangent(const RiverArc& a, float s);

// Radius of curvature in metres; +INFINITY for a straight arc. Unsigned --
// the turn direction lives in the sign of curvature_1_m.
float arc_radius_m(const RiverArc& a);

// Centre of the osculating circle. Undefined (returns p0) for a straight arc,
// so check curvature_1_m first.
glm::vec2 arc_centre(const RiverArc& a);

// Shortest distance from `p` to the arc, treating it as a bounded segment of
// its circle: outside the swept angle the nearest point is an endpoint.
float arc_distance_m(const RiverArc& a, glm::vec2 p);

// Arc length in [0, length_m] of the point arc_distance_m measured to -- the
// companion answer to the same query. A consumer that wants the CHANNEL at the
// nearest point (its width, its bed, its flow) needs the parameter, not just
// the distance, and re-deriving it from the distance is not possible. Uses the
// same containment test as arc_distance_m, so the two always agree on which
// point they are talking about.
float arc_closest_param_m(const RiverArc& a, glm::vec2 p);

// The arcs of one reach, in downstream order. Consecutive arcs share an
// endpoint and a tangent, so the chain is G1 (continuous position and heading);
// it is NOT G2 -- curvature steps at every joint, which is exactly what an arc
// representation is.
struct RiverArcChain {
  int32_t edge = -1;  // index into RiverGraph::edges
  std::vector<RiverArc> arcs;
  float length_m = 0.0f;  // sum of the arc lengths
};

// --- fitting ----------------------------------------------------------------

// The biarc joining (p0, t0) to (p1, t1). Returns TWO arcs in downstream order,
// or ONE when the configuration is already a single arc or degenerates (the
// endpoints coincide, the tangents are antiparallel, the joint lands on an
// endpoint) -- a caller should handle both sizes rather than index [1].
//
// Construction, and why the joint is where it is. Put the joint at
//
//     J = ( (p0 + d*t0) + (p1 - d*t1) ) / 2
//
// for a single tangent length d shared by both ends. Then the two arcs' tangent
// lines meet at p0 + d*t0 and p1 - d*t1 respectively, and BOTH tangent
// directions at J work out to normalize(p1 - p0 - d*(t0 + t1)) -- identical, so
// G1 is satisfied identically instead of being imposed. The one remaining
// unknown d is fixed by requiring the first arc's chord and tangent lengths to
// agree, |J - (p0 + d*t0)| = d, a quadratic with a single positive root.
std::vector<RiverArc> fit_biarc(glm::vec2 p0, glm::vec2 t0, glm::vec2 p1,
                                glm::vec2 t1);

// Unit tangent estimate at each polyline vertex.
//
// Interior vertices take the centred chord P[i+1] - P[i-1], which is the exact
// circle tangent for evenly spaced samples of a circle -- and the polyline this
// runs on is resampled to even arc length, so that is the common case.
//
// Endpoints EXTRAPOLATE IN ANGLE rather than reusing the end segment: on a
// circle, chord(P0,P1) leads the tangent at P0 by half the subtended angle and
// chord(P0,P2) by the whole angle, so 2*a(P0,P1) - a(P0,P2) recovers it. Using
// the end segment directly would leave every reach's first and last arc with a
// visible kink relative to its neighbours.
std::vector<glm::vec2> polyline_tangents(const std::vector<glm::vec2>& pts);

// Normalised cumulative arc length of a polyline: one value per point, [0] == 0
// and [n-1] == 1. Degenerate (zero-length) polylines return all zeros.
std::vector<float> polyline_params(const std::vector<glm::vec2>& pts);

// Piecewise-linear lookup of a per-point attribute at normalised param `u`,
// given that polyline's `params`. Clamped at both ends; an empty or
// mismatched `values` returns 0.
float sample_at_param(const std::vector<float>& params,
                      const std::vector<float>& values, float u);

// Fits an arc chain to `pts`, GREEDILY spanning as many points per biarc as the
// tolerance allows: from the current knot, extend the span while every skipped
// point stays within `tolerance_m` of the fitted biarc, then emit and continue
// from where it stopped. A straight run collapses to a single arc no matter how
// many points it contains, which is the compression that makes this worth doing
// over a per-segment fit.
//
// Tolerance is a real distance in metres and should be set against what the
// polyline itself is worth: it comes out of a Douglas-Peucker pass at ~1 texel,
// so fitting arcs tighter than half a texel spends vertices chasing noise.
RiverArcChain fit_arc_chain(const std::vector<glm::vec2>& pts,
                            float tolerance_m);

// Overload taking explicit unit tangents (one per point), for callers that know
// the true tangents -- tests fitting a known circle, and any future consumer
// that wants to pin the heading where a reach meets a lake or a confluence.
RiverArcChain fit_arc_chain(const std::vector<glm::vec2>& pts,
                            const std::vector<glm::vec2>& tangents,
                            float tolerance_m);

// One chain per reach with at least two points, in graph edge order. Reaches
// too short to fit (a single point, or a lake connection carrying no geometry)
// are SKIPPED, not emitted empty, so `chain.edge` is the only link back to the
// graph -- do not assume chains[i] describes edges[i].
std::vector<RiverArcChain> build_river_arcs(const RiverGraph& g,
                                            float tolerance_m);

}  // namespace badlands::mapgen
