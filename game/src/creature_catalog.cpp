// Compiled defaults for the named-creature catalog (CreatureCatalog, declared in
// badlands_sim.hpp) plus the id<->name tables. This is the single source of truth
// for what a Mercenary / Rat / Deer / ... is; MercenaryDesc/GoblinDesc and
// hero_desc read it, an app may override fields by name from JSON, and
// SpawnCreature spawns by id. Values are PLACEHOLDERS.
//
// NB: the legacy CharacterDesc.attack_range/damage/cooldown fields are NOT set
// here. spawn_entity derives the entity's Stats reach from attacks[0] (the primary
// attack), so those fields would be a redundant mirror that could silently
// disagree with the real attack.

#include "badlands_sim.hpp"

#include <cstring>

namespace badlands {

namespace {

constexpr const char* kNames[kCreatureCount] = {
    "Mercenary", "Hunter", "GraveRobber", "Apprentice", "Rat",         "Goblin",
    "Deer",      "Bandit", "BanditArcher", "BanditLeader", "MudGolem",
};

}  // namespace

const char* CreatureName(CreatureId id) {
    const int i = static_cast<int>(id);
    return (i >= 0 && i < kCreatureCount) ? kNames[i] : "";
}

CreatureId CreatureIdFromName(const char* name) {
    if (name != nullptr) {
        for (int i = 0; i < kCreatureCount; ++i) {
            if (std::strcmp(name, kNames[i]) == 0) {
                return static_cast<CreatureId>(i);
            }
        }
    }
    return CreatureId::Count;
}

const CreatureCatalog& DefaultCreatureCatalog() {
    static const CreatureCatalog kCatalog;
    return kCatalog;
}

CreatureCatalog::CreatureCatalog() {
    auto at = [&](CreatureId id) -> CharacterDesc& {
        CharacterDesc& d = defs[static_cast<int>(id)];
        d = CharacterDesc{};  // zero + apply default member initializers
        d.creature = id;      // so a spawned entity knows what it is (CreatureKind)
        return d;
    };

    // --- heroes (ids 0..3 == HeroClassId) -----------------------------------
    // Mercenary: the tank. High armour turns most slashes into scratches; solid
    // melee damage. Falls flat on utility (none yet), per the design.
    {
        CharacterDesc& d = at(CreatureId::Mercenary);
        d.archetype = Archetype::Hero;
        d.hero_class = HERO_MERCENARY;
        d.hp = 30.0f;
        d.move_speed = 2.5f;
        d.size_x = 0.9f; d.size_y = 1.8f; d.size_z = 0.9f;
        d.color_r = 0.35f; d.color_g = 0.45f; d.color_b = 0.80f;  // blue
        d.vision_radius = 14.0f; d.vision_cone_half_angle_deg = 60.0f;
        d.accuracy = 0.9f; d.evasion = 0.05f; d.defense = 0.1f; d.armour = 3.0f;
        d.stance = CombatStance::Melee;
        d.attack_count = 1;
        d.attacks[0] = {AttackCategory::Melee, DamageType::Slashing, 6.0f, 1.5f, 1.0f, 0.1f, 0.35f, 0.25f};
        // Armour 2, evasion 1, hitpoints 1, dps 1 (the doc's level-15 ratings).
        // The heaviest armour growth of the four -- which is also why the
        // mercenary's curve flattens late: flat reduction loses relative value
        // as everything else's damage climbs.
        d.growth = {.hp = 1.5f, .accuracy = 0.005f, .evasion = 0.004f,
                    .defense = 0.0f, .armour = 0.20f, .damage_frac = 0.04f};
        // Utility arrives with the shield: at level 3 the tank stops being
        // only a wall and starts taking a turn away from whatever it hits.
        d.skill_grants[0] = {static_cast<int32_t>(SkillId::ShieldBash), 3};
        d.skill_grant_count = 1;
    }
    // Hunter: primarily ranged (a bow), with a knife for when something closes.
    {
        CharacterDesc& d = at(CreatureId::Hunter);
        d.archetype = Archetype::Hero;
        d.hero_class = HERO_HUNTER;
        d.hp = 22.0f;
        d.move_speed = 3.2f;  // a touch faster than the deer it hunts
        d.size_x = 0.9f; d.size_y = 1.8f; d.size_z = 0.9f;
        d.color_r = 0.30f; d.color_g = 0.70f; d.color_b = 0.35f;  // green
        d.vision_radius = 14.0f; d.vision_cone_half_angle_deg = 60.0f;
        d.accuracy = 0.85f; d.evasion = 0.15f; d.defense = 0.05f; d.armour = 1.0f;
        d.stance = CombatStance::Ranged;
        d.attack_count = 2;
        d.attacks[0] = {AttackCategory::Ranged, DamageType::Piercing, 5.0f, 8.0f, 1.2f, 0.25f, 0.60f, 0.30f};
        d.attacks[1] = {AttackCategory::Melee, DamageType::Slashing, 3.0f, 1.2f, 0.8f, 0.10f, 0.25f, 0.15f};
        // Armour 1, evasion 1, hitpoints 1, dps 2.
        d.growth = {.hp = 1.5f, .accuracy = 0.005f, .evasion = 0.004f,
                    .defense = 0.0f, .armour = 0.10f, .damage_frac = 0.08f};
        d.skill_grants[0] = {static_cast<int32_t>(SkillId::DressWounds), 2};
        d.skill_grant_count = 1;
    }
    // Grave Robber: mixed -- a hand crossbow OPENER (one heavy, high-crit bolt
    // on a long reload) then blades. The reload is the whole identity: at a
    // short cooldown the crossbow out-DPSes the hunter's bow and inverts the
    // two classes, which their threat anchors (1.0 vs 1.5) say it must not.
    {
        CharacterDesc& d = at(CreatureId::GraveRobber);
        d.archetype = Archetype::Hero;
        d.hero_class = HERO_GRAVE_ROBBER;
        d.hp = 24.0f;
        d.move_speed = 2.8f;
        d.size_x = 0.9f; d.size_y = 1.8f; d.size_z = 0.9f;
        d.color_r = 0.60f; d.color_g = 0.45f; d.color_b = 0.75f;  // violet
        d.vision_radius = 14.0f; d.vision_cone_half_angle_deg = 60.0f;
        d.accuracy = 0.85f; d.evasion = 0.20f; d.defense = 0.10f; d.armour = 1.0f;
        d.stance = CombatStance::Melee;  // closes, but opens with the crossbow
        d.attack_count = 2;
        d.attacks[0] = {AttackCategory::Ranged, DamageType::Piercing, 5.0f, 5.0f, 2.5f, 0.30f, 0.50f, 0.40f};
        d.attacks[1] = {AttackCategory::Melee, DamageType::Slashing, 4.0f, 1.3f, 0.7f, 0.20f, 0.25f, 0.15f};
        // Armour 1, evasion 2 (the nimblest of the four), hitpoints 1, dps 2.
        d.growth = {.hp = 1.5f, .accuracy = 0.005f, .evasion = 0.008f,
                    .defense = 0.0f, .armour = 0.10f, .damage_frac = 0.08f};
        // Sneak and Backstab both at 3, deliberately: the approach and the
        // payoff are one tool, and either alone is half a class.
        d.skill_grants[0] = {static_cast<int32_t>(SkillId::Sneak), 3};
        d.skill_grants[1] = {static_cast<int32_t>(SkillId::Backstab), 3};
        d.skill_grant_count = 2;
    }
    // Apprentice: fragile ranged caster (a magic bolt; Soul comes later).
    {
        CharacterDesc& d = at(CreatureId::Apprentice);
        d.archetype = Archetype::Hero;
        d.hero_class = HERO_APPRENTICE;
        d.hp = 16.0f;
        d.move_speed = 2.4f;
        d.size_x = 0.9f; d.size_y = 1.8f; d.size_z = 0.9f;
        d.color_r = 0.45f; d.color_g = 0.78f; d.color_b = 0.85f;  // cyan
        d.vision_radius = 14.0f; d.vision_cone_half_angle_deg = 60.0f;
        d.accuracy = 0.9f; d.evasion = 0.05f; d.defense = 0.0f; d.armour = 0.0f;
        d.stance = CombatStance::Ranged;
        d.attack_count = 1;
        d.attacks[0] = {AttackCategory::Ranged, DamageType::Piercing, 4.0f, 6.0f, 1.5f, 0.10f, 0.80f, 0.35f};
        // Armour 0, evasion 0, hitpoints 1, dps 2. No defensive growth at all:
        // the apprentice never stops being fragile, and its intended late-game
        // power is SPELLS, not this bolt -- which is why a stats-only
        // apprentice undershoots its curve (design doc §1) rather than
        // being tuned up here to hide that.
        d.growth = {.hp = 1.5f, .accuracy = 0.005f, .evasion = 0.0f,
                    .defense = 0.0f, .armour = 0.0f, .damage_frac = 0.08f};
        // Curse at 1 is the apprentice's only level-1 tool; Calcify moves to 4
        // per the design document (it was compiled at 5), effect still deferred.
        d.skill_grants[0] = {static_cast<int32_t>(SkillId::Curse), 1};
        d.skill_grants[1] = {static_cast<int32_t>(SkillId::Calcify), 4};
        d.skill_grant_count = 2;
    }

    // --- monsters -----------------------------------------------------------
    // Rat: weak swarm melee, no armour of its own.
    {
        CharacterDesc& d = at(CreatureId::Rat);
        d.archetype = Archetype::Monster;
        d.hp = 6.0f;
        d.move_speed = 4.0f;
        d.size_x = 0.5f; d.size_y = 0.5f; d.size_z = 0.5f;
        d.color_r = 0.45f; d.color_g = 0.40f; d.color_b = 0.38f;  // grey-brown
        d.accuracy = 0.7f; d.evasion = 0.05f; d.defense = 0.0f; d.armour = 0.0f;
        d.stance = CombatStance::Melee;
        d.attack_count = 1;
        d.attacks[0] = {AttackCategory::Melee, DamageType::Slashing, 3.0f, 1.0f, 1.0f, 0.05f, 0.20f, 0.10f};
        d.xp_reward = 10;
    }
    // Goblin: a step up from the rat -- more hp, still light melee.
    {
        CharacterDesc& d = at(CreatureId::Goblin);
        d.archetype = Archetype::Monster;
        d.hp = 18.0f;
        d.move_speed = 3.0f;
        d.size_x = 0.8f; d.size_y = 1.2f; d.size_z = 0.8f;
        d.color_r = 0.30f; d.color_g = 0.75f; d.color_b = 0.35f;  // green
        d.accuracy = 0.8f; d.evasion = 0.10f; d.defense = 0.05f; d.armour = 0.0f;
        d.stance = CombatStance::Melee;
        d.attack_count = 1;
        d.attacks[0] = {AttackCategory::Melee, DamageType::Slashing, 3.0f, 1.2f, 0.8f, 0.10f, 0.25f, 0.15f};
        d.xp_reward = 25;
    }

    // Bandit: an armed and armoured human -- the level-1 hero's fair fight, and
    // the first monster built to the same recipe a hero is.
    {
        CharacterDesc& d = at(CreatureId::Bandit);
        d.archetype = Archetype::Monster;
        d.hp = 26.0f;
        d.move_speed = 2.8f;
        d.size_x = 0.9f; d.size_y = 1.8f; d.size_z = 0.9f;
        d.color_r = 0.55f; d.color_g = 0.30f; d.color_b = 0.25f;  // rust
        d.accuracy = 0.85f; d.evasion = 0.10f; d.defense = 0.10f; d.armour = 2.0f;
        d.stance = CombatStance::Melee;
        d.attack_count = 1;
        d.attacks[0] = {AttackCategory::Melee, DamageType::Slashing, 5.0f, 1.4f, 1.0f, 0.10f, 0.35f, 0.25f};
        d.xp_reward = 60;
    }
    // Bandit Archer: the enemy-side kiter. Exists so the skirmish behaviour is
    // tested from BOTH ends -- a hero holding its distance is only half the
    // mechanic if nothing ever holds distance from the hero.
    {
        CharacterDesc& d = at(CreatureId::BanditArcher);
        d.archetype = Archetype::Monster;
        d.hp = 18.0f;
        d.move_speed = 3.0f;
        d.size_x = 0.85f; d.size_y = 1.75f; d.size_z = 0.85f;
        d.color_r = 0.60f; d.color_g = 0.42f; d.color_b = 0.22f;  // tan
        d.accuracy = 0.85f; d.evasion = 0.15f; d.defense = 0.05f; d.armour = 1.0f;
        d.stance = CombatStance::Ranged;
        d.attack_count = 2;
        d.attacks[0] = {AttackCategory::Ranged, DamageType::Piercing, 4.0f, 7.0f, 1.3f, 0.20f, 0.55f, 0.30f};
        d.attacks[1] = {AttackCategory::Melee, DamageType::Slashing, 2.0f, 1.2f, 0.8f, 0.05f, 0.25f, 0.15f};
        d.xp_reward = 60;
    }
    // Bandit Leader: beats every level-1 core class. The matrix needs a ceiling
    // as much as it needs the rat's floor, or every row reads the same.
    {
        CharacterDesc& d = at(CreatureId::BanditLeader);
        d.archetype = Archetype::Monster;
        d.hp = 45.0f;
        d.move_speed = 2.8f;
        d.size_x = 1.0f; d.size_y = 1.9f; d.size_z = 1.0f;
        d.color_r = 0.70f; d.color_g = 0.20f; d.color_b = 0.20f;  // deep red
        d.accuracy = 0.95f; d.evasion = 0.15f; d.defense = 0.20f; d.armour = 4.0f;
        d.stance = CombatStance::Melee;
        d.attack_count = 1;
        d.attacks[0] = {AttackCategory::Melee, DamageType::Slashing, 8.0f, 1.5f, 0.9f, 0.15f, 0.30f, 0.20f};
        d.xp_reward = 200;
    }
    // Mud Golem: the Goliath. Slow, enormously durable, and the only creature
    // anywhere that swings BLUNT -- so it is the only one that exercises
    // resolve_attack's defense bypass and apply_armour's 30% branch in a live
    // fight rather than a unit test. Heavy armour makes it near-immune to the
    // apprentice's piercing bolt (all-or-nothing) while a slash still scratches.
    {
        CharacterDesc& d = at(CreatureId::MudGolem);
        d.archetype = Archetype::Monster;
        d.hp = 90.0f;
        d.move_speed = 1.4f;
        d.size_x = 1.6f; d.size_y = 2.4f; d.size_z = 1.6f;
        d.color_r = 0.38f; d.color_g = 0.32f; d.color_b = 0.24f;  // wet earth
        d.accuracy = 0.80f; d.evasion = 0.0f; d.defense = 0.0f; d.armour = 8.0f;
        d.stance = CombatStance::Melee;
        d.attack_count = 1;
        d.attacks[0] = {AttackCategory::Melee, DamageType::Blunt, 12.0f, 1.8f, 2.2f, 0.05f, 1.00f, 0.60f};
        d.xp_reward = 250;
    }

    // --- critters -----------------------------------------------------------
    // Deer: neutral prey. Fast and evasive, no attacks at all (harmless).
    {
        CharacterDesc& d = at(CreatureId::Deer);
        d.archetype = Archetype::Critter;
        d.hp = 8.0f;
        d.move_speed = 3.0f;
        d.size_x = 0.7f; d.size_y = 1.0f; d.size_z = 0.7f;
        d.color_r = 0.62f; d.color_g = 0.42f; d.color_b = 0.20f;  // brown
        d.accuracy = 0.0f; d.evasion = 0.40f; d.defense = 0.0f; d.armour = 0.0f;
        d.stance = CombatStance::Melee;
        d.attack_count = 0;  // no attacks -> genuinely harmless
    }
}

}  // namespace badlands
