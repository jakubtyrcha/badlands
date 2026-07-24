#include "vision.h"

#include "components.h"
#include "game_state.h"
#include "placement.h"

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>

namespace badlands {

namespace {

inline int clampi(int v, int lo, int hi) { return std::max(lo, std::min(v, hi)); }

inline int texel_i(const VisionGrid& g, float wx) {
    return static_cast<int>(std::floor((wx - g.world_min_x) / g.texel_m));
}
inline int texel_j(const VisionGrid& g, float wz) {
    return static_cast<int>(std::floor((wz - g.world_min_z) / g.texel_m));
}
inline glm::vec2 texel_center(const VisionGrid& g, int i, int j) {
    return {g.world_min_x + (static_cast<float>(i) + 0.5f) * g.texel_m,
            g.world_min_z + (static_cast<float>(j) + 0.5f) * g.texel_m};
}
inline size_t texel_k(const VisionGrid& g, int i, int j) {
    return static_cast<size_t>(j) * static_cast<size_t>(g.nx) + static_cast<size_t>(i);
}

// Marks texel (i,j) visible for this resolve, and discovered if it wasn't
// already -- bumping `fresh` exactly when THIS stamp is what discovered it.
// The one write to the back buffer, shared by every stamp site (the rect
// loop, the cone loop, and the cone's own-texel tail) so the visible/
// discovered bookkeeping can't drift between them.
inline void stamp_texel(VisionGrid& g, int i, int j, int& fresh) {
    const size_t k2 = 2 * texel_k(g, i, j);
    g.back[k2 + 1] = 255;
    if (!g.back[k2]) {
        g.back[k2] = 255;
        ++fresh;
    }
}

// World-space axis-aligned rect covering a footprint (exact for ortho; the
// oriented bounding box for diagonal, which slightly over-reveals at corners --
// acceptable for a vision expansion).
void footprint_world_aabb(const Footprint& fp, float& x0, float& x1, float& z0, float& z1) {
    if (!fp.diagonal) {
        x0 = fp.x0;
        x1 = fp.x1;
        z0 = fp.z0;
        z1 = fp.z1;
        return;
    }
    const float p = static_cast<float>(fp.p), q = static_cast<float>(fp.q);
    const float r = static_cast<float>(fp.r), s = static_cast<float>(fp.s);
    x0 = 0.5f * (p + r);
    x1 = 0.5f * (q + s);
    z0 = 0.5f * (p - s);
    z1 = 0.5f * (q - r);
}

// Stamp `visible` where the euclidean distance from a texel center to the rect
// [x0,x1]x[z0,z1] is <= radius (i.e. the rect grown by `radius`), also setting
// `discovered` for any texel newly discovered by this stamp. Returns the count
// of texels whose discovered bit THIS stamp set (0 if already discovered by an
// earlier source this resolve).
int stamp_rect_expanded(VisionGrid& g, float x0, float x1, float z0, float z1, float radius) {
    const int i0 = clampi(texel_i(g, x0 - radius), 0, g.nx - 1);
    const int i1 = clampi(texel_i(g, x1 + radius), 0, g.nx - 1);
    const int j0 = clampi(texel_j(g, z0 - radius), 0, g.nz - 1);
    const int j1 = clampi(texel_j(g, z1 + radius), 0, g.nz - 1);
    const float r2 = radius * radius;
    int fresh = 0;
    for (int j = j0; j <= j1; ++j) {
        for (int i = i0; i <= i1; ++i) {
            const glm::vec2 c = texel_center(g, i, j);
            const float dx = std::max({x0 - c.x, 0.0f, c.x - x1});
            const float dz = std::max({z0 - c.y, 0.0f, c.y - z1});
            if (dx * dx + dz * dz <= r2) {
                stamp_texel(g, i, j, fresh);
            }
        }
    }
    return fresh;
}

// Stamp `visible` inside a forward cone: within `radius` and, for texels beyond
// the character's own cell, whose direction lies within the cone half-angle.
// Also sets `discovered` alongside, returning the count of texels whose
// discovered bit THIS stamp set.
int stamp_cone(VisionGrid& g, glm::vec2 c, glm::vec2 facing, float radius,
               float cone_half_cos) {
    const int i0 = clampi(texel_i(g, c.x - radius), 0, g.nx - 1);
    const int i1 = clampi(texel_i(g, c.x + radius), 0, g.nx - 1);
    const int j0 = clampi(texel_j(g, c.y - radius), 0, g.nz - 1);
    const int j1 = clampi(texel_j(g, c.y + radius), 0, g.nz - 1);
    const float flen = glm::length(facing);
    const glm::vec2 f = (flen > 1e-6f) ? facing / flen : kCharacterForward;
    const float r2 = radius * radius;
    int fresh = 0;
    for (int j = j0; j <= j1; ++j) {
        for (int i = i0; i <= i1; ++i) {
            const glm::vec2 v = texel_center(g, i, j) - c;
            const float d2 = glm::dot(v, v);
            if (d2 > r2) continue;
            if (d2 > 1e-6f && glm::dot(v * (1.0f / std::sqrt(d2)), f) < cone_half_cos) {
                continue;
            }
            stamp_texel(g, i, j, fresh);
        }
    }
    // A unit always sees the texel it stands on, regardless of the cone (its
    // own texel center can fall behind its facing, which would otherwise leave
    // that tile Unknown for a stationary unit).
    const int ci = texel_i(g, c.x);
    const int cj = texel_j(g, c.y);
    if (ci >= 0 && ci < g.nx && cj >= 0 && cj < g.nz) {
        stamp_texel(g, ci, cj, fresh);
    }
    return fresh;
}

}  // namespace

void configure_vision(VisionGrid& g, float world_min_x, float world_min_z,
                      float world_size_x, float world_size_z, float texel_m) {
    if (texel_m <= 0.0f || world_size_x <= 0.0f || world_size_z <= 0.0f) {
        g = VisionGrid{};
        return;
    }
    const int nx = std::max(1, static_cast<int>(std::ceil(world_size_x / texel_m)));
    const int nz = std::max(1, static_cast<int>(std::ceil(world_size_z / texel_m)));
    // Idempotent for identical params (public contract): keep the accumulated
    // discovered history rather than wiping it, so a caller may re-configure a
    // stable grid each frame without the map flickering back to terra-incognita.
    if (g.configured() && g.nx == nx && g.nz == nz && g.world_min_x == world_min_x &&
        g.world_min_z == world_min_z && g.texel_m == texel_m) {
        return;
    }
    g.nx = nx;
    g.nz = nz;
    g.world_min_x = world_min_x;
    g.world_min_z = world_min_z;
    g.texel_m = texel_m;
    const size_t bytes = static_cast<size_t>(g.nx) * static_cast<size_t>(g.nz) * 2u;
    g.front.assign(bytes, 0);
    g.back.assign(bytes, 0);
}

void resolve_vision(BadlandsGame& game, std::vector<DiscoveryCredit>* credits) {
    VisionGrid& g = game.vision;
    if (!g.configured()) return;

    const size_t n = static_cast<size_t>(g.nx) * static_cast<size_t>(g.nz);
    // Start the back buffer from the (cumulative) discovered history, visibility
    // cleared. discovered is re-OR'd with the fresh visible below.
    for (size_t k = 0; k < n; ++k) {
        g.back[2 * k] = g.front[2 * k];
        g.back[2 * k + 1] = 0;
    }

    // Player buildings: a euclidean radius grown from the footprint edges.
    for (const PlacedBuilding& b : game.placement.buildings) {
        if (!b.alive) continue;
        const BuildingDef def = BuildingDefOf(static_cast<BuildingKind>(b.kind));
        if (def.vision_radius <= 0.0f) continue;
        const Footprint fp = make_footprint(b.kind, b.rot, b.center);
        float x0, x1, z0, z1;
        footprint_world_aabb(fp, x0, x1, z0, z1);
        stamp_rect_expanded(g, x0, x1, z0, z1, def.vision_radius);
    }

    // Player characters: a forward vision cone (hidden/inside heroes excluded).
    for (auto [e, pos, team, vis, facing] :
         game.registry
             .view<const Position, const Team, const Vision, const Facing>(
                 entt::exclude<InsideBuilding>)
             .each()) {
        if (team.id != kPlayerTeam || vis.radius <= 0.0f) continue;
        const int fresh = stamp_cone(g, pos.pos, facing.dir, vis.radius, vis.cone_half_cos);
        // Every stamper is reported here, hero or not -- the crediting policy
        // (today: only heroes gain XP) lives with the caller (tick_world's
        // award_xp, which no-ops non-heroes), not with the resolve.
        if (credits != nullptr && fresh > 0) {
            credits->push_back({slot_for_entity(game, e), fresh});
        }
    }

    // Invariant: visible ⊆ discovered, maintained by the stamps above.
    std::swap(g.front, g.back);
}

VisionField vision_field_of(const VisionGrid& g) {
    VisionField f;
    if (!g.configured()) return f;
    f.nx = g.nx;
    f.nz = g.nz;
    f.world_min_x = g.world_min_x;
    f.world_min_z = g.world_min_z;
    f.texel_m = g.texel_m;
    f.rg = g.front.data();
    return f;
}

VisionLevel query_vision(const VisionGrid& g, float cx, float cz, float radius) {
    if (!g.configured()) return VisionLevel::Unknown;
    const float r = std::max(0.0f, radius);
    // Unclamped texel bounds: texels outside the grid are skipped (not clamped
    // to a border texel), so a query whose bounds miss the grid returns Unknown
    // per the public contract rather than sampling the nearest edge.
    const int i0 = texel_i(g, cx - r);
    const int i1 = texel_i(g, cx + r);
    const int j0 = texel_j(g, cz - r);
    const int j1 = texel_j(g, cz + r);
    // The texel containing the query point (covers radius 0), if in-grid.
    const int ci = texel_i(g, cx);
    const int cj = texel_j(g, cz);
    const float r2 = r * r;
    bool any_discovered = false;
    auto in_grid = [&](int i, int j) {
        return i >= 0 && i < g.nx && j >= 0 && j < g.nz;
    };
    auto consider = [&](int i, int j) -> bool {
        const size_t k = texel_k(g, i, j);
        if (g.front[2 * k + 1]) return true;  // visible -> highest level, stop
        if (g.front[2 * k]) any_discovered = true;
        return false;
    };
    if (in_grid(ci, cj) && consider(ci, cj)) return VisionLevel::Visible;
    for (int j = j0; j <= j1; ++j) {
        for (int i = i0; i <= i1; ++i) {
            if (i == ci && j == cj) continue;  // already considered
            if (!in_grid(i, j)) continue;
            const glm::vec2 v = texel_center(g, i, j) - glm::vec2(cx, cz);
            if (glm::dot(v, v) > r2) continue;
            if (consider(i, j)) return VisionLevel::Visible;
        }
    }
    return any_discovered ? VisionLevel::Dormant : VisionLevel::Unknown;
}

}  // namespace badlands
