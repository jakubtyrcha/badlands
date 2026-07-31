#include "executables/ai_sandbox/duel_mode.hpp"

#include <cstdio>

#include <spdlog/spdlog.h>

namespace badlands {

namespace {

// splitmix64. A tiny, well-mixed, self-contained bit hash: the sampler must be
// a PURE function of (seed, round) so a session replays, which rules out any
// stateful generator whose sequence depends on how many draws came before.
uint64_t mix(uint64_t x) {
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

uint64_t draw(uint64_t seed, uint32_t round, uint32_t axis) {
    return mix(mix(seed ^ (static_cast<uint64_t>(round) << 32)) + axis);
}

float seconds_of(int64_t millis) { return static_cast<float>(millis) / 1000.0f; }

}  // namespace

std::vector<CreatureId> duel_pool(const CreatureCatalog& catalog) {
    std::vector<CreatureId> pool;
    for (int i = 0; i < kCreatureCount; ++i) {
        const CharacterDesc& d = catalog.defs[i];
        if (d.archetype == Archetype::Critter) {
            continue;  // never counts as a combatant, so a round with one never ends
        }
        // attack_count == 0 is not "unarmed": the spawn path derives a single
        // melee attack from the legacy attack_* fields, so the real test is
        // whether either source yields a swing.
        const bool armed = d.attack_count > 0 || d.attack_damage > 0.0f;
        if (!armed) {
            continue;
        }
        pool.push_back(static_cast<CreatureId>(i));
    }
    return pool;
}

DuelSetup sample_duel(const DuelConfig& cfg, const std::vector<CreatureId>& pool,
                      uint32_t round) {
    DuelSetup s;
    if (pool.empty()) {
        return s;
    }
    const uint64_t n = pool.size();
    s.shape = static_cast<ArenaShape>(draw(cfg.seed, round, 0) %
                                      static_cast<uint64_t>(ArenaShape::Count));
    s.left = pool[draw(cfg.seed, round, 1) % n];
    // Mirror matches are left in deliberately: a creature fighting itself is
    // the one pairing whose expected result is known, so a lopsided one says
    // something about the mechanics rather than about the matchup.
    s.right = pool[draw(cfg.seed, round, 2) % n];
    return s;
}

DuelTally tally_duel(const std::vector<CharacterState>& rows) {
    DuelTally t;
    for (const CharacterState& r : rows) {
        if (r.archetype == static_cast<int32_t>(Archetype::Critter)) {
            continue;  // neutral wildlife is not a side
        }
        if (r.team == 0) {
            t.left_alive = true;
        } else if (r.team == 1) {
            t.right_alive = true;
        }
    }
    return t;
}

WorldConfig DuelMode::Configure() {
    if (pool_.empty()) {
        // The compiled catalog, not the Sim's: the pool is about WHICH
        // creatures exist, and a stat override file cannot change that. Reading
        // it here also breaks the ordering knot -- Configure runs before there
        // is a Sim to ask.
        pool_ = duel_pool(DefaultCreatureCatalog());
    }
    setup_ = sample_duel(cfg_, pool_, round_);
    layout_ = build_arena(setup_.shape);

    WorldConfig cfg;
    cfg.prebuild_colony = false;
    cfg.map = MapKind::FlatPlains;
    // Flat plains stops nobody, so this costs no movement rule -- it is what
    // gets the navmesh BUILT, which is how the walls obstruct at all.
    cfg.terrain_blocking = true;
    cfg.plops = layout_.plops;
    return cfg;
}

void DuelMode::Stage(Sim& sim) {
    sim.SpawnCreature(setup_.left, /*team=*/0, layout_.spawn_a.x, layout_.spawn_a.y);
    sim.SpawnCreature(setup_.right, /*team=*/1, layout_.spawn_b.x, layout_.spawn_b.y);
    started_millis_ = sim.World().world_millis;
    report_at_millis_ = 0;
    decided_ = false;
}

bool DuelMode::Observe(const std::vector<CharacterState>& rows, int64_t world_millis) {
    const int64_t elapsed = world_millis - started_millis_;
    if (!decided_) {
        const DuelTally t = tally_duel(rows);
        const bool over = !t.left_alive || !t.right_alive;
        const bool timed_out = elapsed >= cfg_.max_millis;
        if (!over && !timed_out) {
            return false;
        }
        decided_ = true;
        char line[256];
        if (over && t.left_alive) {
            std::snprintf(line, sizeof(line), "%s beats %s on %s in %.1fs",
                          CreatureName(setup_.left), CreatureName(setup_.right),
                          arena_shape_name(setup_.shape), seconds_of(elapsed));
        } else if (over && t.right_alive) {
            std::snprintf(line, sizeof(line), "%s beats %s on %s in %.1fs",
                          CreatureName(setup_.right), CreatureName(setup_.left),
                          arena_shape_name(setup_.shape), seconds_of(elapsed));
        } else {
            // Both gone, or neither: a stalemate and a mutual kill are both
            // draws, and both are worth being able to see in the log.
            std::snprintf(line, sizeof(line), "%s vs %s on %s -> draw (%s) in %.1fs",
                          CreatureName(setup_.left), CreatureName(setup_.right),
                          arena_shape_name(setup_.shape), timed_out ? "timeout" : "mutual",
                          seconds_of(elapsed));
        }
        last_result_ = line;
        spdlog::info("duel {}: {}", round_, last_result_);
        // A decided round lingers so the end of the fight is watchable; a timed
        // out one has nothing left to watch, so it restages at once.
        report_at_millis_ = world_millis + (timed_out ? 0 : cfg_.linger_millis);
    }
    if (world_millis < report_at_millis_) {
        return false;
    }
    ++round_;
    return true;
}

std::string DuelMode::Status() const {
    char line[256];
    std::snprintf(line, sizeof(line), "duel %u: %s vs %s on %s\nlast: %s", round_,
                  CreatureName(setup_.left), CreatureName(setup_.right),
                  arena_shape_name(setup_.shape), last_result_.c_str());
    return line;
}

}  // namespace badlands
