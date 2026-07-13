// Render/UI-only hero-class catalog: display name + tint per class, and the
// guild->class map. Mirrors game/catalog.rs. HeroClass never crosses the C API
// (the sim tints heroes by class at spawn); the panel derives a hero's class
// name from its home guild's kind via guild_class().

use glam::Vec3;

use crate::game_ffi::BuildingKind;

// Mirror of the C++ HeroClassId (game/src/heroes.h). Decorative in v0.3.
#[repr(i32)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum HeroClass {
    Mercenary = 0,
    Hunter = 1,
    GraveRobber = 2,
    Apprentice = 3,
}

impl HeroClass {
    pub fn from_i32(value: i32) -> Option<HeroClass> {
        match value {
            0 => Some(HeroClass::Mercenary),
            1 => Some(HeroClass::Hunter),
            2 => Some(HeroClass::GraveRobber),
            3 => Some(HeroClass::Apprentice),
            _ => None,
        }
    }
}

pub struct ClassInfo {
    pub name: &'static str,
    pub color: Vec3,
}

// Matches the spawn tint in heroes.cpp::hero_desc so the panel label and the
// rendered hero cube read as the same class.
pub fn info(class: HeroClass) -> ClassInfo {
    match class {
        HeroClass::Mercenary => ClassInfo {
            name: "Mercenary",
            color: Vec3::new(0.35, 0.45, 0.80),
        },
        HeroClass::Hunter => ClassInfo {
            name: "Hunter",
            color: Vec3::new(0.30, 0.70, 0.35),
        },
        HeroClass::GraveRobber => ClassInfo {
            name: "Grave Robber",
            color: Vec3::new(0.60, 0.45, 0.75),
        },
        HeroClass::Apprentice => ClassInfo {
            name: "Apprentice",
            color: Vec3::new(0.45, 0.78, 0.85),
        },
    }
}

// The class a guild recruits, or None for non-guild buildings.
pub fn guild_class(kind: BuildingKind) -> Option<HeroClass> {
    match kind {
        BuildingKind::FreeCompanyQuarters => Some(HeroClass::Mercenary),
        BuildingKind::HuntersCamp => Some(HeroClass::Hunter),
        BuildingKind::ThievesDen => Some(HeroClass::GraveRobber),
        BuildingKind::Scriptorium => Some(HeroClass::Apprentice),
        _ => None,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn hero_class_from_i32_round_trips_and_rejects() {
        assert_eq!(HeroClass::from_i32(0), Some(HeroClass::Mercenary));
        assert_eq!(HeroClass::from_i32(3), Some(HeroClass::Apprentice));
        assert_eq!(HeroClass::from_i32(4), None);
        assert_eq!(HeroClass::from_i32(-1), None);
    }

    #[test]
    fn guild_class_maps_the_four_guilds() {
        assert_eq!(guild_class(BuildingKind::FreeCompanyQuarters), Some(HeroClass::Mercenary));
        assert_eq!(guild_class(BuildingKind::HuntersCamp), Some(HeroClass::Hunter));
        assert_eq!(guild_class(BuildingKind::ThievesDen), Some(HeroClass::GraveRobber));
        assert_eq!(guild_class(BuildingKind::Scriptorium), Some(HeroClass::Apprentice));
        assert_eq!(guild_class(BuildingKind::Tavern), None);
        assert_eq!(guild_class(BuildingKind::Castle), None);
    }
}
