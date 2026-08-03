#include "mapview/river_surface.hpp"

#include <algorithm>
#include <cmath>

namespace badlands {

namespace {

// How far apart to place cross-sections along one arc.
//
// Two independent limits, and the tighter wins. LATERALLY the chord must not
// sag off the arc by more than kRiverSagittaM, which for a circle of radius r
// is a step of sqrt(8*sag*r). LONGITUDINALLY the water level tracks the carved
// bed, which is derived from the base lattice -- stepping further than a texel
// would let a straight run of water cut across a drop in the bed under it.
float sample_step_m(const mapgen::RiverArc& a, float texel_m) {
  const float ground_step = std::max(0.5f * texel_m, 0.25f);
  if (a.curvature_1_m == 0.0f) return ground_step;
  const float r = mapgen::arc_radius_m(a);
  const float lateral = std::sqrt(8.0f * kRiverSagittaM * r);
  return std::max(0.25f, std::min(lateral, ground_step));
}

}  // namespace

std::vector<glm::vec3> BuildRiverWaterTriangles(
    int lattice_texels, float world_size_m,
    const mapgen::RiverGraph& graph,
    const std::vector<mapgen::RiverArcChain>& chains,
    const std::function<float(float wx, float wz)>& height_at) {
  std::vector<glm::vec3> tris;
  const int w = lattice_texels;
  if (w <= 0 || world_size_m <= 0.0f || !height_at) return tris;
  const float texel_m = world_size_m / static_cast<float>(w);

  // One cross-section of the free surface: two points at the SAME level, since
  // water is level across a channel.
  struct Section {
    glm::vec3 left{0.0f}, right{0.0f};
  };
  // False = this station carries no water and no geometry may cross it.
  auto section_at = [&](const mapgen::RiverArc& arc, float s,
                        const std::vector<float>& params,
                        const std::vector<float>& widths,
                        const std::vector<float>& depths, Section& out) {
    const glm::vec2 c = mapgen::arc_point(arc, s);
    const glm::vec2 t = mapgen::arc_tangent(arc, s);
    // Parameter along the SOURCE reach, so width and depth come from the
    // hydraulics the graph solved rather than from an interpolation of the
    // arc's ends.
    const float f = (arc.length_m > 0.0f) ? s / arc.length_m : 0.0f;
    const float u = arc.param0 + (arc.param1 - arc.param0) * f;
    const float depth_m = mapgen::sample_at_param(params, depths, u);
    if (!(depth_m > 0.0f)) return false;  // no flow here, so no water
    float half = 0.5f * mapgen::sample_at_param(params, widths, u);
    if (!(half > 0.0f)) return false;
    // An offset further than the radius of curvature puts the inner bank on the
    // far side of the centre -- the surface folds inside out and renders as a
    // bow tie. A real channel never bends that tight (radius is 2-3 widths at a
    // meander apex), but a FITTED arc can where the polyline has a lattice
    // hairpin, so the mesh refuses rather than the representation lying.
    half = std::min(half, 0.9f * mapgen::arc_radius_m(arc));
    // THE WHOLE POINT: the carved surface under the CENTRELINE is the channel
    // bed, so bed + d_flow is the free surface and the banks keep the rest of
    // the cavity as freeboard. Sampling the bank offsets instead would drape
    // the water back over the ground the carve just cut away.
    const float level = height_at(c.x, c.y) + depth_m + kWaterEpsilonM;
    const glm::vec2 n(-t.y, t.x);  // left normal
    const glm::vec2 lp = c + n * half;
    const glm::vec2 rp = c - n * half;
    out.left = glm::vec3(lp.x, level, lp.y);
    out.right = glm::vec3(rp.x, level, rp.y);
    return true;
  };

  for (const mapgen::RiverArcChain& chain : chains) {
    if (chain.edge < 0 ||
        static_cast<size_t>(chain.edge) >= graph.edges.size())
      continue;
    const mapgen::RiverEdge& e = graph.edges[chain.edge];
    if (e.points_m.size() < 2) continue;
    const std::vector<float> params = mapgen::polyline_params(e.points_m);

    for (const mapgen::RiverArc& arc : chain.arcs) {
      if (!(arc.length_m > 0.0f)) continue;
      const float step = sample_step_m(arc, texel_m);
      const int n = std::max(1, static_cast<int>(std::ceil(arc.length_m / step)));
      // The strip is emitted in RUNS: a station with no water breaks it, and
      // the next wet station starts a new one.
      Section prev;
      bool have_prev = section_at(arc, 0.0f, params, e.width_m, e.depth_m, prev);
      for (int k = 1; k <= n; ++k) {
        const float s = arc.length_m * static_cast<float>(k) /
                        static_cast<float>(n);
        Section cur;
        if (!section_at(arc, s, params, e.width_m, e.depth_m, cur)) {
          have_prev = false;
          continue;
        }
        if (have_prev) {
          // CCW seen from +Y: cross(left_normal, tangent) is up, so the right
          // edge leads.
          tris.push_back(prev.right);
          tris.push_back(prev.left);
          tris.push_back(cur.left);
          tris.push_back(prev.right);
          tris.push_back(cur.left);
          tris.push_back(cur.right);
        }
        prev = cur;
        have_prev = true;
      }
    }
  }
  return tris;
}

}  // namespace badlands
