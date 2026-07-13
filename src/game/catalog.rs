// Render-only building catalog: display name, box height, and albedo tint per
// kind. Footprint sizes, placement rules, and all gameplay state live in the
// C++ sim (game/src/placement.cpp) — this is purely how a building looks.

use glam::Vec3;

use crate::game_ffi::BuildingKind;

pub struct BuildingInfo {
    pub name: &'static str,
    pub height: f32,
    pub color: Vec3,
}

pub fn info(kind: BuildingKind) -> BuildingInfo {
    use BuildingKind::*;
    match kind {
        Castle => BuildingInfo {
            name: "Castle",
            height: 5.0,
            color: Vec3::new(0.55, 0.50, 0.45),
        },
        FreeCompanyQuarters => BuildingInfo {
            name: "Free Company Quarters",
            height: 3.0,
            color: Vec3::new(0.35, 0.42, 0.60),
        },
        HuntersCamp => BuildingInfo {
            name: "Hunter's Camp",
            height: 2.6,
            color: Vec3::new(0.35, 0.50, 0.32),
        },
        ThievesDen => BuildingInfo {
            name: "Thieves' Den",
            height: 2.6,
            color: Vec3::new(0.44, 0.33, 0.52),
        },
        Scriptorium => BuildingInfo {
            name: "Scriptorium",
            height: 3.0,
            color: Vec3::new(0.62, 0.55, 0.40),
        },
        Tavern => BuildingInfo {
            name: "Tavern",
            height: 2.2,
            color: Vec3::new(0.60, 0.38, 0.30),
        },
        Apothecary => BuildingInfo {
            name: "Apothecary",
            height: 2.2,
            color: Vec3::new(0.30, 0.55, 0.52),
        },
        Watchtower => BuildingInfo {
            name: "Watchtower",
            height: 4.0,
            color: Vec3::new(0.60, 0.60, 0.62),
        },
        House => BuildingInfo {
            name: "House",
            height: 1.8,
            color: Vec3::new(0.55, 0.45, 0.38),
        },
        Sewer => BuildingInfo {
            name: "Sewer Grate",
            height: 0.4,
            color: Vec3::new(0.28, 0.28, 0.30),
        },
    }
}
