// Test-only camera-depth writer (Task: fog rendering graphical tests). Draws a
// screen-space rect into the camera depth buffer at a fixed reversed-Z depth,
// standing in for an opaque near occluder (the Monolith pillar). The fog
// raymarch then bounds its ray at that near depth. Real color+depth pipeline
// (simpler than depth-only); the color is a throwaway.
struct DepthQuadParams {
    rect: vec4<f32>,  // NDC x0, y0, x1, y1
    depth: f32,       // reversed-Z NDC depth (near ~ 1, far = 0)
    pad0: f32,
    pad1: f32,
    pad2: f32,
}

@group(0) @binding(0) var<uniform> dp: DepthQuadParams;

struct VO {
    @builtin(position) pos: vec4<f32>,
}

@vertex
fn vs_main(@builtin(vertex_index) idx: u32) -> VO {
    var xs = array<f32, 4>(dp.rect.x, dp.rect.z, dp.rect.x, dp.rect.z);
    var ys = array<f32, 4>(dp.rect.y, dp.rect.y, dp.rect.w, dp.rect.w);
    var indices = array<u32, 6>(0u, 1u, 2u, 2u, 1u, 3u);
    let i = indices[idx];
    var o: VO;
    o.pos = vec4<f32>(xs[i], ys[i], dp.depth, 1.0);
    return o;
}

@fragment
fn fs_main(input: VO) -> @location(0) vec4<f32> {
    return vec4<f32>(0.0, 0.0, 0.0, 1.0);
}
