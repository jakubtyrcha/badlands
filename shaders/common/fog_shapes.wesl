// Analytic fog test shapes (Task: fog rendering). Pure functions, no bindings —
// imported by compute/fog_fill.wesl. These are placeholder test content: a few
// world-space shapes scattered on the map, evaluated per voxel. This is the seam
// a real fog simulation replaces (it would write the media 3D texture directly).
//
// Density is unitless in [0, 1]; density 1 maps (in fog_fill) to an extinction
// high enough to fully take over the pixel colour. Shapes are combined by union
// (max). All shapes live inside the fixed 0..64 m band.

fn fogHash3(p: vec3<f32>) -> f32 {
    let h = dot(p, vec3<f32>(127.1, 311.7, 74.7));
    return fract(sin(h) * 43758.5453123);
}

// Trilinear 3D value noise in [0, 1].
fn fogValueNoise3(p: vec3<f32>) -> f32 {
    let i = floor(p);
    let f = fract(p);
    let u = f * f * (3.0 - 2.0 * f);
    let c000 = fogHash3(i + vec3<f32>(0.0, 0.0, 0.0));
    let c100 = fogHash3(i + vec3<f32>(1.0, 0.0, 0.0));
    let c010 = fogHash3(i + vec3<f32>(0.0, 1.0, 0.0));
    let c110 = fogHash3(i + vec3<f32>(1.0, 1.0, 0.0));
    let c001 = fogHash3(i + vec3<f32>(0.0, 0.0, 1.0));
    let c101 = fogHash3(i + vec3<f32>(1.0, 0.0, 1.0));
    let c011 = fogHash3(i + vec3<f32>(0.0, 1.0, 1.0));
    let c111 = fogHash3(i + vec3<f32>(1.0, 1.0, 1.0));
    let x00 = mix(c000, c100, u.x);
    let x10 = mix(c010, c110, u.x);
    let x01 = mix(c001, c101, u.x);
    let x11 = mix(c011, c111, u.x);
    let y0 = mix(x00, x10, u.y);
    let y1 = mix(x01, x11, u.y);
    return mix(y0, y1, u.z);
}

// Union of the test shapes at world position P; returns density in [0, 1].
fn fogShapesDensity(P: vec3<f32>) -> f32 {
    var d = 0.0;

    // Shapes spread in a row across the map (z = -25) so each is distinct.

    // 1. Sphere: radius 16, uniform density 1.
    {
        if (length(P - vec3<f32>(-45.0, 16.0, -25.0)) <= 16.0) {
            d = max(d, 1.0);
        }
    }

    // 2. Box 32x32x32 (y 0..32), density falling linearly with height
    //    (1 at the base, 0 at the top).
    {
        let c = vec3<f32>(-15.0, 16.0, -25.0);
        let q = abs(P - c);
        if (q.x <= 16.0 && q.y <= 16.0 && q.z <= 16.0) {
            d = max(d, clamp(1.0 - P.y / 32.0, 0.0, 1.0));
        }
    }

    // 3. Box 32x32x32 filled with 3D noise, density in [0, 1].
    {
        let c = vec3<f32>(15.0, 16.0, -25.0);
        let q = abs(P - c);
        if (q.x <= 16.0 && q.y <= 16.0 && q.z <= 16.0) {
            d = max(d, clamp(fogValueNoise3(P * 0.15), 0.0, 1.0));
        }
    }

    // 4. Cylinder: radius 8, full band height (0..64), density falling
    //    radially from 1 at the centre to 0 at the rim.
    {
        let c = vec2<f32>(45.0, -25.0);
        let rr = length(P.xz - c);
        if (rr <= 8.0 && P.y >= 0.0 && P.y <= 64.0) {
            d = max(d, 1.0 - rr / 8.0);
        }
    }

    return d;
}
