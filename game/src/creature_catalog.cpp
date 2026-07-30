// Compiled defaults for the named-creature catalog (CreatureCatalog, declared in
// badlands_sim.hpp) plus the id<->name tables. This is the single source of truth
// for what a Mercenary / Rat / Deer / ... is; MercenaryDesc/GoblinDesc and
// hero_desc read it, an app may override fields by name from JSON, and the arena
// spawns creatures by id. Values are PLACEHOLDERS -- the arena exists to tune them.
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
    "Mercenary", "Hunter", "GraveRobber", "Apprentice", "Rat", "Goblin", "Deer",
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
        d.attacks[0] = {AttackCategory::Melee, DamageType::Slashing, 6.0f, 1.5f, 1.0f, 0.1f};
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
        d.attacks[0] = {AttackCategory::Ranged, DamageType::Piercing, 5.0f, 8.0f, 1.2f, 0.25f};
        d.attacks[1] = {AttackCategory::Melee, DamageType::Slashing, 3.0f, 1.2f, 0.8f, 0.10f};
    }
    // Grave Robber: mixed -- a hand crossbow opener (high crit) then blades.
    {
        CharacterDesc& d = at(CreatureId::GraveRobber);
        d.archetype = Archetype::Hero;
        d.hero_class = HERO_GRAVE_ROBBER;
        d.hp = 24.0f;
        d.move_speed = 2.6f;
        d.size_x = 0.9f; d.size_y = 1.8f; d.size_z = 0.9f;
        d.color_r = 0.60f; d.color_g = 0.45f; d.color_b = 0.75f;  // violet
        d.vision_radius = 14.0f; d.vision_cone_half_angle_deg = 60.0f;
        d.accuracy = 0.85f; d.evasion = 0.20f; d.defense = 0.10f; d.armour = 1.0f;
        d.stance = CombatStance::Melee;  // closes, but opens with the crossbow
        d.attack_count = 2;
        d.attacks[0] = {AttackCategory::Ranged, DamageType::Piercing, 3.0f, 5.0f, 0.6f, 0.30f};
        d.attacks[1] = {AttackCategory::Melee, DamageType::Slashing, 4.0f, 1.3f, 0.7f, 0.20f};
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
        d.attacks[0] = {AttackCategory::Ranged, DamageType::Piercing, 4.0f, 6.0f, 1.5f, 0.10f};
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
        d.attacks[0] = {AttackCategory::Melee, DamageType::Slashing, 3.0f, 1.0f, 1.0f, 0.05f};
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
        d.attacks[0] = {AttackCategory::Melee, DamageType::Slashing, 3.0f, 1.2f, 0.8f, 0.10f};
        d.xp_reward = 25;
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
