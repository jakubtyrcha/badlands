#include "placement.h"

#include "game_state.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace badlands {

namespace {

// Footprint sizes and poppable flag, indexed by GameBuildingKind.
constexpr GameBuildingDef kDefs[GAME_BUILDING_KIND_COUNT] = {
    {4, 4, 0},  // Castle
    {3, 3, 0},  // Free Company Quarters
    {3, 3, 0},  // Hunter's Camp
    {3, 3, 0},  // Thieves' Den
    {3, 3, 0},  // Scriptorium
    {2, 1, 0},  // Tavern
    {2, 1, 0},  // Apothecary
    {1, 1, 0},  // Watchtower
    {2, 1, 1},  // House (poppable)
    {1, 1, 1},  // Sewer (poppable)
};

// Urban-sprawl contribution in quarter-units. Watchtower is a small structure
// (3/4); every other player building is a full unit (4/4). Poppables and the
// castle contribute nothing.
uint32_t urban_contribution(int kind) {
    if (kind == GAME_BUILDING_WATCHTOWER) {
        return 3;
    }
    if (kind == GAME_BUILDING_HOUSE || kind == GAME_BUILDING_SEWER ||
        kind == GAME_BUILDING_CASTLE) {
        return 0;
    }
    return 4;
}

const GameBuildingDef& def_of(int kind) {
    if (kind < 0 || kind >= GAME_BUILDING_KIND_COUNT) {
        kind = GAME_BUILDING_WATCHTOWER;  // 1x1 fallback; never expected
    }
    return kDefs[kind];
}

bool is_diagonal(int rot) { return (rot & 1) != 0; }

// Nearest lattice value for a footprint span: even span -> integer, odd span ->
// half-integer. Used per world axis (ortho) and per u/v axis (diagonal).
float snap_axis(float value, int span) {
    if ((span & 1) == 0) {
        return std::round(value);
    }
    return std::floor(value) + 0.5f;
}

}  // namespace

glm::vec2 triangle_centroid(int tx, int tz, int corner) {
    constexpr float lo = 1.0f / 6.0f;
    constexpr float hi = 5.0f / 6.0f;
    float dx, dz;
    switch (corner) {
        case 0: dx = 0.5f; dz = lo; break;  // N (-Z)
        case 1: dx = hi;   dz = 0.5f; break;  // E (+X)
        case 2: dx = 0.5f; dz = hi; break;  // S (+Z)
        default: dx = lo;  dz = 0.5f; break;  // W (-X)
    }
    return {static_cast<float>(tx) + dx, static_cast<float>(tz) + dz};
}

glm::ivec2 diagonal_spans(int w, int d, int rot) {
    constexpr float kSqrt2 = 1.41421356237f;
    int su = std::max(1, static_cast<int>(std::lround(w * kSqrt2)));
    int sv = std::max(1, static_cast<int>(std::lround(d * kSqrt2)));
    if (rot == 3) {  // 135 deg transposes the diagonal axes
        std::swap(su, sv);
    }
    return {su, sv};
}

glm::vec2 snap_center(int kind, int rot, glm::vec2 raw) {
    const GameBuildingDef& def = def_of(kind);
    if (!is_diagonal(rot)) {
        int nx = (rot == 0) ? def.width_tiles : def.depth_tiles;
        int nz = (rot == 0) ? def.depth_tiles : def.width_tiles;
        return {snap_axis(raw.x, nx), snap_axis(raw.y, nz)};
    }
    glm::ivec2 spans = diagonal_spans(def.width_tiles, def.depth_tiles, rot);
    float uc = snap_axis(raw.x + raw.y, spans.x);
    float vc = snap_axis(raw.x - raw.y, spans.y);
    return {(uc + vc) * 0.5f, (uc - vc) * 0.5f};
}

Footprint make_footprint(int kind, int rot, glm::vec2 center) {
    const GameBuildingDef& def = def_of(kind);
    Footprint fp{};
    if (!is_diagonal(rot)) {
        fp.diagonal = false;
        int nx = (rot == 0) ? def.width_tiles : def.depth_tiles;
        int nz = (rot == 0) ? def.depth_tiles : def.width_tiles;
        fp.x0 = center.x - nx * 0.5f;
        fp.x1 = fp.x0 + nx;
        fp.z0 = center.y - nz * 0.5f;
        fp.z1 = fp.z0 + nz;
    } else {
        fp.diagonal = true;
        glm::ivec2 spans = diagonal_spans(def.width_tiles, def.depth_tiles, rot);
        float uc = center.x + center.y;
        float vc = center.x - center.y;
        fp.p = static_cast<int>(std::lround(uc - spans.x * 0.5f));
        fp.q = fp.p + spans.x;
        fp.r = static_cast<int>(std::lround(vc - spans.y * 0.5f));
        fp.s = fp.r + spans.y;
    }
    return fp;
}

namespace {

bool in_footprint(const Footprint& fp, glm::vec2 c) {
    if (!fp.diagonal) {
        return c.x > fp.x0 && c.x < fp.x1 && c.y > fp.z0 && c.y < fp.z1;
    }
    float u = c.x + c.y;
    float v = c.x - c.y;
    return u > fp.p && u < fp.q && v > fp.r && v < fp.s;
}

// L1 distance in world units from a point to the footprint region (0 inside).
// Exact because the footprint boundary lies on lattice lines and the point is a
// triangle centroid (never on a line). Diagonal uses L-inf in (u,v), which is
// the L1 metric in (x,z).
float footprint_l1_dist(const Footprint& fp, glm::vec2 c) {
    if (!fp.diagonal) {
        float dx = std::max({fp.x0 - c.x, 0.0f, c.x - fp.x1});
        float dz = std::max({fp.z0 - c.y, 0.0f, c.y - fp.z1});
        return dx + dz;
    }
    float u = c.x + c.y;
    float v = c.x - c.y;
    float du = std::max({fp.p - u, 0.0f, u - static_cast<float>(fp.q)});
    float dv = std::max({fp.r - v, 0.0f, v - static_cast<float>(fp.s)});
    return std::max(du, dv);
}

// Tile bbox covering the footprint plus `pad` tiles, clipped to the grid.
void footprint_tile_bounds(const Footprint& fp, int pad, int& tx0, int& tx1, int& tz0,
                           int& tz1) {
    float minx, maxx, minz, maxz;
    if (!fp.diagonal) {
        minx = fp.x0; maxx = fp.x1; minz = fp.z0; maxz = fp.z1;
    } else {
        float xs[4] = {(fp.p + fp.r) * 0.5f, (fp.p + fp.s) * 0.5f, (fp.q + fp.r) * 0.5f,
                       (fp.q + fp.s) * 0.5f};
        float zs[4] = {(fp.p - fp.r) * 0.5f, (fp.p - fp.s) * 0.5f, (fp.q - fp.r) * 0.5f,
                       (fp.q - fp.s) * 0.5f};
        minx = *std::min_element(xs, xs + 4); maxx = *std::max_element(xs, xs + 4);
        minz = *std::min_element(zs, zs + 4); maxz = *std::max_element(zs, zs + 4);
    }
    tx0 = std::max(-kGridHalf, static_cast<int>(std::floor(minx)) - pad);
    tx1 = std::min(kGridHalf - 1, static_cast<int>(std::ceil(maxx)) + pad);
    tz0 = std::max(-kGridHalf, static_cast<int>(std::floor(minz)) - pad);
    tz1 = std::min(kGridHalf - 1, static_cast<int>(std::ceil(maxz)) + pad);
}

constexpr float kMarginRadius = 1.0f;

}  // namespace

bool footprint_in_bounds(const Footprint& fp) {
    if (!fp.diagonal) {
        return fp.x0 >= -kGridHalf && fp.x1 <= kGridHalf && fp.z0 >= -kGridHalf &&
               fp.z1 <= kGridHalf;
    }
    float xs[4] = {(fp.p + fp.r) * 0.5f, (fp.p + fp.s) * 0.5f, (fp.q + fp.r) * 0.5f,
                   (fp.q + fp.s) * 0.5f};
    float zs[4] = {(fp.p - fp.r) * 0.5f, (fp.p - fp.s) * 0.5f, (fp.q - fp.r) * 0.5f,
                   (fp.q - fp.s) * 0.5f};
    float minx = *std::min_element(xs, xs + 4), maxx = *std::max_element(xs, xs + 4);
    float minz = *std::min_element(zs, zs + 4), maxz = *std::max_element(zs, zs + 4);
    return minx >= -kGridHalf && maxx <= kGridHalf && minz >= -kGridHalf && maxz <= kGridHalf;
}

void footprint_triangles(const Footprint& fp, std::vector<TriRef>& out) {
    int tx0, tx1, tz0, tz1;
    footprint_tile_bounds(fp, 0, tx0, tx1, tz0, tz1);
    for (int tz = tz0; tz <= tz1; ++tz) {
        for (int tx = tx0; tx <= tx1; ++tx) {
            for (int c = 0; c < 4; ++c) {
                if (in_footprint(fp, triangle_centroid(tx, tz, c))) {
                    out.push_back({tx, tz, static_cast<uint32_t>(c)});
                }
            }
        }
    }
}

void margin_triangles(const Footprint& fp, std::vector<TriRef>& out) {
    int tx0, tx1, tz0, tz1;
    footprint_tile_bounds(fp, 1, tx0, tx1, tz0, tz1);
    for (int tz = tz0; tz <= tz1; ++tz) {
        for (int tx = tx0; tx <= tx1; ++tx) {
            for (int c = 0; c < 4; ++c) {
                glm::vec2 ct = triangle_centroid(tx, tz, c);
                if (!in_footprint(fp, ct) && footprint_l1_dist(fp, ct) <= kMarginRadius) {
                    out.push_back({tx, tz, static_cast<uint32_t>(c)});
                }
            }
        }
    }
}

bool placement_valid(const PlacementState& st, const Footprint& fp) {
    if (!footprint_in_bounds(fp)) {
        return false;
    }
    // The candidate footprint may not land on any existing footprint or margin.
    std::vector<TriRef> foot;
    footprint_triangles(fp, foot);
    for (const TriRef& t : foot) {
        if (st.blocked[tri_index(t.tx, t.tz, static_cast<int>(t.corner))]) {
            return false;
        }
    }
    // ...and the candidate's own blocking margin may not cover an existing
    // footprint (the reverse direction, which the discrete margins make a
    // genuinely separate test — placement is now order-independent).
    std::vector<TriRef> marg;
    margin_triangles(fp, marg);
    for (const TriRef& t : marg) {
        if (st.footprint[tri_index(t.tx, t.tz, static_cast<int>(t.corner))]) {
            return false;
        }
    }
    return true;
}

float poppable_score(const PlacementState& st, glm::vec2 candidate) {
    float d_castle = std::numeric_limits<float>::infinity();
    float d_apoth = std::numeric_limits<float>::infinity();
    for (const PlacedBuilding& b : st.buildings) {
        if (!b.alive) {
            continue;
        }
        float d = glm::distance(candidate, b.center);
        if (b.kind == GAME_BUILDING_CASTLE) {
            d_castle = std::min(d_castle, d);
        } else if (b.kind == GAME_BUILDING_APOTHECARY) {
            d_apoth = std::min(d_apoth, d);
        }
    }
    float score = 0.0f;
    if (std::isfinite(d_castle)) {
        score += 1.0f / (1.0f + d_castle);
    }
    if (std::isfinite(d_apoth)) {
        score += 1.0f / (1.0f + d_apoth);
    }
    return score;
}

namespace {

// Stamps a footprint + margin into the blocked grid and records the building.
uint32_t commit(PlacementState& st, int kind, int rot, glm::vec2 center) {
    Footprint fp = make_footprint(kind, rot, center);
    std::vector<TriRef> foot, marg;
    footprint_triangles(fp, foot);
    margin_triangles(fp, marg);
    for (const TriRef& t : foot) {
        int idx = tri_index(t.tx, t.tz, static_cast<int>(t.corner));
        st.footprint[idx] = 1;
        st.blocked[idx] = 1;
    }
    for (const TriRef& t : marg) {
        st.blocked[tri_index(t.tx, t.tz, static_cast<int>(t.corner))] = 1;
    }
    const GameBuildingDef& def = def_of(kind);
    uint32_t id = static_cast<uint32_t>(st.buildings.size());
    st.buildings.push_back({kind, center, rot, def.width_tiles, def.depth_tiles});
    ++st.nav_epoch;
    return id;
}

// The distinct rotations for a kind: square footprints only need 0/45.
int rotation_count(int kind) {
    const GameBuildingDef& def = def_of(kind);
    return (def.width_tiles == def.depth_tiles) ? 2 : 4;
}

constexpr float kPoppableRadius = 6.0f;

// Best valid poppable spot near `anchor`, scanning the snap lattice on a
// half-tile grid over all distinct rotations. Deterministic: rotation-major,
// then z, then x ascending, keeping the strictly-highest score.
bool best_poppable(const PlacementState& st, int kind, glm::vec2 anchor, glm::vec2& out_center,
                   int& out_rot) {
    bool found = false;
    float best = -std::numeric_limits<float>::infinity();
    int rots = rotation_count(kind);
    int lo_x = static_cast<int>(std::floor(anchor.x - kPoppableRadius));
    int hi_x = static_cast<int>(std::ceil(anchor.x + kPoppableRadius));
    int lo_z = static_cast<int>(std::floor(anchor.y - kPoppableRadius));
    int hi_z = static_cast<int>(std::ceil(anchor.y + kPoppableRadius));
    for (int rot = 0; rot < rots; ++rot) {
        for (int zi = lo_z * 2; zi <= hi_z * 2; ++zi) {
            for (int xi = lo_x * 2; xi <= hi_x * 2; ++xi) {
                glm::vec2 raw{xi * 0.5f, zi * 0.5f};
                glm::vec2 center = snap_center(kind, rot, raw);
                if (glm::distance(center, anchor) > kPoppableRadius) {
                    continue;
                }
                Footprint fp = make_footprint(kind, rot, center);
                if (!placement_valid(st, fp)) {
                    continue;
                }
                float score = poppable_score(st, center);
                if (score > best) {
                    best = score;
                    out_center = center;
                    out_rot = rot;
                    found = true;
                }
            }
        }
    }
    return found;
}

}  // namespace

void process_poppables(BadlandsGame& game, glm::vec2 anchor) {
    PlacementState& st = game.placement;
    uint32_t houses_owed = st.urban_quarters / 4;
    uint32_t sewers_owed = st.urban_quarters / 8;
    while (st.sewers_made < sewers_owed) {
        glm::vec2 center;
        int rot;
        if (!best_poppable(st, GAME_BUILDING_SEWER, anchor, center, rot)) {
            break;
        }
        notify_obstacle_added(game, commit(st, GAME_BUILDING_SEWER, rot, center));
        ++st.sewers_made;
    }
    while (st.houses_made < houses_owed) {
        glm::vec2 center;
        int rot;
        if (!best_poppable(st, GAME_BUILDING_HOUSE, anchor, center, rot)) {
            break;
        }
        notify_obstacle_added(game, commit(st, GAME_BUILDING_HOUSE, rot, center));
        ++st.houses_made;
    }
}

std::array<glm::vec2, 4> building_footprint_corners(const PlacedBuilding& b) {
    // Reuse the render-box geometry so the obstacle polygon matches what the
    // renderer draws (axis-aligned for rot 0/90, a 45 deg diamond otherwise).
    GameRenderBox rb = game_render_box(b.kind, b.rot);
    float hx = rb.size_x * 0.5f;
    float hz = rb.size_z * 0.5f;
    float c = std::cos(rb.yaw_radians);
    float s = std::sin(rb.yaw_radians);
    auto corner = [&](float lx, float lz) {
        return b.center + glm::vec2(c * lx - s * lz, s * lx + c * lz);
    };
    return {corner(-hx, -hz), corner(hx, -hz), corner(hx, hz), corner(-hx, hz)};
}

glm::vec2 building_entrance(const PlacedBuilding& b) {
    // Midpoint of the +Z (local) footprint edge, rotated into world space.
    GameRenderBox rb = game_render_box(b.kind, b.rot);
    float hz = rb.size_z * 0.5f;
    float c = std::cos(rb.yaw_radians);
    float s = std::sin(rb.yaw_radians);
    return b.center + glm::vec2(-s * hz, c * hz);
}

bool building_approach_tile(const PlacementState& st, const PlacedBuilding& b, glm::vec2& out) {
    glm::vec2 door = building_entrance(b);
    int reach = std::max(b.w, b.d) + 2;
    int cx = static_cast<int>(std::floor(b.center.x));
    int cz = static_cast<int>(std::floor(b.center.y));
    bool found = false;
    float best = std::numeric_limits<float>::infinity();
    for (int tz = cz - reach; tz <= cz + reach; ++tz) {
        for (int tx = cx - reach; tx <= cx + reach; ++tx) {
            if (!in_bounds_tile(tx, tz)) {
                continue;
            }
            bool free = true;
            for (int c = 0; c < 4; ++c) {
                if (st.blocked[tri_index(tx, tz, c)]) {
                    free = false;
                    break;
                }
            }
            if (!free) {
                continue;
            }
            glm::vec2 center{tx + 0.5f, tz + 0.5f};
            float d = glm::distance(center, door);
            if (d < best) {
                best = d;
                out = center;
                found = true;
            }
        }
    }
    return found;
}

uint32_t nearest_building_of(const PlacementState& st, int kind, glm::vec2 p) {
    uint32_t best = std::numeric_limits<uint32_t>::max();
    float bestd = std::numeric_limits<float>::infinity();
    for (uint32_t i = 0; i < st.buildings.size(); ++i) {
        const PlacedBuilding& b = st.buildings[i];
        if (!b.alive || b.kind != kind) {
            continue;
        }
        float d = glm::distance(p, b.center);
        if (d < bestd) {
            bestd = d;
            best = i;
        }
    }
    return best;
}

void rebuild_occupancy(PlacementState& st) {
    std::fill(st.blocked.begin(), st.blocked.end(), uint8_t{0});
    std::fill(st.footprint.begin(), st.footprint.end(), uint8_t{0});
    std::vector<TriRef> foot, marg;
    for (const PlacedBuilding& b : st.buildings) {
        if (!b.alive) {
            continue;
        }
        Footprint fp = make_footprint(b.kind, b.rot, b.center);
        foot.clear();
        marg.clear();
        footprint_triangles(fp, foot);
        margin_triangles(fp, marg);
        for (const TriRef& t : foot) {
            int idx = tri_index(t.tx, t.tz, static_cast<int>(t.corner));
            st.footprint[idx] = 1;
            st.blocked[idx] = 1;
        }
        for (const TriRef& t : marg) {
            st.blocked[tri_index(t.tx, t.tz, static_cast<int>(t.corner))] = 1;
        }
    }
}

void notify_obstacle_added(BadlandsGame& game, uint32_t building_id) {
    const GamePathfinder& pf = game.pathfinder;
    if (pf.add_obstacle == nullptr || building_id >= game.placement.buildings.size()) {
        return;
    }
    std::array<glm::vec2, 4> corners = building_footprint_corners(game.placement.buildings[building_id]);
    float flat[8];
    for (int i = 0; i < 4; ++i) {
        flat[i * 2] = corners[i].x;
        flat[i * 2 + 1] = corners[i].y;
    }
    pf.add_obstacle(pf.ctx, building_id, flat, 4);
}

uint32_t place_building(BadlandsGame& game, const GamePlacementDesc& desc, bool player) {
    PlacementState& st = game.placement;
    int rot = ((desc.rotation_index % 4) + 4) % 4;
    glm::vec2 center = snap_center(desc.kind, rot, {desc.world_x, desc.world_z});
    Footprint fp = make_footprint(desc.kind, rot, center);
    if (!placement_valid(st, fp)) {
        return std::numeric_limits<uint32_t>::max();
    }
    uint32_t id = commit(st, desc.kind, rot, center);
    notify_obstacle_added(game, id);
    if (player) {
        st.urban_quarters += urban_contribution(desc.kind);
        process_poppables(game, center);
    }
    return id;
}

}  // namespace badlands

using namespace badlands;

extern "C" {

GameBuildingDef game_building_def(int32_t kind) { return def_of(kind); }

GameRenderBox game_render_box(int32_t kind, int32_t rotation_index) {
    int rot = ((rotation_index % 4) + 4) % 4;
    const GameBuildingDef& def = def_of(kind);
    GameRenderBox box{};
    if (!is_diagonal(rot)) {
        // Axis-aligned: the box is the tile rectangle, rotated 0 or 90 degrees.
        box.size_x = static_cast<float>(def.width_tiles);
        box.size_z = static_cast<float>(def.depth_tiles);
        box.yaw_radians = rot * 0.7853981634f;  // 0 or 90 deg
    } else {
        // Diagonal: the footprint is a lattice diamond [p,q]x[r,s] in (u,v).
        // Its world sides have length span/sqrt2 along the u (x+z) and v (x-z)
        // diagonals. At a constant 45 deg yaw, local +X maps to the v diagonal
        // and local +Z to the u diagonal; the 45-vs-135 orientation is already
        // baked into diagonal_spans (which swaps su/sv for rot 135), so both
        // diagonal rotations render at 45 deg with swapped dims.
        constexpr float kInvSqrt2 = 0.70710678f;
        glm::ivec2 spans = diagonal_spans(def.width_tiles, def.depth_tiles, rot);
        box.size_x = spans.y * kInvSqrt2;  // sv/sqrt2 along v
        box.size_z = spans.x * kInvSqrt2;  // su/sqrt2 along u
        box.yaw_radians = 0.7853981634f;   // 45 deg
    }
    return box;
}

uint32_t game_place_building(BadlandsGame* game, const GamePlacementDesc* desc) {
    return place_building(*game, *desc, /*player=*/true);
}

uint32_t game_probe_placement(const BadlandsGame* game, const GamePlacementDesc* desc,
                              GamePlacementProbe* out, GameGridTriangle* out_triangles,
                              uint32_t cap) {
    const PlacementState& st = game->placement;
    int rot = ((desc->rotation_index % 4) + 4) % 4;
    glm::vec2 center = snap_center(desc->kind, rot, {desc->world_x, desc->world_z});
    Footprint fp = make_footprint(desc->kind, rot, center);

    out->valid = placement_valid(st, fp) ? 1u : 0u;
    out->snapped_x = center.x;
    out->snapped_z = center.y;

    int tx0, tx1, tz0, tz1;
    footprint_tile_bounds(fp, 2, tx0, tx1, tz0, tz1);
    uint32_t total = 0;
    for (int tz = tz0; tz <= tz1; ++tz) {
        for (int tx = tx0; tx <= tx1; ++tx) {
            for (int c = 0; c < 4; ++c) {
                uint32_t state;
                if (st.blocked[tri_index(tx, tz, c)]) {
                    state = 1;
                } else {
                    glm::vec2 ct = triangle_centroid(tx, tz, c);
                    bool would = in_footprint(fp, ct) || footprint_l1_dist(fp, ct) <= kMarginRadius;
                    state = would ? 2u : 0u;
                }
                if (total < cap) {
                    out_triangles[total] = GameGridTriangle{
                        .tile_x = tx,
                        .tile_z = tz,
                        .corner = static_cast<uint32_t>(c),
                        .state = state,
                    };
                }
                ++total;
            }
        }
    }
    return total;
}

uint32_t game_buildings(const BadlandsGame* game, GameBuildingState* out, uint32_t cap) {
    const PlacementState& st = game->placement;
    // Skip tombstoned buildings; each emitted row keeps its stable slot id, and
    // the return is the ALIVE count (count-return snapshot idiom).
    uint32_t alive = 0;
    for (uint32_t i = 0; i < st.buildings.size(); ++i) {
        const PlacedBuilding& b = st.buildings[i];
        if (!b.alive) {
            continue;
        }
        if (alive < cap) {
            out[alive] = GameBuildingState{
                .id = i,
                .kind = b.kind,
                .center_x = b.center.x,
                .center_z = b.center.y,
                .rotation_index = b.rot,
                .width_tiles = b.w,
                .depth_tiles = b.d,
            };
        }
        ++alive;
    }
    return alive;
}

void game_world(const BadlandsGame* game, GameWorldState* out) {
    const PlacementState& st = game->placement;
    uint32_t houses_owed = st.urban_quarters / 4;
    uint32_t sewers_owed = st.urban_quarters / 8;
    uint32_t queued = (houses_owed - st.houses_made) + (sewers_owed - st.sewers_made);
    *out = GameWorldState{
        .gold = game->gold,
        .grid_half_extent_tiles = kGridHalf,
        .queued_poppables = queued,
        .urban_quarters = st.urban_quarters,
    };
}

}  // extern "C"
