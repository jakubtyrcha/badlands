#include "movement.h"

#include "badlands_game.h"
#include "components.h"
#include "game_state.h"
#include "placement.h"

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <unordered_map>
#include <vector>

namespace badlands {

namespace {

constexpr float kArriveRadius = 0.25f;     // cursor advances within this of a waypoint
constexpr float kRepathCooldown = 0.3f;    // seconds between repaths
constexpr int kMaxWaypoints = 64;          // find_path output buffer
constexpr float kGoalMovedThreshold = 1.0f;// repath a moving target when it drifts this far
constexpr float kMeleeHysteresis = 1.15f;  // unlock past attack_range * this
constexpr float kSepCell = 2.0f;           // separation spatial-hash cell size
constexpr float kWorldBound = static_cast<float>(kGridHalf);

// Query the injected pathfinder, or a straight-line fallback if none. Returns
// the waypoint polyline (empty = no path).
std::vector<glm::vec2> query_path(BadlandsGame& game, glm::vec2 start, glm::vec2 goal, float radius,
                                  uint32_t exempt) {
    const GamePathfinder& pf = game.pathfinder;
    if (pf.find_path != nullptr) {
        std::array<float, 2 * kMaxWaypoints> buf{};
        int n = pf.find_path(pf.ctx, start.x, start.y, goal.x, goal.y, radius, exempt, buf.data(),
                             kMaxWaypoints);
        n = std::min(n, kMaxWaypoints);
        std::vector<glm::vec2> wp;
        wp.reserve(static_cast<size_t>(std::max(0, n)));
        for (int i = 0; i < n; ++i) {
            wp.push_back({buf[2 * i], buf[2 * i + 1]});
        }
        return wp;
    }
    // No provider: straight line (start included so follow_paths advances past it).
    return {start, goal};
}

// --- convex footprint point tests (for re-projection) ----------------------

bool point_in_convex(const std::array<glm::vec2, 4>& poly, glm::vec2 p) {
    // Consistent sign of the cross product across all edges => inside.
    bool has_pos = false, has_neg = false;
    for (int i = 0; i < 4; ++i) {
        glm::vec2 a = poly[i];
        glm::vec2 b = poly[(i + 1) % 4];
        float cross = (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
        if (cross > 1e-5f) {
            has_pos = true;
        } else if (cross < -1e-5f) {
            has_neg = true;
        }
    }
    return !(has_pos && has_neg);
}

// Nearest point on segment [a,b] to p.
glm::vec2 closest_on_segment(glm::vec2 a, glm::vec2 b, glm::vec2 p) {
    glm::vec2 ab = b - a;
    float len2 = glm::dot(ab, ab);
    if (len2 <= 1e-9f) {
        return a;
    }
    float t = glm::clamp(glm::dot(p - a, ab) / len2, 0.0f, 1.0f);
    return a + ab * t;
}

// Push a point that is inside any alive footprint out to the nearest boundary
// (plus a small margin), so a separated unit never rests inside a building.
void reproject_out_of_footprints(BadlandsGame& game, glm::vec2& p) {
    for (const PlacedBuilding& b : game.placement.buildings) {
        if (!b.alive) {
            continue;
        }
        std::array<glm::vec2, 4> poly = building_footprint_corners(b);
        if (!point_in_convex(poly, p)) {
            continue;
        }
        glm::vec2 best = p;
        float bestd = std::numeric_limits<float>::infinity();
        for (int i = 0; i < 4; ++i) {
            glm::vec2 q = closest_on_segment(poly[i], poly[(i + 1) % 4], p);
            float d = glm::distance(q, p);
            if (d < bestd) {
                bestd = d;
                best = q;
            }
        }
        glm::vec2 out = best - p;
        float len = glm::length(out);
        p = (len > 1e-5f) ? best + out / len * 0.01f : best;
    }
}

}  // namespace

void plan_paths(BadlandsGame& game, float dt) {
    entt::registry& reg = game.registry;
    auto view = reg.view<MoveTarget, NavPath, const Position, const Agent>(entt::exclude<InsideBuilding>);
    for (entt::entity e : view) {
        MoveTarget& mt = view.get<MoveTarget>(e);
        NavPath& np = view.get<NavPath>(e);
        glm::vec2 pos = view.get<const Position>(e).pos;
        float radius = view.get<const Agent>(e).radius;

        glm::vec2 goal{0.0f, 0.0f};
        uint32_t exempt = UINT32_MAX;
        bool have_goal = true;
        switch (mt.kind) {
            case MoveTarget::Kind::None:
                have_goal = false;
                break;
            case MoveTarget::Kind::Point:
                goal = mt.point;
                break;
            case MoveTarget::Kind::Entity:
                if (reg.valid(mt.entity) && reg.all_of<Position>(mt.entity)) {
                    goal = reg.get<Position>(mt.entity).pos;
                } else {
                    have_goal = false;  // target died / invalid
                }
                break;
            case MoveTarget::Kind::Building:
                if (mt.building < game.placement.buildings.size() &&
                    game.placement.buildings[mt.building].alive) {
                    goal = building_entrance(game.placement.buildings[mt.building]);
                    exempt = mt.building;
                } else {
                    have_goal = false;
                }
                break;
        }

        if (!have_goal || glm::distance(pos, goal) <= mt.stop_distance) {
            np.waypoints.clear();
            np.cursor = 0;
            continue;
        }

        np.repath_cooldown = std::max(0.0f, np.repath_cooldown - dt);
        bool goal_drifted = mt.kind != MoveTarget::Kind::Point && !np.waypoints.empty() &&
                            glm::distance(np.waypoints.back(), goal) > kGoalMovedThreshold;
        bool need = np.waypoints.empty() || np.cursor >= np.waypoints.size() ||
                    np.epoch != game.placement.nav_epoch || goal_drifted;
        if (need && np.repath_cooldown <= 0.0f) {
            np.waypoints = query_path(game, pos, goal, radius, exempt);
            np.cursor = 0;
            np.epoch = game.placement.nav_epoch;
            np.repath_cooldown = kRepathCooldown;
            np.blocked = np.waypoints.empty();
        }
    }
}

void follow_paths(BadlandsGame& game, float dt) {
    auto view = game.registry.view<NavPath, Position, const Stats>(
        entt::exclude<InsideBuilding, MeleeLock>);
    for (entt::entity e : view) {
        NavPath& np = view.get<NavPath>(e);
        Position& pos = view.get<Position>(e);
        float speed = view.get<const Stats>(e).move_speed;

        while (np.cursor < np.waypoints.size() &&
               glm::distance(pos.pos, np.waypoints[np.cursor]) <= kArriveRadius) {
            ++np.cursor;
        }
        if (np.cursor >= np.waypoints.size()) {
            continue;
        }
        glm::vec2 d = np.waypoints[np.cursor] - pos.pos;
        float len = glm::length(d);
        if (len > 0.0f) {
            pos.pos += d / len * std::min(len, speed * dt);
        }
    }
}

void update_melee_locks(BadlandsGame& game) {
    entt::registry& reg = game.registry;
    std::vector<entt::entity> to_lock, to_unlock;
    auto view = reg.view<const Position, const Stats>(entt::exclude<InsideBuilding>);
    for (entt::entity e : view) {
        bool locked = reg.all_of<MeleeLock>(e);
        entt::entity enemy = nearest_enemy(game, e);
        if (enemy == entt::null) {
            if (locked) {
                to_unlock.push_back(e);
            }
            continue;
        }
        float range = view.get<const Stats>(e).attack_range;
        float dist = glm::distance(view.get<const Position>(e).pos, reg.get<Position>(enemy).pos);
        if (!locked && dist <= range) {
            to_lock.push_back(e);
        } else if (locked && dist > range * kMeleeHysteresis) {
            to_unlock.push_back(e);
        }
    }
    for (entt::entity e : to_lock) {
        if (!reg.all_of<MeleeLock>(e)) {
            reg.emplace<MeleeLock>(e);
        }
    }
    for (entt::entity e : to_unlock) {
        if (reg.all_of<MeleeLock>(e)) {
            reg.remove<MeleeLock>(e);
        }
    }
}

void separate_units(BadlandsGame& game) {
    entt::registry& reg = game.registry;
    auto view = reg.view<Position, const Agent>(entt::exclude<InsideBuilding>);

    auto cell_of = [](glm::vec2 p) {
        return glm::ivec2(static_cast<int>(std::floor(p.x / kSepCell)),
                          static_cast<int>(std::floor(p.y / kSepCell)));
    };
    auto key = [](int cx, int cz) { return static_cast<int64_t>(cx) * 100000 + cz; };

    std::unordered_map<int64_t, std::vector<entt::entity>> grid;
    for (entt::entity e : view) {
        glm::ivec2 c = cell_of(view.get<Position>(e).pos);
        grid[key(c.x, c.y)].push_back(e);
    }

    std::unordered_map<entt::entity, glm::vec2> push;
    for (entt::entity e : view) {
        glm::vec2 pe = view.get<Position>(e).pos;
        float re = view.get<const Agent>(e).radius;
        bool e_locked = reg.all_of<MeleeLock>(e);
        glm::ivec2 c = cell_of(pe);
        for (int dz = -1; dz <= 1; ++dz) {
            for (int dx = -1; dx <= 1; ++dx) {
                auto it = grid.find(key(c.x + dx, c.y + dz));
                if (it == grid.end()) {
                    continue;
                }
                for (entt::entity o : it->second) {
                    if (o <= e) {
                        continue;  // each unordered pair once
                    }
                    glm::vec2 po = view.get<Position>(o).pos;
                    float ro = view.get<const Agent>(o).radius;
                    bool o_locked = reg.all_of<MeleeLock>(o);
                    glm::vec2 d = po - pe;
                    float dist = glm::length(d);
                    float mind = re + ro;
                    if (dist > mind) {
                        continue;
                    }
                    glm::vec2 dir = (dist > 1e-4f) ? d / dist : glm::vec2(1.0f, 0.0f);
                    float overlap = mind - std::max(dist, 0.0f);
                    if (e_locked && o_locked) {
                        continue;
                    } else if (e_locked) {
                        push[o] += dir * overlap;
                    } else if (o_locked) {
                        push[e] -= dir * overlap;
                    } else {
                        push[e] -= dir * overlap * 0.5f;
                        push[o] += dir * overlap * 0.5f;
                    }
                }
            }
        }
    }

    bool has_pf = game.pathfinder.find_path != nullptr;
    for (entt::entity e : view) {
        Position& pos = view.get<Position>(e);
        auto pit = push.find(e);
        if (pit != push.end()) {
            pos.pos += pit->second;
        }
        if (has_pf) {
            reproject_out_of_footprints(game, pos.pos);
        }
        pos.pos = glm::clamp(pos.pos, glm::vec2(-kWorldBound), glm::vec2(kWorldBound - 1e-3f));
    }
}

}  // namespace badlands
