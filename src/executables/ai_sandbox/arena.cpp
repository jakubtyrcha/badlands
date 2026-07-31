#include "executables/ai_sandbox/arena.hpp"

#include <algorithm>
#include <cstdlib>

namespace badlands {

namespace {

constexpr int32_t kWallKind = static_cast<int32_t>(BuildingKind::Wall);

// Tiles per block: the Wall footprint, and therefore the lattice pitch.
constexpr int kBlockTiles = 4;

// How far out to scan for wall blocks. Comfortably past every shape below, and
// far inside the 256-tile placement grid.
constexpr int kScanBlocks = 12;

// Is block (i, j) inside the arena floor?
bool interior_block(ArenaShape shape, int i, int j) {
    const int ai = std::abs(i);
    const int aj = std::abs(j);
    switch (shape) {
        case ArenaShape::Tube:
            return ai <= 5 && aj <= 2;  // 44 x 20 m
        case ArenaShape::Octagon:
            return ai <= 4 && aj <= 4 && ai + aj <= 6;  // 36 m across the flats
        case ArenaShape::Diamond:
            return ai + aj <= 5;  // 44 m across the diagonals
        case ArenaShape::Count:
            break;
    }
    return false;
}

// Free-standing obstacles inside the floor, in block coordinates.
std::vector<glm::ivec2> column_blocks(ArenaShape shape) {
    if (shape != ArenaShape::Diamond) {
        return {};  // the other two are deliberately bare
    }
    // Four pillars ringing an open centre, far enough apart that every lane
    // between them stays wider than the navmesh's clearance dilation -- a
    // column that seals a corridor is a wall with extra steps.
    return {{3, 0}, {-3, 0}, {0, 3}, {0, -3}};
}

PlacementDesc block_plop(int i, int j) {
    const glm::vec2 c = arena_block_center(i, j);
    return PlacementDesc{kWallKind, 0, c.x, c.y};
}

}  // namespace

const char* arena_shape_name(ArenaShape s) {
    switch (s) {
        case ArenaShape::Tube: return "tube";
        case ArenaShape::Octagon: return "octagon";
        case ArenaShape::Diamond: return "diamond";
        case ArenaShape::Count: break;
    }
    return "?";
}

glm::vec2 arena_block_center(int i, int j) {
    return {static_cast<float>(i * kBlockTiles), static_cast<float>(j * kBlockTiles)};
}

ArenaLayout build_arena(ArenaShape shape) {
    ArenaLayout layout;
    if (shape == ArenaShape::Count) {
        return layout;
    }

    // The wall is the 8-neighbour dilation of the interior, minus the interior.
    // 8 and not 4: a 4-dilated staircase leaves consecutive blocks touching at
    // a corner only, which is a gap a body can slip through even though a
    // block-stepping flood fill would call it sealed.
    int max_block = 0;
    for (int j = -kScanBlocks; j <= kScanBlocks; ++j) {
        for (int i = -kScanBlocks; i <= kScanBlocks; ++i) {
            if (interior_block(shape, i, j)) {
                continue;
            }
            bool touches_interior = false;
            for (int dj = -1; dj <= 1 && !touches_interior; ++dj) {
                for (int di = -1; di <= 1 && !touches_interior; ++di) {
                    touches_interior = interior_block(shape, i + di, j + dj);
                }
            }
            if (!touches_interior) {
                continue;
            }
            layout.plops.push_back(block_plop(i, j));
            max_block = std::max({max_block, std::abs(i), std::abs(j)});
        }
    }

    for (const glm::ivec2& c : column_blocks(shape)) {
        layout.plops.push_back(block_plop(c.x, c.y));
    }

    // Half the outer footprint: the outermost wall block's far face.
    const float extent = static_cast<float>(max_block * kBlockTiles + kBlockTiles / 2);
    layout.half_extent = {extent, extent};

    // Opposed spawn points, on the floor and clear of any column. The diamond's
    // sit off-axis because its columns are on the axes -- and putting the two
    // fighters on opposite sides of a pillar is the whole reason that arena
    // exists.
    switch (shape) {
        case ArenaShape::Tube:
            layout.spawn_a = arena_block_center(-4, 0);
            layout.spawn_b = arena_block_center(4, 0);
            break;
        case ArenaShape::Octagon:
            layout.spawn_a = arena_block_center(-3, 0);
            layout.spawn_b = arena_block_center(3, 0);
            break;
        case ArenaShape::Diamond:
            layout.spawn_a = arena_block_center(-3, -2);
            layout.spawn_b = arena_block_center(3, 2);
            break;
        case ArenaShape::Count:
            break;
    }
    return layout;
}

}  // namespace badlands
