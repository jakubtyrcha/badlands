// Projected decals — a fullscreen pass that stamps world-space outline decals
// onto the scene by reconstructing each pixel's world position from the
// G-buffer depth (the same trick as game/vision.wesl).
//
// For each decal: reject the pixel unless the reconstructed surface lies within
// the decal's vertical projector band, un-rotate it into the decal's local XZ
// frame, evaluate the shape's outline SDF, and blend a dashed line. The result
// is accumulated premultiplied and composited by the pipeline's
// One / OneMinusSrcAlpha blend, so N decals resolve in one pass.
//
// KEEP IN SYNC WITH src/engine/rendering/decal_math.hpp — every math helper
// below is a 1:1 transcription of that header, which is what
// src/engine/tests/decal_math_tests.cpp pins. WESL and C++ can't share code;
// if an expression changes here it must change there (and vice versa).

import package::common::frame::{frame, getFullscreenTriangleVertex, isValidDepth, reconstructWorldSpacePosition, reconstructLinearZ, getTanHalfFov, decodeOctahedron};

@group(0) @binding(1) var depth_texture: texture_depth_2d;    // reversed-Z
@group(0) @binding(2) var normals_texture: texture_2d<f32>;   // RG16Float octahedron

const PI: f32 = 3.14159265358979323846;
const TWO_PI: f32 = 6.28318530717958647692;
const HALF_PI: f32 = 1.57079632679489661923;

const SHAPE_RING: f32 = 0.0;
const MAX_DECALS: u32 = 32u;

struct DecalData {
    center_yaw: vec4<f32>,          // xyz = centre (world), w = yaw (radians)
    extents_band_width: vec4<f32>,  // xy = half extents, z = projector half height, w = line width
    dash: vec4<f32>,                // x = corner radius, y = dash length, z = dash gap, w = scroll speed
    shape: vec4<f32>,               // x = DecalShape as f32, y/z = receiver normal-Y fade band; w reserved
    color_a: vec4<f32>,
    color_b: vec4<f32>,
}

struct DecalUniforms {
    count_time: vec4<f32>,  // x = decal count, y = time (seconds); zw reserved
    decals: array<DecalData, MAX_DECALS>,
}
@group(0) @binding(3) var<uniform> decal_uniforms: DecalUniforms;

// === Shape SDFs (mirror: decal_math.hpp) ===================================

fn ringOutlineDistance(local: vec2<f32>, radius: f32) -> f32 {
    return abs(length(local) - radius);
}

fn roundedRectSignedDistance(local: vec2<f32>, halfExtents: vec2<f32>, radius: f32) -> f32 {
    let r = clamp(radius, 0.0, min(halfExtents.x, halfExtents.y));
    let q = abs(local) - (halfExtents - vec2<f32>(r));
    let qPos = vec2<f32>(max(q.x, 0.0), max(q.y, 0.0));
    return length(qPos) + min(max(q.x, q.y), 0.0) - r;
}

// === Perimeters ============================================================

fn roundedRectPerimeter(halfExtents: vec2<f32>, radius: f32) -> f32 {
    let r = clamp(radius, 0.0, min(halfExtents.x, halfExtents.y));
    let b = max(halfExtents - vec2<f32>(r), vec2<f32>(0.0));
    return 4.0 * b.x + 4.0 * b.y + TWO_PI * r;
}

// === Perimeter parameterisation ============================================
// Both run counter-clockwise from s = 0 on the +X axis.

fn ringPerimeterParam(local: vec2<f32>, radius: f32) -> f32 {
    var angle = atan2(local.y, local.x);
    if (angle < 0.0) {
        angle = angle + TWO_PI;
    }
    return angle * max(radius, 0.0);
}

fn roundedRectPerimeterParam(local: vec2<f32>, halfExtents: vec2<f32>, radius: f32) -> f32 {
    let r = clamp(radius, 0.0, min(halfExtents.x, halfExtents.y));
    let b = max(halfExtents - vec2<f32>(r), vec2<f32>(0.0));
    let arc = r * HALF_PI;
    let perimeter = 4.0 * b.x + 4.0 * b.y + TWO_PI * r;

    let sRightUpper = b.y;
    let sCornerPP = sRightUpper + arc;
    let sTop = sCornerPP + 2.0 * b.x;
    let sCornerMP = sTop + arc;
    let sLeft = sCornerMP + 2.0 * b.y;
    let sCornerMM = sLeft + arc;
    let sBottom = sCornerMM + 2.0 * b.x;

    // Inclusive comparisons: at radius 0 the outline IS the inner box boundary,
    // so strict tests would drop the whole sharp outline into the interior case.
    let right = local.x >= b.x;
    let left = local.x <= -b.x;
    let up = local.y >= b.y;
    let down = local.y <= -b.y;

    if (right && up) {          // +X+Y arc: 0 -> pi/2
        let d = local - b;
        let theta = atan2(d.y, d.x);
        return sRightUpper + r * clamp(theta, 0.0, HALF_PI);
    }
    if (left && up) {           // -X+Y arc: pi/2 -> pi
        let d = local - vec2<f32>(-b.x, b.y);
        let theta = atan2(d.y, d.x);
        return sTop + r * (clamp(theta, HALF_PI, PI) - HALF_PI);
    }
    if (left && down) {         // -X-Y arc: pi -> 3pi/2
        let d = local - vec2<f32>(-b.x, -b.y);
        var theta = atan2(d.y, d.x);
        if (theta < 0.0) {
            theta = theta + TWO_PI;
        }
        return sLeft + r * (clamp(theta, PI, PI + HALF_PI) - PI);
    }
    if (right && down) {        // +X-Y arc: 3pi/2 -> 2pi
        let d = local - vec2<f32>(b.x, -b.y);
        var theta = atan2(d.y, d.x);
        // This arc ends at the wrap seam, where atan2 returns 0 rather than
        // 2*pi. d.y <= 0 throughout this branch, so shifting on <= 0 maps the
        // seam to 2*pi and makes this agree with the +X-edge branch below.
        if (theta <= 0.0) {
            theta = theta + TWO_PI;
        }
        return sBottom + r * (clamp(theta, PI + HALF_PI, TWO_PI) - (PI + HALF_PI));
    }
    if (right) {                // +X edge, wrapping through s = 0
        if (local.y >= 0.0) {
            return local.y;
        }
        return perimeter + local.y;
    }
    if (up) {                   // +Y edge, running -X-ward
        return sCornerPP + (b.x - local.x);
    }
    if (left) {                 // -X edge, running -Y-ward
        return sCornerMP + (b.y - local.y);
    }
    if (down) {                 // -Y edge, running +X-ward
        return sCornerMM + (local.x + b.x);
    }
    return 0.0;                 // interior: no outline here
}

// === Dash pattern ==========================================================

// Snap the period to a whole number of repeats around the closed curve, so the
// pattern has no seam where the parameter wraps.
//
// floor(x + 0.5), NOT round(): WGSL's round() breaks ties to even while C++'s
// std::round breaks them away from zero, so round() here would disagree with
// the decal_math.hpp mirror on exactly-.5 repeat counts.
fn fitDashPeriod(period: f32, perimeter: f32) -> f32 {
    if (period <= 0.0 || perimeter <= 0.0) {
        return period;
    }
    let n = max(1.0, floor(perimeter / period + 0.5));
    return perimeter / n;
}

fn dashDuty(dashLength: f32, dashGap: f32) -> f32 {
    let period = dashLength + dashGap;
    if (period <= 0.0) {
        return 1.0;
    }
    return clamp(dashLength / period, 0.0, 1.0);
}

fn isDashA(s: f32, period: f32, duty: f32, time: f32, scroll: f32) -> bool {
    if (period <= 0.0 || duty >= 1.0) {
        return true;
    }
    if (duty <= 0.0) {
        return false;
    }
    let phase = fract((s - time * scroll * period) / period);
    return phase < duty;
}

// === Coverage ==============================================================

fn outlineCoverage(outlineDistance: f32, halfWidth: f32, aa: f32) -> f32 {
    let e = max(aa, 1e-6);
    return 1.0 - smoothstep(halfWidth - e, halfWidth + e, outlineDistance);
}

// How much a receiving surface accepts a decal, from its world normal's Y.
// Mirrors decal_math.hpp's Smoothstep guard: a degenerate band (hi <= lo)
// becomes a hard step at `lo` instead of dividing by zero.
fn receiverFactor(normalY: f32, lo: f32, hi: f32) -> f32 {
    if (hi <= lo) {
        return select(1.0, 0.0, normalY < lo);
    }
    return smoothstep(lo, hi, normalY);
}

// ===========================================================================

struct VertexOutput {
    @builtin(position) Position: vec4<f32>,
    @location(0) screenUV: vec2<f32>,
}

@vertex
fn vs_main(@builtin(vertex_index) VertexIndex: u32) -> VertexOutput {
    let fullscreen = getFullscreenTriangleVertex(VertexIndex);
    var output: VertexOutput;
    output.Position = fullscreen.clipPosition;
    output.screenUV = fullscreen.screenUV;
    return output;
}

@fragment
fn fs_main(input: VertexOutput) -> @location(0) vec4<f32> {
    let coord = vec2<i32>(input.Position.xy);
    let depth = textureLoad(depth_texture, coord, 0);
    if (!isValidDepth(depth)) {
        return vec4<f32>(0.0);  // sky: nothing to project onto
    }

    let world = reconstructWorldSpacePosition(input.screenUV, depth);

    // Antialiasing width in WORLD units, derived analytically rather than with
    // fwidth(): a derivative here would sit in non-uniform control flow (after
    // the sky early-out), which is invalid. The view is 2*z*tan(fovY/2) world
    // units tall at this depth, spread over screenSize.y pixels.
    let aa = reconstructLinearZ(depth) * getTanHalfFov() * 2.0 / max(frame.screenSize.y, 1.0);

    // Receiving surface orientation. Decals must not smear down walls or up the
    // sides of units: the vertical projector band alone cannot reject a wall
    // that passes through it. The accepted band is PER DECAL (shape.y/.z), so a
    // ground highlight and a future wall decal can coexist in one pass.
    let normal = decodeOctahedron(textureLoad(normals_texture, coord, 0).rg);

    let count = min(u32(decal_uniforms.count_time.x), MAX_DECALS);
    let time = decal_uniforms.count_time.y;

    var accum = vec4<f32>(0.0);
    for (var i: u32 = 0u; i < count; i = i + 1u) {
        let d = decal_uniforms.decals[i];

        let center = d.center_yaw.xyz;
        let band = d.extents_band_width.z;
        if (abs(world.y - center.y) > band) {
            continue;  // outside this decal's projector volume
        }

        let receiver = receiverFactor(normal.y, d.shape.y, d.shape.z);
        if (receiver <= 0.0) {
            continue;  // this surface does not accept this decal
        }

        // World XZ -> decal-local XZ. Rotation about +Y by `yaw` maps local to
        // world as (x*cos + z*sin, -x*sin + z*cos), so the inverse applied to
        // (rel.x, rel.z) is the standard 2D rotation by +yaw.
        let yaw = d.center_yaw.w;
        let rel = world.xz - center.xz;
        let cs = cos(yaw);
        let sn = sin(yaw);
        let local = vec2<f32>(rel.x * cs - rel.y * sn, rel.x * sn + rel.y * cs);

        let halfExtents = d.extents_band_width.xy;
        let lineWidth = d.extents_band_width.w;
        let cornerRadius = d.dash.x;

        // Distance to the outline first -- it is cheap. The perimeter
        // parameterisation below costs an atan2 and a segment search, so it is
        // only worth computing for the few pixels the line actually covers.
        let isRing = d.shape.x == SHAPE_RING;
        var outlineDistance: f32;
        if (isRing) {
            outlineDistance = ringOutlineDistance(local, halfExtents.x);
        } else {
            outlineDistance = abs(roundedRectSignedDistance(local, halfExtents, cornerRadius));
        }

        let coverage = outlineCoverage(outlineDistance, 0.5 * lineWidth, aa) * receiver;
        if (coverage <= 0.0) {
            continue;
        }

        var s: f32;
        var perimeter: f32;
        if (isRing) {
            s = ringPerimeterParam(local, halfExtents.x);
            perimeter = TWO_PI * max(halfExtents.x, 0.0);
        } else {
            s = roundedRectPerimeterParam(local, halfExtents, cornerRadius);
            perimeter = roundedRectPerimeter(halfExtents, cornerRadius);
        }

        let dashLength = d.dash.y;
        let dashGap = d.dash.z;
        let period = fitDashPeriod(dashLength + dashGap, perimeter);
        let duty = dashDuty(dashLength, dashGap);
        var color = d.color_b;
        if (isDashA(s, period, duty, time, d.dash.w)) {
            color = d.color_a;
        }

        // Premultiplied "over": later decals composite under the coverage
        // already accumulated.
        let srcA = coverage * color.a;
        accum = accum + vec4<f32>(color.rgb * srcA, srcA) * (1.0 - accum.a);
    }

    return accum;
}
