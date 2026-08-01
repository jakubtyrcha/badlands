#include "mapview/river_surface.hpp"

#include <algorithm>
#include <cmath>

namespace badlands {

namespace {

using mapgen::Field2D;

// Terrain node value at lattice index (i, j). The terrain mesh is built on a
// (w+1) x (h+1) node grid whose edge nodes REPEAT the last texel (see
// MakeOneHotMapData); reproducing that here is what keeps the ribbon on the
// rendered surface rather than half a texel off it at the map border.
float node_at(const Field2D<float>& f, int i, int j) {
  return f.at(std::min(i, f.width - 1), std::min(j, f.height - 1));
}

// Bilinear sample of height + standing water on that same node lattice.
float surface_at(const mapgen::MapArtifacts& art, float texel_m, glm::vec2 p) {
  const Field2D<float>& hf = art.heightmap;
  const int w = hf.width, h = hf.height;
  const bool has_water =
      art.water_depth.width == w && art.water_depth.height == h;

  const float fx = std::clamp(p.x / texel_m, 0.0f, static_cast<float>(w));
  const float fz = std::clamp(p.y / texel_m, 0.0f, static_cast<float>(h));
  const int i0 = std::clamp(static_cast<int>(std::floor(fx)), 0, w);
  const int j0 = std::clamp(static_cast<int>(std::floor(fz)), 0, h);
  const int i1 = std::min(i0 + 1, w);
  const int j1 = std::min(j0 + 1, h);
  const float tx = fx - static_cast<float>(i0);
  const float tz = fz - static_cast<float>(j0);

  auto sample = [&](int i, int j) {
    float v = node_at(hf, i, j);
    if (has_water) v += node_at(art.water_depth, i, j);
    return v;
  };
  const float a = sample(i0, j0) + (sample(i1, j0) - sample(i0, j0)) * tx;
  const float b = sample(i0, j1) + (sample(i1, j1) - sample(i0, j1)) * tx;
  return a + (b - a) * tz;
}

// How far apart to place cross-sections along one arc.
//
// Two independent limits, and the tighter wins. LATERALLY the chord must not
// sag off the arc by more than kRibbonSagittaM, which for a circle of radius r
// is a step of sqrt(8*sag*r). VERTICALLY the ribbon has to follow the ground,
// and the ground is only defined every texel -- stepping further would let a
// straight run cut through a rise, which is the failure a flat debug line has
// and a draped mesh should not.
float sample_step_m(const mapgen::RiverArc& a, float texel_m) {
  const float ground_step = std::max(0.5f * texel_m, 0.25f);
  if (a.curvature_1_m == 0.0f) return ground_step;
  const float r = mapgen::arc_radius_m(a);
  const float lateral = std::sqrt(8.0f * kRibbonSagittaM * r);
  return std::max(0.25f, std::min(lateral, ground_step));
}

}  // namespace

std::vector<glm::vec3> BuildRiverRibbonTriangles(
    const mapgen::MapArtifacts& art, float world_size_m,
    const mapgen::RiverGraph& graph,
    const std::vector<mapgen::RiverArcChain>& chains) {
  std::vector<glm::vec3> tris;
  const int w = art.heightmap.width;
  if (w <= 0 || art.heightmap.height <= 0 || world_size_m <= 0.0f) return tris;
  const float texel_m = world_size_m / static_cast<float>(w);

  // One cross-section: centre, half-width and the left offset, all in world.
  struct Section {
    glm::vec3 left{0.0f}, right{0.0f};
  };
  auto section_at = [&](const mapgen::RiverArc& arc, float s,
                        const std::vector<float>& params,
                        const std::vector<float>& widths) {
    const glm::vec2 c = mapgen::arc_point(arc, s);
    const glm::vec2 t = mapgen::arc_tangent(arc, s);
    // Parameter along the SOURCE reach, so the width comes from the hydraulics
    // the graph solved rather than from an interpolation of the arc's ends.
    const float f = (arc.length_m > 0.0f) ? s / arc.length_m : 0.0f;
    const float u = arc.param0 + (arc.param1 - arc.param0) * f;
    float half = 0.5f * std::max(mapgen::sample_at_param(params, widths, u),
                                 kMinRibbonWidthM);
    // An offset further than the radius of curvature puts the inner bank on the
    // far side of the centre -- the ribbon folds inside out and renders as a
    // bow tie. A real channel never bends that tight (radius is 2-3 widths at a
    // meander apex), but a FITTED arc can where the polyline has a lattice
    // hairpin, so the mesh refuses rather than the representation lying.
    half = std::min(half, 0.9f * mapgen::arc_radius_m(arc));
    const glm::vec2 n(-t.y, t.x);  // left normal
    const glm::vec2 lp = c + n * half;
    const glm::vec2 rp = c - n * half;
    Section out;
    out.left = glm::vec3(lp.x, surface_at(art, texel_m, lp) + kRiverLiftM, lp.y);
    out.right = glm::vec3(rp.x, surface_at(art, texel_m, rp) + kRiverLiftM, rp.y);
    return out;
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
      Section prev = section_at(arc, 0.0f, params, e.width_m);
      for (int k = 1; k <= n; ++k) {
        const float s = arc.length_m * static_cast<float>(k) /
                        static_cast<float>(n);
        const Section cur = section_at(arc, s, params, e.width_m);
        // CCW seen from +Y: cross(left_normal, tangent) is up, so the right
        // edge leads.
        tris.push_back(prev.right);
        tris.push_back(prev.left);
        tris.push_back(cur.left);
        tris.push_back(prev.right);
        tris.push_back(cur.left);
        tris.push_back(cur.right);
        prev = cur;
      }
    }
  }
  return tris;
}

}  // namespace badlands
