// Game state for the MVP: player gold and a handful of placed "buildings"
// (plain boxes) on a small flat map. No simulation yet.

use glam::{Vec2, Vec3};

pub struct Building {
    pub pos: Vec2,   // XZ center of the footprint
    pub size: Vec3,  // footprint x/z, height y
    pub color: Vec3, // linear albedo tint
}

pub struct World {
    pub gold: u32,
    pub ground_half_extent: f32,
    // How far the camera center may pan from the origin (keeps the map edge
    // mostly off-screen).
    pub pan_extent: f32,
    pub buildings: Vec<Building>,
}

impl World {
    pub fn new() -> World {
        World {
            gold: 1000,
            ground_half_extent: 48.0,
            pan_extent: 14.0,
            buildings: vec![
                // A keep in the middle and a few houses around it.
                Building {
                    pos: Vec2::new(0.0, 0.0),
                    size: Vec3::new(6.0, 5.0, 6.0),
                    color: Vec3::new(0.55, 0.50, 0.45),
                },
                Building {
                    pos: Vec2::new(-10.0, -6.0),
                    size: Vec3::new(3.0, 2.5, 3.5),
                    color: Vec3::new(0.62, 0.42, 0.30),
                },
                Building {
                    pos: Vec2::new(9.0, -8.0),
                    size: Vec3::new(3.5, 2.0, 3.0),
                    color: Vec3::new(0.45, 0.48, 0.60),
                },
                Building {
                    pos: Vec2::new(12.0, 6.0),
                    size: Vec3::new(2.5, 3.0, 2.5),
                    color: Vec3::new(0.60, 0.55, 0.35),
                },
                Building {
                    pos: Vec2::new(-8.0, 10.0),
                    size: Vec3::new(4.0, 2.2, 3.0),
                    color: Vec3::new(0.50, 0.35, 0.35),
                },
                Building {
                    pos: Vec2::new(-16.0, 2.0),
                    size: Vec3::new(2.0, 4.0, 2.0),
                    color: Vec3::new(0.40, 0.52, 0.42),
                },
            ],
        }
    }
}
