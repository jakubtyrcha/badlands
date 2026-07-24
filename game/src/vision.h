// Fog-of-war (vision) resolve: a pure-CPU, double-buffered visibility field
// derived each tick from the player's vision sources (buildings expand a
// euclidean radius from their footprint edges; player characters see a forward
// cone). The field lives in the SIM coordinate frame. The public surface is
// game/include/badlands_sim.hpp (ConfigureVision / GetVisionField /
// QueryVision); these internals are shared between vision.cpp and sim.cpp.

#pragma once

#include "badlands_sim.hpp"  // VisionField, VisionLevel

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

struct BadlandsGame;

namespace badlands {

// Double-buffered visibility grid, SIM frame. Each buffer is nx*nz*2 bytes,
// interleaved per texel: [2k+0] = discovered, [2k+1] = visible (0 or 255),
// k = j*nx + i. `front` is published (read by GetVisionField/QueryVision);
// resolve_vision writes `back` then swaps. See VisionField for the layout.
struct VisionGrid {
    int32_t nx = 0, nz = 0;
    float world_min_x = 0.0f, world_min_z = 0.0f;
    float texel_m = 1.0f;
    std::vector<uint8_t> front;
    std::vector<uint8_t> back;

    bool configured() const { return nx > 0 && nz > 0; }

    bool in_grid(int i, int j) const { return i >= 0 && i < nx && j >= 0 && j < nz; }

    // Cumulative knowledge: has this texel EVER been seen? Out-of-grid reads as
    // false -- off the map is not undiscovered territory, and conflating the
    // two would send explorers over the edge of the world.
    bool discovered(int i, int j) const {
        if (!in_grid(i, j)) {
            return false;
        }
        return front[2 * (static_cast<size_t>(j) * static_cast<size_t>(nx) +
                          static_cast<size_t>(i))] != 0;
    }

    glm::vec2 texel_center(int i, int j) const {
        return {world_min_x + (static_cast<float>(i) + 0.5f) * texel_m,
                world_min_z + (static_cast<float>(j) + 0.5f) * texel_m};
    }
};

// (Re)allocate the grid to cover [world_min, world_min+world_size) per axis at
// `texel_m` resolution. Clears discovered history. No-op-safe with degenerate
// sizes (leaves the grid unconfigured).
void configure_vision(VisionGrid& g, float world_min_x, float world_min_z,
                      float world_size_x, float world_size_z, float texel_m);

// One vision source's newly-discovered texel count this resolve -- the
// exploration-XP input. Only heroes are credited; buildings and non-hero
// characters discover silently. First stamper wins a texel (the resolve's
// deterministic source order breaks same-tick ties).
struct DiscoveryCredit {
    uint32_t slot;
    int32_t texels;
};

// Resolve the next visibility field from the world's player vision sources and
// publish it (swap). No-op when the grid is unconfigured. Call once per tick.
// When `credits` is non-null it is filled with per-hero discovery counts.
void resolve_vision(BadlandsGame& game, std::vector<DiscoveryCredit>* credits = nullptr);

// Snapshot of the published (front) buffer.
VisionField vision_field_of(const VisionGrid& g);

// Highest VisionLevel over the front-buffer texels within `radius` of (cx, cz).
VisionLevel query_vision(const VisionGrid& g, float cx, float cz, float radius);

}  // namespace badlands
