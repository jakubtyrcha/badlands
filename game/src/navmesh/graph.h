// Adjacency graph over the passable leaves of a Quadtree, plus A* over it.
//
// Nodes are the passable leaves (impassable leaves are dropped -- a unit never
// stands on one). Two nodes are joined when their leaf rectangles share a border
// with non-zero overlap; the edge weight is the travel cost between their
// centres (geometric distance x average terrain-cost multiplier). 4-connected
// only, so a path never cuts a building corner diagonally.
//
// Deterministic: nodes follow the quadtree's leaf order and each node's edges
// are sorted by target, so A* tie-breaking (and thus the sim's replay) is
// reproducible.

#pragma once

#include "quadtree.h"

#include <glm/glm.hpp>

#include <vector>

namespace badlands::nav {

struct Edge {
    int to = 0;
    float w = 0.0f;
};

class NavGraph {
   public:
    // Build over an already-built quadtree. Borrows it (must outlive the graph).
    void Build(const Quadtree& qt);

    int node_count() const { return static_cast<int>(node_leaf_.size()); }
    int leaf_of(int node) const { return node_leaf_[node]; }
    const std::vector<Edge>& edges(int node) const { return adj_[node]; }

    // Node covering cell (cx, cz), or -1 if the cell is impassable / off-grid.
    int NodeAt(int cx, int cz) const;

    // Leaf centre in (fractional) cell coordinates.
    glm::vec2 center_cells(int node) const;

    // A* from start to goal (node indices). Returns the node path inclusive of
    // both ends and writes the total edge cost to out_cost; empty + out_cost 0
    // if unreachable or either endpoint invalid.
    std::vector<int> AStar(int start, int goal, float& out_cost) const;

   private:
    const Quadtree* qt_ = nullptr;
    std::vector<int> node_leaf_;   // node -> leaf index
    std::vector<int> leaf_node_;   // leaf index -> node, or -1
    std::vector<std::vector<Edge>> adj_;
};

}  // namespace badlands::nav
