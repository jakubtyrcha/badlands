#include "quadtree.h"

#include <algorithm>
#include <cmath>

namespace badlands::nav {

namespace {

// Summary of one square block's cells, gathered in a single scan.
struct BlockStats {
    bool all_free = true;   // every cell wholly walkable
    bool all_solid = true;  // every cell wholly obstructed
    float min_cost = 0.0f, max_cost = 0.0f, sum_cost = 0.0f;
    float min_h = 0.0f, max_h = 0.0f;
    int count = 0;      // cells scanned
    int free_count = 0; // cells contributing to sum_cost
};

// Offsets from a solid triangle to the triangles its clearance blocks.
//
// The dilation is a METRIC radius, so it has to be derived rather than
// hardcoded as a ring count: which neighbours fall inside depends on the
// radius, and the interesting radii are sub-cell. Derived once per Build and
// then applied as a stamp.
//
// At the sim's 0.2 m this resolves to ONE triangle per solid triangle: the one
// across its outer edge, whose centroid is 1/6 ~ 0.167 away. The two in-tile
// neighbours are sqrt(2)/6 ~ 0.236 away and survive, and that is the whole
// point -- they are the free half of a boundary cell, so blocking them would
// fill every half-covered cell back in and throw the triangle mask away.
// NavParams::clearance_m has the full regime table; the ceiling is real and
// 0.25 is already over it.
struct ClearanceStencil {
    std::vector<TriId> from[kTriPerCell];
};

ClearanceStencil make_stencil(float clearance_cells) {
    ClearanceStencil s;
    if (!(clearance_cells > 0.0f)) {
        return s;
    }
    // A centroid is at most sqrt(2)/2 from its own cell's corners, so one cell
    // of slack past the radius covers every triangle that can qualify.
    const int r = static_cast<int>(std::ceil(clearance_cells)) + 1;
    for (int cs = 0; cs < kTriPerCell; ++cs) {
        const std::array<glm::vec2, 3> src = tri_vertices_cells(0, 0, cs);
        for (int dz = -r; dz <= r; ++dz) {
            for (int dx = -r; dx <= r; ++dx) {
                for (int c = 0; c < kTriPerCell; ++c) {
                    if (dx == 0 && dz == 0 && c == cs) {
                        continue;  // the source triangle is already solid
                    }
                    if (point_tri_distance(tri_centroid_cells(dx, dz, c), src) <=
                        clearance_cells) {
                        s.from[cs].push_back(TriId{dx, dz, c});
                    }
                }
            }
        }
    }
    return s;
}

}  // namespace

// Read the per-triangle obstacle masks, dilate them by clearance over the
// triangle lattice, subdivide the root block, and emit leaves in a fixed
// NW,NE,SW,SE pre-order.
void Quadtree::Build(const NavSource& src, const NavParams& params) {
    side_ = src.side();
    const int n = side_;
    leaves_.clear();
    cell_leaf_.assign(static_cast<size_t>(n) * n, -1);
    if (n <= 0) {
        return;
    }
    const auto at = [n](int x, int z) { return static_cast<size_t>(z) * n + x; };

    // Raw obstruction, per triangle. Impassable TERRAIN has no sub-cell
    // structure (biomes are sampled per cell), so it enters as a whole solid
    // cell; only footprints are authored finer than that.
    std::vector<uint8_t> raw(static_cast<size_t>(n) * n, kMaskFree);
    for (int z = 0; z < n; ++z) {
        for (int x = 0; x < n; ++x) {
            raw[at(x, z)] = cell_terrain_passable(src, x, z) ? src.blocked_mask(x, z) : kMaskSolid;
        }
    }

    // Clearance, stamped from `raw` into `blocked` -- never in place, or the
    // dilation would feed on itself and grow without bound.
    std::vector<uint8_t> blocked = raw;
    const float clearance_cells =
        src.cell_size_m() > 0.0f ? params.clearance_m / src.cell_size_m() : 0.0f;
    const ClearanceStencil stencil = make_stencil(clearance_cells);
    for (int z = 0; z < n; ++z) {
        for (int x = 0; x < n; ++x) {
            const uint8_t m = raw[at(x, z)];
            if (m == kMaskFree) {
                continue;
            }
            for (int cs = 0; cs < kTriPerCell; ++cs) {
                if (!mask_has(m, cs)) {
                    continue;
                }
                for (const TriId& o : stencil.from[cs]) {
                    const int nx = x + o.cx, nz = z + o.cz;
                    if (nx < 0 || nz < 0 || nx >= n || nz >= n) {
                        continue;
                    }
                    blocked[at(nx, nz)] |= tri_bit(o.corner);
                }
            }
        }
    }

    auto scan = [&](int x0, int z0, int size) {
        BlockStats s;
        for (int z = z0; z < z0 + size; ++z) {
            for (int x = x0; x < x0 + size; ++x) {
                const uint8_t m = blocked[at(x, z)];
                const bool solid = m == kMaskSolid;
                const float cost = solid ? kImpassable : src.cost(x, z);
                const float h = src.height(x, z);
                if (s.count == 0) {
                    s.min_cost = s.max_cost = cost;
                    s.min_h = s.max_h = h;
                } else {
                    s.min_cost = std::min(s.min_cost, cost);
                    s.max_cost = std::max(s.max_cost, cost);
                    s.min_h = std::min(s.min_h, h);
                    s.max_h = std::max(s.max_h, h);
                }
                if (m != kMaskFree) {
                    s.all_free = false;
                }
                if (!solid) {
                    s.all_solid = false;
                    s.sum_cost += cost;
                    ++s.free_count;
                }
                ++s.count;
            }
        }
        return s;
    };

    auto emit = [&](int x0, int z0, int size, bool passable, float cost, uint8_t mask) {
        const int leaf_idx = static_cast<int>(leaves_.size());
        leaves_.push_back(Leaf{x0, z0, size, passable ? cost : kImpassable, passable, mask});
        for (int z = z0; z < z0 + size; ++z) {
            for (int x = x0; x < x0 + size; ++x) {
                cell_leaf_[at(x, z)] = leaf_idx;
            }
        }
    };

    // Recursive subdivision. A block merges only when its cells are WHOLLY one
    // thing -- all solid, or all free within the cost + height tolerances. A
    // half-covered cell is neither, and merging it either way is exactly the
    // information loss this decomposition exists to stop, so it bottoms out at
    // a single cell that carries its own mask.
    auto subdivide = [&](auto&& self, int x0, int z0, int size) -> void {
        const BlockStats s = scan(x0, z0, size);
        if (s.all_solid) {
            emit(x0, z0, size, /*passable=*/false, kImpassable, kMaskSolid);
            return;
        }
        if (s.all_free && (s.max_cost - s.min_cost) <= params.cost_epsilon &&
            (s.max_h - s.min_h) <= params.height_epsilon) {
            emit(x0, z0, size, /*passable=*/true,
                 s.sum_cost / static_cast<float>(s.free_count), kMaskFree);
            return;
        }
        if (size == 1) {
            // Partial: some corners solid, some free. Passable, because a unit
            // has somewhere to stand here -- the graph decides WHICH corners.
            // (A wholly-free single cell over tolerance cannot reach this: one
            // cell has zero spread.)
            emit(x0, z0, 1, /*passable=*/true, src.cost(x0, z0), blocked[at(x0, z0)]);
            return;
        }
        const int half = size / 2;
        self(self, x0, z0, half);
        self(self, x0 + half, z0, half);
        self(self, x0, z0 + half, half);
        self(self, x0 + half, z0 + half, half);
    };

    subdivide(subdivide, 0, 0, n);
}

int Quadtree::LeafAt(int cx, int cz) const {
    if (cx < 0 || cz < 0 || cx >= side_ || cz >= side_) {
        return -1;
    }
    return cell_leaf_[static_cast<size_t>(cz) * side_ + cx];
}

uint8_t Quadtree::MaskAt(int cx, int cz) const {
    const int li = LeafAt(cx, cz);
    return li < 0 ? kMaskSolid : leaves_[li].tri_mask;
}

bool Quadtree::TriPassable(int cx, int cz, int corner) const {
    const int li = LeafAt(cx, cz);
    if (li < 0 || !leaves_[li].passable) {
        return false;
    }
    return !mask_has(leaves_[li].tri_mask, corner);
}

}  // namespace badlands::nav
