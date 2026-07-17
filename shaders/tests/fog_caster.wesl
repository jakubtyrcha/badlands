// Test-only shadow caster (Task: fog rendering graphical tests). Renders a
// world-space quad (4 corners) through the light's view-projection into the
// shadow map's depth, so the fog's fogSunVisibility sees an occluded region and
// produces a light-shaft gap. Writes a dummy color (a real color+depth pipeline
// is simpler/better-supported than a depth-only one); the color target is a
// throwaway R8.
struct CasterParams {
    lightViewProj: mat4x4<f32>,
    corners: array<vec4<f32>, 4>,
}

@group(0) @binding(0) var<uniform> cp: CasterParams;

struct VO {
    @builtin(position) pos: vec4<f32>,
}

@vertex
fn vs_main(@builtin(vertex_index) idx: u32) -> VO {
    var indices = array<u32, 6>(0u, 1u, 2u, 2u, 1u, 3u);
    let w = cp.corners[indices[idx]];
    var o: VO;
    o.pos = cp.lightViewProj * vec4<f32>(w.xyz, 1.0);
    return o;
}

@fragment
fn fs_main(input: VO) -> @location(0) vec4<f32> {
    return vec4<f32>(1.0, 1.0, 1.0, 1.0);
}
