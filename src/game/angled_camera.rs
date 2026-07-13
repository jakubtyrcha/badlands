// Angled (Fallout/Majesty-style) game camera: a fixed down-tilted view with a
// narrow FOV, so the projection stays perspective but reads close to
// orthographic. Looks toward world -Z (north) and down; panned only by
// press-drag along the two ground axes. No zoom, tilt, or rotation.

use glam::{Vec2, Vec3};

use crate::scene::camera::Camera;

pub struct AngledCamera {
    pub center: Vec2, // XZ ground point the view is centered on
    pub pitch_deg: f32, // angle below horizontal
    pub distance: f32, // camera distance from center along the view direction
    pub fov: f32,     // vertical FOV, degrees — narrow for a near-orthographic look
}

impl AngledCamera {
    pub fn new() -> AngledCamera {
        AngledCamera {
            center: Vec2::ZERO,
            pitch_deg: 50.0,
            distance: 95.0,
            fov: 18.0,
        }
    }

    // Normalized look direction: north (-Z) tilted down by `pitch_deg`.
    fn direction(&self) -> Vec3 {
        let p = self.pitch_deg.to_radians();
        Vec3::new(0.0, -p.sin(), -p.cos())
    }

    // World-space eye position: sit back and up along -direction from the ground
    // center. Since direction points down and north, the eye ends up above and
    // to the south (+Z) of the center.
    fn eye(&self) -> Vec3 {
        Vec3::new(self.center.x, 0.0, self.center.y) - self.distance * self.direction()
    }

    pub fn camera(&self, aspect: f32) -> Camera {
        Camera {
            position: self.eye(),
            direction: self.direction(),
            // World up is valid now that the view is no longer parallel to it.
            up: Vec3::Y,
            fov: self.fov,
            aspect,
            near_plane: 0.1,
            far_plane: 1000.0,
        }
    }

    // Map a cursor position (physical pixels, full window) to the world XZ point
    // on the ground plane (y = 0) under it, by casting a ray through the pixel
    // and intersecting y = 0. Works for any tilt. Basis matches glam's
    // right-handed look_at (right = normalize(fwd x up), true_up = right x fwd).
    pub fn screen_to_ground(&self, cursor_px: Vec2, screen_px: Vec2) -> Vec2 {
        let fwd = self.direction();
        let right = fwd.cross(Vec3::Y).normalize();
        let up = right.cross(fwd);
        let half_h = (0.5 * self.fov.to_radians()).tan();
        let half_w = half_h * (screen_px.x / screen_px.y.max(1.0));
        let ndc_x = (cursor_px.x / screen_px.x.max(1.0)) * 2.0 - 1.0;
        let ndc_y = 1.0 - (cursor_px.y / screen_px.y.max(1.0)) * 2.0; // screen y is down
        let dir = fwd + right * (ndc_x * half_w) + up * (ndc_y * half_h);
        let origin = self.eye();
        // origin.y > 0 and the ray points down, so t > 0; clamp dir.y away from
        // zero to guard against near-horizon rays.
        let t = -origin.y / dir.y.min(-1e-4);
        let hit = origin + dir * t;
        Vec2::new(hit.x, hit.z)
    }

    // Pan by a press-drag: keep the ground point that was under the cursor locked
    // as the cursor moves. `screen_to_ground` is affine in `center` (constant eye
    // height and ray direction), so shifting center by the ground delta is exact
    // for any tilt — no per-axis foreshortening fudge.
    pub fn pan(&mut self, prev_px: Vec2, cur_px: Vec2, screen_px: Vec2, clamp_extent: f32) {
        let before = self.screen_to_ground(prev_px, screen_px);
        let after = self.screen_to_ground(cur_px, screen_px);
        self.center += before - after;
        self.center = self.center.clamp(Vec2::splat(-clamp_extent), Vec2::splat(clamp_extent));
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn screen_center_maps_to_camera_center() {
        let mut camera = AngledCamera::new();
        camera.center = Vec2::new(3.0, -2.0);
        let screen = Vec2::new(1600.0, 800.0);

        let ground = camera.screen_to_ground(screen * 0.5, screen);
        assert!(
            (ground - camera.center).length() < 1e-3,
            "screen center should hit the ground center, got {ground:?}"
        );
    }

    #[test]
    fn pan_keeps_grabbed_point_under_cursor() {
        let mut camera = AngledCamera::new();
        let screen = Vec2::new(1600.0, 800.0);

        // Grab an off-center point, then drag the cursor elsewhere.
        let grab_px = Vec2::new(400.0, 300.0);
        let drop_px = Vec2::new(1000.0, 600.0);
        let grabbed = camera.screen_to_ground(grab_px, screen);

        // Large extent so the clamp doesn't interfere with the invariant.
        camera.pan(grab_px, drop_px, screen, 1.0e6);

        // The grabbed ground point should now sit under the new cursor position.
        let under_cursor = camera.screen_to_ground(drop_px, screen);
        assert!(
            (under_cursor - grabbed).length() < 1e-3,
            "grabbed point {grabbed:?} should stay under the cursor, got {under_cursor:?}"
        );
    }
}
