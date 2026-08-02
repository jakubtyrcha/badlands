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
        // Which corner triangles of the rect are solid (navmesh/tri.h).
        // kMaskFree or kMaskSolid on a merged leaf; anything else means the
        // rect is only PARTLY the thing `passable` says it is, and a consumer
        // that draws the rect whole is drawing the artefact this mask exists to
        // remove.
        uint8_t tri_mask = kMaskFree;
    };
    void DebugCells(std::vector<DebugCell>& out) const;

    // A SPREAD SAMPLE of the leaves within `radius` of `origin` AND inside the
    // view cone (`facing`, `cone_half_cos` = cos of the half-angle, -1 for a
    // full circle -- the same encoding the sim's Vision component uses): at
    // most `max_out` of them, nearest-first, thinned so no two kept centres sit
    // closer than about radius * sqrt(pi / max_out).
    //
    // The cone is applied BEFORE the thinning, not after, or the budget would
    // be spent on ground that is then discarded.
    //
    // The thinning is the whole point, and taking simply the nearest N is the
    // trap it avoids. The quadtree subdivides finely near obstacles and coarsely
    // in the open, so "the 32 nearest leaves" is a 6 m disc against a wall and a
    // 60 m one in a field -- a caller asking for ground within 30 m would get a
    // window that silently shrinks exactly where the geometry is interesting.
    // Spacing them out instead makes the window cover the RADIUS ASKED FOR at a
    // resolution the budget can afford.
    //
    // Both passable and impassable leaves are returned -- a consumer deciding
    // where to go needs to see the walls too, and filtering here would make
    // "nothing nearby" and "nothing walkable nearby" indistinguishable.
    void CellsNear(glm::vec2 origin, float radius, size_t max_out,
                   std::vector<DebugCell>& out, glm::vec2 facing = {0.0f, 1.0f},
                   float cone_half_cos = -1.0f) const;

    // Is this world point on a passable leaf? False on an empty mesh and false
    // outside the grid -- both are "you cannot stand there", which is the only
    // question the caller is asking.
    bool PassableAt(glm::vec2 w) const;

    // --- world <-> cell mapping (world XZ metres) ---
    glm::ivec2 WorldToCell(glm::vec2 w) const;
    glm::vec2 CellCenterWorld(int cx, int cz) const;
    // The triangle a world point falls in. Clamped to the grid like
    // WorldToCell, so callers that care about being off-grid check first.
    TriId WorldToTri(glm::vec2 w) const;

   private:
    // A clearance-exempt world rect (a target building being entered). When
    // active, cells within the rect expanded by the mesh clearance are treated
    // passable for line-of-sight, lifting that building's own clearance ring.
    struct Exempt {
        glm::vec2 min{0.0f};
        glm::vec2 max{0.0f};
        bool active = false;
    };

    // Is triangle (cx,cz,corner) traversable for this query? A free triangle
    // always is; a solid one is too when it lies within an active exempt rect.
    bool TriOk(int cx, int cz, int corner, const Exempt& exempt) const;
    // True if the straight segment a->b (world) crosses only TriOk triangles.
    bool SegmentClear(glm::vec2 a, glm::vec2 b, const Exempt& exempt) const;
    // The part of a->b inside cell (cx,cz), split at the cell's two diagonals
    // so each piece lies wholly in one triangle. `pa`/`pb` are in cell
    // coordinates and [t0,t1] is the sub-segment's parameter range.
    bool SubsegmentClear(int cx, int cz, glm::vec2 pa, glm::vec2 pb, float t0, float t1,
                         const Exempt& exempt) const;
    // Nearest free triangle to `p_cells` (cell coordinates) within a bounded
    // spiral; false if none. Nearest by centroid, not first-in-scan-order --
    // opposite corners of a cell are not graph-joined, so the choice can put a
    // recovered body on the wrong side of a wall.
    bool RecoverTri(glm::vec2 p_cells, TriId& out) const;
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
    float clearance_m_ = 0.0f;  // obstacle dilation baked at Build; exempt margin
    // Cost query memo, keyed (from_cell_index << 32 | to_cell_index). Cleared by
    // Build. mutable so Cost() stays a const query.
    mutable std::unordered_map<uint64_t, float> cost_cache_;
};

}  // namespace badlands::nav
