#include "mapgen/river_prune.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace badlands::mapgen {

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
      // Nor is a FrameEntry node one, even though it also has in_deg == 0: its
      // upstream continues beyond the window, so a short in-frame stretch means
      // the WINDOW is short, not the river. Without this, in_deg == 0 cannot
      // tell a genuine headwater from a trunk that merely crosses in, and this
      // walk eats the trunk exactly like a real stubby headwater.
      if (g.nodes[e0.from].kind == RiverNodeKind::FrameEntry) continue;
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
        n.kind == RiverNodeKind::Mouth || n.kind == RiverNodeKind::FrameEntry)
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
  // Re-derive Source/Confluence from what survived. A lake, mouth or frame-
  // entry node keeps its kind: those describe a boundary, not an upstream
  // count.
  for (size_t i = 0; i < nodes.size(); ++i) {
    RiverNode& n = nodes[i];
    if (n.kind == RiverNodeKind::LakeInlet || n.kind == RiverNodeKind::LakeOutlet ||
        n.kind == RiverNodeKind::Mouth || n.kind == RiverNodeKind::FrameEntry)
      continue;
    if (in_deg[i] == 0)
      n.kind = RiverNodeKind::Source;
    else if (in_deg[i] >= 2)
      n.kind = RiverNodeKind::Confluence;
  }
  g.nodes = std::move(nodes);
  g.edges = std::move(kept);
}

}  // namespace badlands::mapgen
