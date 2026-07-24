#include "entity_memory.h"

#include "badlands_sim.hpp"  // Archetype
#include "components.h"      // archetype_of
#include "game_state.h"
#include "placement.h"

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <optional>
#include <vector>

namespace badlands {

namespace {

// Refreshes (or inserts) `slot`'s entry as seen right now. On a full array,
// evicts the oldest-seen entry (ties -> largest slot) -- EXCEPT: a visible
// entry's last_seen_millis == `now`, the maximum possible value this tick, so
// the oldest entry can only be visible_now if EVERY entry currently is. In
// that case the newcomer is dropped rather than bumping someone mid-sighting
// (attention is full) -- see entity_memory.h / the task brief for why this is
// the simple, deterministic choice over any "closer wins" heuristic.
void upsert_char(EntityMemory& mem, uint32_t slot, int32_t archetype, int32_t team,
                  glm::vec2 pos, float hp, int64_t now) {
    for (int32_t i = 0; i < mem.char_count; ++i) {
        if (mem.chars[i].slot == slot) {
            MemoryChar& c = mem.chars[i];
            c.archetype = archetype;
            c.team = team;
            c.last_pos = pos;
            c.last_hp = hp;
            c.visible_now = true;
            c.last_seen_millis = now;
            return;
        }
    }

    MemoryChar fresh{};
    fresh.slot = slot;
    fresh.archetype = archetype;
    fresh.team = team;
    fresh.last_pos = pos;
    fresh.last_hp = hp;
    fresh.visible_now = true;
    fresh.last_seen_millis = now;

    if (mem.char_count < BL_MAX_CHARS) {
        mem.chars[mem.char_count++] = fresh;
        return;
    }
    int32_t oldest = 0;
    for (int32_t i = 1; i < mem.char_count; ++i) {
        const MemoryChar& c = mem.chars[i];
        const MemoryChar& best = mem.chars[oldest];
        if (c.last_seen_millis < best.last_seen_millis ||
            (c.last_seen_millis == best.last_seen_millis && c.slot > best.slot)) {
            oldest = i;
        }
    }
    if (mem.chars[oldest].visible_now) {
        return;  // every slot is watching someone right now; drop the newcomer
    }
    mem.chars[oldest] = fresh;
}

// Same shape as upsert_char, minus the "drop when full" clause: buildings
// have no visible_now to protect, so a full array simply loses its
// oldest-seen entry to the newcomer (ties -> largest id). Buildings never
// TTL-expire, so this is the only way a building entry is ever removed.
//
// `is_home` has TWO different update rules, chosen by `authoritative`
// (whether the OBSERVER carries HeroSimulationState -- see
// update_entity_memory's call site):
//  - Hero observers (authoritative=true): HeroSimulationState::
//    home_building_id is a LIVE, correctable signal -- raze_building's
//    resident-reassignment loop (heroes.cpp) rehomes a hero whose guild was
//    destroyed, and a razed building stays in placement.buildings
//    (alive=false) and can still be re-observed. So `is_home` is
//    OVERWRITTEN with the fresh derivation every sighting, exactly like
//    every other field here: a hero re-observing the ruins of its OLD home
//    must see that entry's is_home flip to false once it's no longer home.
//  - Non-hero observers (authoritative=false: tax collectors etc.): this
//    pass has no live per-tick home signal for them at all (it only ever
//    derives `is_home` from HeroSimulationState) -- their only source of
//    truth is seed_home_town_memory's spawn-time seeding, a ONE-SHOT
//    identity fact ("this is where I started"), not something re-derivable
//    here. So their `is_home` is OR'd onto whatever is already recorded,
//    never overwritten back down to false.
void upsert_building(EntityMemory& mem, uint32_t id, int32_t kind, glm::vec2 door, bool alive,
                     bool is_home, bool authoritative, int64_t now) {
    for (int32_t i = 0; i < mem.building_count; ++i) {
        if (mem.buildings[i].id == id) {
            MemoryBuilding& b = mem.buildings[i];
            b.kind = kind;
            b.door = door;
            b.alive = alive;
            b.is_home = authoritative ? is_home : (b.is_home || is_home);
            b.last_seen_millis = now;
            return;
        }
    }

    MemoryBuilding fresh{};
    fresh.id = id;
    fresh.kind = kind;
    fresh.door = door;
    fresh.alive = alive;
    fresh.is_home = is_home;
    fresh.last_seen_millis = now;

    if (mem.building_count < kMemoryMaxBuildings) {
        mem.buildings[mem.building_count++] = fresh;
        return;
    }
    int32_t oldest = 0;
    for (int32_t i = 1; i < mem.building_count; ++i) {
        const MemoryBuilding& c = mem.buildings[i];
        const MemoryBuilding& best = mem.buildings[oldest];
        if (c.last_seen_millis < best.last_seen_millis ||
            (c.last_seen_millis == best.last_seen_millis && c.id > best.id)) {
            oldest = i;
        }
    }
    mem.buildings[oldest] = fresh;
}

// Per-call memo of building_approach_tile results, indexed by building id:
// every observer in a single update_entity_memory call sweeps the SAME
// buildings list, so without this a town with H heroes and B buildings pays
// building_approach_tile's nav-grid probe up to H*B times a tick instead of
// B. Lazily computed on first request and reused for the rest of this call
// only -- no cross-tick caching (YAGNI: placement changes mid-run would need
// explicit invalidation this pass doesn't do, and update_entity_memory is
// re-entered fresh every tick anyway). A lookup that failed (no approach
// tile found) is deliberately NOT distinguished from "not yet looked up" --
// both read as nullopt, so a failing building is simply re-probed by the
// next observer that asks; that miss path is not what this memo targets
// (buildings without a free approach tile are the rare case).
class ApproachTileMemo {
public:
    explicit ApproachTileMemo(const PlacementState& placement) : placement_(placement) {}

    std::optional<glm::vec2> get(uint32_t building_id, const PlacedBuilding& b) {
        if (building_id >= cache_.size()) {
            cache_.resize(building_id + 1);
        }
        std::optional<glm::vec2>& slot = cache_[building_id];
        if (!slot.has_value()) {
            glm::vec2 door;
            if (building_approach_tile(placement_, b, door)) {
                slot = door;
            }
        }
        return slot;
    }

private:
    const PlacementState& placement_;
    std::vector<std::optional<glm::vec2>> cache_;
};

}  // namespace

void update_entity_memory(BadlandsGame& game) {
    entt::registry& reg = game.registry;
    const int64_t now = game.world_millis;
    const int64_t ttl = game.factors.hero.memory_ttl_millis;
    const auto& buildings = game.placement.buildings;
    ApproachTileMemo approach_tile(game.placement);

    // Observers by slot index ascending: slots are never reused (game_state.h),
    // so this order is stable across identical runs.
    for (uint32_t slot = 0; slot < game.slots.size(); ++slot) {
        entt::entity obs = game.slots[slot];
        if (!reg.valid(obs)) {
            continue;
        }
        EntityMemory* mem = reg.try_get<EntityMemory>(obs);
        const Position* obs_pos = reg.try_get<Position>(obs);
        if (mem == nullptr || obs_pos == nullptr) {
            continue;  // not an observer (no memory sandbox, or no position)
        }

        // Every existing char entry starts this tick's pass presumed unseen;
        // the scan below flips back to true whatever is actually in radius.
        // Stale entries keep their last_* values untouched -- that IS the
        // memory (rule 3 in the task brief).
        for (int32_t i = 0; i < mem->char_count; ++i) {
            mem->chars[i].visible_now = false;
        }

        // A hidden hero sees nothing this tick (entries still age below).
        // v1 is RADIUS-ONLY -- Vision::cone_half_cos is deliberately unused
        // here: this pass is the host's knowledge-of-the-world, at parity
        // with the radius-based scans the C++ brains already run for
        // perception (nearest_enemy, hunt, chat...), not the player's
        // rendered fog-of-war cone (vision.cpp), which is a later, separate
        // knowledge-limiting step layered on top of the same kind of scan.
        const bool inside = reg.all_of<InsideBuilding>(obs);
        const Vision* vision = reg.try_get<Vision>(obs);
        const float radius = (!inside && vision != nullptr) ? vision->radius : 0.0f;

        if (radius > 0.0f) {
            // Candidate targets by slot index ascending.
            for (uint32_t t_slot = 0; t_slot < game.slots.size(); ++t_slot) {
                if (t_slot == slot) {
                    continue;
                }
                entt::entity target = game.slots[t_slot];
                if (!reg.valid(target) || reg.all_of<InsideBuilding>(target)) {
                    continue;
                }
                const Position* tpos = reg.try_get<Position>(target);
                if (tpos == nullptr || glm::distance(obs_pos->pos, tpos->pos) > radius) {
                    continue;
                }
                upsert_char(*mem, t_slot, static_cast<int32_t>(archetype_of(reg, target)),
                           reg.get<Team>(target).id, tpos->pos, reg.get<Health>(target).hp, now);
            }
        }

        // TTL: any char entry not refreshed above ages out once too stale.
        // A visible entry has last_seen_millis == now, so it never qualifies
        // here. Dense swap-remove -- array order is not part of the contract.
        for (int32_t i = 0; i < mem->char_count;) {
            if (now - mem->chars[i].last_seen_millis > ttl) {
                mem->chars[i] = mem->chars[mem->char_count - 1];
                --mem->char_count;
            } else {
                ++i;
            }
        }

        // Buildings: never TTL-expire, so this is purely an upsert pass,
        // gated by the same "sees nothing" radius check as characters above.
        if (radius > 0.0f) {
            // Only a hero's home signal (HeroSimulationState::home_building_id)
            // is live/correctable -- see upsert_building's doc comment for why
            // that gates OVERWRITE vs OR below.
            const auto* home = reg.try_get<HeroSimulationState>(obs);
            const bool authoritative_home = home != nullptr;
            const int32_t home_id = home != nullptr ? home->home_building_id : -1;
            for (uint32_t bid = 0; bid < buildings.size(); ++bid) {
                const PlacedBuilding& b = buildings[bid];
                if (glm::distance(obs_pos->pos, b.center) > radius) {
                    continue;
                }
                const std::optional<glm::vec2> door = approach_tile.get(bid, b);
                if (!door.has_value()) {
                    continue;  // no approach tile this tick: skip the record
                }
                const bool is_home = home_id >= 0 && static_cast<uint32_t>(home_id) == bid;
                upsert_building(*mem, bid, b.kind, *door, b.alive, is_home, authoritative_home, now);
            }
        }
    }
}

void seed_home_town_memory(BadlandsGame& game, EntityMemory& mem, uint32_t home_building_id) {
    const auto& buildings = game.placement.buildings;
    if (home_building_id >= buildings.size()) {
        return;  // defensive; callers only pass a just-validated building id
    }

    // Append-only fill, oldest-first by id (home first, unconditionally) --
    // simply stops once kMemoryMaxBuildings is reached. Unlike the update
    // pass's upsert_building, seeding never needs to evict: it runs once,
    // against a list it can size itself against.
    auto insert = [&](uint32_t id, bool is_home) {
        if (mem.building_count >= kMemoryMaxBuildings) {
            return false;  // full: stop
        }
        const PlacedBuilding& b = buildings[id];
        glm::vec2 door;
        if (!building_approach_tile(game.placement, b, door)) {
            return true;  // skip the record (matches the update pass's rule), keep going
        }
        MemoryBuilding rec{};
        rec.id = id;
        rec.kind = b.kind;
        rec.door = door;
        rec.alive = b.alive;
        rec.is_home = is_home;
        rec.last_seen_millis = game.world_millis;
        mem.buildings[mem.building_count++] = rec;
        return true;
    };

    if (!insert(home_building_id, /*is_home=*/true)) {
        return;
    }
    for (uint32_t id = 0; id < buildings.size(); ++id) {
        if (id == home_building_id || !buildings[id].alive) {
            continue;
        }
        if (!insert(id, /*is_home=*/false)) {
            break;
        }
    }
}

}  // namespace badlands
