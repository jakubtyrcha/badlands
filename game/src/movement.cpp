#include "movement.h"

#include "combat.h"  // melee_range, select_target
#include "components.h"
#include "game_state.h"
#include "heroes.h"     // biome_at
#include "intention.h"  // InboxEvent, push_inbox_event -- the MoveBlocked mirror
#include "placement.h"
#include "status.h"  // has_status/apply_status -- stun stops walking, disengaging costs actions
#include "skill_focus.h"  // focusing -- a long cast holds you still, like a wind-up
#include "strike.h"  // striking -- so does one committed to a swing

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace badlands {

namespace {

// kArriveRadius now lives in movement.h (shared with intention.cpp).
constexpr float kRepathCooldown = 0.3f;    // seconds between repaths
constexpr float kGoalMovedThreshold = 1.0f;// repath a moving target when it drifts this far
constexpr float kMeleeHysteresis = 1.15f;  // unlock past attack_range * this
// How long a unit that walked out of melee contact can do nothing at all.
// Long enough that disengaging is never the better option -- see the charge
// site in update_melee_locks.
constexpr float kDisengagePenaltySeconds = 3.0f;
constexpr float kSepCell = 2.0f;           // separation spatial-hash cell size
constexpr float kWorldBound = static_cast<float>(kGridHalf);

// Query the navmesh for a route, or a straight-line fallback when no navmesh has
// been built. Returns the waypoint polyline (empty = unreachable). The result
// distinguishes "no navmesh" (fallback, always a 2-point line) from "navmesh
// says unreachable" (empty) so the caller can raise MoveBlocked only for the
// latter.
std::vector<glm::vec2> query_path(BadlandsGame& game, glm::vec2 start, glm::vec2 goal,
                                  bool& unreachable, const glm::vec2* exempt_min = nullptr,
                                  const glm::vec2* exempt_max = nullptr) {
    unreachable = false;
    if (!game.navmesh.empty()) {
        // When entering a building, exempt its footprint's clearance so a door
        // sealed by the building's own dilation stays reachable (dense towns).
        const nav::NavMesh::PathResult r =
            (exempt_min != nullptr && exempt_max != nullptr)
                ? game.navmesh.FindPath(start, goal, *exempt_min, *exempt_max)
                : game.navmesh.FindPath(start, goal);
        if (!r.reachable) {
            unreachable = true;
            return {};
        }
        return r.waypoints;
    }
    // No navmesh built: straight-line fallback, obstacle-oblivious BY DESIGN.
    // Only headless mechanics tests that never rebuild the navmesh hit this; the
    // sim rebuilds it every tick (rebuild_navmesh_if_stale in step_world), so
    // shipping units always route around buildings + impassable terrain. (start
    // included so follow_paths advances past it.)
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

// Refused-step bookkeeping shared by plan_paths (goal unreachable) and
// follow_paths (a live step blocked): stamps MoveBlocked and mirrors the
// refusal into the inbox at this one site, so the two never drift apart
// (a guaranteed-wake event feeding should_wake, docs/design/
// intention-contract.html §2). No-ops for non-heroes (no EventInbox).
// Collapses what used to be two independent hand-copies of the same three
// lines, one per caller.
void note_move_blocked(BadlandsGame& game, entt::entity e, glm::vec2 point) {
    game.registry.emplace_or_replace<MoveBlocked>(e, point, game.world_ticks);
    InboxEvent ev;
    ev.kind = InboxEventKind::MoveBlocked;
    push_inbox_event(game, e, ev);
}

}  // namespace

bool is_walkable(mapgen::Biome biome) {
    // v1: open water is the only thing that stops anyone. Swamp and Mountain
    // stay passable until real terrain nav can express a cost rather than a
    // wall. Deliberately the crudest rule that makes the EVENT real, because
    // the event is what the AI contract is built on.
    return biome != mapgen::Biome::Lake;
}

void plan_paths(BadlandsGame& game) {
    entt::registry& reg = game.registry;
    // Goals the navmesh reported unreachable this pass -> MoveBlocked, applied
    // after the loop (emplacing a component while iterating a view can invalidate
    // it, same reason follow_paths defers its blocked list).
    std::vector<std::pair<entt::entity, glm::vec2>> blocked;

    // ChattingState excluded alongside InsideBuilding -- the MeleeLock
    // precedent (below, follow_paths): a conversation holds both
    // participants in place, the same way a melee lock holds fighters,
    // rather than just refusing to plan a route toward wherever they were
    // headed before the chat started.
    auto view =
        reg.view<MoveTarget, NavPath, const Position>(entt::exclude<InsideBuilding, ChattingState>);
    for (entt::entity e : view) {
        MoveTarget& mt = view.get<MoveTarget>(e);
        NavPath& np = view.get<NavPath>(e);
        glm::vec2 pos = view.get<const Position>(e).pos;

        glm::vec2 goal{0.0f, 0.0f};
        bool have_goal = true;
        glm::vec2 exempt_min{0.0f}, exempt_max{0.0f};
        bool have_exempt = false;
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
                    // The target building is an obstacle; its entrance sits on the
                    // perimeter. Exempt the building's own footprint bbox so its
                    // clearance ring never seals off its door.
                    const PlacedBuilding& b = game.placement.buildings[mt.building];
                    goal = building_entrance(b);
                    const std::array<glm::vec2, 4> corners = building_footprint_corners(b);
                    exempt_min = exempt_max = corners[0];
                    for (const glm::vec2& c : corners) {
                        exempt_min = glm::min(exempt_min, c);
                        exempt_max = glm::max(exempt_max, c);
                    }
                    have_exempt = true;
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

        np.repath_cooldown = std::max(0.0f, np.repath_cooldown - kSecondsPerStep);
        // Repath when the resolved goal has drifted from the planned route's end.
        // Applies to Kind::Point too: scripted pursuit re-issues enqueue_move_to
        // (a Point) at the target's fresh position every tick, so a moving Point
        // must re-plan or the pursuer trails one route-length behind. A static
        // Point (a committed move order) has back()==goal, so it never repaths.
        bool goal_drifted = !np.waypoints.empty() &&
                            glm::distance(np.waypoints.back(), goal) > kGoalMovedThreshold;
        bool need = np.waypoints.empty() || np.cursor >= np.waypoints.size() ||
                    np.epoch != game.placement.nav_epoch || goal_drifted;
        if (need && np.repath_cooldown <= 0.0f) {
            bool unreachable = false;
            np.waypoints = query_path(game, pos, goal, unreachable,
                                      have_exempt ? &exempt_min : nullptr,
                                      have_exempt ? &exempt_max : nullptr);
            np.cursor = 0;
            np.epoch = game.placement.nav_epoch;
            np.repath_cooldown = kRepathCooldown;
            // The world says the goal cannot be reached. Raise the event the
            // brain reacts to (abandon the goal) instead of stalling silently.
            if (unreachable) {
                blocked.emplace_back(e, goal);
            }
        }
    }

    for (const auto& [e, point] : blocked) {
        note_move_blocked(game, e, point);
    }
}

void follow_paths(BadlandsGame& game) {
    // Steps refused this tick. Collected rather than emplaced inline, because
    // adding a component while iterating a view can invalidate it.
    std::vector<std::pair<entt::entity, glm::vec2>> blocked;

    // Who moved under their own power this tick. update_melee_locks (below,
    // same tick, after this) reads it to tell a DELIBERATE disengage from
    // simply being left behind -- see its own comment.
    game.moved_by_path_scratch.assign(game.slots.size(), glm::vec2{0.0f, 0.0f});

    // MeleeLock is NO LONGER excluded (contact/disengage slice): contact
    // forbids ranged attacks, it does not nail feet to the ground. Freezing
    // movement made leaving contact impossible -- you could not move, so the
    // distance never grew, so the lock never released -- which made "move,
    // shoot, move" unreachable for anything that was touched once. Leaving is
    // possible now, and ruinously expensive (StatusKind::Disengaged).
    //
    // ChattingState stays excluded: a conversation really does hold both
    // participants in place, so a hero mid-chat must not keep walking toward
    // whatever MoveTarget it had queued up before the chat started.
    auto view = game.registry.view<NavPath, Position, const Stats>(
        entt::exclude<InsideBuilding, ChattingState>);
    for (entt::entity e : view) {
        // Stunned: the character stops WALKING, but its NavPath is left exactly
        // as it is -- the route survives the stun and resumes on expiry, which
        // is the difference between being interrupted and being reset. Checked
        // here rather than as a view exclusion because a status is a timer on
        // a component, not a component of its own (game/src/status.h).
        //
        // Deliberately NOT gated in separate_units below: collision resolution
        // still nudges a stunned body, so units cannot stack on top of one.
        if (has_status(game.registry, e, StatusKind::Stunned)) {
            continue;
        }
        // Committed to a swing: the same "stops walking, keeps its route"
        // treatment, for the same reason. This is what makes standing still to
        // shoot a real cost -- a kiter's effective retreat speed is its speed
        // times the fraction of its cycle it is NOT committed.
        if (striking(game.registry, e)) {
            continue;
        }
        // ...and the same for a long cast (game/src/skill_focus.h). Standing
        // still for two seconds is what a focused shot COSTS; a caster that
        // could walk through it would be paying nothing for the guarantee.
        if (focusing(game.registry, e)) {
            continue;
        }
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
            const glm::vec2 next = pos.pos + d / len * std::min(len, speed * kSecondsPerStep);
            // The world gets the last word on where a character can go. A path
            // may cross terrain nobody has surveyed -- the planner routes around
            // buildings only -- so the refusal happens HERE, at the step, and
            // becomes an event the brain can act on.
            //
            // Terrain is the only thing that refuses a STEP. A building does
            // not: the planner already routes around its footprint, and
            // separate_units reprojects a body that ends up inside one anyway.
            if (game.terrain_blocking && !is_walkable(biome_at(game, next))) {
                blocked.emplace_back(e, next);
                continue;  // stop at the edge rather than cross it
            }
            if (const uint32_t slot = slot_for_entity(game, e);
                slot < game.moved_by_path_scratch.size()) {
                game.moved_by_path_scratch[slot] += next - pos.pos;  // its own power
            }
            pos.pos = next;
            // Fog-of-war: face the direction of travel (idle keeps last facing).
            if (Facing* f = game.registry.try_get<Facing>(e)) {
                f->dir = d / len;
            }
        }
    }

    for (const auto& [e, point] : blocked) {
        note_move_blocked(game, e, point);
    }
}

void update_melee_locks(BadlandsGame& game) {
    entt::registry& reg = game.registry;
    std::vector<entt::entity> to_lock;
    // The entity being unlocked, and WHO it was locked with (null when the
    // opponent is simply gone) -- the disengage charge below needs both.
    std::vector<std::pair<entt::entity, entt::entity>> to_unlock;
    // Melee lock keys off the MELEE attack's reach, not a ranged one: a unit that
    // only fights at range (melee_range == 0) never locks, so a kiter holding at
    // bow distance keeps moving instead of freezing.
    auto view = reg.view<const Position, const Attacks>(entt::exclude<InsideBuilding>);
    for (entt::entity e : view) {
        bool locked = reg.all_of<MeleeLock>(e);
        entt::entity enemy = select_target(game, e);
        const float range = melee_range(view.get<const Attacks>(e));
        if (enemy == entt::null || range <= 0.0f) {
            if (locked) {
                // No opponent left to have walked away FROM: unlock, charge
                // nothing. It died, or left view -- neither is a disengage.
                to_unlock.emplace_back(e, entt::null);
            }
            continue;
        }
        float dist = glm::distance(view.get<const Position>(e).pos, reg.get<Position>(enemy).pos);
        if (!locked && dist <= range) {
            to_lock.push_back(e);
        } else if (locked && dist > range * kMeleeHysteresis) {
            to_unlock.emplace_back(e, enemy);
        }
    }
    for (entt::entity e : to_lock) {
        if (!reg.all_of<MeleeLock>(e)) {
            reg.emplace<MeleeLock>(e);
        }
    }
    for (const auto& [e, opponent] : to_unlock) {
        if (!reg.all_of<MeleeLock>(e)) {
            continue;
        }
        reg.remove<MeleeLock>(e);
        // The price of LEAVING contact -- and only leaving it.
        //
        // Charged when this unit's own path movement carried it AWAY from the
        // opponent it was locked with. Three things therefore pay nothing, and
        // each matters: a unit whose opponent walked off or died (no opponent
        // to test against); a unit that was merely nudged apart by
        // separate_units (not path movement, so no displacement is recorded);
        // and -- the case a bare "did it move" flag gets wrong -- a PURSUER.
        // A mercenary chasing a retreating archer moves every tick and the gap
        // still opens past the hysteresis; punishing it for closing would
        // invert the whole mechanic.
        //
        // Deliberately steep, so a brain that can count never chooses it: the
        // ranged classes' real job is spacing, and being caught is meant to be
        // a genuine loss condition rather than an inconvenience to walk off.
        if (opponent == entt::null || !reg.all_of<Position>(opponent)) {
            continue;
        }
        const uint32_t slot = slot_for_entity(game, e);
        if (slot >= game.moved_by_path_scratch.size()) {
            continue;
        }
        const glm::vec2 step = game.moved_by_path_scratch[slot];
        const glm::vec2 away = view.get<const Position>(e).pos - reg.get<Position>(opponent).pos;
        if (glm::dot(step, step) <= 0.0f || glm::dot(step, away) <= 0.0f) {
            continue;  // did not move, or moved TOWARD it
        }
        apply_status(game, e, StatusKind::Disengaged,
                     ticks_of(kDisengagePenaltySeconds), slot);
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

    // Reproject units out of building footprints exactly when the path layer is
    // obstacle-aware -- i.e. a navmesh exists. Gating on the same signal as
    // plan_paths/query_path (navmesh presence, not terrain_blocking) keeps the
    // two consistent: a flat world with no navmesh skips both routing and
    // reprojection, and any world with a built mesh gets both.
    const bool reproject = !game.navmesh.empty();
    for (entt::entity e : view) {
        Position& pos = view.get<Position>(e);
        // Locked units are immovable colliders: they are never *pushed* by
        // separation. They are still reprojected out of footprints and clamped
        // to the world bound, so a unit that locks while overlapping a building
        // or past the edge is corrected instead of left embedded.
        if (!reg.all_of<MeleeLock>(e)) {
            auto pit = push.find(e);
            if (pit != push.end()) {
                pos.pos += pit->second;
            }
        }
        if (reproject) {
            reproject_out_of_footprints(game, pos.pos);
        }
        pos.pos = glm::clamp(pos.pos, glm::vec2(-kWorldBound), glm::vec2(kWorldBound - 1e-3f));
    }
}

}  // namespace badlands
