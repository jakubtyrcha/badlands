// Test-only Venetian-blind shadow caster (Task: fog rendering graphical tests).
// A light-space fullscreen quad written into the shadow map at near depth, but
// DISCARDING in the slit bands so parallel strips occlude the sun and the slits
// let it through -> alternating light shafts in the fog. Stripe phase is by
// shadow-map column (`period` pixels per slit+strip cycle).
struct StripeParams {
    period: f32,  // shadow-map pixels per (slit + strip) cycle
    pad0: f32,
    pad1: f32,
    pad2: f32,
}

@group(0) @binding(0) var<uniform> sp: StripeParams;

struct VO {
    @builtin(position) pos: vec4<f32>,
}

@vertex
fn vs_main(@builtin(vertex_index) idx: u32) -> VO {
    let uv = vec2<f32>(f32((idx << 1u) & 2u), f32(idx & 2u));
    var o: VO;
    // depth 0 = near the light (conventional-Z shadow map).
    o.pos = vec4<f32>(uv.x * 2.0 - 1.0, uv.y * 2.0 - 1.0, 0.0, 1.0);
    return o;
}

@fragment
fn fs_main(input: VO) -> @location(0) vec4<f32> {
    // First half of each cycle is a slit (no occluder -> lit); second half is a
    // strip (occluder writes near depth -> shadowed).
    let phase = fract(input.pos.x / sp.period);
    if (phase < 0.5) {
        discard;
    }
    return vec4<f32>(1.0, 1.0, 1.0, 1.0);
}
