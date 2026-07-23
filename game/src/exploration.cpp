#include "exploration.h"

#include "behaviours/rng.h"
#include "vision.h"

#include <cmath>

namespace badlands {

namespace {

// The 4-neighbourhood. Diagonals are deliberately excluded: a diagonal-only
// contact is a corner touch, which would make ragged single-texel notches count
// as frontier and scatter targets along the inside of an explored region.
constexpr int kNeighbourDx[4] = {1, -1, 0, 0};
constexpr int kNeighbourDz[4] = {0, 0, 1, -1};

}  // namespace

std::optional<glm::vec2> pick_exploration_target(const VisionGrid& grid, glm::vec2 origin,
                                                 uint64_t seed, const HeroFactors& factors) {
    if (!grid.configured()) {
        return std::nullopt;  // no fog of war -> nothing is "unknown"
    }

    uint64_t rng = seed == 0 ? 1ull : seed;
    const float search2 = factors.explore_search_radius * factors.explore_search_radius;

    // Reservoir sampling over the frontier: one pass, no allocation, and a
    // uniform pick that depends only on the seed. The alternative -- collecting
    // every frontier texel and indexing one -- allocates per hero per lease over
    // a grid that is 65k texels on the shipping map.
    bool found = false;
    glm::vec2 chosen_center{0.0f, 0.0f};
    glm::vec2 chosen_outward{0.0f, 0.0f};
    int64_t seen = 0;

    for (int j = 0; j < grid.nz; ++j) {
        for (int i = 0; i < grid.nx; ++i) {
            if (!grid.discovered(i, j)) {
                continue;  // you can only stand on the outline from the inside
            }
            // Which way is the dark? Sum the directions of unknown neighbours,
            // so a texel on a flat edge points straight out and one in a
            // concave pocket points along the bisector.
            glm::vec2 outward{0.0f, 0.0f};
            for (int n = 0; n < 4; ++n) {
                if (!grid.discovered(i + kNeighbourDx[n], j + kNeighbourDz[n]) &&
                    grid.in_grid(i + kNeighbourDx[n], j + kNeighbourDz[n])) {
                    outward += glm::vec2{static_cast<float>(kNeighbourDx[n]),
                                         static_cast<float>(kNeighbourDz[n])};
                }
            }
            if (outward == glm::vec2{0.0f, 0.0f}) {
                // Either interior, or its only unknown neighbours are off-map.
                // Neither is somewhere to explore.
                const bool any_unknown_in_grid = [&] {
                    for (int n = 0; n < 4; ++n) {
                        const int ni = i + kNeighbourDx[n];
                        const int nj = j + kNeighbourDz[n];
                        if (grid.in_grid(ni, nj) && !grid.discovered(ni, nj)) {
                            return true;
                        }
                    }
                    return false;
                }();
                if (!any_unknown_in_grid) {
                    continue;
                }
            }

            const glm::vec2 center = grid.texel_center(i, j);
            const glm::vec2 to_origin = center - origin;
            if (glm::dot(to_origin, to_origin) > search2) {
                continue;  // too far afield to be worth setting out for
            }

            ++seen;
            // Keep this one with probability 1/seen -- the standard reservoir
            // step, which leaves every frontier texel equally likely.
            if (range_i64(rng, 1, seen) == 1) {
                found = true;
                chosen_center = center;
                chosen_outward = outward;
            }
        }
    }

    if (!found) {
        return std::nullopt;
    }

    const float outward_len = glm::length(chosen_outward);
    // A texel whose unknown neighbours cancel out (a one-texel isthmus) has no
    // meaningful outward direction; aim along +X rather than dividing by zero.
    const glm::vec2 direction =
        outward_len > 1e-4f ? chosen_outward / outward_len : glm::vec2{1.0f, 0.0f};

    const float span = factors.explore_max_distance - factors.explore_min_distance;
    const float distance = factors.explore_min_distance + unit_float(rng) * std::max(0.0f, span);
    return chosen_center + direction * distance;
}

}  // namespace badlands
