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

    // World units per physical pixel for this straight-down camera (square
    // pixels, so the same factor applies on both axes).
    fn world_per_px(&self, screen_height_px: f32) -> f32 {
        2.0 * self.height * (0.5 * self.fov.to_radians()).tan() / screen_height_px.max(1.0)
    }

    // Pan by a cursor delta in physical pixels so the map follows the pointer.
    pub fn pan(&mut self, delta_px: Vec2, screen_height_px: f32, clamp_extent: f32) {
        // Screen right = +X, screen down = +Z (up vector is -Z).
        self.center -= delta_px * self.world_per_px(screen_height_px);
        self.center = self.center.clamp(Vec2::splat(-clamp_extent), Vec2::splat(clamp_extent));
    }

    // Map a cursor position (physical pixels, full window) to the world XZ point
    // on the ground plane (y = 0) under it — the exact inverse of the pan/pixel
    // mapping. Screen center = camera center; screen right = +X, down = +Z.
    pub fn screen_to_ground(&self, cursor_px: Vec2, screen_px: Vec2) -> Vec2 {
        let world_per_px = self.world_per_px(screen_px.y);
        self.center + (cursor_px - screen_px * 0.5) * world_per_px
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn screen_to_ground_maps_center_and_offsets() {
        let mut camera = TopDownCamera::new();
        camera.center = Vec2::new(3.0, -2.0);
        let screen = Vec2::new(1600.0, 800.0);

        // The screen center resolves to the camera center.
        let center = camera.screen_to_ground(screen * 0.5, screen);
        assert!((center - camera.center).length() < 1e-4);

        // A cursor 100 px right and 50 px down moves by that many world units.
        let wpp = camera.world_per_px(screen.y);
        let off = camera.screen_to_ground(screen * 0.5 + Vec2::new(100.0, 50.0), screen);
        assert!((off - (camera.center + Vec2::new(100.0, 50.0) * wpp)).length() < 1e-4);
    }
}
