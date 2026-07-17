// Fog emitter evaluator (Task: map fog generator). A world-static volumetric fog
// source, evaluated per froxel in emitter-local space by the composer
// (compute/fog_fill.wesl). Mirrors the CPU `badlands::fog::Emitter`
// (src/engine/rendering/fog_sim.hpp) and its GPU-upload packing in
// fog_simulation.cpp (80-byte stride).
//
// value = magnitude × envelope(radial × vertical) × fill, where fill is 1 for a
// disc emitter and a time-scrolled 3D-noise field for a noise emitter. magnitude
// is physical σ_t (m^-1), written to the media directly (no normalization).
import package::common::fog_shapes::{fogValueNoise3};

struct GpuEmitter {
    center: vec2<f32>,       // world XZ
    halfExtent: vec2<f32>,   // footprint half-size (disc: radius in .x)
    rotation: f32,           // yaw about Y (local frame)
    baseY: f32,              // world Y of the volume base
    height: f32,             // vertical extent above baseY
    shape: u32,              // 0 = disc, 1 = OBB (footprint / radial shape)
    emitterType: u32,        // 0 = disc, 1 = noise (fill)
    magnitude: f32,          // peak σ_t (physical, m^-1)
    radialFalloff: f32,      // radial edge softness [0,1]
    verticalFalloff: f32,    // vertical edge softness [0,1]
    noiseFreq: f32,
    noiseContrast: f32,
    scrollX: f32,
    scrollY: f32,
    scrollZ: f32,
    seed: f32,
    pad0: f32,
    pad1: f32,
}

// World position → emitter-local (translate to center/base, rotate by −yaw).
fn fogEmitterLocal(e: GpuEmitter, worldPos: vec3<f32>) -> vec3<f32> {
    let d = worldPos.xz - e.center;
    let c = cos(e.rotation);
    let s = sin(e.rotation);
    return vec3<f32>(d.x * c + d.y * s, worldPos.y - e.baseY, -d.x * s + d.y * c);
}

// Radial normalized distance from the footprint centre (0 centre → 1 edge).
fn fogRadialNd(e: GpuEmitter, local: vec3<f32>) -> f32 {
    if (e.shape == 0u) {
        return length(local.xz) / max(e.halfExtent.x, 1e-4);
    }
    return max(abs(local.x) / max(e.halfExtent.x, 1e-4),
               abs(local.z) / max(e.halfExtent.y, 1e-4));
}

// Distribution falloff: 1 in the core, ramping to 0 at the edge (nd == 1) over
// the outer `f` fraction. f == 0 → hard edge.
fn fogFalloff(nd: f32, f: f32) -> f32 {
    let inner = 1.0 - f;
    return clamp(1.0 - (nd - inner) / max(f, 1e-4), 0.0, 1.0);
}

// σ_t contribution of emitter `e` at `worldPos` and `time`. 0 outside the volume.
fn fogEmitterEval(e: GpuEmitter, worldPos: vec3<f32>, time: f32) -> f32 {
    let local = fogEmitterLocal(e, worldPos);
    if (local.y < 0.0 || local.y > e.height) {
        return 0.0;
    }
    let rnd = fogRadialNd(e, local);
    if (rnd > 1.0) {
        return 0.0;
    }
    // Vertical distribution: peak at mid-height, symmetric falloff to the ends.
    let vnd = abs(local.y / max(e.height, 1e-4) - 0.5) * 2.0;
    let envelope = fogFalloff(rnd, e.radialFalloff) * fogFalloff(vnd, e.verticalFalloff);

    var fill = 1.0;
    if (e.emitterType == 1u) {
        let scroll = vec3<f32>(e.scrollX, e.scrollY, e.scrollZ);
        let n = fogValueNoise3(local * e.noiseFreq + scroll * time + e.seed);
        fill = clamp((n - 0.5) * e.noiseContrast + 0.5, 0.0, 1.0);
    }
    return e.magnitude * envelope * fill;
}
