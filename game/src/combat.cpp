#include "combat.h"

#include "behaviours/rng.h"  // seed_of / unit_float -- the sim's only randomness
#include "game_state.h"      // BadlandsGame, entity_for_slot, nearest_enemy, slot_for_entity, emit_char_hit
#include "status.h"          // has_status -- Stunned zeroes the ACTIVE defense (effective_combatant)
#include "strike.h"          // declare_strike -- fire_attack commits, it no longer resolves

#include <algorithm>
#include <vector>

namespace badlands {

namespace {

// Placeholder tuning constants. Watching fights is what finds the right
// values; the pipeline SHAPE is what the tests pin, not the numbers.
constexpr float kRangedEvasionMult = 0.5f;   // a shot is hard to dodge
constexpr float kMeleeThrustEvasionMult = 1.3f;  // a telegraphed thrust, easy to sidestep
constexpr float kBluntArmourFraction = 0.3f;  // blunt crushes through 70% of armour
// What StatusKind::Cursed takes off a victim (effective_combatant, below).
constexpr float kCurseAccuracyPenalty = 0.15f;
constexpr float kCurseArmourPenalty = 2.0f;
// What StatusKind::Sneaking is worth to the blow that ends it, and what
// StatusKind::Calcified is worth to whoever wears it. COMPILED, like the curse
// penalties above and for the same reason: how much a status is worth is a
// property of the status, and a skill's own constants tune its DURATION.
constexpr float kSneakAccuracyBonus = 0.15f;
constexpr float kSneakCritMultiplier = 3.0f;
constexpr float kCalcifyArmourBonus = 4.0f;

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
    // a piercing thrust -- authored with a higher chance -- benefits most). The
    // MULTIPLIER is the attacker's, not a global: a status can raise it
    // (effective_combatant), and it can never shrink a hit.
    if (damage > 0.0f &&
        unit_float(s) < std::clamp(req.attack.crit_chance, 0.0f, 1.0f)) {
        res.crit = true;
        damage *= std::max(1.0f, req.attacker.crit_multiplier);
    }

    res.damage = damage;
    return res;
}

void end_sneak_on_aggression(BadlandsGame& game, entt::entity e) {
    clear_status(game, e, StatusKind::Sneaking);
}

Combatant effective_combatant(const entt::registry& reg, entt::entity e) {
    const auto* base = (e != entt::null && reg.valid(e)) ? reg.try_get<Combatant>(e) : nullptr;
    if (base == nullptr) {
        return Combatant{};
    }
    Combatant c = *base;
    if (has_status(reg, e, StatusKind::Stunned)) {
        c.defense = 0.0f;
        c.evasion = 0.0f;
    }
    if (has_status(reg, e, StatusKind::Cursed)) {
        // A curse saps the guard: harder to land a blow, and the armour turns
        // brittle. Deliberately NOT evasion -- a cursed target still dodges,
        // it has just stopped warding, which is what distinguishes it from a
        // stun. Penalties are compiled constants; the skill's own constants
        // tune its DURATION, and how much a curse is worth is a property of
        // the status rather than of whatever applied it.
        c.accuracy = std::max(0.0f, c.accuracy - kCurseAccuracyPenalty);
        c.armour = std::max(0.0f, c.armour - kCurseArmourPenalty);
    }
    if (has_status(reg, e, StatusKind::Sneaking)) {
        // Attacker-side only, and the blow that spends it is the blow that
        // gets it: declare_strike captures these stats and THEN clears the
        // status (strike.cpp). Nothing defensive moves -- an unseen grave
        // robber that is somehow hit is no harder to hit than usual.
        c.accuracy = std::clamp(c.accuracy + kSneakAccuracyBonus, 0.0f, 1.0f);
        c.crit_multiplier = std::max(c.crit_multiplier, kSneakCritMultiplier);
    }
    if (has_status(reg, e, StatusKind::Calcified)) {
        c.armour += kCalcifyArmourBonus;
    }
    return c;
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

bool attack_usable(const Attacks& atk, int idx, float dist, bool melee_locked) {
    if (idx < 0 || idx >= atk.count || idx >= kMaxAttacks) {
        return false;  // out of bounds -- covers a stale/adversarial index too
    }
    const Attack& a = atk.defs[idx];
    if (atk.cooldown_remaining[idx] > 0.0f) {
        return false;  // still recovering
    }
    if (dist > a.range) {
        return false;  // out of reach
    }
    if (melee_locked && a.category == AttackCategory::Ranged) {
        return false;  // no ranged attacks while locked in melee
    }
    return true;
}

int pick_attack(const Attacks& atk, float dist, bool melee_locked) {
    int best = -1;
    for (int i = 0; i < atk.count && i < kMaxAttacks; ++i) {
        if (!attack_usable(atk, i, dist, melee_locked)) {
            continue;
        }
        if (best == -1) {
            best = i;
            continue;
        }
        // Prefer a ranged attack when free to take it (a melee-stance unit still
        // opens with a shot while closing); otherwise keep the first usable.
        const bool cur_ranged = atk.defs[i].category == AttackCategory::Ranged;
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
    // Deliberately ALWAYS a live scan, even though sim.cpp's ThreatSighted
    // pass computes a same-tick nearest_enemy for every EventInbox-bearing
    // entity moments earlier (BadlandsGame::nearest_enemy_scratch,
    // game_state.h). Single-gateway combat (docs/superpowers/specs/2026-07-
    // 25-contract-v3-alignment-design.md) deleted the one call site that
    // used to be provably exact-equivalent to that pass' result
    // (sim.cpp's combat_preempt, called from the think loop before
    // apply_commands resolved anything) -- its cache-reading shortcut moved
    // to monster_think (monster_brain.cpp), the new same-tick-as-the-pass
    // caller, which consults the cache directly instead of coming through
    // here (see that function's own comment). This function's remaining
    // call sites are all NOT equivalent to the pass' result, each for
    // its own reason: fire_attack's UINT32_MAX re-pick (command.cpp's
    // Attack handler) runs during apply_commands, AFTER earlier commands in
    // the same drain may have already dropped a shared target to hp<=0 (a
    // stale cache entry would have a later hero in a scrum swing at a
    // corpse instead of retargeting); update_melee_locks (movement.cpp)
    // runs after the whole movement pipeline, where position -- and so
    // engagement range -- may have changed since the pass ran;
    // resolve_action's target-inference branch (game/src/intention.h) can
    // be called from a context the cache was never populated for (a direct
    // test, or any future caller outside sim.cpp's think loop); and
    // apply_intention's Attack-engagement executor + advance_intentions'
    // Attack-abort check (both intention.cpp) run during/after a wake that
    // is not guaranteed to be the same moment the pass ran (a wasm hero's
    // wake, or a call driven directly by a test). Keeping every cache
    // consult local to its one safe caller, rather than hiding one in here,
    // is what makes each call site's correctness independently obvious.
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

void fire_attack(BadlandsGame& game, uint32_t attacker_slot, uint32_t target_slot,
                 int32_t attack_index) {
    entt::registry& reg = game.registry;
    entt::entity self = entity_for_slot(game, static_cast<int32_t>(attacker_slot));
    if (self == entt::null) {
        return;
    }
    // Disengaged (movement.h): walked out of melee contact, so it can do
    // nothing at all for a few seconds. Checked HERE rather than in the Attack
    // command handler so every caller is covered -- the handler, a test, a
    // future engine-side producer -- the same reason the cooldown/range
    // re-checks live here and not at the queue site.
    if (has_status(reg, self, StatusKind::Disengaged)) {
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
    // Finding 4: a corpse is never a valid target, explicit index or not --
    // mirrors select_target's auto-pick path (nearest_enemy's hp<=0.0f
    // filter, game.cpp), which already refuses one. Without this, an
    // in-batch kill (a first Attack command dropping the target to hp<=0
    // earlier in this same apply_commands drain) would still let a second
    // queued Attack command naming that target's slot explicitly land a hit
    // on the corpse. Checked before ANY cooldown is spent, so a corpse-swing
    // no-ops exactly like an out-of-range/on-cooldown one.
    if (reg.get<Health>(target).hp <= 0.0f) {
        return;
    }
    int idx;
    if (attack_index < 0) {
        idx = select_attack(game, self, target);  // legacy auto-pick
    } else {
        // Explicit pick: the authoritative re-check (this function's own doc
        // comment) -- re-validate exactly this index against the world AS IT
        // STANDS NOW, via the same per-index check pick_attack's scan uses
        // (attack_usable, combat.h), rather than trusting whoever chose it a
        // moment earlier (resolve_action, game/src/intention.h). This is what
        // makes in-batch self-invalidation work: a second queued Attack
        // command naming the same index this tick finds it already on
        // cooldown here and silently no-ops, exactly like an unusable
        // auto-pick would.
        const float dist =
            glm::distance(reg.get<Position>(self).pos, reg.get<Position>(target).pos);
        const Attacks& loadout = reg.get<Attacks>(self);
        idx = attack_usable(loadout, attack_index, dist, reg.all_of<MeleeLock>(self))
                  ? attack_index
                  : -1;
    }
    if (idx < 0) {
        return;  // out of range / on cooldown / ranged while melee-locked
    }
    const uint32_t tslot =
        (target_slot == UINT32_MAX) ? slot_for_entity(game, target) : target_slot;

    // Validated -- now COMMIT rather than resolve. The blow lands when the
    // attack's wind-up elapses (strike.h); this function's whole job above is
    // deciding that the swing is legal to start, which has not changed.
    // declare_strike stamps nothing: the cooldown is spent at resolve, so an
    // interrupted wind-up costs the attacker its tempo but not its attack.
    declare_strike(game, self, idx, tslot);
}

void deliver_strike(BadlandsGame& game, entt::entity attacker, const StrikeInProgress& s) {
    entt::registry& reg = game.registry;
    const uint32_t attacker_slot = slot_for_entity(game, attacker);
    entt::entity target = entity_for_slot(game, static_cast<int32_t>(s.target_slot));

    // The world moved while the attacker was committed. A blow thrown at
    // someone who died, vanished, or stepped out of reach WHIFFS -- it is not
    // re-aimed, and it is not refunded. That is the point of committing, and
    // it is what makes backing out of reach worth doing.
    if (target == entt::null || !reg.all_of<Position, Health, Combatant>(target) ||
        reg.get<Health>(target).hp <= 0.0f) {
        return;
    }
    if (!reg.all_of<Position>(attacker)) {
        return;
    }
    if (s.attack.category == AttackCategory::Ranged) {
        // NO range re-check for a shot. The arrow leaves the bow at the end of
        // the draw and then homes on its target (advance_projectiles), so a
        // target that backed off during the wind-up is chased by the arrow, not
        // missed by it -- which is also what happened before commitment
        // existed, when the projectile spawned the instant the attack was
        // declared. Re-checking here would silently eat the shot AND its
        // cooldown, and with both heroes and archers now actively backing away
        // it would do so routinely.
        // The arrow leaves NOW, at the end of the draw. Everything it needs is
        // already captured on the strike, so it resolves correctly on arrival
        // even if the shooter dies mid-flight.
        Projectile proj;
        proj.attacker_slot = attacker_slot;
        proj.target_slot = s.target_slot;
        proj.pos = reg.get<Position>(attacker).pos;
        proj.speed = kProjectileSpeed;
        proj.attack = s.attack;
        proj.attacker = s.attacker;
        proj.attack_index = s.attack_index;
        proj.fire_millis = s.declared_millis;
        reg.emplace<Projectile>(reg.create(), proj);
        return;
    }

    // Melee only: a swing at someone who stepped out of reach hits empty air.
    // Not re-aimed, not refunded -- that is what makes backing off worth doing.
    const float dist =
        glm::distance(reg.get<Position>(attacker).pos, reg.get<Position>(target).pos);
    if (dist > s.attack.range) {
        return;
    }

    CombatRequest req;
    req.attacker = s.attacker;   // captured at declaration, not re-read
    req.attack = s.attack;
    req.defender = effective_combatant(reg, target);  // but the DEFENDER is live:
    req.attacker_slot = attacker_slot;                // a target stunned mid-swing
    req.target_slot = s.target_slot;                  // is defenceless when it lands
    req.world_millis = s.declared_millis;
    req.attack_index = s.attack_index;
    const CombatResult res = resolve_attack(req);
    if (res.damage > 0.0f) {
        Health& th = reg.get<Health>(target);
        th.hp -= res.damage;
        emit_char_hit(game, attacker_slot, s.target_slot, res.damage, th.hp,
                      reg.get<Position>(target).pos);
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
            req.defender = effective_combatant(reg, target);
            req.attacker_slot = proj.attacker_slot;
            req.target_slot = proj.target_slot;
            req.world_millis = proj.fire_millis;  // seed fixed at fire time
            req.attack_index = proj.attack_index;
            const CombatResult res = resolve_attack(req);
            if (res.damage > 0.0f) {
                Health& th = reg.get<Health>(target);
                th.hp -= res.damage;
                emit_char_hit(game, proj.attacker_slot, proj.target_slot, res.damage,
                              th.hp, reg.get<Position>(target).pos);
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
