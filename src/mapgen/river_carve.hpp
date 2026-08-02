#pragma once

// The river network CARVED into the terrain: a corridor mask on the height
// lattice, plus the carved surface height at any world position.
//
// Two things decide the shape of this file, and both are physical rather than
// aesthetic (see the design doc, section 1):
//
//   cavity_depth(s) = 1.390 * d_flow(s)  +  k_bank * w(s)^0.6
//                     \__ bankfull __/      \__ bank cut __/
//
// The bankfull factor is the 1-2 yr flood the channel form is actually cut by
// (Q_bf = 3*Q and d ~ Q^0.3 give 1.390), so it is a climate assumption, not a
// look knob. The bank exponent is NOT invented either: downstream hydraulic
// geometry already gives d ~ Q^0.3 and w ~ Q^0.5, hence d ~ w^0.6. `k_bank` is
// the single free coefficient in the whole carve.
//
// Two properties are load-bearing and are what the tests pin:
//
//   - COMPACT SUPPORT. The cross-section reaches exactly zero at the corridor
//     half-width, so HeightAt returns the base surface BITWISE outside the
//     corridor. A Gaussian never reaches zero and would dish the entire map.
//   - THE BED COMES FROM THE CENTRELINE, not from local terrain. Subtracting a
//     profile from the existing height inherits every terrain wiggle and lets
//     the channel run uphill; taking the running downstream minimum of the base
//     surface along the chain (which the routing already sent downhill) and
//     subtracting the cavity from THAT is what makes a carved channel flow.
//
// Pure CPU, pure function of its inputs -- no I/O, no failure path.

#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

#include "mapgen/field2d.hpp"
#include "mapgen/river_arcs.hpp"
#include "mapgen/river_graph.hpp"

namespace badlands::mapgen {

struct RiverCarve;

// Builds the corridor mask and the carved-height field for `chains` (as
// produced by build_river_arcs) over `base_height`.
//
// `base_height` is COPIED into the result. The carve is handed on as a value
// whose HeightAt outlives the scope that built it (Task 5 stores it behind a
// std::function in the terrain detail field), and a dangling reference there
// would be a silent wrong-height bug rather than a crash. At the production
// 2048^2 that copy is 16 MB against a cluster DAG of ~380 MB.
//
// `k_bank` is the bank-cut coefficient; the default is the calibrated value.
RiverCarve build_river_carve(const RiverGraph& g,
                             const std::vector<RiverArcChain>& chains,
                             const Field2D<float>& base_height,
                             float world_size_m, float k_bank = 0.45f);

struct RiverCarve {
  // Corridor mask on the SAME lattice as base_height (1 m texels on the
  // production map). Non-zero where the texel's SQUARE comes within the local
  // corridor half-width of a channel centreline -- conservative, following the
  // same "square, not centre" rule as segment_aabb_distance (river_graph.hpp).
  // Centre sampling would drop texels a sub-metre channel passes straight
  // through.
  Field2D<uint8_t> mask;

  // Carved surface height at any world position.
  //
  // OUTSIDE every corridor this returns the base surface BITWISE: the profile
  // has compact support, so the outside path returns the value base_at()
  // produced without touching it. Never above the base surface anywhere, and
  // where channels overlap the DEEPEST one wins (a confluence is a min, not a
  // sum).
  float HeightAt(float wx, float wz) const;

 private:
  friend RiverCarve build_river_carve(const RiverGraph&,
                                      const std::vector<RiverArcChain>&,
                                      const Field2D<float>&, float, float);

  // One station along a chain's centreline, at a fixed 0.25 m spacing. Holding
  // the channel's cross-section AND its bed elevation per station is what lets
  // HeightAt answer from the nearest point on an arc without re-deriving the
  // running minimum, which is a whole-chain quantity.
  struct Station {
    float half_m = 0.0f;         // corridor half-width, max(1.5*w, 1 m)
    float wetted_half_m = 0.0f;  // w/2: the full cavity is cut inside this
    float cavity_m = 0.0f;
    float bed_m = 0.0f;  // absolute elevation of the channel bed
  };

  // One arc, plus where it sits in its chain's station array.
  struct ArcRec {
    RiverArc arc;
    float chain_s0_m = 0.0f;  // arc length along the CHAIN at arc.p0
    uint32_t station_begin = 0;
    uint32_t station_count = 0;
  };

  // Bilinear sample of the base surface on the NODE lattice: node (i, j)
  // carries base_(min(i, w-1), min(j, h-1)) and sits at (i*texel_m, j*texel_m).
  // This is the terrain mesh's own convention (surface_at in
  // mapview/river_surface.cpp, MakeOneHotMapData in map_view_view.cpp); a
  // second, differently-rounded expression for the same value would break the
  // bitwise "outside is unchanged" contract.
  float base_at(float wx, float wz) const;

  // Linear interpolation of the station record at `chain_s_m` along the chain
  // that owns `a`.
  Station station_at(const ArcRec& a, float chain_s_m) const;

  Field2D<float> base_;
  float texel_m_ = 0.0f;
  std::vector<ArcRec> arcs_;
  std::vector<Station> stations_;
  // Per-texel candidate arcs, CSR over the whole mask: the arcs whose corridor
  // touched this texel during rasterization. HeightAt evaluates the exact
  // profile for these and nothing else, and a texel with no candidates is
  // outside every corridor by the conservative-rasterization guarantee.
  std::vector<uint32_t> cand_offset_;  // size mask.size() + 1
  std::vector<uint32_t> cand_arc_;     // indices into arcs_
};

}  // namespace badlands::mapgen
