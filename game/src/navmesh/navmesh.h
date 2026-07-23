// NavMesh: the world-facing nav facade the sim talks to.
//
// Build() snapshots a NavSource into an adaptive quadtree + its adjacency graph;
// after that the source is no longer needed (the quadtree encodes per-cell
// passability, including agent-clearance dilation, via LeafAt). Everything is in
// world XZ metres. Two query kinds:
//   FindPath -- a smooth (string-pulled) waypoint polyline for a unit to walk.
//   Cost     -- a broadphase travel-cost estimate for the AI to compare goals.
// Plus DebugCells() for the ImGui overlay.
//
// The HPA* abstract layer (Stage-1, added incrementally) accelerates Cost; it is
// validated against the exact graph A* it approximates.

#pragma once

#include "graph.h"
#include "quadtree.h"
#include "source.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace badlands::nav {

class NavMesh {
   public:
    // (Re)build from a source. src.side() must be a power of two.
    void Build(const NavSource& src, const NavParams& params);

    bool empty() const { return side_ <= 0; }

    struct PathResult {
        std::vector<glm::vec2> waypoints;  // world XZ; empty if unreachable
        float cost = 0.0f;                 // cost-weighted metres along the path
        bool reachable = false;
    };

    // Smooth path from -> to (world XZ). Endpoints inside an obstacle are
    // recovered to the nearest passable cell within a bounded window.
    PathResult FindPath(glm::vec2 from, glm::vec2 to) const;

    // As above, but the axis-aligned world rect [exempt_min, exempt_max] (a
    // target building's footprint) has its CLEARANCE lifted for this query: cells
    // it blocked only via agent-clearance dilation are treated passable, so a
    // unit can reach a door that the building's own clearance ring would
    // otherwise seal (dense towns). The footprint interior stays solid enough
    // that normal routing is unaffected when the door is already reachable.
    PathResult FindPath(glm::vec2 from, glm::vec2 to, glm::vec2 exempt_min,
                        glm::vec2 exempt_max) const;

    // Travel cost from -> to; kImpassable if unreachable. Memoized by the
    // (from-cell, to-cell) pair (1 m granularity); the cache is dropped on every
    // Build, so it can never outlive the mesh it was computed against -- a
    // nav_epoch-driven rebuild invalidates it automatically.
    float Cost(glm::vec2 from, glm::vec2 to) const;

    // Debug enumeration: one entry per quadtree leaf (passable + impassable).
    struct DebugCell {
        glm::vec2 min_world{0.0f};
        glm::vec2 max_world{0.0f};
        float cost = 0.0f;
        bool passable = false;
    };
    void DebugCells(std::vector<DebugCell>& out) const;

    // --- world <-> cell mapping (world XZ metres) ---
    glm::ivec2 WorldToCell(glm::vec2 w) const;
    glm::vec2 CellCenterWorld(int cx, int cz) const;

   private:
    // A clearance-exempt world rect (a target building being entered). When
    // active, cells within the rect expanded by the mesh clearance are treated
    // passable for line-of-sight, lifting that building's own clearance ring.
    struct Exempt {
        glm::vec2 min{0.0f};
        glm::vec2 max{0.0f};
        bool active = false;
    };

    // Is cell (cx,cz) traversable for this query? A real passable leaf always is;
    // an impassable cell is too when it lies within an active exempt rect.
    bool CellOk(int cx, int cz, const Exempt& exempt) const;
    // True if the straight segment a->b (world) crosses only CellOk cells.
    bool SegmentClear(glm::vec2 a, glm::vec2 b, const Exempt& exempt) const;
    // Nearest passable cell to (cx,cz) within a bounded spiral; false if none.
    bool RecoverCell(int cx, int cz, glm::ivec2& out) const;
    // Node covering a world point (after recovery), or -1.
    int NodeAtWorld(glm::vec2 w) const;
    // Shared path core; exempt.active == false is the plain (no-exempt) path.
    PathResult FindPathImpl(glm::vec2 from, glm::vec2 to, const Exempt& exempt) const;
    // Cost-weighted length of a finished polyline (terrain cost per segment).
    float PolylineCost(const std::vector<glm::vec2>& pts) const;

    Quadtree qt_;
    NavGraph graph_;
    float cell_size_ = 1.0f;
    glm::vec2 origin_{0.0f};
    int side_ = 0;
    int clearance_ = 0;  // obstacle dilation baked at Build (cells); exempt margin
    // Cost query memo, keyed (from_cell_index << 32 | to_cell_index). Cleared by
    // Build. mutable so Cost() stays a const query.
    mutable std::unordered_map<uint64_t, float> cost_cache_;
};

}  // namespace badlands::nav
