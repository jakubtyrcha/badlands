#include "graph.h"

#include <algorithm>
#include <limits>
#include <queue>
#include <set>
#include <utility>

namespace badlands::nav {

// Nodes = passable leaves, split per free corner where a leaf is partial. Edges
// join nodes that share an EDGE, weighted by centre-to-centre distance x
// average terrain cost.
void NavGraph::Build(const Quadtree& qt) {
    qt_ = &qt;
    const std::vector<Leaf>& leaves = qt.leaves();
    const int n = qt.side();
    node_leaf_.clear();
    node_tri_.clear();
    cell_tri_node_.assign(static_cast<size_t>(n) * n * kTriPerCell, -1);
    const auto tri_slot = [n](int x, int z, int c) {
        return (static_cast<size_t>(z) * n + x) * kTriPerCell + c;
    };

    // Numbered in LEAF order, and within a partial leaf in corner order
    // N,E,S,W -- the determinism the replay contract rests on comes from this
    // walk, not from the cell scan below.
    for (int li = 0; li < static_cast<int>(leaves.size()); ++li) {
        const Leaf& l = leaves[li];
        if (!l.passable) {
            continue;
        }
        if (l.tri_mask == kMaskFree) {
            const int node = static_cast<int>(node_leaf_.size());
            node_leaf_.push_back(li);
            node_tri_.push_back(-1);
            for (int z = l.z0; z < l.z0 + l.size; ++z) {
                for (int x = l.x0; x < l.x0 + l.size; ++x) {
                    for (int c = 0; c < kTriPerCell; ++c) {
                        cell_tri_node_[tri_slot(x, z, c)] = node;
                    }
                }
            }
            continue;
        }
        // Partial leaves are always a single cell (the quadtree refuses to
        // merge one), so the corners map straight onto that cell.
        for (int c = 0; c < kTriPerCell; ++c) {
            if (mask_has(l.tri_mask, c)) {
                continue;
            }
            const int node = static_cast<int>(node_leaf_.size());
            node_leaf_.push_back(li);
            node_tri_.push_back(c);
            cell_tri_node_[tri_slot(l.x0, l.z0, c)] = node;
        }
    }
    adj_.assign(node_leaf_.size(), {});

    std::set<std::pair<int, int>> seen;
    auto try_link = [&](int na, int nb) {
        if (na < 0 || nb < 0 || na == nb) {
            return;  // off-grid, solid, or both sides inside one leaf
        }
        const std::pair<int, int> key = std::minmax(na, nb);
        if (!seen.insert(key).second) {
            return;  // border already linked
        }
        const float dist = glm::distance(center_cells(na), center_cells(nb));
        const float w =
            dist * 0.5f * (leaves[node_leaf_[na]].cost + leaves[node_leaf_[nb]].cost);
        adj_[na].push_back({nb, w});
        adj_[nb].push_back({na, w});
    };

    // Walk every triangle and link it to its three edge-sharing neighbours.
    // Inside a big whole-leaf node the in-tile links are self-links and drop
    // out, so what survives there is exactly the old across-the-border rule.
    for (int z = 0; z < n; ++z) {
        for (int x = 0; x < n; ++x) {
            for (int c = 0; c < kTriPerCell; ++c) {
                const int here = cell_tri_node_[tri_slot(x, z, c)];
                if (here < 0) {
                    continue;
                }
                for (const TriId& t : tri_neighbors(x, z, c)) {
                    if (t.cx < 0 || t.cz < 0 || t.cx >= n || t.cz >= n) {
                        continue;
                    }
                    try_link(here, cell_tri_node_[tri_slot(t.cx, t.cz, t.corner)]);
                }
            }
        }
    }
    for (std::vector<Edge>& es : adj_) {
        std::sort(es.begin(), es.end(), [](const Edge& a, const Edge& b) { return a.to < b.to; });
    }
}

int NavGraph::NodeAt(int cx, int cz, int corner) const {
    if (qt_ == nullptr) {
        return -1;
    }
    const int n = qt_->side();
    if (cx < 0 || cz < 0 || cx >= n || cz >= n || corner < 0 || corner >= kTriPerCell) {
        return -1;
    }
    return cell_tri_node_[(static_cast<size_t>(cz) * n + cx) * kTriPerCell + corner];
}

glm::vec2 NavGraph::center_cells(int node) const {
    const Leaf& l = qt_->leaves()[node_leaf_[node]];
    const int c = node_tri_[node];
    if (c >= 0) {
        return tri_centroid_cells(l.x0, l.z0, c);
    }
    const float h = static_cast<float>(l.size) * 0.5f;
    return {static_cast<float>(l.x0) + h, static_cast<float>(l.z0) + h};
}

std::vector<int> NavGraph::AStar(int start, int goal, float& out_cost) const {
    out_cost = 0.0f;
    const int n = node_count();
    if (start < 0 || goal < 0 || start >= n || goal >= n) {
        return {};
    }
    if (start == goal) {
        return {start};
    }

    const glm::vec2 goal_c = center_cells(goal);
    // Heuristic: straight-line cell distance. Admissible because every edge
    // weight >= its geometric length (terrain cost multipliers are >= 1).
    auto heuristic = [&](int node) { return glm::distance(center_cells(node), goal_c); };

    std::vector<float> g(n, std::numeric_limits<float>::infinity());
    std::vector<int> came(n, -1);
    std::vector<char> closed(n, 0);

    // Min-heap on (f, node); node breaks f-ties so pops are deterministic.
    struct QN {
        float f;
        int node;
    };
    struct Cmp {
        bool operator()(const QN& a, const QN& b) const {
            return a.f > b.f || (a.f == b.f && a.node > b.node);
        }
    };
    std::priority_queue<QN, std::vector<QN>, Cmp> open;

    g[start] = 0.0f;
    open.push({heuristic(start), start});
    while (!open.empty()) {
        const int u = open.top().node;
        open.pop();
        if (closed[u]) {
            continue;
        }
        if (u == goal) {
            break;
        }
        closed[u] = 1;
        for (const Edge& e : adj_[u]) {  // edges pre-sorted by target
            const float ng = g[u] + e.w;
            if (ng < g[e.to]) {
                g[e.to] = ng;
                came[e.to] = u;
                open.push({ng + heuristic(e.to), e.to});
            }
        }
    }

    if (came[goal] < 0) {
        return {};  // unreachable
    }
    std::vector<int> path;
    for (int at = goal; at != -1; at = came[at]) {
        path.push_back(at);
    }
    std::reverse(path.begin(), path.end());
    out_cost = g[goal];
    return path;
}

void NavGraph::Dijkstra(int start, std::vector<float>& dist, std::vector<int>& came) const {
    const int n = node_count();
    dist.assign(n, std::numeric_limits<float>::infinity());
    came.assign(n, -1);
    if (start < 0 || start >= n) {
        return;
    }
    struct QN {
        float d;
        int node;
    };
    struct Cmp {
        bool operator()(const QN& a, const QN& b) const {
            return a.d > b.d || (a.d == b.d && a.node > b.node);
        }
    };
    std::priority_queue<QN, std::vector<QN>, Cmp> open;
    std::vector<char> closed(n, 0);
    dist[start] = 0.0f;
    open.push({0.0f, start});
    while (!open.empty()) {
        const int u = open.top().node;
        open.pop();
        if (closed[u]) {
            continue;
        }
        closed[u] = 1;
        for (const Edge& e : adj_[u]) {
            const float nd = dist[u] + e.w;
            if (nd < dist[e.to]) {
                dist[e.to] = nd;
                came[e.to] = u;
                open.push({nd, e.to});
            }
        }
    }
}

}  // namespace badlands::nav
