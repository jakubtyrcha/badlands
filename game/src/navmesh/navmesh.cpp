#include "navmesh.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace badlands::nav {

void NavMesh::Build(const NavSource& src, const NavParams& params) {
    cell_size_ = src.cell_size_m();
    origin_ = src.origin_m();
    side_ = src.side();
    clearance_ = std::max(0, params.clearance_cells);
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

// Traversable for a query: a real passable leaf, or (when an exempt rect is
// active) an impassable cell within that rect expanded by the mesh clearance --
// i.e. the target building's clearance ring, lifted.
bool NavMesh::CellOk(int cx, int cz, const Exempt& ex) const {
    const int li = qt_.LeafAt(cx, cz);
    if (li >= 0 && qt_.leaves()[li].passable) {
        return true;
    }
    if (ex.active) {
        const glm::vec2 c = CellCenterWorld(cx, cz);
        const float m = static_cast<float>(clearance_) * cell_size_ + 1e-3f;
        if (c.x >= ex.min.x - m && c.x <= ex.max.x + m && c.y >= ex.min.y - m &&
            c.y <= ex.max.y + m) {
            return true;
        }
    }
    return false;
}

// Amanatides-Woo cell traversal: the segment is clear iff every cell it touches
// is CellOk. Out-of-range cells count as blocked.
bool NavMesh::SegmentClear(glm::vec2 a, glm::vec2 b, const Exempt& ex) const {
    auto passable = [&](int cx, int cz) { return CellOk(cx, cz, ex); };
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

    for (int guard = 0; guard < 4 * side_ + 4; ++guard) {
        if (!passable(cx, cz)) {
            return false;
        }
        if (cx == gx && cz == gz) {
            return true;
        }
        if (t_max_x < t_max_z) {
            t_max_x += t_delta_x;
            cx += stepx;
        } else {
            t_max_z += t_delta_z;
            cz += stepz;
        }
        if (t_max_x > 1.0f && t_max_z > 1.0f) {
            // Past the segment end without landing exactly on (gx,gz); validate
            // the end cell directly.
            return passable(gx, gz);
        }
    }
    return passable(gx, gz);
}

// Nearest passable cell to (cx,cz), searched ring by ring so the closest wins;
// deterministic scan order within a ring.
bool NavMesh::RecoverCell(int cx, int cz, glm::ivec2& out) const {
    auto ok = [&](int x, int z) {
        const int li = qt_.LeafAt(x, z);
        return li >= 0 && qt_.leaves()[li].passable;
    };
    if (ok(cx, cz)) {
        out = {cx, cz};
        return true;
    }
    const int max_r = side_;
    for (int r = 1; r <= max_r; ++r) {
        for (int dz = -r; dz <= r; ++dz) {
            for (int dx = -r; dx <= r; ++dx) {
                if (std::max(std::abs(dx), std::abs(dz)) != r) {
                    continue;  // ring shell only
                }
                const int x = cx + dx, z = cz + dz;
                if (ok(x, z)) {
                    out = {x, z};
                    return true;
                }
            }
        }
    }
    return false;
}

int NavMesh::NodeAtWorld(glm::vec2 w) const {
    const glm::ivec2 c = WorldToCell(w);
    glm::ivec2 r;
    if (!RecoverCell(c.x, c.y, r)) {
        return -1;
    }
    return graph_.NodeAt(r.x, r.y);
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
        for (int c = 0; c < graph_.node_count(); ++c) {
            if (!std::isfinite(dist[c])) {
                continue;
            }
            const glm::vec2 cc = graph_.center_cells(c);
            const glm::vec2 cw = origin_ + cc * cell_size_;
            if (!SegmentClear(cw, to, ex)) {
                continue;
            }
            const float metric = dist[c] + glm::distance(cc, to_cells);
            if (metric < best_metric) {
                best_metric = metric;
                best = c;
            }
        }
        if (best < 0) {
            return res;  // unreachable even with the exemption
        }
        for (int at = best; at != -1; at = came[at]) {
            nodes.push_back(at);
        }
        std::reverse(nodes.begin(), nodes.end());
    }

    // Coarse polyline: the true endpoints with the leaf centres between them.
    std::vector<glm::vec2> coarse;
    coarse.reserve(nodes.size() + 2);
    coarse.push_back(from);
    for (int nd : nodes) {
        const glm::vec2 cc = graph_.center_cells(nd);
        coarse.push_back(CellCenterWorld(static_cast<int>(cc.x), static_cast<int>(cc.y)));
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
        out.push_back(c);
    }
}

}  // namespace badlands::nav
