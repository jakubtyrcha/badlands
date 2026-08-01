#include "mapgen/window_rivers.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

#include "mapgen/hydrology.hpp"

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

// Fraction along a->b at which the segment crosses the window rect, or -1 if it
// does not cross. Axis-aligned rect, so this is the nearest axis crossing.
float boundary_crossing_t(glm::vec2 a, glm::vec2 b, float size_m) {
  float best = -1.0f;
  const auto consider = [&](float av, float bv, float bound) {
    if ((av < bound) == (bv < bound)) return;  // both sides, no crossing
    const float d = bv - av;
    if (std::fabs(d) < 1e-9f) return;
    const float t = (bound - av) / d;
    if (t <= 0.0f || t >= 1.0f) return;
    if (best < 0.0f || t < best) best = t;
  };
  consider(a.x, b.x, 0.0f);
  consider(a.x, b.x, size_m);
  consider(a.y, b.y, 0.0f);
  consider(a.y, b.y, size_m);
  return best;
}

// Upper bound EXCLUSIVE. Texel centres sit at `index * texel_m` with the ghost
// ring at origin_m = -texel_m, so the -x/-y ghosts land at -texel_m (clearly
// out) but the +x/+y ghosts land at exactly `world_size_m`. An inclusive bound
// kept the overhang on two sides of four, which is both wrong and asymmetric.
// No real texel reaches world_size_m -- the last is at (n-1)*texel_m.
bool inside_window(glm::vec2 p, float size_m) {
  return p.x >= 0.0f && p.y >= 0.0f && p.x < size_m && p.y < size_m;
}

}  // namespace

void clip_river_graph_to_window(RiverGraph& g, float world_size_m) {
  if (world_size_m <= 0.0f) return;
  std::vector<RiverEdge> kept;
  kept.reserve(g.edges.size());

  // Mints a node on the frame, with every property interpolated along the
  // crossing segment. A reach that meets the boundary must start or end at a
  // real place: without this it stops at whichever sample happened to be last
  // inside, which moves whenever the resampling changes.
  const auto add_crossing = [&](const RiverEdge& e, size_t i, size_t j, float t,
                                RiverNodeKind kind) {
    RiverNode n;
    n.pos_m = glm::mix(e.points_m[i], e.points_m[j], t);
    n.discharge_m3_s = glm::mix(e.discharge_m3_s[i], e.discharge_m3_s[j], t);
    n.width_m = glm::mix(e.width_m[i], e.width_m[j], t);
    n.depth_m = glm::mix(e.depth_m[i], e.depth_m[j], t);
    n.speed_m_s = glm::mix(e.speed_m_s[i], e.speed_m_s[j], t);
    n.kind = kind;
    g.nodes.push_back(n);
    return static_cast<int32_t>(g.nodes.size() - 1);
  };

  for (const RiverEdge& e : g.edges) {
    // THROUGH-LAKE EDGES CARRY NO GEOMETRY ON PURPOSE (river_graph.cpp pass 3):
    // an inlet -> outlet edge exists so the network stays traversable
    // river -> lake -> river, and the lake surface is already water so drawing a
    // channel across it would be wrong. Dropping them for having < 2 points
    // severed that link, which also left every lake's OUTFLOW reach with
    // in_deg == 0 -- so the length prune saw it as a headwater and could delete
    // the trunk below a lake. They have nothing to clip; pass them through.
    if (e.points_m.size() < 2) {
      kept.push_back(e);
      continue;
    }

    const size_t np = e.points_m.size();
    size_t i = 0;
    while (i < np) {
      if (!inside_window(e.points_m[i], world_size_m)) { ++i; continue; }

      RiverEdge run;
      run.strahler_order = e.strahler_order;
      run.shreve_magnitude = e.shreve_magnitude;
      const auto push = [&](glm::vec2 pt, float q, float wd, float dp, float sp) {
        run.points_m.push_back(pt);
        run.discharge_m3_s.push_back(q);
        run.width_m.push_back(wd);
        run.depth_m.push_back(dp);
        run.speed_m_s.push_back(sp);
      };

      if (i == 0) {
        run.from = e.from;
      } else {
        // ENTERING the window. The upstream end gets a frame node too -- the
        // same interpolation as the exit, mirrored. Without it a reach began at
        // whichever resampled sample first landed inside.
        const float t = boundary_crossing_t(e.points_m[i - 1], e.points_m[i],
                                            world_size_m);
        if (t >= 0.0f) {
          const int32_t nid =
              add_crossing(e, i - 1, i, t, RiverNodeKind::Source);
          push(g.nodes[nid].pos_m, g.nodes[nid].discharge_m3_s,
               g.nodes[nid].width_m, g.nodes[nid].depth_m,
               g.nodes[nid].speed_m_s);
          run.from = nid;
        } else {
          run.from = -1;  // anchored below
        }
      }

      push(e.points_m[i], e.discharge_m3_s[i], e.width_m[i], e.depth_m[i],
           e.speed_m_s[i]);

      size_t k = i;
      while (k + 1 < np && inside_window(e.points_m[k + 1], world_size_m)) {
        ++k;
        push(e.points_m[k], e.discharge_m3_s[k], e.width_m[k], e.depth_m[k],
             e.speed_m_s[k]);
      }

      if (k + 1 < np) {
        // LEAVING the window. `to` is assigned on every path out of this branch:
        // a run that exited with to == -1 was a downstream endpoint no node
        // explained, which happened whenever the last inside sample sat exactly
        // on the frame and the crossing parameter degenerated.
        const float t =
            boundary_crossing_t(e.points_m[k], e.points_m[k + 1], world_size_m);
        const int32_t nid =
            add_crossing(e, k, k + 1, t >= 0.0f ? t : 0.0f, RiverNodeKind::Mouth);
        push(g.nodes[nid].pos_m, g.nodes[nid].discharge_m3_s,
             g.nodes[nid].width_m, g.nodes[nid].depth_m, g.nodes[nid].speed_m_s);
        run.to = nid;
      } else {
        run.to = e.to;
      }

      if (run.points_m.size() >= 2) kept.push_back(std::move(run));
      i = k + 1;
    }
  }
  g.edges = std::move(kept);

  // A run that started mid-chain with a degenerate crossing has no upstream node
  // yet; give it one where it enters, so every reach is anchored at both ends.
  for (RiverEdge& e : g.edges) {
    if (e.from >= 0 || e.points_m.empty()) continue;
    RiverNode n;
    n.pos_m = e.points_m.front();
    n.discharge_m3_s = e.discharge_m3_s.front();
    n.width_m = e.width_m.front();
    n.depth_m = e.depth_m.front();
    n.speed_m_s = e.speed_m_s.front();
    n.kind = RiverNodeKind::Source;
    g.nodes.push_back(n);
    e.from = static_cast<int32_t>(g.nodes.size() - 1);
  }

  // Clipping can strand a node whose downstream reach left the window: water
  // arrives and never leaves, which is not a thing terrain does. As far as this
  // window can tell the network exits there, so say so -- otherwise the graph
  // reports a sink that no sink kind explains, and a consumer looking for
  // outflow points would miss it.
  std::vector<int> in_deg(g.nodes.size(), 0), out_deg(g.nodes.size(), 0);
  for (const RiverEdge& e : g.edges) {
    if (e.to >= 0) in_deg[e.to]++;
    if (e.from >= 0) out_deg[e.from]++;
  }
  for (size_t i = 0; i < g.nodes.size(); ++i) {
    if (in_deg[i] == 0 || out_deg[i] != 0) continue;
    RiverNode& n = g.nodes[i];
    if (n.kind == RiverNodeKind::Source || n.kind == RiverNodeKind::Confluence)
      n.kind = RiverNodeKind::Mouth;
  }
}

namespace {

float polyline_length_m(const std::vector<glm::vec2>& pts) {
  float len = 0.0f;
  for (size_t i = 1; i < pts.size(); ++i) {
    const glm::vec2 d = pts[i] - pts[i - 1];
    len += std::sqrt(d.x * d.x + d.y * d.y);
  }
  return len;
}

}  // namespace

void prune_river_graph_by_length(RiverGraph& g, float min_length_m) {
  if (!(min_length_m > 0.0f)) return;

  bool changed = true;
  while (changed) {
    changed = false;
    const size_t nn = g.nodes.size();
    std::vector<int> in_deg(nn, 0), out_deg(nn, 0);
    std::vector<int32_t> single_out(nn, -1);
    for (size_t ei = 0; ei < g.edges.size(); ++ei) {
      const RiverEdge& e = g.edges[ei];
      if (e.to >= 0 && e.to < static_cast<int32_t>(nn)) in_deg[e.to]++;
      if (e.from >= 0 && e.from < static_cast<int32_t>(nn)) {
        out_deg[e.from]++;
        single_out[e.from] = static_cast<int32_t>(ei);
      }
    }

    // A BRANCH is the whole chain from a headwater down to the first
    // confluence, not one reach. Measuring per reach was wrong: clipping splits
    // a reach at the frame and gives each fragment its own start node, so every
    // fragment looked like a headwater and the pass ate a 700 m trunk one
    // fragment at a time (peak Q fell 0.7183 -> 0.0218 m3/s). Accumulating along
    // the chain is immune to how the reach happens to be subdivided.
    std::vector<uint8_t> drop(g.edges.size(), 0);
    for (size_t ei = 0; ei < g.edges.size(); ++ei) {
      const RiverEdge& e0 = g.edges[ei];
      if (e0.from < 0 || e0.from >= static_cast<int32_t>(nn)) continue;
      if (in_deg[e0.from] != 0) continue;  // not a headwater
      // A through-lake connector has no geometry, so it contributes no length
      // and must never START a branch measurement -- otherwise a lake's outflow
      // chain measures 0 m and gets deleted.
      if (g.edges[ei].points_m.size() < 2) continue;
      float len = 0.0f;
      std::vector<size_t> chain;
      int32_t cur = static_cast<int32_t>(ei);
      while (cur >= 0) {
        chain.push_back(static_cast<size_t>(cur));
        len += polyline_length_m(g.edges[cur].points_m);
        if (len >= min_length_m) break;  // long enough; stop walking
        const int32_t to = g.edges[cur].to;
        // Stop at a confluence: past it the water is no longer this branch.
        if (to < 0 || to >= static_cast<int32_t>(nn) || in_deg[to] >= 2 ||
            out_deg[to] != 1)
          break;
        cur = single_out[to];
      }
      // A branch that ENDS IN WATER is kept whatever its length. Channel
      // geometry stops at the shoreline, so a river crossing a lake arm survives
      // only as short dry connectors between arms -- on this window the four
      // highest-discharge reaches are 1.0-4.5 m long and carry 0.72 m3/s, the
      // trunk itself. Length is a noise filter for channels that peter out on dry
      // ground; a channel that reaches water is a feature no matter how short.
      const int32_t last_to = g.edges[chain.back()].to;
      const bool ends_in_water =
          last_to >= 0 && last_to < static_cast<int32_t>(nn) &&
          (g.nodes[last_to].kind == RiverNodeKind::LakeInlet ||
           g.nodes[last_to].kind == RiverNodeKind::LakeOutlet);
      if (len < min_length_m && !ends_in_water)
        for (size_t c : chain) drop[c] = 1;
    }

    std::vector<RiverEdge> kept;
    kept.reserve(g.edges.size());
    for (size_t ei = 0; ei < g.edges.size(); ++ei) {
      if (drop[ei]) { changed = true; continue; }
      kept.push_back(std::move(g.edges[ei]));
    }
    g.edges = std::move(kept);
  }

  // Compact the node set to what survived and re-derive the kinds, exactly as
  // the width prune does -- a confluence that lost every tributary is a source.
  std::vector<int32_t> remap(g.nodes.size(), -1);
  std::vector<RiverNode> nodes;
  const auto keep_node = [&](int32_t id) -> int32_t {
    if (id < 0 || id >= static_cast<int32_t>(remap.size())) return -1;
    if (remap[id] < 0) {
      remap[id] = static_cast<int32_t>(nodes.size());
      nodes.push_back(g.nodes[id]);
    }
    return remap[id];
  };
  for (RiverEdge& e : g.edges) {
    e.from = keep_node(e.from);
    e.to = keep_node(e.to);
  }
  std::vector<int> in_deg(nodes.size(), 0);
  for (const RiverEdge& e : g.edges)
    if (e.to >= 0) in_deg[e.to]++;
  for (size_t i = 0; i < nodes.size(); ++i) {
    RiverNode& n = nodes[i];
    if (n.kind == RiverNodeKind::LakeInlet || n.kind == RiverNodeKind::LakeOutlet ||
        n.kind == RiverNodeKind::Mouth)
      continue;
    if (in_deg[i] == 0) n.kind = RiverNodeKind::Source;
    else if (in_deg[i] >= 2) n.kind = RiverNodeKind::Confluence;
  }
  g.nodes = std::move(nodes);
}

void prune_river_graph_by_width(RiverGraph& g, float min_width_m) {
  if (!(min_width_m > 0.0f)) return;

  std::vector<RiverEdge> kept;
  kept.reserve(g.edges.size());
  for (RiverEdge& e : g.edges) {
    // Through-lake connectors carry no geometry and therefore no width. They are
    // topology, not channel -- dropping them for having no samples severed
    // river -> lake -> river and orphaned every lake's outflow reach.
    if (e.points_m.size() < 2) {
      kept.push_back(std::move(e));
      continue;
    }
    if (e.width_m.empty()) continue;
    float widest = 0.0f;
    for (float w : e.width_m) widest = std::max(widest, w);
    if (widest < min_width_m) continue;  // a brook end to end

    // Trim the headwater end back to where the reach first qualifies. Width is
    // monotone downstream, so this is a single cut, not a filter.
    size_t first = 0;
    while (first < e.width_m.size() && e.width_m[first] < min_width_m) ++first;
    // Keep one point upstream of the crossing where there is one, so a trimmed
    // reach still meets its parent instead of starting in mid-air.
    if (first > 0) --first;
    if (e.points_m.size() - first < 2) continue;
    if (first > 0) {
      e.points_m.erase(e.points_m.begin(), e.points_m.begin() + first);
      e.discharge_m3_s.erase(e.discharge_m3_s.begin(),
                             e.discharge_m3_s.begin() + first);
      e.width_m.erase(e.width_m.begin(), e.width_m.begin() + first);
      e.depth_m.erase(e.depth_m.begin(), e.depth_m.begin() + first);
      e.speed_m_s.erase(e.speed_m_s.begin(), e.speed_m_s.begin() + first);
    }
    kept.push_back(std::move(e));
  }

  // Compact the node set to what the survivors reference, remapping indices.
  std::vector<int32_t> remap(g.nodes.size(), -1);
  std::vector<RiverNode> nodes;
  auto keep_node = [&](int32_t id) -> int32_t {
    if (id < 0 || id >= static_cast<int32_t>(remap.size())) return -1;
    if (remap[id] < 0) {
      remap[id] = static_cast<int32_t>(nodes.size());
      nodes.push_back(g.nodes[id]);
    }
    return remap[id];
  };
  std::vector<int> in_deg, out_deg;
  for (RiverEdge& e : kept) {
    e.from = keep_node(e.from);
    e.to = keep_node(e.to);
  }
  in_deg.assign(nodes.size(), 0);
  out_deg.assign(nodes.size(), 0);
  for (const RiverEdge& e : kept) {
    if (e.to >= 0) in_deg[e.to]++;
    if (e.from >= 0) out_deg[e.from]++;
  }
  // Re-derive Source/Confluence from what survived. A lake or mouth node keeps
  // its kind: those describe a boundary, not an upstream count.
  for (size_t i = 0; i < nodes.size(); ++i) {
    RiverNode& n = nodes[i];
    if (n.kind == RiverNodeKind::LakeInlet || n.kind == RiverNodeKind::LakeOutlet ||
        n.kind == RiverNodeKind::Mouth)
      continue;
    if (in_deg[i] == 0)
      n.kind = RiverNodeKind::Source;
    else if (in_deg[i] >= 2)
      n.kind = RiverNodeKind::Confluence;
  }
  g.nodes = std::move(nodes);
  g.edges = std::move(kept);
}

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
  clip_river_graph_to_window(out.graph, world_size_m);
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
