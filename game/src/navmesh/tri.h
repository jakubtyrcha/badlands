// The quarter-tile TRIANGLE lattice the obstacle layer is authored on.
//
// A nav cell is not the atom. Building occupancy is stamped per triangle: each
// tile is cut by BOTH its diagonals into four corner triangles (N, E, S, W),
// and a diagonally-rotated building snaps to the (u, v) = (x+z, x-z) lattice,
// whose lines ARE those diagonals. A footprint is therefore exactly a union of
// triangles, and collapsing a tile's four bits into one boolean turns every
// 45-degree wall into a square staircase a metre fatter than the one on screen.
//
// No square grid can represent this: the (u, v) lines run through tile centres
// AND tile corners, so a centre-sample is degenerate at every resolution. Hence
// the triangle, not a finer cell.
//
//     +--------+       N = the -Z corner       (0,0) (1,0) (c)
//     |\  N   /|       E = the +X corner       (1,0) (1,1) (c)
//     | \    / |       S = the +Z corner       (0,1) (1,1) (c)
//     |  \  /  |       W = the -X corner       (0,0) (0,1) (c)
//     | W  \/  |E      c = the tile centre (0.5, 0.5)
//     |    /\  |
//     |   /  \ |       Adjacency is by SHARED EDGE: c touches c+1 and c-1
//     |  /  S \|       inside the tile, and its opposite across the tile
//     +--------+       border. c and c+2 meet at the centre POINT only, which
//                      is what keeps a path from cutting a corner diagonally.
//
// The corner numbering and centroid positions mirror placement.cpp's
// triangle_centroid EXACTLY -- the sim stamps footprints with that convention
// and the nav core reads them back with this one. They are two copies on
// purpose: the nav core may not include placement.h (that injected-source
// boundary, navmesh/source.h, is what makes the core unit-testable without a
// sim). game/tests/navmesh_tests.cpp pins them together so they cannot drift.

#pragma once

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>

namespace badlands::nav {

// Corner ids. Values are load-bearing: c+1 (mod 4) is an edge neighbour, c+2 is
// the opposite corner, and the order matches placement.cpp's switch.
inline constexpr int kTriN = 0;  // -Z
inline constexpr int kTriE = 1;  // +X
inline constexpr int kTriS = 2;  // +Z
inline constexpr int kTriW = 3;  // -X
inline constexpr int kTriPerCell = 4;

// Per-cell occupancy, one bit per corner. A merged leaf is only ever wholly
// free or wholly solid, so these two are meaningful at any leaf size.
inline constexpr uint8_t kMaskFree = 0x0;
inline constexpr uint8_t kMaskSolid = 0xF;

inline constexpr uint8_t tri_bit(int corner) { return static_cast<uint8_t>(1u << corner); }
inline constexpr bool mask_has(uint8_t mask, int corner) { return (mask & tri_bit(corner)) != 0; }

// Which corner triangle contains the point at cell-local (fx, fz) in [0,1)^2.
//
// The two diagonals are z = x and z = 1-x. A point exactly ON a diagonal is
// awarded to the +z side (`<` rather than `<=`), which is arbitrary but FIXED
// -- determinism only needs the tie broken the same way every time. Callers
// that care about being off the lines entirely use tri_centroid_cells, which
// placement.cpp picks for the same reason.
inline int corner_at(float fx, float fz) {
    const bool below_main = fz < fx;         // below the (0,0)-(1,1) diagonal
    const bool below_anti = fz < 1.0f - fx;  // below the (1,0)-(0,1) diagonal
    if (below_main) {
        return below_anti ? kTriN : kTriE;
    }
    return below_anti ? kTriW : kTriS;
}

// Every corner triangle whose CLOSED region contains cell-local (fx, fz): one
// in the interior, two on a diagonal, all four at the cell centre. Returns the
// count and fills `out` in N,E,S,W order.
//
// For a question of the form "can something be HERE", this is the honest
// answer and corner_at is not: a point on a diagonal belongs to two triangles,
// and corner_at has to pick one, so it reports a wall whenever the tie-break
// happens to land on the solid side. That disagrees with SegmentClear, which
// samples strictly inside triangles and so never asks the boundary question at
// all -- and a caller like nav_point_free would then refuse ground the
// pathfinder had just routed a unit across.
inline int corners_at(float fx, float fz, int out[kTriPerCell]) {
    const float main_d = fz - fx;           // 0 on the (0,0)-(1,1) diagonal
    const float anti_d = fz - (1.0f - fx);  // 0 on the (1,0)-(0,1) diagonal
    int n = 0;
    if (main_d <= 0.0f && anti_d <= 0.0f) out[n++] = kTriN;
    if (main_d <= 0.0f && anti_d >= 0.0f) out[n++] = kTriE;
    if (main_d >= 0.0f && anti_d >= 0.0f) out[n++] = kTriS;
    if (main_d >= 0.0f && anti_d <= 0.0f) out[n++] = kTriW;
    return n;
}

// Interior representative point of a triangle, in CELL coordinates. Sits off
// every lattice line, so a region-membership test through it is exact and
// boundary-free (placement.cpp:83 makes the same argument for the same point).
inline glm::vec2 tri_centroid_cells(int cx, int cz, int corner) {
    constexpr float lo = 1.0f / 6.0f;
    constexpr float hi = 5.0f / 6.0f;
    float dx = 0.5f, dz = 0.5f;
    switch (corner) {
        case kTriN: dz = lo; break;
        case kTriE: dx = hi; break;
        case kTriS: dz = hi; break;
        default: dx = lo; break;  // kTriW
    }
    return {static_cast<float>(cx) + dx, static_cast<float>(cz) + dz};
}

// The three corners of a triangle, in CELL coordinates.
inline std::array<glm::vec2, 3> tri_vertices_cells(int cx, int cz, int corner) {
    const float x = static_cast<float>(cx);
    const float z = static_cast<float>(cz);
    const glm::vec2 c{x + 0.5f, z + 0.5f};
    switch (corner) {
        case kTriN: return {glm::vec2{x, z}, glm::vec2{x + 1.0f, z}, c};
        case kTriE: return {glm::vec2{x + 1.0f, z}, glm::vec2{x + 1.0f, z + 1.0f}, c};
        case kTriS: return {glm::vec2{x, z + 1.0f}, glm::vec2{x + 1.0f, z + 1.0f}, c};
        default: return {glm::vec2{x, z}, glm::vec2{x, z + 1.0f}, c};  // kTriW
    }
}

// One triangle's address.
struct TriId {
    int cx = 0, cz = 0, corner = 0;
};

// The tile a corner's OUTER edge faces, and the corner facing back. The two
// in-tile neighbours are just (corner+1)%4 and (corner+3)%4.
inline TriId tri_across(int cx, int cz, int corner) {
    switch (corner) {
        case kTriN: return {cx, cz - 1, kTriS};
        case kTriE: return {cx + 1, cz, kTriW};
        case kTriS: return {cx, cz + 1, kTriN};
        default: return {cx - 1, cz, kTriE};  // kTriW
    }
}

// The three edge-sharing neighbours: both in-tile corners, then the one across
// the tile border. Fixed order, so any traversal built on it is deterministic.
inline std::array<TriId, 3> tri_neighbors(int cx, int cz, int corner) {
    return {TriId{cx, cz, (corner + 1) % kTriPerCell},
            TriId{cx, cz, (corner + 3) % kTriPerCell}, tri_across(cx, cz, corner)};
}

// Distance from a point to a triangle (0 inside), all in cell coordinates.
// Feeds the clearance dilation, which is a metric radius rather than a ring
// count -- on the old cell grid clearance could only ever be 0 or 1 whole
// metres, and the agent radius is neither.
inline float point_tri_distance(glm::vec2 p, const std::array<glm::vec2, 3>& t) {
    // Inside test: consistent sign of the three edge cross-products. The
    // winding of tri_vertices_cells is not uniform across corners, so accept
    // either sign rather than assuming one.
    bool neg = false, pos = false;
    for (int i = 0; i < 3; ++i) {
        const glm::vec2 a = t[i];
        const glm::vec2 b = t[(i + 1) % 3];
        const float cross = (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
        neg = neg || cross < 0.0f;
        pos = pos || cross > 0.0f;
    }
    if (!(neg && pos)) {
        return 0.0f;  // inside, or on an edge
    }
    float best = std::numeric_limits<float>::infinity();
    for (int i = 0; i < 3; ++i) {
        const glm::vec2 a = t[i];
        const glm::vec2 b = t[(i + 1) % 3];
        const glm::vec2 ab = b - a;
        const float len2 = glm::dot(ab, ab);
        const float u = len2 > 0.0f ? glm::clamp(glm::dot(p - a, ab) / len2, 0.0f, 1.0f) : 0.0f;
        best = std::min(best, glm::distance(p, a + ab * u));
    }
    return best;
}

}  // namespace badlands::nav
