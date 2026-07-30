#include "mapgen/river_graph.hpp"

#include <algorithm>
#include <cmath>

namespace badlands::mapgen {

ChannelHydraulics channel_hydraulics(float discharge_m3_s, float slope,
                                     const ErosionParams& p) {
  ChannelHydraulics out;
  if (!(discharge_m3_s > 0.0f)) return out;  // also rejects NaN
  const float q = discharge_m3_s;
  const float s = std::max(slope, kMinChannelSlope);

  // Regime width closure: width is set by discharge, essentially independent
  // of slope.
  out.width_m = p.channel_width_coeff * std::sqrt(q);
  // Manning + continuity solved together (see the header): eliminating v from
  // Q = w*d*v and v = (1/n)*d^(2/3)*sqrt(S) gives Q = (w*sqrt(S)/n) * d^(5/3).
  out.depth_m = std::pow(q * p.manning_n / (out.width_m * std::sqrt(s)), 0.6f);
  // Recover v from continuity rather than from Manning, so w*d*v == Q holds to
  // float precision instead of merely agreeing analytically.
  out.speed_m_s = q / (out.width_m * out.depth_m);
  return out;
}

RiverClass classify_discharge(float discharge_m3_s) {
  if (!(discharge_m3_s > 0.0f)) return RiverClass::None;
  if (discharge_m3_s < 1e-4f) return RiverClass::Rill;
  if (discharge_m3_s < 1e-3f) return RiverClass::Brook;
  if (discharge_m3_s < 1e-2f) return RiverClass::Stream;
  if (discharge_m3_s < 1e-1f) return RiverClass::Creek;
  if (discharge_m3_s < 1.0f) return RiverClass::River;
  return RiverClass::Major;
}

namespace {

// Perpendicular distance from `p` to the segment ab (to the nearest endpoint
// when the projection falls outside it).
float point_segment_distance(glm::vec2 p, glm::vec2 a, glm::vec2 b) {
  const glm::vec2 ab = b - a;
  const float len2 = glm::dot(ab, ab);
  if (len2 <= 0.0f) return glm::length(p - a);
  const float t = std::clamp(glm::dot(p - a, ab) / len2, 0.0f, 1.0f);
  return glm::length(p - (a + t * ab));
}

// Iterative Douglas-Peucker over the half-open ranges [lo, hi]; `keep` marks
// surviving vertices. Iterative rather than recursive so a pathological chain
// cannot blow the stack.
void douglas_peucker(const std::vector<glm::vec2>& pts, float tolerance_m,
                     std::vector<uint8_t>& keep) {
  std::vector<std::pair<size_t, size_t>> stack{{0, pts.size() - 1}};
  while (!stack.empty()) {
    const auto [lo, hi] = stack.back();
    stack.pop_back();
    if (hi <= lo + 1) continue;
    float worst = -1.0f;
    size_t worst_i = lo;
    for (size_t i = lo + 1; i < hi; ++i) {
      const float d = point_segment_distance(pts[i], pts[lo], pts[hi]);
      if (d > worst) {
        worst = d;
        worst_i = i;
      }
    }
    if (worst <= tolerance_m) continue;  // whole span is within tolerance
    keep[worst_i] = 1;
    stack.push_back({lo, worst_i});
    stack.push_back({worst_i, hi});
  }
}

}  // namespace

std::vector<glm::vec2> simplify_polyline(const std::vector<glm::vec2>& pts,
                                         float tolerance_m) {
  if (pts.size() <= 2) return pts;
  std::vector<uint8_t> keep(pts.size(), 0);
  keep.front() = 1;
  keep.back() = 1;
  douglas_peucker(pts, tolerance_m, keep);
  std::vector<glm::vec2> out;
  out.reserve(pts.size());
  for (size_t i = 0; i < pts.size(); ++i)
    if (keep[i]) out.push_back(pts[i]);
  return out;
}

std::vector<glm::vec2> resample_polyline(const std::vector<glm::vec2>& pts,
                                         float spacing_m) {
  if (pts.size() <= 1 || spacing_m <= 0.0f) return pts;
  float total = 0.0f;
  for (size_t i = 1; i < pts.size(); ++i) total += glm::length(pts[i] - pts[i - 1]);
  if (total <= spacing_m) return {pts.front(), pts.back()};

  std::vector<glm::vec2> out{pts.front()};
  // Walk the polyline emitting a vertex every `spacing_m` of arc length. The
  // final endpoint is appended verbatim rather than interpolated, so topology
  // (and any lake attachment) survives resampling exactly.
  float carried = 0.0f;
  for (size_t i = 1; i < pts.size(); ++i) {
    const glm::vec2 a = pts[i - 1], b = pts[i];
    float seg = glm::length(b - a);
    if (seg <= 0.0f) continue;
    const glm::vec2 dir = (b - a) / seg;
    float t = spacing_m - carried;
    while (t <= seg) {
      out.push_back(a + dir * t);
      t += spacing_m;
    }
    carried = seg - (t - spacing_m);
  }
  // Drop a final sample that landed almost on the endpoint, so the pinned
  // endpoint does not produce a zero-length last segment.
  if (out.size() > 1 && glm::length(out.back() - pts.back()) < 0.5f * spacing_m)
    out.pop_back();
  out.push_back(pts.back());
  return out;
}

namespace {

// Distance from a point to an axis-aligned box (0 inside).
float point_aabb_distance(glm::vec2 p, glm::vec2 lo, glm::vec2 hi) {
  const glm::vec2 d = glm::max(glm::vec2(0.0f), glm::max(lo - p, p - hi));
  return glm::length(d);
}

// Does the segment ab touch the box at all? Liang-Barsky slab clip.
bool segment_hits_aabb(glm::vec2 a, glm::vec2 b, glm::vec2 lo, glm::vec2 hi) {
  float t0 = 0.0f, t1 = 1.0f;
  const glm::vec2 d = b - a;
  for (int axis = 0; axis < 2; ++axis) {
    const float p = d[axis], q0 = lo[axis] - a[axis], q1 = hi[axis] - a[axis];
    if (std::abs(p) < 1e-20f) {  // parallel to this slab
      if (a[axis] < lo[axis] || a[axis] > hi[axis]) return false;
      continue;
    }
    float lo_t = q0 / p, hi_t = q1 / p;
    if (lo_t > hi_t) std::swap(lo_t, hi_t);
    t0 = std::max(t0, lo_t);
    t1 = std::min(t1, hi_t);
    if (t0 > t1) return false;
  }
  return true;
}

// Exact minimum distance between a segment and an axis-aligned box. For two
// convex sets the minimum is attained at a vertex of one against the other, so
// checking the box's corners against the segment and the segment's endpoints
// against the box covers every case once intersection is excluded.
float segment_aabb_distance(glm::vec2 a, glm::vec2 b, glm::vec2 lo, glm::vec2 hi) {
  if (segment_hits_aabb(a, b, lo, hi)) return 0.0f;
  const glm::vec2 corners[4] = {lo, {hi.x, lo.y}, {lo.x, hi.y}, hi};
  float best = std::min(point_aabb_distance(a, lo, hi), point_aabb_distance(b, lo, hi));
  for (const auto& c : corners) best = std::min(best, point_segment_distance(c, a, b));
  return best;
}

// Per-cell scratch shared by the extraction passes.
struct ChannelField {
  std::vector<int32_t> lake_id;   // >= 0 inside a surviving lake
  std::vector<uint8_t> channel;   // in the channel set (dry, above threshold)
  std::vector<int32_t> donors;    // upstream channel neighbours
};

ChannelField build_channel_field(const FlowRouting& r, const Field2D<float>& area,
                                 const Field2D<float>& water_depth,
                                 const ErosionParams& p) {
  const size_t n = r.receiver.size();
  ChannelField cf;
  cf.lake_id.assign(n, -1);
  cf.channel.assign(n, 0);
  cf.donors.assign(n, 0);

  // Lakes come from the PRUNED water field, not r.in_lake. That is the whole
  // fix for the old fragmentation: river_intensity zeroed every in_lake cell,
  // including the ~11% in depressions pruning had already returned to dry
  // land, which punched gaps with no visible lake to explain them.
  std::vector<uint8_t> wet(n, 0);
  for (size_t i = 0; i < n; ++i) wet[i] = water_depth.data[i] > 0.0f ? 1 : 0;
  const auto comps = label_lake_components(r.width, r.height, wet);
  for (size_t c = 0; c < comps.size(); ++c)
    for (const int i : comps[c]) cf.lake_id[static_cast<size_t>(i)] = static_cast<int32_t>(c);

  for (size_t i = 0; i < n; ++i)
    cf.channel[i] = (cf.lake_id[i] < 0 && area.data[i] >= p.min_channel_area_m2) ? 1 : 0;

  for (size_t i = 0; i < n; ++i) {
    if (!cf.channel[i]) continue;
    const int32_t rcv = r.receiver[i];
    if (rcv >= 0 && cf.channel[static_cast<size_t>(rcv)]) ++cf.donors[static_cast<size_t>(rcv)];
  }
  return cf;
}

// Strahler and Shreve over the edge forest, by Kahn's algorithm on edges. An
// explicit topological pass rather than sorting by cell pop-order: through-lake
// edges connect cells whose relative pop order is not meaningful.
void compute_stream_orders(RiverGraph& g) {
  const size_t ne = g.edges.size();
  std::vector<std::vector<int32_t>> incoming(g.nodes.size());
  for (size_t e = 0; e < ne; ++e)
    incoming[static_cast<size_t>(g.edges[e].to)].push_back(static_cast<int32_t>(e));

  std::vector<int32_t> pending(ne, 0);
  std::vector<int32_t> ready;
  for (size_t e = 0; e < ne; ++e) {
    pending[e] = static_cast<int32_t>(incoming[static_cast<size_t>(g.edges[e].from)].size());
    if (pending[e] == 0) ready.push_back(static_cast<int32_t>(e));
  }
  // ALL outgoing edges per node, not one. Storing a single index silently
  // dropped every edge but the last written: any edge not reachable from the
  // downstream walk never has its `pending` decremented to zero, so it is
  // never processed and keeps the default order 1 / magnitude 1 regardless of
  // its real upstream network. A wrong answer with no diagnostic.
  std::vector<std::vector<int32_t>> out_edges(g.nodes.size());
  for (size_t e = 0; e < ne; ++e)
    out_edges[static_cast<size_t>(g.edges[e].from)].push_back(static_cast<int32_t>(e));

  size_t head = 0;
  while (head < ready.size()) {
    const int32_t e = ready[head++];
    auto& edge = g.edges[static_cast<size_t>(e)];
    const auto& ups = incoming[static_cast<size_t>(edge.from)];
    if (ups.empty()) {
      edge.strahler_order = 1;
      edge.shreve_magnitude = 1;
    } else {
      int32_t best = 0, best_count = 0, shreve = 0;
      for (const int32_t u : ups) {
        const int32_t o = g.edges[static_cast<size_t>(u)].strahler_order;
        shreve += g.edges[static_cast<size_t>(u)].shreve_magnitude;
        if (o > best) { best = o; best_count = 1; }
        else if (o == best) { ++best_count; }
      }
      // Strahler: two or more reaches of the joint-highest order promote;
      // a strictly lower tributary joining a trunk does not.
      edge.strahler_order = best_count >= 2 ? best + 1 : best;
      edge.shreve_magnitude = shreve;
    }
    for (const int32_t down : out_edges[static_cast<size_t>(edge.to)])
      if (--pending[static_cast<size_t>(down)] == 0) ready.push_back(down);
  }
}

}  // namespace

RiverGraph extract_river_graph(const FlowRouting& r, const Field2D<float>& area,
                               const Field2D<float>& water_depth,
                               const Field2D<float>& ground,
                               const ErosionParams& p, float texel_m,
                               float origin_m, const Field2D<int32_t>* lake_id,
                               const std::vector<LakeInfo>* lakes) {
  RiverGraph g;
  const int w = r.width, ht = r.height;
  if (w <= 0 || ht <= 0) return g;
  const ChannelField cf = build_channel_field(r, area, water_depth, p);
  const size_t n = r.receiver.size();

  auto world_of = [&](int i) {
    return glm::vec2(static_cast<float>(i % w) * texel_m + origin_m,
                     static_cast<float>(i / w) * texel_m + origin_m);
  };
  auto cell_of = [&](glm::vec2 pos) {
    const int x = std::clamp(static_cast<int>(std::lround((pos.x - origin_m) / texel_m)), 0, w - 1);
    const int y = std::clamp(static_cast<int>(std::lround((pos.y - origin_m) / texel_m)), 0, ht - 1);
    return y * w + x;
  };

  // One node per distinguished cell, deduplicated so a confluence is shared by
  // the edges meeting there.
  std::vector<int32_t> node_at(n, -1);
  // `lake_cell` is where provenance is READ FROM, and it is not the node's own
  // cell: a LakeInlet sits on the last dry channel cell before the shore and a
  // LakeOutlet on the sill just outside the lake, so neither is ever a lake
  // cell. Reading lake_id at the node's own cell always returned -1 and left
  // every node's kind at the default.
  auto add_node = [&](int cell, RiverNodeKind kind, int32_t lake,
                      int lake_cell = -1) {
    if (node_at[static_cast<size_t>(cell)] >= 0) return node_at[static_cast<size_t>(cell)];
    RiverNode nd;
    nd.pos_m = world_of(cell);
    nd.ground_m = ground.data[static_cast<size_t>(cell)];
    nd.drainage_area_m2 = area.data[static_cast<size_t>(cell)];
    nd.discharge_m3_s = p.runoff_m_per_s * nd.drainage_area_m2;
    nd.lake_id = lake;
    // Report the lake's provenance where the caller supplied it. Resolved from
    // the erosion pass's own labelling rather than re-derived, so the graph and
    // the water field cannot disagree about which lake a node touches.
    if (lake >= 0 && lake_cell >= 0 && lake_id != nullptr && lakes != nullptr) {
      const int32_t real = lake_id->data[static_cast<size_t>(lake_cell)];
      if (real >= 0 && static_cast<size_t>(real) < lakes->size())
        nd.lake_kind = (*lakes)[static_cast<size_t>(real)].kind;
    }
    nd.kind = kind;
    node_at[static_cast<size_t>(cell)] = static_cast<int32_t>(g.nodes.size());
    g.nodes.push_back(nd);
    return node_at[static_cast<size_t>(cell)];
  };

  // A lake's TRUE sill: among dry cells that receive flow out of the lake, the
  // one with the lowest water_level. Deliberately not deposit's find_exit,
  // which returns whichever exit member[0]'s chain happens to reach — see the
  // v1.1 addendum.
  const int lake_count = 1 + *std::max_element(cf.lake_id.begin(), cf.lake_id.end());
  std::vector<int> sill(static_cast<size_t>(std::max(lake_count, 0)), -1);
  std::vector<int> lake_member(static_cast<size_t>(std::max(lake_count, 0)), -1);
  for (size_t i = 0; i < n; ++i) {
    const int32_t lid = cf.lake_id[i];
    if (lid < 0) continue;
    if (lake_member[static_cast<size_t>(lid)] < 0)
      lake_member[static_cast<size_t>(lid)] = static_cast<int>(i);
    const int32_t rcv = r.receiver[i];
    if (rcv < 0 || cf.lake_id[static_cast<size_t>(rcv)] == lid) continue;
    int& s = sill[static_cast<size_t>(lid)];
    if (s < 0 || r.water_level[static_cast<size_t>(rcv)] < r.water_level[static_cast<size_t>(s)])
      s = rcv;
  }
  // Sill cells are claimed by pass 2. Without this, pass 1 also starts a reach
  // at each of them — a lake's sill has ZERO channel donors, since the only
  // thing draining into it is the lake and lake cells are not in the channel
  // set — so the same chain was walked twice and emitted as two identical
  // edges. add_node dedups by cell, so pass 1 got there first and the node
  // kept kind=Source; the LakeOutlet kind never stuck.
  std::vector<uint8_t> is_sill(n, 0);
  for (const int sc : sill)
    if (sc >= 0) is_sill[static_cast<size_t>(sc)] = 1;

  auto is_terminal = [&](int i) {  // the chain cannot continue past here
    const int32_t rcv = r.receiver[i];
    return rcv < 0 || cf.lake_id[static_cast<size_t>(rcv)] >= 0;
  };

  // Walk one reach downstream from `start`, returning the cell chain.
  auto walk = [&](int start) {
    std::vector<int> chain{start};
    int cur = start;
    while (!is_terminal(cur)) {
      cur = r.receiver[static_cast<size_t>(cur)];
      chain.push_back(cur);
      if (cf.donors[static_cast<size_t>(cur)] >= 2) break;  // confluence: reach ends
    }
    return chain;
  };

  const float tol_m = p.simplify_tolerance_texels * texel_m;
  const float spacing_m = p.resample_spacing_texels * texel_m;

  auto finish_edge = [&](int32_t from_node, int32_t to_node,
                         const std::vector<int>& chain) {
    RiverEdge e;
    e.from = from_node;
    e.to = to_node;
    std::vector<glm::vec2> pts;
    pts.reserve(chain.size());
    for (const int c : chain) pts.push_back(world_of(c));
    // De-lattice: D8 offers only 8 headings, so a raw chain staircases at any
    // other heading. The heading survives in the step ratio, which simplify
    // recovers; resampling then gives evenly spaced vertices.
    pts = resample_polyline(simplify_polyline(pts, tol_m), spacing_m);
    e.points_m = pts;

    const size_t np = pts.size();
    e.discharge_m3_s.resize(np);
    e.width_m.resize(np);
    e.depth_m.resize(np);
    e.speed_m_s.resize(np);
    // Attributes come from the nearest CHAIN cell, not from cell_of(point).
    // Resampling inserts NEW vertices between the Douglas-Peucker survivors,
    // and those can land on a hillslope cell beside the staircased chain —
    // whose drainage area is nothing. Measured on a straight 22.5-degree
    // valley: discharge collapsing by up to 99.2% at interior vertices, which
    // then classified stretches of a real reach down to Rill and banded the
    // raster. Both sequences run along the path, so a monotone cursor resolves
    // every point in linear time.
    std::vector<int> pt_cell(np);
    {
      size_t cursor = 0;
      for (size_t k = 0; k < np; ++k) {
        auto d2 = [&](size_t idx) {
          const glm::vec2 d = world_of(chain[idx]) - pts[k];
          return glm::dot(d, d);
        };
        while (cursor + 1 < chain.size() && d2(cursor + 1) <= d2(cursor)) ++cursor;
        pt_cell[k] = chain[cursor];
      }
    }
    for (size_t k = 0; k < np; ++k) {
      const int c = pt_cell[k];
      const float q = p.runoff_m_per_s * area.data[static_cast<size_t>(c)];
      // Local reach slope, from the neighbouring vertices' ground over their
      // world separation — read at their CHAIN cells for the same reason.
      const size_t a = k > 0 ? k - 1 : k;
      const size_t b = k + 1 < np ? k + 1 : k;
      float slope = 0.0f;
      if (b > a) {
        const float dz = ground.data[static_cast<size_t>(pt_cell[a])] -
                         ground.data[static_cast<size_t>(pt_cell[b])];
        const float dist = glm::length(pts[b] - pts[a]);
        if (dist > 0.0f) slope = dz / dist;
      }
      const auto hyd = channel_hydraulics(q, slope, p);
      e.discharge_m3_s[k] = q;
      e.width_m[k] = hyd.width_m;
      e.depth_m[k] = hyd.depth_m;
      e.speed_m_s[k] = hyd.speed_m_s;
    }
    g.edges.push_back(std::move(e));
  };

  // Pass 1: reaches beginning at a source or a confluence.
  for (size_t i = 0; i < n; ++i) {
    if (!cf.channel[i]) continue;
    const bool starts = cf.donors[i] == 0 || cf.donors[i] >= 2;
    if (!starts) continue;
    if (is_sill[i]) continue;  // pass 2 owns this reach
    // No reach LEAVES a terminal cell. Guarding only the donors==0 case would
    // let a confluence that sits right on a lake shore (or on the map edge)
    // walk a single-cell chain whose head and tail are the same cell, emitting
    // a self-edge — which then reads as a cycle to any downstream traversal.
    // Terminal cells still get nodes, created by the upstream edges ending
    // there.
    if (is_terminal(static_cast<int>(i))) continue;
    const auto chain = walk(static_cast<int>(i));
    const int tail = chain.back();
    const int32_t from_node =
        add_node(static_cast<int>(i),
                 cf.donors[i] >= 2 ? RiverNodeKind::Confluence : RiverNodeKind::Source, -1);
    const int32_t rcv = r.receiver[static_cast<size_t>(tail)];
    int32_t to_node;
    if (rcv >= 0 && cf.lake_id[static_cast<size_t>(rcv)] >= 0)
      to_node = add_node(tail, RiverNodeKind::LakeInlet,
                         cf.lake_id[static_cast<size_t>(rcv)], rcv);
    else if (rcv < 0)
      to_node = add_node(tail, RiverNodeKind::Mouth, -1);
    else
      to_node = add_node(tail, RiverNodeKind::Confluence, -1);
    finish_edge(from_node, to_node, chain);
  }

  // Pass 2: each lake's outflow. The reach starts at the true sill.
  for (size_t lid = 0; lid < sill.size(); ++lid) {
    const int s = sill[lid];
    if (s < 0 || !cf.channel[static_cast<size_t>(s)]) continue;
    const int32_t out_node = add_node(s, RiverNodeKind::LakeOutlet,
                                      static_cast<int32_t>(lid),
                                      lake_member[lid]);
    if (!is_terminal(s)) {
      const auto chain = walk(s);
      const int tail = chain.back();
      const int32_t rcv = r.receiver[static_cast<size_t>(tail)];
      int32_t to_node;
      if (rcv >= 0 && cf.lake_id[static_cast<size_t>(rcv)] >= 0)
        to_node = add_node(tail, RiverNodeKind::LakeInlet,
                           cf.lake_id[static_cast<size_t>(rcv)], rcv);
      else if (rcv < 0)
        to_node = add_node(tail, RiverNodeKind::Mouth, -1);
      else
        to_node = add_node(tail, RiverNodeKind::Confluence, -1);
      finish_edge(out_node, to_node, chain);
    }
  }

  // Pass 3: through-lake edges, inlet -> that lake's outlet. These carry NO
  // geometry: the lake surface is already water, so drawing a channel across
  // it would be wrong. They exist so the network is traversable end to end —
  // river into lake, lake out to river — which is what "the flow does not
  // connect the lakes" was about.
  for (size_t ni = 0; ni < g.nodes.size(); ++ni) {
    const RiverNode& nd = g.nodes[ni];
    if (nd.kind != RiverNodeKind::LakeInlet || nd.lake_id < 0) continue;
    const int s = sill[static_cast<size_t>(nd.lake_id)];
    if (s < 0) continue;
    const int32_t out_node = node_at[static_cast<size_t>(s)];
    if (out_node < 0 || out_node == static_cast<int32_t>(ni)) continue;
    RiverEdge e;
    e.from = static_cast<int32_t>(ni);
    e.to = out_node;
    g.edges.push_back(std::move(e));
  }

  compute_stream_orders(g);

  // Node hydraulics, now that discharge is known per node.
  for (auto& nd : g.nodes) {
    const int c = cell_of(nd.pos_m);
    const int32_t rcv = r.receiver[static_cast<size_t>(c)];
    float slope = 0.0f;
    if (rcv >= 0) {
      const float dz = ground.data[static_cast<size_t>(c)] - ground.data[static_cast<size_t>(rcv)];
      const int dx = std::abs(c % w - rcv % w), dy = std::abs(c / w - rcv / w);
      const float dist = (dx + dy == 2) ? texel_m * std::sqrt(2.0f) : texel_m;
      slope = dz / dist;
    }
    const auto hyd = channel_hydraulics(nd.discharge_m3_s, slope, p);
    nd.width_m = hyd.width_m;
    nd.depth_m = hyd.depth_m;
    nd.speed_m_s = hyd.speed_m_s;
  }
  return g;
}

RiverRasters rasterize_rivers(const RiverGraph& g, int res, float texel_m) {
  RiverRasters out;
  if (res <= 0 || texel_m <= 0.0f) return out;
  out.discharge_m3_s = Field2D<float>(res, res, 0.0f);
  out.cls = Field2D<uint8_t>(res, res, static_cast<uint8_t>(RiverClass::None));
  out.depth_m = Field2D<float>(res, res, 0.0f);
  out.speed_m_s = Field2D<float>(res, res, 0.0f);
  out.flow_dir = Field2D<glm::vec2>(res, res, glm::vec2(0.0f));

  const float half = 0.5f * texel_m;  // texel (x,y) is centred at x*texel_m
  for (const auto& e : g.edges) {
    // Through-lake edges carry no geometry on purpose: the lake surface is
    // already water, so there is nothing to draw across it.
    if (e.points_m.size() < 2) continue;
    for (size_t k = 1; k < e.points_m.size(); ++k) {
      const glm::vec2 a = e.points_m[k - 1], b = e.points_m[k];
      const glm::vec2 ab = b - a;
      const float seg_len2 = glm::dot(ab, ab);
      if (seg_len2 <= 0.0f) continue;
      const float r0 = 0.5f * e.width_m[k - 1], r1 = 0.5f * e.width_m[k];
      const float rmax = std::max(r0, r1);
      const glm::vec2 dir = ab / std::sqrt(seg_len2);

      const int x0 = std::max(0, static_cast<int>(std::floor((std::min(a.x, b.x) - rmax) / texel_m)) - 1);
      const int x1 = std::min(res - 1, static_cast<int>(std::ceil((std::max(a.x, b.x) + rmax) / texel_m)) + 1);
      const int y0 = std::max(0, static_cast<int>(std::floor((std::min(a.y, b.y) - rmax) / texel_m)) - 1);
      const int y1 = std::min(res - 1, static_cast<int>(std::ceil((std::max(a.y, b.y) + rmax) / texel_m)) + 1);

      for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
          const glm::vec2 c(static_cast<float>(x) * texel_m, static_cast<float>(y) * texel_m);
          const glm::vec2 lo = c - half, hi = c + half;
          // CONSERVATIVE: the texel's SQUARE must come within the radius, not
          // its centre. That is what keeps the covered set connected end to
          // end even when the channel is narrower than a texel — the axis
          // still passes through the square, so radius 0 still covers it.
          const float t = std::clamp(glm::dot(c - a, ab) / seg_len2, 0.0f, 1.0f);
          const float radius = r0 + (r1 - r0) * t;
          if (segment_aabb_distance(a, b, lo, hi) > radius) continue;

          const float q = e.discharge_m3_s[k - 1] +
                          (e.discharge_m3_s[k] - e.discharge_m3_s[k - 1]) * t;
          // Higher discharge wins where reaches overlap, so a trunk beats its
          // tributary at a confluence — and that decides the rendered class.
          const size_t i = static_cast<size_t>(y) * res + static_cast<size_t>(x);
          if (q <= out.discharge_m3_s.data[i]) continue;
          out.discharge_m3_s.data[i] = q;
          out.cls.data[i] = static_cast<uint8_t>(classify_discharge(q));
          out.depth_m.data[i] = e.depth_m[k - 1] + (e.depth_m[k] - e.depth_m[k - 1]) * t;
          out.speed_m_s.data[i] = e.speed_m_s[k - 1] + (e.speed_m_s[k] - e.speed_m_s[k - 1]) * t;
          out.flow_dir.data[i] = dir;
        }
      }
    }
  }
  return out;
}

}  // namespace badlands::mapgen
