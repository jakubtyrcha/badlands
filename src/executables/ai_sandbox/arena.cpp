#include "executables/ai_sandbox/arena.hpp"

#include <algorithm>
#include <cmath>

namespace badlands {

namespace {

constexpr int32_t kWallKind = static_cast<int32_t>(BuildingKind::Wall);

// A Wall is 4x4 tiles. Axis-aligned it covers [c-2, c+2] on each world axis, so
// centres 4 apart share a face.
constexpr int kAxisStep = 4;
constexpr float kAxisReach = 2.0f;

// Rotation 1 snaps to the (u, v) lattice, where u = x+z and v = x-z. A 4x4
// block spans round(4 * sqrt2) = 6 in each of u and v (placement.cpp's
// diagonal_spans), so centres 6 apart in u share a face -- and 6 in u is 3 in
// each of x and z. Its world bounding box is 6x6, hence the reach below.
constexpr int kDiagStep = 6;
constexpr float kDiagReach = 3.0f;

// Centre of the rotation-1 block at lattice (u, v). Both integral keeps
// snap_center an identity, so what is asked for is what is placed.
PlacementDesc diag_block(int u, int v) {
    return PlacementDesc{kWallKind, 1, static_cast<float>(u + v) * 0.5f,
                         static_cast<float>(u - v) * 0.5f};
}

PlacementDesc axis_block(float x, float z) { return PlacementDesc{kWallKind, 0, x, z}; }

// A wall along x at fixed z, covering [x0, x1] inclusive of the end blocks.
void axis_run_x(std::vector<PlacementDesc>& out, int x0, int x1, int z) {
    for (int x = x0; x <= x1; x += kAxisStep) {
        out.push_back(axis_block(static_cast<float>(x), static_cast<float>(z)));
    }
}

void axis_run_z(std::vector<PlacementDesc>& out, int x, int z0, int z1) {
    for (int z = z0; z <= z1; z += kAxisStep) {
        out.push_back(axis_block(static_cast<float>(x), static_cast<float>(z)));
    }
}

// A 45-degree wall on the line u = uc, running over v in [v0, v1]. In world
// terms it advances 3 m in x and 3 m in z per block.
void diag_run_u(std::vector<PlacementDesc>& out, int uc, int v0, int v1) {
    for (int v = v0; v <= v1; v += kDiagStep) {
        out.push_back(diag_block(uc, v));
    }
}

// The other diagonal: v = vc, running over u. Advances 3 m in x and -3 m in z.
void diag_run_v(std::vector<PlacementDesc>& out, int vc, int u0, int u1) {
    for (int u = u0; u <= u1; u += kDiagStep) {
        out.push_back(diag_block(u, vc));
    }
}

// --- the shapes -------------------------------------------------------------
//
// Every run deliberately RUNS PAST its junctions. Overlap is legal for a plop
// and a gap is not survivable, so each side is extended a block beyond where
// the next one starts rather than trimmed to meet it exactly -- which, with one
// boundary slanted and the other vertical, is not achievable anyway.

// Interior |x| <= 22, |z| <= 10: a 44 x 20 m corridor.
void build_tube(ArenaLayout& out) {
    axis_run_x(out.plops, -24, 24, -12);
    axis_run_x(out.plops, -24, 24, 12);
    axis_run_z(out.plops, -24, -8, 8);
    axis_run_z(out.plops, 24, -8, 8);
    out.spawn_a = {-16.0f, 0.0f};
    out.spawn_b = {16.0f, 0.0f};
}

// EIGHT sides: interior |x| <= 16, |z| <= 16, |u| <= 24, |v| <= 24 -- a square
// with its four corners cut off, 32 m across the flats. Four axis sides, four
// 45-degree sides, vertices at (+-16, +-8) and (+-8, +-16).
void build_octagon(ArenaLayout& out) {
    axis_run_x(out.plops, -10, 10, -18);  // N flat
    axis_run_x(out.plops, -10, 10, 18);   // S flat
    axis_run_z(out.plops, -18, -10, 10);  // W flat
    axis_run_z(out.plops, 18, -10, 10);   // E flat
    diag_run_u(out.plops, 27, -12, 12);   // SE cut: (16,8) -> (8,16)
    diag_run_v(out.plops, 27, -12, 12);   // NE cut: (16,-8) -> (8,-16)
    diag_run_u(out.plops, -27, -12, 12);  // NW cut
    diag_run_v(out.plops, -27, -12, 12);  // SW cut
    out.spawn_a = {-12.0f, 0.0f};
    out.spawn_b = {12.0f, 0.0f};
}

// Interior |x| + |z| <= 21, i.e. |u| <= 21 and |v| <= 21: a rhombus 42 m across
// each diagonal, with four pillars ringing an open centre.
void build_rhomboid(ArenaLayout& out) {
    // The u runs own the four tips: a v run reaching u = +-24 would emit a
    // block the u run already placed, and a coincident duplicate is two
    // identical meshes z-fighting on the sharpest, most visible corner of the
    // arena. Stopping the v runs one step short costs no coverage -- the tip
    // block is there either way.
    diag_run_u(out.plops, 24, -24, 24);
    diag_run_u(out.plops, -24, -24, 24);
    diag_run_v(out.plops, 24, -18, 18);
    diag_run_v(out.plops, -24, -18, 18);
    // Far enough apart that every lane between them stays wider than the
    // navmesh's clearance dilation -- a column that seals a corridor is a wall
    // with extra steps.
    out.plops.push_back(axis_block(12.0f, 0.0f));
    out.plops.push_back(axis_block(-12.0f, 0.0f));
    out.plops.push_back(axis_block(0.0f, 12.0f));
    out.plops.push_back(axis_block(0.0f, -12.0f));
    // Off-axis, so the two fighters start with a pillar between them -- which
    // is the whole reason this shape exists.
    out.spawn_a = {-10.0f, -6.0f};
    out.spawn_b = {10.0f, 6.0f};
}

}  // namespace

const char* arena_shape_name(ArenaShape s) {
    switch (s) {
        case ArenaShape::Tube: return "tube";
        case ArenaShape::Octagon: return "octagon";
        case ArenaShape::Rhomboid: return "rhomboid";
        case ArenaShape::Count: break;
    }
    return "?";
}

ArenaLayout build_arena(ArenaShape shape) {
    ArenaLayout layout;
    switch (shape) {
        case ArenaShape::Tube: build_tube(layout); break;
        case ArenaShape::Octagon: build_octagon(layout); break;
        case ArenaShape::Rhomboid: build_rhomboid(layout); break;
        case ArenaShape::Count: break;
    }
    return layout;
}

}  // namespace badlands
