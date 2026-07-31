#include "duel_matrix.h"

#include "components.h"
#include "game_state.h"
#include "progression.h"
#include "sim_internal.hpp"

#include "game/brain_asset.hpp"  // LoadBrainWasm -- heroes have no decision layer without it

#include <cstdio>

#include <algorithm>

namespace duelsim {

namespace {

using namespace badlands;

constexpr float kDt = 1.0f / 30.0f;

// The shipping hero brain, read once and shared by every duel in the run.
//
// LOAD-BEARING, not a nicety: the wasm module is the ONLY hero decision layer
// (game/src/wasm_brain.h), so a world built without it has heroes that issue
// no intentions and no actions at all -- they stand still and are killed by
// anything with a brain. The first version of this tool omitted it and every
// hero row of the matrix came back 0%, which is exactly the class of bug the
// matrix exists to surface.
const std::vector<uint8_t>& hero_wasm() {
    static const std::vector<uint8_t> kBytes = [] {
        std::vector<uint8_t> b = LoadBrainWasm(kHeroBrainPath);
        if (b.empty()) {
            std::fprintf(stderr,
                         "duelsim: no hero brain at %s -- every hero would idle and lose "
                         "every duel. Run from the repo root, with git-lfs fetched.\n",
                         kHeroBrainPath);
        }
        return b;
    }();
    return kBytes;
}

BrainDesc brain_desc() {
    BrainDesc d;
    const std::vector<uint8_t>& b = hero_wasm();
    if (!b.empty()) {
        d.wasm_bytes = b.data();
        d.wasm_len = b.size();
    }
    return d;
}

// Set a spawned creature to `level` and rescale it. Heroes carry the level on
// HeroSimulationState; anything else has no level of its own, so only the stat
// recompute applies (and a monster's zeroed growth makes that a no-op).
void set_level(BadlandsGame& g, entt::entity e, int32_t level) {
    if (auto* hs = g.registry.try_get<HeroSimulationState>(e); hs != nullptr) {
        hs->level = std::max(1, level);
    }
    apply_level_stats(g.registry, e, level);
}

// Alive combatants per team. A duel is over when one side has none left.
void team_counts(const BadlandsGame& g, int32_t& left_alive, int32_t& right_alive) {
    left_alive = 0;
    right_alive = 0;
    for (auto [e, team, health] : g.registry.view<const Team, const Health>().each()) {
        if (health.hp <= 0.0f) {
            continue;
        }
        (team.id == 0 ? left_alive : right_alive) += 1;
    }
}

}  // namespace

DuelOutcome run_duel(CreatureId left, CreatureId right, int32_t level, float separation,
                     int32_t max_ticks) {
    // A walled arena wide enough that a kiter has somewhere to retreat TO --
    // confining it tighter would make the standoff behaviour untestable by
    // pinning it against a wall, which is a property of the arena rather than
    // of the classes.
    WorldConfig cfg;
    cfg.prebuild_colony = false;
    cfg.terrain_blocking = false;
    cfg.arena_half_x = 60.0f;
    cfg.arena_half_z = 20.0f;
    auto owned = make_world(brain_desc(), cfg);
    BadlandsGame& g = *owned;

    const float half = separation * 0.5f;
    const uint32_t ls = spawn_creature_into(g, left, /*team=*/0, {-half, 0.0f});
    const uint32_t rs = spawn_creature_into(g, right, /*team=*/1, {half, 0.0f});
    set_level(g, g.slots[ls], level);
    set_level(g, g.slots[rs], level);

    DuelOutcome out;
    for (int32_t t = 1; t <= max_ticks; ++t) {
        tick_world(g, kDt);
        int32_t la = 0, ra = 0;
        team_counts(g, la, ra);
        if (la > 0 && ra > 0) {
            continue;
        }
        out.ticks = t;
        if (la > 0) {
            out.winner_index = 0;
        } else if (ra > 0) {
            out.winner_index = 1;
        }
        return out;  // one side (or neither) left standing
    }
    out.ticks = max_ticks;
    return out;  // timed out: a draw, and reported as one
}

std::vector<std::vector<MatrixCell>> run_matrix(const std::vector<CreatureId>& roster,
                                                int32_t level, int32_t samples) {
    const size_t n = roster.size();
    std::vector<std::vector<MatrixCell>> m(n, std::vector<MatrixCell>(n));
    const int32_t k = std::max(1, samples);

    for (size_t i = 0; i < n; ++i) {
        for (size_t j = 0; j < n; ++j) {
            std::vector<int32_t> tick_counts;
            tick_counts.reserve(static_cast<size_t>(k));
            for (int32_t s = 0; s < k; ++s) {
                // Separations spread over a band a hero can actually cross.
                // Each one is a different closing time and so a different roll
                // stream -- the "samples" axis is only meaningful because of
                // that, which is why it varies the SETUP rather than a seed.
                const float sep = 6.0f + static_cast<float>(s) * 2.0f;
                const DuelOutcome o = run_duel(roster[i], roster[j], level, sep);
                tick_counts.push_back(o.ticks);
                if (o.winner_index == 0) {
                    ++m[i][j].wins;
                } else if (o.winner_index == 1) {
                    ++m[i][j].losses;
                } else {
                    ++m[i][j].draws;
                }
            }
            std::sort(tick_counts.begin(), tick_counts.end());
            m[i][j].median_ticks = tick_counts[tick_counts.size() / 2];
        }
    }
    return m;
}

LevelStats stats_at_level(CreatureId creature, int32_t level) {
    WorldConfig cfg;
    cfg.prebuild_colony = false;
    cfg.terrain_blocking = false;
    auto owned = make_world(brain_desc(), cfg);
    BadlandsGame& g = *owned;
    const uint32_t slot = spawn_creature_into(g, creature, 0, {0.0f, 0.0f});
    entt::entity e = g.slots[slot];
    set_level(g, e, level);

    LevelStats s;
    s.max_hp = g.registry.get<Health>(e).max_hp;
    s.armour = g.registry.get<Combatant>(e).armour;
    if (const auto& atk = g.registry.get<Attacks>(e); atk.count > 0) {
        s.damage = atk.defs[0].base_damage;
    }
    return s;
}

}  // namespace duelsim
