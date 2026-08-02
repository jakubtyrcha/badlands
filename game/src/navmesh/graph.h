// Adjacency graph over the passable leaves of a Quadtree, plus A* over it.
//
// Nodes are the passable leaves (impassable leaves are dropped -- a unit never
// stands on one), except that a PARTIAL leaf -- one a diagonal footprint only
// half covers -- contributes one node per FREE corner triangle instead of one
// for the whole cell. That is the whole point of the triangle mask: half a cell
// beside a 45-degree wall is somewhere a unit can genuinely stand, and a
// per-cell node can only call it wholly walkable or wholly wall.
//
// Two nodes are joined when they SHARE AN EDGE; the weight is the travel cost
// between their centres (geometric distance x average terrain-cost multiplier).
// For whole leaves that is the old 4-connected border rule unchanged. For
// triangles it is the two in-tile neighbours plus the one across the tile
// border (navmesh/tri.h) -- opposite corners touch at the cell centre POINT
// only and are NOT joined, which is how the "a path never cuts a building
// corner diagonally" guarantee survives the finer decomposition.
//
// Deterministic: nodes follow the quadtree's leaf order (and, within a partial
// leaf, corner order N,E,S,W) and each node's edges are sorted by target, so A*
// tie-breaking (and thus the sim's replay) is reproducible.

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
    // The corner this node covers, or -1 when it covers a whole leaf.
    int tri_of(int node) const { return node_tri_[node]; }
    const std::vector<Edge>& edges(int node) const { return adj_[node]; }

    // Node covering triangle (cx, cz, corner), or -1 if it is solid / off-grid.
    int NodeAt(int cx, int cz, int corner) const;

    // Node centre in (fractional) cell coordinates: the leaf centre for a whole
    // leaf, the triangle centroid for a triangle node. The centroid is
    // deliberately off every lattice line -- a cell CENTRE sits exactly on the
    // (u,v) diagonals, so using one as a waypoint would put it on the boundary
    // of the very wall the node exists to route around.
    glm::vec2 center_cells(int node) const;

    // A* from start to goal (node indices). Returns the node path inclusive of
    // both ends and writes the total edge cost to out_cost; empty + out_cost 0
    // if unreachable or either endpoint invalid.
    std::vector<int> AStar(int start, int goal, float& out_cost) const;

    // Single-source shortest paths from `start` over the whole graph. Fills
    // dist (infinity for unreachable) and came (-1 for start/unreached) sized to
    // node_count(). Used by the exempt-building goal augmentation, which picks
    // the cheapest node with clearance-exempt line-of-sight to the goal.
    void Dijkstra(int start, std::vector<float>& dist, std::vector<int>& came) const;

   private:
    const Quadtree* qt_ = nullptr;
    std::vector<int> node_leaf_;  // node -> leaf index
    std::vector<int> node_tri_;   // node -> corner, or -1 for a whole leaf
    // (cell, corner) -> node, or -1. Sized side^2 * 4 and filled during Build,
    // which keeps every lookup O(1) -- the same trade Quadtree::cell_leaf_
    // makes, at four entries per cell instead of one.
    std::vector<int32_t> cell_tri_node_;
    std::vector<std::vector<Edge>> adj_;
};

}  // namespace badlands::nav
