#include "mapgen/window_rivers.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>

#include "mapgen/hydrology.hpp"
#include "mapgen/river_clip.hpp"
#include "mapgen/river_prune.hpp"

namespace badlands::mapgen {

std::vector<RiverInflow> load_inflows(const std::string& dir,
                                      float* runoff_m_per_yr) {
  std::vector<RiverInflow> out;
  std::ifstream f(dir + "/inflows.txt");
  if (!f) return out;  // absence means "no river crosses the edge", not an error
  std::string line;
  while (std::getline(f, line)) {
    if (line.empty()) continue;
    if (line[0] == '#') {
      // The runoff the parent map ran at rides in a comment, so a reader can
      // check it rather than assume the two agree.
      std::istringstream cs(line.substr(1));
      std::string key;
      float v = 0.0f;
      if ((cs >> key >> v) && key == "runoff_m_per_yr" && runoff_m_per_yr)
        *runoff_m_per_yr = v;
      continue;
    }
    std::istringstream ls(line);
    RiverInflow in;
    if (ls >> in.texel_x >> in.texel_y >> in.discharge_m3_s)
      out.push_back(in);
  }
  return out;
}

namespace {

// Grows a field by one GHOST cell on every side.
//
// route_flow makes its border base level -- receiver -1, a sink. On a whole map
// that is right: the edge IS the outlet. On a CUTOUT it is wrong, and measurably
// so: with the raw window, ~90% of the discharge "left the map", while tracing
// the same ground on the parent showed 0% leaving and at least 27% reaching a
// lake. Every frame edge had become an artificial outlet, so water drained off
// whichever side was nearest instead of crossing the window to the lake.
//
// Padding moves that sink out onto ghost cells, which makes every REAL edge cell
// an interior cell that routes by gradient like any other. The ghost elevation
// is the local gradient continued outward,
//
//     ghost = 2*edge - inner
//
// so terrain that rises as it leaves the window gives a ghost ABOVE the edge and
// pushes water back inward, while terrain that genuinely falls away gives a
// ghost below it and lets the water out. The boundary stops being a decision and
// becomes a measurement.
//
// One cell is enough to stop the edge acting as a sink. It does NOT recover a
// depression whose true spill point lies outside the window -- nothing short of
// real outside data can -- but that is a second-order error, not a frame that
// drains the map.
Field2D<float> pad_extrapolated(const Field2D<float>& f) {
  const int w = f.width, h = f.height;
  Field2D<float> p(w + 2, h + 2, 0.0f);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) p.at(x + 1, y + 1) = f.at(x, y);
  const auto lin = [](float edge, float inner) { return 2.0f * edge - inner; };
  for (int x = 0; x < w; ++x) {
    p.at(x + 1, 0) = lin(f.at(x, 0), f.at(x, std::min(1, h - 1)));
    p.at(x + 1, h + 1) = lin(f.at(x, h - 1), f.at(x, std::max(0, h - 2)));
  }
  for (int y = 0; y < h; ++y) {
    p.at(0, y + 1) = lin(f.at(0, y), f.at(std::min(1, w - 1), y));
    p.at(w + 1, y + 1) = lin(f.at(w - 1, y), f.at(std::max(0, w - 2), y));
  }
  // Corners from their diagonal, so they cannot sit below both neighbours and
  // become a spurious sink that drains the whole corner region.
  p.at(0, 0) = lin(f.at(0, 0), f.at(std::min(1, w - 1), std::min(1, h - 1)));
  p.at(w + 1, 0) = lin(f.at(w - 1, 0), f.at(std::max(0, w - 2), std::min(1, h - 1)));
  p.at(0, h + 1) = lin(f.at(0, h - 1), f.at(std::min(1, w - 1), std::max(0, h - 2)));
  p.at(w + 1, h + 1) =
      lin(f.at(w - 1, h - 1), f.at(std::max(0, w - 2), std::max(0, h - 2)));
  return p;
}

// Same padding, replicating the edge instead of extrapolating it. For fields
// where continuing a gradient is meaningless -- a depth or a lake id -- and the
// honest ghost is "whatever the edge was".
template <typename T>
Field2D<T> pad_replicated(const Field2D<T>& f, T fallback) {
  const int w = f.width, h = f.height;
  Field2D<T> p(w + 2, h + 2, fallback);
  if (w <= 0 || h <= 0) return p;
  for (int y = -1; y <= h; ++y)
    for (int x = -1; x <= w; ++x)
      p.at(x + 1, y + 1) =
          f.at(std::clamp(x, 0, w - 1), std::clamp(y, 0, h - 1));
  return p;
}

}  // namespace

WindowRivers build_window_rivers(const MapArtifacts& art, float world_size_m,
                                 const std::vector<RiverInflow>& inflows,
                                 const ErosionParams& p,
                                 float min_channel_width_m,
                                 float min_branch_length_m) {
  WindowRivers out;
  const int w = art.heightmap.width, h = art.heightmap.height;
  if (w <= 0 || h <= 0 || world_size_m <= 0.0f) return out;
  const float texel_m = world_size_m / static_cast<float>(w);
  const float texel_area = texel_m * texel_m;

  // Lakes route by their flood tree, not steepest descent: a lake surface is
  // flat, so descent on it invents exits through the rim. route_flow wants the
  // tag to cover WHOLE lakes, which lake_id does by construction.
  Field2D<uint8_t> lake_tag(w, h, 0);
  if (art.lake_id.width == w && art.lake_id.height == h) {
    for (size_t i = 0; i < lake_tag.data.size(); ++i)
      lake_tag.data[i] = art.lake_id.data[i] >= 0 ? 1 : 0;
  }

  // The surface water flows over is bed + standing water, not the bed alone.
  Field2D<float> surface = art.heightmap;
  if (art.water_depth.width == w && art.water_depth.height == h) {
    for (size_t i = 0; i < surface.data.size(); ++i)
      surface.data[i] += art.water_depth.data[i];
  }

  // Route on a GHOST-PADDED grid so the window's own edge is not a sink. See
  // pad_extrapolated for why, and for what it measured before the padding.
  const Field2D<float> psurface = pad_extrapolated(surface);
  const Field2D<uint8_t> plake_tag = pad_replicated<uint8_t>(lake_tag, 0);
  const Field2D<float> pdepth =
      (art.water_depth.width == w && art.water_depth.height == h)
          ? pad_replicated<float>(art.water_depth, 0.0f)
          : Field2D<float>(w + 2, h + 2, 0.0f);
  Field2D<int32_t> plake_id =
      (art.lake_id.width == w && art.lake_id.height == h)
          ? pad_replicated<int32_t>(art.lake_id, -1)
          : Field2D<int32_t>(w + 2, h + 2, -1);

  out.routing = route_flow(psurface, texel_m, kEpsilonM, &plake_tag);

  // Each crossing becomes the upstream catchment it implies. Seeding AREA keeps
  // it in the accumulation's own units, so no second conversion can disagree.
  // Offset by one for the ghost ring.
  Field2D<float> extra(w + 2, h + 2, 0.0f);
  const float runoff_m_s = p.runoff_m_per_s > 0.0f ? p.runoff_m_per_s : 3.17e-8f;
  for (const RiverInflow& in : inflows) {
    if (in.texel_x < 0 || in.texel_y < 0 || in.texel_x >= w || in.texel_y >= h)
      continue;
    if (!(in.discharge_m3_s > 0.0f)) continue;
    extra.at(in.texel_x + 1, in.texel_y + 1) += in.discharge_m3_s / runoff_m_s;
    out.inflow_m3_s += in.discharge_m3_s;
  }
  out.rain_m3_s = runoff_m_s * world_size_m * world_size_m;

  const Field2D<float> parea =
      accumulate_drainage(out.routing, texel_area, &extra);

  // origin_m = -texel_m puts padded texel (1,1) at world (0,0), so the graph
  // comes out in WINDOW coordinates and needs no rebasing afterwards.
  out.graph = extract_river_graph(out.routing, parea, pdepth, psurface, p,
                                  texel_m, -texel_m, &plake_id,
                                  art.lakes.empty() ? nullptr : &art.lakes);

  // Trim the ghost overhang and mint a node exactly where each reach crosses.
  clip_river_graph_to_rect(out.graph, glm::vec2(0.0f, 0.0f),
                           glm::vec2(world_size_m, world_size_m));
  prune_river_graph_by_width(out.graph, min_channel_width_m);
  // Length AFTER width: the width cut trims reaches back to where they qualify,
  // so a reach's length is only final once that has happened. Running length
  // first would measure stretches the width cut was about to discard anyway.
  prune_river_graph_by_length(out.graph, min_branch_length_m);

  // Report drainage over the REAL window, not the padded grid.
  out.drainage_area_m2 = Field2D<float>(w, h, 0.0f);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x)
      out.drainage_area_m2.at(x, y) = parea.at(x + 1, y + 1);
  return out;
}

}  // namespace badlands::mapgen
