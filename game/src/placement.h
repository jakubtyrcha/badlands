// Building placement grid: triangle occupancy, footprint/margin rasterization,
// grid snapping, and the poppable (urban-sprawl) system. The public C surface
// is game/include/badlands_game.h; these are the internal, unit-tested helpers
// shared between placement.cpp and game.cpp (castle prebuild).

#pragma once

#include "badlands_game.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

struct BadlandsGame;

namespace badlands {

constexpr int kGridHalf = GAME_GRID_HALF_EXTENT_TILES;  // 48
constexpr int kGridSize = 2 * kGridHalf;                // 96 tiles per axis
constexpr int kTriangleCount = kGridSize * kGridSize * 4;

struct PlacedBuilding {
    int32_t kind;
    glm::vec2 center;  // XZ, snapped
    int32_t rot;       // 0..3
    int32_t w, d;      // footprint tiles
};

struct PlacementState {
    // Union of every placed footprint and margin, indexed by tri_index(). A set
    // bit blocks any future footprint triangle from that cell.
    std::vector<uint8_t> blocked = std::vector<uint8_t>(kTriangleCount, 0);
    // Footprints only (no margins), so blocking can be checked both ways: a
    // candidate's margin may not cover an existing footprint either.
    std::vector<uint8_t> footprint = std::vector<uint8_t>(kTriangleCount, 0);
    std::vector<PlacedBuilding> buildings;  // index == public id; never removed
    uint32_t urban_quarters = 0;            // sprawl accumulator (base 4)
    uint32_t houses_made = 0;
    uint32_t sewers_made = 0;
};

struct TriRef {
    int32_t tx, tz;
    uint32_t corner;
};

// tx,tz in [-kGridHalf, kGridHalf), corner 0..3.
inline bool in_bounds_tile(int tx, int tz) {
    return tx >= -kGridHalf && tx < kGridHalf && tz >= -kGridHalf && tz < kGridHalf;
}
inline int tri_index(int tx, int tz, int corner) {
    return (((tz + kGridHalf) * kGridSize) + (tx + kGridHalf)) * 4 + corner;
}

// The interior representative point of a triangle (off every lattice line, so
// region-membership tests are exact and boundary-free).
glm::vec2 triangle_centroid(int tx, int tz, int corner);

// --- Footprint geometry -----------------------------------------------------

// A snapped building footprint: either an axis-aligned world rect (ortho) or an
// integer (u,v)-box with u=x+z, v=x-z (diagonal).
struct Footprint {
    bool diagonal;
    float x0, x1, z0, z1;  // ortho world rect
    int p, q, r, s;        // diagonal (u,v) box [p,q] x [r,s]
};

// (u,v) span counts for a diagonal footprint of a w x d building. Per local
// axis: round(dim * sqrt2) (>=1); transposed for rotation 135 vs 45.
glm::ivec2 diagonal_spans(int w, int d, int rot);

// Snaps a raw center to the grid lattice for a kind+rotation (single source of
// truth: the probe returns this, and place uses it).
glm::vec2 snap_center(int kind, int rot, glm::vec2 raw);

Footprint make_footprint(int kind, int rot, glm::vec2 center);

// True if the footprint's tile bbox lies fully within the grid.
bool footprint_in_bounds(const Footprint& fp);

// Appends the footprint (occupied) triangles; margin appends the 1-tile L1
// dilation minus footprint. Both clip silently to the grid.
void footprint_triangles(const Footprint& fp, std::vector<TriRef>& out);
void margin_triangles(const Footprint& fp, std::vector<TriRef>& out);

// A footprint is placeable iff it fits the grid and no footprint triangle hits
// an already-blocked cell.
bool placement_valid(const PlacementState& st, const Footprint& fp);

// --- Placement + poppables --------------------------------------------------

// Places a building (snapping desc center). Returns the id, or UINT32_MAX on an
// invalid footprint. `player` gates the urban-sprawl accumulator + poppables.
uint32_t place_building(BadlandsGame& game, const GamePlacementDesc& desc, bool player);

// Inconvenience heuristic: closeness to the castle plus the nearest apothecary.
float poppable_score(const PlacementState& st, glm::vec2 candidate);

// Spawns any owed poppables near `anchor` (sewers before houses), stopping a
// kind early when the neighbourhood has no valid spot (retried next placement).
void process_poppables(BadlandsGame& game, glm::vec2 anchor);

}  // namespace badlands
