#include "mapgen/river_clip.hpp"

#include <cmath>
#include <vector>

namespace badlands::mapgen {

namespace {

// Fraction along a->b at which the segment crosses the rect boundary, or -1 if
// it does not cross. Axis-aligned rect, so this is the nearest axis crossing.
float boundary_crossing_t(glm::vec2 a, glm::vec2 b, glm::vec2 lo_m,
                          glm::vec2 hi_m) {
  float best = -1.0f;
  const auto consider = [&](float av, float bv, float bound) {
    if ((av < bound) == (bv < bound)) return;  // both sides, no crossing
    const float d = bv - av;
    if (std::fabs(d) < 1e-9f) return;
    const float t = (bound - av) / d;
    if (t <= 0.0f || t >= 1.0f) return;
    if (best < 0.0f || t < best) best = t;
  };
  consider(a.x, b.x, lo_m.x);
  consider(a.x, b.x, hi_m.x);
  consider(a.y, b.y, lo_m.y);
  consider(a.y, b.y, hi_m.y);
  return best;
}

// Upper bound EXCLUSIVE. Texel centres sit at `index * texel_m` with the ghost
// ring at origin_m = -texel_m, so the -x/-y ghosts land at lo_m - texel_m
// (clearly out) but the +x/+y ghosts land at exactly `hi_m`. An inclusive
// bound kept the overhang on two sides of four, which is both wrong and
// asymmetric. No real texel reaches hi_m -- the last is one texel short of it.
bool inside_window(glm::vec2 p, glm::vec2 lo_m, glm::vec2 hi_m) {
  return p.x >= lo_m.x && p.y >= lo_m.y && p.x < hi_m.x && p.y < hi_m.y;
}

}  // namespace

void clip_river_graph_to_rect(RiverGraph& g, glm::vec2 lo_m, glm::vec2 hi_m) {
  if (hi_m.x <= lo_m.x || hi_m.y <= lo_m.y) return;
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
    // the trunk below a lake. They have nothing to clip.
    //
    // But CONTAINMENT still applies. Passing them through unconditionally kept
    // one orphan edge per lake in the WHOLE source graph: a 128 m patch cut
    // from a 16 km world inherited every lake connector in that world, with
    // endpoints that later rebase to wildly out-of-range patch-local
    // coordinates. Nothing downstream draws them (build_river_arcs skips a
    // geometry-free edge), so it never showed on screen -- it showed as an
    // inflated reach count and a graph that disagreed with its own patch. The
    // length prune cannot clean them up either: it never starts a chain on a
    // connector.
    //
    // A connector is kept when BOTH its endpoints are in the rect, which is
    // exactly when the lake it crosses belongs to this patch.
    if (e.points_m.size() < 2) {
      const auto node_inside = [&](int32_t id) {
        return id >= 0 && id < static_cast<int32_t>(g.nodes.size()) &&
               inside_window(g.nodes[id].pos_m, lo_m, hi_m);
      };
      if (node_inside(e.from) && node_inside(e.to)) kept.push_back(e);
      continue;
    }

    const size_t np = e.points_m.size();
    size_t i = 0;
    while (i < np) {
      if (!inside_window(e.points_m[i], lo_m, hi_m)) { ++i; continue; }

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
        // ENTERING the rect. The upstream end gets a frame node too -- the same
        // interpolation as the exit, mirrored. Without it a reach began at
        // whichever resampled sample first landed inside. This is the inbound
        // twin of the Mouth minted below, so it is FrameEntry, not Source: a
        // trunk that merely crosses into the rect is not a headwater.
        const float t = boundary_crossing_t(e.points_m[i - 1], e.points_m[i],
                                            lo_m, hi_m);
        if (t >= 0.0f) {
          const int32_t nid =
              add_crossing(e, i - 1, i, t, RiverNodeKind::FrameEntry);
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
      while (k + 1 < np && inside_window(e.points_m[k + 1], lo_m, hi_m)) {
        ++k;
        push(e.points_m[k], e.discharge_m3_s[k], e.width_m[k], e.depth_m[k],
             e.speed_m_s[k]);
      }

      if (k + 1 < np) {
        // LEAVING the rect. `to` is assigned on every path out of this branch:
        // a run that exited with to == -1 was a downstream endpoint no node
        // explained, which happened whenever the last inside sample sat exactly
        // on the frame and the crossing parameter degenerated.
        const float t =
            boundary_crossing_t(e.points_m[k], e.points_m[k + 1], lo_m, hi_m);
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
  //
  // FrameEntry, not Source, for exactly the reason the interpolated case above
  // is. This branch is reached only when i > 0 -- the previous sample was
  // OUTSIDE -- so the reach's upstream continues beyond the frame by
  // construction. It is not a rare path either: boundary_crossing_t rejects
  // t >= 1, so a sample landing EXACTLY on the frame degenerates here, which is
  // what a lattice-aligned channel does routinely. Minting Source here would
  // reopen the trunk-eating bug in the one case most likely to hit it.
  for (RiverEdge& e : g.edges) {
    if (e.from >= 0 || e.points_m.empty()) continue;
    RiverNode n;
    n.pos_m = e.points_m.front();
    n.discharge_m3_s = e.discharge_m3_s.front();
    n.width_m = e.width_m.front();
    n.depth_m = e.depth_m.front();
    n.speed_m_s = e.speed_m_s.front();
    n.kind = RiverNodeKind::FrameEntry;
    g.nodes.push_back(n);
    e.from = static_cast<int32_t>(g.nodes.size() - 1);
  }

  // Clipping can strand a node whose downstream reach left the rect: water
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

}  // namespace badlands::mapgen
