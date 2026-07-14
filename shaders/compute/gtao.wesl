// Ground Truth Ambient Occlusion (XeGTAO variant)
// Compute shader for screen-space ambient occlusion
//
// References:
// - Intel XeGTAO: github.com/GameTechDev/XeGTAO
// - Three.js WebGPU GTAO Node
//
// Algorithm: For each pixel, trace rays in N directions (slices) around the pixel,
// tracking the maximum horizon angle. Visibility is computed by integrating the
// horizon angles weighted by the surface normal.

// Import shared frame utilities (FrameUniforms, depth/position reconstruction, etc.)
// WESL requires imports before enable directives
import package::common::frame::{
    FrameUniforms,
    frame,
    isSkyDepth,
    reconstructLinearZ,
    getTanHalfFov,
    getAspect,
    getRayDirectionInViewSpace,
    reconstructViewSpacePosition,
    decodeOctahedron,
    worldSpaceToViewSpaceDirection
};

// === Bind Group 1: GTAO-specific resources ===
@group(1) @binding(0) var depth_texture: texture_depth_2d;
@group(1) @binding(1) var normals_texture: texture_2d<f32>;
// R8Unorm storage texture (Tier 2 format) - efficient single-channel AO
@group(1) @binding(2) var ao_output: texture_storage_2d<r8unorm, write>;

// Algorithm parameters
const NUM_DIRECTIONS: u32 = 4u;   // XeGTAO uses 4 slices
const NUM_STEPS: u32 = 6u;        // Steps per direction
const RADIUS: f32 = 2.0;          // World-space search radius
const THICKNESS: f32 = 0.5;       // Thickness heuristic for thin geometry
const PI: f32 = 3.14159265359;

// === GTAO-specific functions ===

// Interleaved gradient noise for per-pixel direction rotation
// Converts banding artifacts to high-frequency noise (easier to blur)
fn interleavedGradientNoise(pixel: vec2<f32>) -> f32 {
    return fract(52.9829189 * fract(0.06711056 * pixel.x + 0.00583715 * pixel.y));
}

// Fast inverse square root approximation
fn fastRsqrt(x: f32) -> f32 {
    return 1.0 / sqrt(max(x, 0.0001));
}

// Compute horizon angle from origin position to sample position
// Returns sin(angle) where angle is elevation from horizontal in view space
// Positive angle = sample is CLOSER to camera (potential occluder)
// Negative angle = sample is FURTHER from camera (not an occluder)
fn computeHorizonAngle(originVS: vec3<f32>, sampleVS: vec3<f32>) -> f32 {
    let delta = sampleVS - originVS;
    let horizontalDistSq = delta.x * delta.x + delta.y * delta.y;
    // In view space, -Z is forward (into screen). Sample closer to camera has larger Z.
    // delta.z > 0 means sample is closer (positive angle = occluding)
    // delta.z < 0 means sample is further (negative angle = not occluding)
    let verticalDist = delta.z;  // FIXED: removed negation

    let lengthSq = horizontalDistSq + verticalDist * verticalDist;
    if (lengthSq < 0.0001) {
        return -1.0;  // Degenerate case
    }
    return verticalDist * fastRsqrt(lengthSq);
}

// Integrate visibility for a single slice given the horizon angle and surface normal
fn integrateSlice(horizonSin: f32, normalVS: vec3<f32>, sliceDir: vec2<f32>) -> f32 {
    // Project normal onto the slice plane
    let normalDotSlice = dot(normalVS.xy, sliceDir);

    // Visibility is the integral of cos(theta) from the horizon to the normal
    let baseVisibility = saturate((1.0 - horizonSin) * 0.5);

    // Weight by how much the normal faces this direction
    let normalWeight = saturate(normalDotSlice * 0.5 + 0.5);

    return mix(baseVisibility, 1.0, 1.0 - normalWeight);
}

@compute @workgroup_size(8, 8, 1)
fn main(@builtin(global_invocation_id) global_id: vec3<u32>) {
    let pixel_coord = vec2<i32>(global_id.xy);
    let screen_size = vec2<i32>(frame.screenSize);

    // Bounds check
    if (any(pixel_coord >= screen_size)) {
        return;
    }

    let inv_screen_size = 1.0 / frame.screenSize;
    let uv = (vec2<f32>(pixel_coord) + 0.5) * inv_screen_size;

    // Sample depth
    let depth = textureLoad(depth_texture, pixel_coord, 0);

    // Sky pixels: output fully lit (no occlusion)
    if (isSkyDepth(depth)) {
        textureStore(ao_output, pixel_coord, vec4<f32>(1.0));
        return;
    }

    // Reconstruct view-space position
    let posVS = reconstructViewSpacePosition(uv, depth);

    // Get world-space normal (decode from octahedron)
    let normalOct = textureLoad(normals_texture, pixel_coord, 0).xy;
    let normalWS = decodeOctahedron(normalOct);

    // Transform normal to view space
    let normalVS = worldSpaceToViewSpaceDirection(normalWS);

    // Per-pixel noise for direction rotation (Option B: converts banding to noise)
    let noise = interleavedGradientNoise(vec2<f32>(pixel_coord));
    let angleOffset = noise * (PI * 2.0 / f32(NUM_DIRECTIONS));

    // Calculate screen-space step size based on world-space radius
    let radiusVS = vec3<f32>(RADIUS, 0.0, posVS.z);
    let radiusClip = frame.projMatrix * vec4<f32>(radiusVS, 1.0);
    let originClip = frame.projMatrix * vec4<f32>(0.0, 0.0, posVS.z, 1.0);
    let screenRadius = abs(radiusClip.x / radiusClip.w - originClip.x / originClip.w) * 0.5;

    // Clamp to reasonable screen extent
    let maxScreenRadius = min(screenRadius, 0.2);  // Max 20% of screen
    let minScreenRadius = 2.0 * inv_screen_size.x;  // At least 2 pixels
    let effectiveRadius = max(maxScreenRadius, minScreenRadius);

    // === Horizon Search ===
    var totalVisibility = 0.0;

    for (var d = 0u; d < NUM_DIRECTIONS; d++) {
        let angle = (f32(d) + 0.5) * (PI * 2.0 / f32(NUM_DIRECTIONS)) + angleOffset;
        let dirScreen = vec2<f32>(cos(angle), sin(angle));

        // Track maximum horizon angle for this direction
        var maxHorizonSin: f32 = -1.0;  // Start below horizon

        for (var s = 1u; s <= NUM_STEPS; s++) {
            let t = f32(s) / f32(NUM_STEPS);
            let stepDist = t * effectiveRadius;

            // Sample position in screen space
            let sampleUV = uv + dirScreen * stepDist;

            // Bounds check
            if (any(sampleUV < vec2<f32>(0.0)) || any(sampleUV > vec2<f32>(1.0))) {
                break;
            }

            // Sample depth at this position
            let sampleCoord = vec2<i32>(sampleUV * frame.screenSize);
            let sampleDepth = textureLoad(depth_texture, sampleCoord, 0);

            // Skip sky samples
            if (isSkyDepth(sampleDepth)) {
                continue;
            }

            // Reconstruct view-space position of sample
            let samplePosVS = reconstructViewSpacePosition(sampleUV, sampleDepth);

            // Thickness check: reject samples that are too far behind the origin
            let depthDiff = -samplePosVS.z - (-posVS.z);
            if (depthDiff > THICKNESS) {
                continue;
            }

            // Get sample's normal and check if it's compatible with the origin normal
            // For a convex surface, nearby samples have compatible normals (facing same general direction)
            // Only incompatible normals (facing opposite directions) indicate potential occlusion
            let sampleNormalOct = textureLoad(normals_texture, sampleCoord, 0).xy;
            let sampleNormalWS = decodeOctahedron(sampleNormalOct);
            let sampleNormalVS = worldSpaceToViewSpaceDirection(sampleNormalWS);

            // Check normal compatibility: if sample normal faces similar direction as origin normal,
            // they're on the same convex surface and shouldn't cause self-occlusion
            let normalCompatibility = dot(normalVS, sampleNormalVS);
            if (normalCompatibility > 0.3) {
                // Compatible normals = same smooth surface, no self-occlusion
                continue;
            }

            // Compute horizon angle - only samples CLOSER to camera (positive angle)
            // contribute to occlusion. Samples further away (negative angle) are ignored.
            let horizonSin = computeHorizonAngle(posVS, samplePosVS);
            maxHorizonSin = max(maxHorizonSin, horizonSin);
        }

        // Integrate visibility for this slice
        let sliceVisibility = integrateSlice(maxHorizonSin, normalVS, dirScreen);
        totalVisibility += sliceVisibility;
    }

    // Average visibility across all directions
    let ao = totalVisibility / f32(NUM_DIRECTIONS);

    // Output AO (1.0 = no occlusion, 0.0 = fully occluded)
    textureStore(ao_output, pixel_coord, vec4<f32>(ao, 0.0, 0.0, 1.0));
}
