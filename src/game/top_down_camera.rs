// Birds-eye camera for the MVP: fixed height, looking straight down, panned
// only by press-drag (touchpad press). No zoom, tilt, or rotation.

use glam::{Vec2, Vec3};

use crate::scene::camera::Camera;

pub struct TopDownCamera {
    pub center: Vec2, // XZ point the camera hovers over
    pub height: f32,
    pub fov: f32, // vertical, degrees
}

impl TopDownCamera {
    pub fn new() -> TopDownCamera {
        TopDownCamera {
            center: Vec2::ZERO,
            height: 40.0,
            fov: 45.0,
        }
    }

    pub fn camera(&self, aspect: f32) -> Camera {
        Camera {
            position: Vec3::new(self.center.x, self.height, self.center.y),
            direction: Vec3::NEG_Y,
            // Straight-down look: screen-up points toward world -Z (north).
            up: Vec3::NEG_Z,
            fov: self.fov,
            aspect,
            near_plane: 0.1,
            far_plane: 1000.0,
        }
    }

    // Pan by a cursor delta in physical pixels so the map follows the pointer:
    // world units visible vertically = 2 * height * tan(fov/2).
    pub fn pan(&mut self, delta_px: Vec2, screen_height_px: f32, clamp_extent: f32) {
        let world_per_px =
            2.0 * self.height * (0.5 * self.fov.to_radians()).tan() / screen_height_px.max(1.0);
        // Screen right = +X, screen down = +Z (up vector is -Z).
        self.center -= delta_px * world_per_px;
        self.center = self.center.clamp(Vec2::splat(-clamp_extent), Vec2::splat(clamp_extent));
    }
}
