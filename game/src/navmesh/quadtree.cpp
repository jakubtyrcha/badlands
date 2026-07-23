#include "quadtree.h"

#include <algorithm>

namespace badlands::nav {

namespace {

// Summary of one square block's cells, gathered in a single scan.
struct BlockStats {
    bool all_passable = true;
    bool all_impassable = true;
    float min_cost = 0.0f, max_cost = 0.0f, sum_cost = 0.0f;
    float min_h = 0.0f, max_h = 0.0f;
    int count = 0;
};

}  // namespace

// Chebyshev-dilate obstacles + impassable terrain by clearance, subdivide the
// root block, and emit leaves in a fixed NW,NE,SW,SE pre-order.
void Quadtree::Build(const NavSource& src, const NavParams& params) {
    side_ = src.side();
    const int n = side_;
    leaves_.clear();
    cell_leaf_.assign(static_cast<size_t>(n) * n, -1);
    if (n <= 0) {
        return;
    }

    // Raw impassability: a building, or terrain no unit may cross.
    std::vector<char> raw(static_cast<size_t>(n) * n, 0);
    for (int z = 0; z < n; ++z) {
        for (int x = 0; x < n; ++x) {
            raw[static_cast<size_t>(z) * n + x] = cell_passable(src, x, z) ? 0 : 1;
        }
    }
    // Dilate by clearance_cells (agent radius): a cell is blocked if any raw-
    // blocked cell lies within the Chebyshev clearance window.
    const int c = std::max(0, params.clearance_cells);
    std::vector<char> blocked(static_cast<size_t>(n) * n, 0);
    for (int z = 0; z < n; ++z) {
        for (int x = 0; x < n; ++x) {
            bool hit = false;
            for (int dz = -c; dz <= c && !hit; ++dz) {
                for (int dx = -c; dx <= c && !hit; ++dx) {
                    const int nx = x + dx, nz = z + dz;
                    if (nx >= 0 && nz >= 0 && nx < n && nz < n &&
                        raw[static_cast<size_t>(nz) * n + nx]) {
                        hit = true;
                    }
                }
            }
            blocked[static_cast<size_t>(z) * n + x] = hit ? 1 : 0;
        }
    }

    auto scan = [&](int x0, int z0, int size) {
        BlockStats s;
        for (int z = z0; z < z0 + size; ++z) {
            for (int x = x0; x < x0 + size; ++x) {
                const bool passable = !blocked[static_cast<size_t>(z) * n + x];
                const float cost = passable ? src.cost(x, z) : kImpassable;
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
                if (passable) {
                    s.all_impassable = false;
                    s.sum_cost += cost;
                } else {
                    s.all_passable = false;
                }
                ++s.count;
            }
        }
        return s;
    };

    auto emit = [&](int x0, int z0, int size, bool passable, float cost) {
        const int leaf_idx = static_cast<int>(leaves_.size());
        leaves_.push_back(Leaf{x0, z0, size, passable ? cost : kImpassable, passable});
        for (int z = z0; z < z0 + size; ++z) {
            for (int x = x0; x < x0 + size; ++x) {
                cell_leaf_[static_cast<size_t>(z) * n + x] = leaf_idx;
            }
        }
    };

    // Recursive subdivision. A block is a leaf when uniform: all impassable, or
    // all passable within the cost + height tolerances. A single cell is always
    // uniform, so the recursion terminates.
    auto subdivide = [&](auto&& self, int x0, int z0, int size) -> void {
        const BlockStats s = scan(x0, z0, size);
        if (s.all_impassable) {
            emit(x0, z0, size, /*passable=*/false, kImpassable);
            return;
        }
        if (s.all_passable && (s.max_cost - s.min_cost) <= params.cost_epsilon &&
            (s.max_h - s.min_h) <= params.height_epsilon) {
            emit(x0, z0, size, /*passable=*/true, s.sum_cost / static_cast<float>(s.count));
            return;
        }
        // Mixed or over-tolerance: split into NW, NE, SW, SE (size >= 2 here,
        // since a 1-cell block is always uniform).
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

}  // namespace badlands::nav
