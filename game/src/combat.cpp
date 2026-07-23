#include "combat.h"

#include "behaviours/rng.h"  // seed_of / unit_float -- the sim's only randomness
#include "game_state.h"      // BadlandsGame, entity_for_slot, nearest_enemy, slot_of

#include <algorithm>
#include <vector>

namespace badlands {

namespace {

// Placeholder tuning constants. The arena exists to find the right values; the
// pipeline SHAPE is what these tests pin, not the numbers.
constexpr float kCritMultiplier = 2.0f;
constexpr float kRangedEvasionMult = 0.5f;   // a shot is hard to dodge
constexpr float kMeleeThrustEvasionMult = 1.3f;  // a telegraphed thrust, easy to sidestep
constexpr float kBluntArmourFraction = 0.3f;  // blunt crushes through 70% of armour

// How much the defender's evasion is worth against this attack.
float evasion_mult(AttackCategory cat, DamageType type) {
    if (cat == AttackCategory::Ranged) {
        return kRangedEvasionMult;
    }
    if (type == DamageType::Piercing) {
        return kMeleeThrustEvasionMult;  // melee thrust
    }
    return 1.0f;
}

// Penetrated damage for a landed, undodged blow (pre-crit), by damage type.
float apply_armour(DamageType type, float base, float armour) {
    switch (type) {
        case DamageType::Slashing:
            return std::max(1.0f, base - armour);  // high raw, stopped well by armour
        case DamageType::Piercing: {
            const float pen = base - armour;
            return pen > 0.0f ? pen : 0.0f;  // all-or-nothing: no penetration -> no damage
        }
        case DamageType::Blunt:
            return std::max(1.0f, base - armour * kBluntArmourFraction);
    }
    return 0.0f;  // (Soul is reserved for the deferred psychology layer)
}

// Fold the four replay-reproducible identity axes into one non-zero seed, so the
// whole roll stream is a pure function of (who, whom, when, which attack).
uint64_t combat_seed(const CombatRequest& r) {
    uint64_t s = seed_of(r.attacker_slot, r.world_millis);
    s ^= seed_of(r.target_slot, static_cast<int64_t>(r.attack_index) + 1);
    return s == 0 ? 1ull : s;
}

}  // namespace

CombatResult resolve_attack(const CombatRequest& req) {
    CombatResult res;
    uint64_t s = combat_seed(req);

    // 1. Attack vs Defense -> hit or blocked. Blunt crushes through the shield.
    const float defense =
        req.attack.damage_type == DamageType::Blunt ? 0.0f : req.defender.defense;
    const float hit_chance = std::clamp(req.attacker.accuracy - defense, 0.0f, 1.0f);
    if (unit_float(s) >= hit_chance) {
        res.blocked = true;
        return res;
    }

    // 2. Evade -> dodged or through.
    const float evade_chance = std::clamp(
        req.defender.evasion * evasion_mult(req.attack.category, req.attack.damage_type),
        0.0f, 1.0f);
    if (unit_float(s) < evade_chance) {
        res.dodged = true;
        return res;
    }

    // 3. Armour -> penetrated damage.
    float damage = apply_armour(req.attack.damage_type, req.attack.base_damage,
                                req.defender.armour);

    // 4. Crit multiplies the penetrated damage (the attack's own crit_chance, so
    // a piercing thrust -- authored with a higher chance -- benefits most).
    if (damage > 0.0f &&
        unit_float(s) < std::clamp(req.attack.crit_chance, 0.0f, 1.0f)) {
        res.crit = true;
        damage *= kCritMultiplier;
    }

    res.damage = damage;
    return res;
}

namespace {

float range_of_category(const Attacks& atk, AttackCategory cat) {
    float r = 0.0f;
    for (int i = 0; i < atk.count && i < kMaxAttacks; ++i) {
        if (atk.defs[i].category == cat) {
            r = std::max(r, atk.defs[i].range);
        }
    }
    return r;
}

}  // namespace

float melee_range(const Attacks& atk) { return range_of_category(atk, AttackCategory::Melee); }
float ranged_range(const Attacks& atk) { return range_of_category(atk, AttackCategory::Ranged); }

float engagement_range(const Combatant& stats, const Attacks& atk) {
    const float m = melee_range(atk);
    const float rg = ranged_range(atk);
    if (stats.stance == CombatStance::Ranged) {
        return rg > 0.0f ? rg : m;  // hold at bow distance (melee reach if no bow)
    }
    return m > 0.0f ? m : rg;  // close to melee (bow reach if no melee)
}

int pick_attack(const Attacks& atk, float dist, bool melee_locked) {
    int best = -1;
    for (int i = 0; i < atk.count && i < kMaxAttacks; ++i) {
        const Attack& a = atk.defs[i];
        if (atk.cooldown_remaining[i] > 0.0f) {
            continue;  // still recovering
        }
        if (dist > a.range) {
            continue;  // out of reach
        }
        if (melee_locked && a.category == AttackCategory::Ranged) {
            continue;  // no ranged attacks while locked in melee
        }
        if (best == -1) {
            best = i;
            continue;
        }
        // Prefer a ranged attack when free to take it (a melee-stance unit still
        // opens with a shot while closing); otherwise keep the first usable.
        const bool cur_ranged = a.category == AttackCategory::Ranged;
        const bool best_ranged = atk.defs[best].category == AttackCategory::Ranged;
        if (cur_ranged && !best_ranged) {
            best = i;
        }
    }
    return best;
}

// --- game-coupled seams -----------------------------------------------------

namespace {
constexpr float kProjectileSpeed = 24.0f;      // world units / second
constexpr float kProjectileHitRadius = 0.35f;  // contact epsilon on arrival
}  // namespace

entt::entity select_target(const BadlandsGame& game, entt::entity self) {
    return nearest_enemy(game, self);  // Threat-Score drops in here later
}

int select_attack(const BadlandsGame& game, entt::entity self, entt::entity target) {
    const entt::registry& reg = game.registry;
    if (!reg.all_of<Attacks, Position>(self) || !reg.all_of<Position>(target)) {
        return -1;
    }
    const Attacks& atk = reg.get<Attacks>(self);
    const float dist =
        glm::distance(reg.get<Position>(self).pos, reg.get<Position>(target).pos);
    return pick_attack(atk, dist, reg.all_of<MeleeLock>(self));
}

void fire_attack(BadlandsGame& game, uint32_t attacker_slot, uint32_t target_slot) {
    entt::registry& reg = game.registry;
    entt::entity self = entity_for_slot(game, static_cast<int32_t>(attacker_slot));
    if (self == entt::null) {
        return;
    }
    // Engine picks the enemy when the producer named none (mock/scripted brains);
    // the hunter names its neutral prey explicitly.
    entt::entity target = (target_slot == UINT32_MAX)
                              ? select_target(game, self)
                              : entity_for_slot(game, static_cast<int32_t>(target_slot));
    if (target == entt::null) {
        return;
    }
    if (!reg.all_of<Attacks, Combatant, Position>(self) ||
        !reg.all_of<Health, Combatant, Position>(target)) {
        return;
    }
    const int idx = select_attack(game, self, target);
    if (idx < 0) {
        return;  // out of range / on cooldown / ranged while melee-locked
    }
    const uint32_t tslot = (target_slot == UINT32_MAX)
                               ? static_cast<uint32_t>(slot_of(game, target))
                               : target_slot;

    Attacks& attacks = reg.get<Attacks>(self);
    const Attack a = attacks.defs[idx];
    attacks.cooldown_remaining[idx] = a.cooldown;

    if (a.category == AttackCategory::Ranged) {
        // Capture everything the shot needs so it resolves correctly on arrival
        // even if the shooter dies mid-flight.
        Projectile proj;
        proj.attacker_slot = attacker_slot;
        proj.target_slot = tslot;
        proj.pos = reg.get<Position>(self).pos;
        proj.speed = kProjectileSpeed;
        proj.attack = a;
        proj.attacker = reg.get<Combatant>(self);
        proj.attack_index = idx;
        proj.fire_millis = game.world_millis;
        reg.emplace<Projectile>(reg.create(), proj);
        return;
    }

    // Melee: resolve immediately.
    CombatRequest req;
    req.attacker = reg.get<Combatant>(self);
    req.attack = a;
    req.defender = reg.get<Combatant>(target);
    req.attacker_slot = attacker_slot;
    req.target_slot = tslot;
    req.world_millis = game.world_millis;
    req.attack_index = idx;
    const CombatResult res = resolve_attack(req);
    if (res.damage > 0.0f) {
        reg.get<Health>(target).hp -= res.damage;
    }
}

void advance_projectiles(BadlandsGame& game, float dt) {
    entt::registry& reg = game.registry;
    std::vector<entt::entity> spent;
    for (auto [e, proj] : reg.view<Projectile>().each()) {
        entt::entity target = entity_for_slot(game, static_cast<int32_t>(proj.target_slot));
        if (target == entt::null || !reg.all_of<Position, Health, Combatant>(target)) {
            spent.push_back(e);  // target gone -> the shot fizzles
            continue;
        }
        const glm::vec2 to = reg.get<Position>(target).pos - proj.pos;
        const float dist = glm::length(to);
        const float step = proj.speed * dt;
        if (dist <= step + kProjectileHitRadius) {
            CombatRequest req;
            req.attacker = proj.attacker;
            req.attack = proj.attack;
            req.defender = reg.get<Combatant>(target);
            req.attacker_slot = proj.attacker_slot;
            req.target_slot = proj.target_slot;
            req.world_millis = proj.fire_millis;  // seed fixed at fire time
            req.attack_index = proj.attack_index;
            const CombatResult res = resolve_attack(req);
            if (res.damage > 0.0f) {
                reg.get<Health>(target).hp -= res.damage;
            }
            spent.push_back(e);
        } else {
            proj.pos += to / dist * step;
        }
    }
    for (entt::entity e : spent) {
        reg.destroy(e);
    }
}

}  // namespace badlands
