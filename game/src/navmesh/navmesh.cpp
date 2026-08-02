#include "navmesh.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace badlands::nav {

void NavMesh::Build(const NavSource& src, const NavParams& params) {
    cell_size_ = src.cell_size_m();
    origin_ = src.origin_m();
    side_ = src.side();
    clearance_m_ = std::max(0.0f, params.clearance_m);
    qt_.Build(src, params);
    graph_.Build(qt_);
    cost_cache_.clear();  // stale against the old mesh
}

glm::ivec2 NavMesh::WorldToCell(glm::vec2 w) const {
    const glm::vec2 c = (w - origin_) / cell_size_;
    int cx = static_cast<int>(std::floor(c.x));
    int cz = static_cast<int>(std::floor(c.y));
    cx = std::clamp(cx, 0, side_ - 1);
    cz = std::clamp(cz, 0, side_ - 1);
    return {cx, cz};
}

glm::vec2 NavMesh::CellCenterWorld(int cx, int cz) const {
    return origin_ + (glm::vec2(static_cast<float>(cx), static_cast<float>(cz)) + 0.5f) * cell_size_;
}

TriId NavMesh::WorldToTri(glm::vec2 w) const {
    const glm::ivec2 c = WorldToCell(w);
    const glm::vec2 p = (w - origin_) / cell_size_;
    // WorldToCell CLAMPS to the grid, so the local fraction has to be clamped
    // with it. Left raw, a point well off the map gives a fraction in the tens
    // and corner_at names an arbitrary corner of the edge cell -- which then
    // gets probed as if it meant something.
    const float fx = std::clamp(p.x - static_cast<float>(c.x), 0.0f, 1.0f);
    const float fz = std::clamp(p.y - static_cast<float>(c.y), 0.0f, 1.0f);
    return TriId{c.x, c.y, corner_at(fx, fz)};
}

// Traversable for a query: a free triangle, or (when an exempt rect is active)
// a solid one within that rect expanded by the mesh clearance -- i.e. the
// target building's clearance ring, lifted.
bool NavMesh::TriOk(int cx, int cz, int corner, const Exempt& ex) const {
    if (qt_.TriPassable(cx, cz, corner)) {
        return true;
    }
    if (ex.active) {
        // Measured at the triangle's centroid rather than the cell centre: a
        // cell centre sits exactly on the (u,v) diagonals, so on a partial cell
        // it belongs to no triangle in particular.
        const glm::vec2 c = origin_ + tri_centroid_cells(cx, cz, corner) * cell_size_;
        const float m = clearance_m_ + 1e-3f;
        if (c.x >= ex.min.x - m && c.x <= ex.max.x + m && c.y >= ex.min.y - m &&
            c.y <= ex.max.y + m) {
            return true;
        }
    }
    return false;
}

// The piece of the segment inside one cell, tested per triangle.
//
// The cell's two diagonals (z = x and z = 1-x in local coordinates) are linear
// in the segment parameter, so their crossings split [t0,t1] into at most three
// pieces, each lying wholly inside one corner triangle. Evaluating each piece
// at its MIDPOINT keeps the test off the diagonals themselves, where
// corner_at's tie-break would otherwise decide the answer.
bool NavMesh::SubsegmentClear(int cx, int cz, glm::vec2 pa, glm::vec2 pb, float t0, float t1,
                              const Exempt& ex) const {
    const glm::vec2 lo = pa - glm::vec2(static_cast<float>(cx), static_cast<float>(cz));
    const glm::vec2 d = pb - pa;
    float ts[4] = {t0, t1, 0.0f, 0.0f};
    int nt = 2;
    // g(t) = 0 on the diagonal. Both are affine in t, so at most one root each.
    const auto add_root = [&](float g0, float g1) {
        if (g0 == g1) {
            return;  // parallel to this diagonal: no crossing
        }
        const float t = -g0 / (g1 - g0);
        if (t > t0 && t < t1) {
            ts[nt++] = t;
        }
    };
    // main diagonal: (z - cz) - (x - cx) = 0; anti: (z - cz) + (x - cx) - 1 = 0
    add_root(lo.y - lo.x, (lo.y + d.y) - (lo.x + d.x));
    add_root(lo.y + lo.x - 1.0f, (lo.y + d.y) + (lo.x + d.x) - 1.0f);
    std::sort(ts, ts + nt);

    for (int i = 0; i + 1 < nt; ++i) {
        const float mid = 0.5f * (ts[i] + ts[i + 1]);
        if (ts[i + 1] - ts[i] <= 0.0f) {
            continue;  // degenerate sliver (the segment grazed a diagonal)
        }
        const glm::vec2 p = pa + d * mid;
        const int corner = corner_at(p.x - static_cast<float>(cx), p.y - static_cast<float>(cz));
        if (!TriOk(cx, cz, corner, ex)) {
            return false;
        }
    }
    return true;
}

// Amanatides-Woo cell traversal, testing TRIANGLES within each cell it visits.
// Out-of-range cells count as blocked.
bool NavMesh::SegmentClear(glm::vec2 a, glm::vec2 b, const Exempt& ex) const {
    const glm::vec2 pa = (a - origin_) / cell_size_;
    const glm::vec2 pb = (b - origin_) / cell_size_;
    int cx = static_cast<int>(std::floor(pa.x));
    int cz = static_cast<int>(std::floor(pa.y));
    const int gx = static_cast<int>(std::floor(pb.x));
    const int gz = static_cast<int>(std::floor(pb.y));
    const float dx = pb.x - pa.x;
    const float dz = pb.y - pa.y;

    const int stepx = dx > 0 ? 1 : -1;
    const int stepz = dz > 0 ? 1 : -1;
    const float inf = std::numeric_limits<float>::infinity();
    // Distance (in t units of the full segment) to the next cell boundary.
    float t_max_x = inf, t_max_z = inf, t_delta_x = inf, t_delta_z = inf;
    if (dx != 0.0f) {
        const float nx = (dx > 0) ? std::floor(pa.x) + 1.0f : std::floor(pa.x);
        t_max_x = (nx - pa.x) / dx;
        t_delta_x = static_cast<float>(stepx) / dx;
    }
    if (dz != 0.0f) {
        const float nz = (dz > 0) ? std::floor(pa.y) + 1.0f : std::floor(pa.y);
        t_max_z = (nz - pa.y) / dz;
        t_delta_z = static_cast<float>(stepz) / dz;
    }

    // `t_enter` trails the traversal so each cell knows the parameter range of
    // the piece of segment inside it -- what SubsegmentClear splits on.
    float t_enter = 0.0f;
    for (int guard = 0; guard < 4 * side_ + 4; ++guard) {
        const bool last = (cx == gx && cz == gz);
        const float t_exit = last ? 1.0f : std::min(1.0f, std::min(t_max_x, t_max_z));
        if (cx < 0 || cz < 0 || cx >= side_ || cz >= side_) {
            return false;  // off-grid is a wall
        }
        if (!SubsegmentClear(cx, cz, pa, pb, t_enter, t_exit, ex)) {
            return false;
        }
        if (last) {
            return true;
        }
        t_enter = t_exit;
        if (t_max_x < t_max_z) {
            t_max_x += t_delta_x;
            cx += stepx;
        } else {
            t_max_z += t_delta_z;
            cz += stepz;
        }
        if (t_max_x > 1.0f && t_max_z > 1.0f) {
            // Both next boundaries are past the segment end, so the cell just
            // stepped into holds it. Validate the remaining [t_enter, 1] there
            // -- against (cx,cz) and NOT (gx,gz): t_enter is the exit parameter
            // of the cell before this one, so it only describes a range inside
            // the cell the traversal is actually in. They agree except under
            // float drift at a corner, and on that path the pair would split
            // the sub-segment against the wrong cell's diagonals.
            break;
        }
    }
    if (cx < 0 || cz < 0 || cx >= side_ || cz >= side_) {
        return false;
    }
    // The guard is sized well past a full grid crossing (a DDA needs at most
    // 2*side steps), so reaching it means the walk did not terminate. Nothing
    // has validated the tail, and "clear" is not a safe thing to guess.
    if (!SubsegmentClear(cx, cz, pa, pb, t_enter, 1.0f, ex)) {
        return false;
    }
    return cx == gx && cz == gz;
}

// Nearest free triangle to `p_cells`, searched ring by ring so the closest ring
// wins, and NEAREST BY CENTROID within a ring rather than first-in-scan-order.
//
// Which corner is picked is not cosmetic. Opposite corners of a cell are not
// graph-joined (they meet at a point), so on a cell split by a wall the two
// free halves can sit on opposite sides of it. Taking N because N is scanned
// first would recover a body standing in the S half to a node across the wall,
// and the path would be planned the long way round -- or reported unreachable.
// Ties go to scan order, so the choice stays deterministic.
bool NavMesh::RecoverTri(glm::vec2 p_cells, TriId& out) const {
    const int cx = static_cast<int>(std::floor(p_cells.x));
    const int cz = static_cast<int>(std::floor(p_cells.y));
    bool found = false;
    float best = std::numeric_limits<float>::infinity();
    auto scan_cell = [&](int x, int z) {
        for (int c = 0; c < kTriPerCell; ++c) {
            if (!qt_.TriPassable(x, z, c)) {
                continue;
            }
            const glm::vec2 d = tri_centroid_cells(x, z, c) - p_cells;
            const float d2 = glm::dot(d, d);
            if (d2 < best) {
                best = d2;
                out = TriId{x, z, c};
                found = true;
            }
        }
    };
    scan_cell(cx, cz);
    if (found) {
        return true;
    }
    const int max_r = side_;
    for (int r = 1; r <= max_r; ++r) {
        for (int dz = -r; dz <= r; ++dz) {
            for (int dx = -r; dx <= r; ++dx) {
                if (std::max(std::abs(dx), std::abs(dz)) != r) {
                    continue;  // ring shell only
                }
                scan_cell(cx + dx, cz + dz);
            }
        }
        if (found) {
            return true;  // nearest within the closest ring that has anything
        }
    }
    return false;
}

int NavMesh::NodeAtWorld(glm::vec2 w) const {
    const TriId t = WorldToTri(w);
    // The point's OWN triangle first -- recovery is for endpoints that landed
    // inside an obstacle, and on a partial cell the free half is usually right
    // there rather than a ring away.
    if (qt_.TriPassable(t.cx, t.cz, t.corner)) {
        return graph_.NodeAt(t.cx, t.cz, t.corner);
    }
    // Recover from the CLAMPED cell's own coordinates, not the raw point: an
    // off-grid query would otherwise measure every candidate against somewhere
    // outside the map and rank them all alike.
    const glm::vec2 p = glm::clamp((w - origin_) / cell_size_, glm::vec2(0.0f),
                                   glm::vec2(static_cast<float>(side_)));
    TriId r;
    if (!RecoverTri(p, r)) {
        return -1;
    }
    return graph_.NodeAt(r.cx, r.cz, r.corner);
}

NavMesh::PathResult NavMesh::FindPath(glm::vec2 from, glm::vec2 to) const {
    return FindPathImpl(from, to, Exempt{});
}

NavMesh::PathResult NavMesh::FindPath(glm::vec2 from, glm::vec2 to, glm::vec2 exempt_min,
                                      glm::vec2 exempt_max) const {
    return FindPathImpl(from, to, Exempt{exempt_min, exempt_max, true});
}

float NavMesh::PolylineCost(const std::vector<glm::vec2>& pts) const {
    float cost = 0.0f;
    for (size_t i = 1; i < pts.size(); ++i) {
        const glm::vec2 mid = 0.5f * (pts[i - 1] + pts[i]);
        const glm::ivec2 mc = WorldToCell(mid);
        const int li = qt_.LeafAt(mc.x, mc.y);
        const float mult = (li >= 0 && qt_.leaves()[li].passable) ? qt_.leaves()[li].cost : 1.0f;
        cost += glm::distance(pts[i - 1], pts[i]) * std::max(1.0f, mult);
    }
    return cost;
}

NavMesh::PathResult NavMesh::FindPathImpl(glm::vec2 from, glm::vec2 to, const Exempt& ex) const {
    PathResult res;
    if (empty()) {
        return res;
    }
    const int s = NodeAtWorld(from);
    if (s < 0) {
        return res;
    }

    // Node path start -> goal-side node.
    std::vector<int> nodes;
    if (!ex.active) {
        const int g = NodeAtWorld(to);
        if (g < 0) {
            return res;
        }
        float node_cost = 0.0f;
        nodes = graph_.AStar(s, g, node_cost);
        if (nodes.empty()) {
            return res;  // unreachable
        }
    } else {
        // Exempt: shortest paths from s, then the cheapest reachable node that
        // has a clearance-exempt line of sight to the goal (so a door sealed by
        // the target building's own clearance still connects through it).
        std::vector<float> dist;
        std::vector<int> came;
        graph_.Dijkstra(s, dist, came);
        const glm::vec2 to_cells = (to - origin_) / cell_size_;
        int best = -1;
        float best_metric = std::numeric_limits<float>::infinity();
        // The metric is tested BEFORE the line-of-sight, which is a pure
        // speed-up and not a change of answer: a node whose metric cannot beat
        // the incumbent loses whether or not it can see the goal, so the same
        // node wins and ties still go to the lowest index. It matters because
        // SegmentClear is O(side) and this loop runs over every reachable node
        // -- and the triangle decomposition made that node count larger, since
        // a cell in an obstacle's standoff ring is now a partial leaf with its
        // own nodes rather than part of one merged solid leaf.
        for (int c = 0; c < graph_.node_count(); ++c) {
            if (!std::isfinite(dist[c])) {
                continue;
            }
            const glm::vec2 cc = graph_.center_cells(c);
            const float metric = dist[c] + glm::distance(cc, to_cells);
            if (metric >= best_metric) {
                continue;
            }
            if (!SegmentClear(origin_ + cc * cell_size_, to, ex)) {
                continue;
            }
            best_metric = metric;
            best = c;
        }
        if (best < 0) {
            return res;  // unreachable even with the exemption
        }
        for (int at = best; at != -1; at = came[at]) {
            nodes.push_back(at);
        }
        std::reverse(nodes.begin(), nodes.end());
    }

    // Coarse polyline: the true endpoints with the node centres between them.
    //
    // The centre is used AS IT IS, not rounded to a cell. Truncating it to
    // CellCenterWorld put every waypoint back on the cell centre -- which is
    // exactly where the two (u,v) diagonals cross, so on a partial cell it lands
    // on the boundary of the wall the node exists to avoid, and on a triangle
    // node it discards which half was free in the first place.
    std::vector<glm::vec2> coarse;
    coarse.reserve(nodes.size() + 2);
    coarse.push_back(from);
    for (int nd : nodes) {
        coarse.push_back(origin_ + graph_.center_cells(nd) * cell_size_);
    }
    coarse.push_back(to);

    // String-pull: from each anchor keep the farthest still-visible point.
    std::vector<glm::vec2> smooth;
    smooth.push_back(coarse.front());
    int anchor = 0;
    const int n = static_cast<int>(coarse.size());
    while (anchor < n - 1) {
        int next = anchor + 1;
        for (int j = n - 1; j > anchor + 1; --j) {
            if (SegmentClear(coarse[anchor], coarse[j], ex)) {
                next = j;
                break;
            }
        }
        smooth.push_back(coarse[next]);
        anchor = next;
    }

    res.waypoints = std::move(smooth);
    res.cost = PolylineCost(res.waypoints);
    res.reachable = true;
    return res;
}

float NavMesh::Cost(glm::vec2 from, glm::vec2 to) const {
    if (empty()) {
        return kImpassable;
    }
    const glm::ivec2 fc = WorldToCell(from);
    const glm::ivec2 tc = WorldToCell(to);
    const uint64_t key = (static_cast<uint64_t>(fc.y * side_ + fc.x) << 32) |
                         static_cast<uint32_t>(tc.y * side_ + tc.x);
    const auto it = cost_cache_.find(key);
    if (it != cost_cache_.end()) {
        return it->second;
    }
    const PathResult r = FindPath(from, to);
    const float cost = r.reachable ? r.cost : kImpassable;
    cost_cache_[key] = cost;
    return cost;
}

void NavMesh::DebugCells(std::vector<DebugCell>& out) const {
    out.clear();
    out.reserve(qt_.leaves().size());
    for (const Leaf& l : qt_.leaves()) {
        DebugCell c;
        c.min_world = origin_ + glm::vec2(static_cast<float>(l.x0), static_cast<float>(l.z0)) * cell_size_;
        c.max_world = origin_ + glm::vec2(static_cast<float>(l.x0 + l.size),
                                          static_cast<float>(l.z0 + l.size)) *
                                    cell_size_;
        c.cost = l.cost;
        c.passable = l.passable;
        c.tri_mask = l.tri_mask;
        out.push_back(c);
    }
}

void NavMesh::CellsNear(glm::vec2 origin, float radius, size_t max_out,
                        std::vector<DebugCell>& out, glm::vec2 facing,
                        float cone_half_cos) const {
    out.clear();
    if (empty() || radius <= 0.0f || max_out == 0) {
        return;
    }
    const float r2 = radius * radius;

    // Collected with their distances, then partially sorted: the leaves are
    // quadtree order, which is not distance order, so taking the first N would
    // return an arbitrary handful rather than the nearest ones.
    std::vector<std::pair<float, DebugCell>> scored;
    scored.reserve(qt_.leaves().size());
    for (const Leaf& l : qt_.leaves()) {
        DebugCell c;
        c.min_world =
            origin_ + glm::vec2(static_cast<float>(l.x0), static_cast<float>(l.z0)) * cell_size_;
        c.max_world = origin_ + glm::vec2(static_cast<float>(l.x0 + l.size),
                                          static_cast<float>(l.z0 + l.size)) *
                                    cell_size_;
        c.cost = l.cost;
        c.passable = l.passable;
        c.tri_mask = l.tri_mask;

        const glm::vec2 centre = (c.min_world + c.max_world) * 0.5f;
        const glm::vec2 d = centre - origin;
        const float d2 = glm::dot(d, d);
        if (d2 > r2) {
            continue;
        }
        // Inside the cone. The cell the caller is STANDING ON has no meaningful
        // direction, so it is always kept -- excluding it on a degenerate dot
        // product would drop the one square anyone is certain about.
        if (cone_half_cos > -1.0f && d2 > 1e-6f) {
            const glm::vec2 dir = d / std::sqrt(d2);
            if (glm::dot(dir, facing) < cone_half_cos) {
                continue;
            }
        }
        scored.emplace_back(d2, c);
    }
    std::sort(scored.begin(), scored.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    // Thinned, nearest-first: enough separation that `max_out` cells span the
    // whole disc rather than piling up wherever the quadtree happens to be
    // finely subdivided (see the header -- taking the nearest N is the trap).
    const float spacing =
        radius * std::sqrt(3.14159265f / static_cast<float>(max_out));
    const float spacing2 = spacing * spacing;
    out.reserve(std::min(max_out, scored.size()));
    for (const auto& [d2, cell] : scored) {
        if (out.size() >= max_out) {
            break;
        }
        const glm::vec2 centre = (cell.min_world + cell.max_world) * 0.5f;
        bool too_close = false;
        for (const DebugCell& kept : out) {
            const glm::vec2 k = (kept.min_world + kept.max_world) * 0.5f;
            const glm::vec2 delta = centre - k;
            if (glm::dot(delta, delta) < spacing2) {
                too_close = true;
                break;
            }
        }
        if (!too_close) {
            out.push_back(cell);
        }
    }
}

bool NavMesh::PassableAt(glm::vec2 w) const {
    if (empty()) {
        return false;
    }
    // WorldToCell CLAMPS, so a point far outside the grid would otherwise
    // report the passability of the nearest edge cell -- which is a different
    // question, and a yes to it would teleport somebody off the map.
    const glm::vec2 c = (w - origin_) / cell_size_;
    const int cx = static_cast<int>(std::floor(c.x));
    const int cz = static_cast<int>(std::floor(c.y));
    if (cx < 0 || cz < 0 || cx >= side_ || cz >= side_) {
        return false;
    }
    // Per triangle, not per cell: half a cell beside a diagonal wall is a real
    // place to stand, and answering for the whole cell is what made it look
    // like a wall.
    //
    // EVERY triangle whose closed region holds the point, which on a boundary
    // means more than one -- two across a diagonal or a cell edge, four at a
    // cell corner. Picking one arbitrarily made this disagree with
    // SegmentClear, which samples strictly inside triangles and so never meets
    // the tie; nav_point_free would then refuse ground a unit had just been
    // routed across. That is not hypothetical or rare: a whole leaf's centre is
    // the waypoint the string-pull emits, and an even-sized leaf's centre has
    // integer coordinates -- i.e. lands exactly on a cell corner.
    //
    // Permissive on the boundary is also the direction the rest of this layer
    // errs, for the reason sim_nav_params gives.
    const bool on_x_border = c.x == std::floor(c.x);
    const bool on_z_border = c.y == std::floor(c.y);
    for (int dz = on_z_border ? -1 : 0; dz <= 0; ++dz) {
        for (int dx = on_x_border ? -1 : 0; dx <= 0; ++dx) {
            const int nx = cx + dx, nz = cz + dz;
            if (nx < 0 || nz < 0) {
                continue;
            }
            int corners[kTriPerCell];
            const int n =
                corners_at(c.x - static_cast<float>(nx), c.y - static_cast<float>(nz), corners);
            for (int i = 0; i < n; ++i) {
                if (qt_.TriPassable(nx, nz, corners[i])) {
                    return true;
                }
            }
        }
    }
    return false;
}

}  // namespace badlands::nav
