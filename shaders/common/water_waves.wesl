// Water surface wave math — MUST mirror src/engine/rendering/water_waves.hpp
// (kWaveCount, DefaultWaves(), WaveHeight, WaveNormal) exactly. The water
// material is a pure XZ height field; the normal is the standard height-field
// normal normalize(vec3(-dH/dx, 1, -dH/dz)). badlands_water_gpu_test reads the
// shader-written normal back from the G-buffer and asserts it matches the C++
// core, so keep the constants and formulas identical.

struct WaterWave {
    dir: vec2<f32>,
    amplitude: f32,
    frequency: f32,
    speed: f32,
    phase: f32,
}

const kWaterWaveCount: u32 = 3u;

fn waterDefaultWaves() -> array<WaterWave, 3> {
    return array<WaterWave, 3>(
        WaterWave(normalize(vec2<f32>(1.0, 0.0)), 0.18, 0.35, 0.9, 0.0),
        WaterWave(normalize(vec2<f32>(0.6, 0.8)), 0.12, 0.60, 1.1, 1.7),
        WaterWave(normalize(vec2<f32>(-0.8, 0.4)), 0.07, 1.10, 1.6, 4.2),
    );
}

fn waterWaveHeight(xz: vec2<f32>, t: f32) -> f32 {
    var waves = waterDefaultWaves();
    var h = 0.0;
    for (var i = 0u; i < kWaterWaveCount; i = i + 1u) {
        let w = waves[i];
        let p = dot(w.dir, xz) * w.frequency + t * w.speed + w.phase;
        h = h + w.amplitude * sin(p);
    }
    return h;
}

fn waterWaveNormal(xz: vec2<f32>, t: f32) -> vec3<f32> {
    var waves = waterDefaultWaves();
    var dhdx = 0.0;
    var dhdz = 0.0;
    for (var i = 0u; i < kWaterWaveCount; i = i + 1u) {
        let w = waves[i];
        let p = dot(w.dir, xz) * w.frequency + t * w.speed + w.phase;
        let c = w.amplitude * cos(p) * w.frequency;
        dhdx = dhdx + c * w.dir.x;
        dhdz = dhdz + c * w.dir.y;
    }
    return normalize(vec3<f32>(-dhdx, 1.0, -dhdz));
}
