#include "graph.h"

#include <algorithm>
#include <limits>
#include <queue>
#include <set>
#include <utility>

namespace badlands::nav {

// Nodes = passable leaves. Edges join passable leaves that share a 4-connected
// border, weighted by centre-to-centre distance x average terrain cost.
void NavGraph::Build(const Quadtree& qt) {
    qt_ = &qt;
    const std::vector<Leaf>& leaves = qt.leaves();
    node_leaf_.clear();
    leaf_node_.assign(leaves.size(), -1);
    for (int li = 0; li < static_cast<int>(leaves.size()); ++li) {
        if (leaves[li].passable) {
            leaf_node_[li] = static_cast<int>(node_leaf_.size());
            node_leaf_.push_back(li);
        }
    }
    adj_.assign(node_leaf_.size(), {});

    std::set<std::pair<int, int>> seen;
    auto try_link = [&](int a_leaf, int b_leaf) {
        if (a_leaf < 0 || b_leaf < 0 || a_leaf == b_leaf) {
            return;
        }
        const int na = leaf_node_[a_leaf], nb = leaf_node_[b_leaf];
        if (na < 0 || nb < 0) {
            return;  // one side impassable
        }
        const std::pair<int, int> key = std::minmax(na, nb);
        if (!seen.insert(key).second) {
            return;  // border already linked
        }
        const float dist = glm::distance(center_cells(na), center_cells(nb));
        const float w = dist * 0.5f * (leaves[a_leaf].cost + leaves[b_leaf].cost);
        adj_[na].push_back({nb, w});
        adj_[nb].push_back({na, w});
    };

    const int n = qt.side();
    for (int z = 0; z < n; ++z) {
        for (int x = 0; x < n; ++x) {
            const int here = qt.LeafAt(x, z);
            if (x + 1 < n) {
                try_link(here, qt.LeafAt(x + 1, z));
            }
            if (z + 1 < n) {
                try_link(here, qt.LeafAt(x, z + 1));
            }
        }
    }
    for (std::vector<Edge>& es : adj_) {
        std::sort(es.begin(), es.end(), [](const Edge& a, const Edge& b) { return a.to < b.to; });
    }
}

int NavGraph::NodeAt(int cx, int cz) const {
    if (qt_ == nullptr) {
        return -1;
    }
    const int li = qt_->LeafAt(cx, cz);
    return li < 0 ? -1 : leaf_node_[li];
}

glm::vec2 NavGraph::center_cells(int node) const {
    const Leaf& l = qt_->leaves()[node_leaf_[node]];
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

}  // namespace badlands::nav
