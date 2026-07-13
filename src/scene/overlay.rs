// Builds the placement-grid overlay: one flat, shrunk triangle per probe cell,
// colored by occupancy. Rebuilt every frame from the sim's probe readout and
// drawn by SceneRenderer with the game/overlay pipeline (pos3 + color4 verts).

use glam::Vec2;

use crate::game_ffi::GameGridTriangle;

// Lifted just above the ground quad (y = 0) to avoid z-fighting.
const OVERLAY_Y: f32 = 0.02;
// Fraction each triangle keeps after shrinking toward its centroid (the gap).
const FILL: f32 = 0.9;

// Linear, straight-alpha colors for the ALPHA_BLENDING overlay pipeline.
const COLOR_FREE: [f32; 4] = [0.30, 0.80, 0.35, 0.35]; // pale green
const COLOR_BLOCKED: [f32; 4] = [0.55, 0.08, 0.06, 0.55]; // dark red
const COLOR_WOULD_BLOCK: [f32; 4] = [0.25, 0.25, 0.28, 0.55]; // dark gray

// The three world-XZ corners of an X-split triangle (before shrinking).
fn triangle_corners(tri: &GameGridTriangle) -> [Vec2; 3] {
    let tx = tri.tile_x as f32;
    let tz = tri.tile_z as f32;
    let center = Vec2::new(tx + 0.5, tz + 0.5);
    let (a, b) = match tri.corner {
        0 => (Vec2::new(tx, tz), Vec2::new(tx + 1.0, tz)),               // N (-Z)
        1 => (Vec2::new(tx + 1.0, tz), Vec2::new(tx + 1.0, tz + 1.0)),   // E (+X)
        2 => (Vec2::new(tx + 1.0, tz + 1.0), Vec2::new(tx, tz + 1.0)),   // S (+Z)
        _ => (Vec2::new(tx, tz + 1.0), Vec2::new(tx, tz)),               // W (-X)
    };
    [a, b, center]
}

fn color_for(state: u32) -> [f32; 4] {
    match state {
        1 => COLOR_BLOCKED,
        2 => COLOR_WOULD_BLOCK,
        _ => COLOR_FREE,
    }
}

// Interleaved pos(3) + color(4) vertices; 3 per triangle. Empty when there is
// nothing to draw.
pub fn build_vertices(triangles: &[GameGridTriangle]) -> Vec<f32> {
    let mut verts = Vec::with_capacity(triangles.len() * 3 * 7);
    for tri in triangles {
        let corners = triangle_corners(tri);
        let centroid = (corners[0] + corners[1] + corners[2]) / 3.0;
        let color = color_for(tri.state);
        for corner in corners {
            let p = centroid + (corner - centroid) * FILL;
            verts.extend_from_slice(&[p.x, OVERLAY_Y, p.y]);
            verts.extend_from_slice(&color);
        }
    }
    verts
}
