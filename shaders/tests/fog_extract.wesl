// Test-only channel extract (Task: fog rendering graphical tests). Copies one
// channel of the RGBA16Float fog target to an R32Float target so CpuImage /
// TextureReadback (which decode R32Float) can read it back. Self-contained
// fullscreen triangle (no frame import, so group 0 binding 0 is free for params).
struct ExtractParams {
    channel: f32,  // 0=r, 1=g, 2=b, 3=a, 4=rgb luminance
    pad0: f32,
    pad1: f32,
    pad2: f32,
}

@group(0) @binding(0) var<uniform> ep: ExtractParams;
@group(0) @binding(1) var srcTex: texture_2d<f32>;

struct VO {
    @builtin(position) Position: vec4<f32>,
}

@vertex
fn vs_main(@builtin(vertex_index) idx: u32) -> VO {
    let uv = vec2<f32>(f32((idx << 1u) & 2u), f32(idx & 2u));
    var o: VO;
    o.Position = vec4<f32>(uv.x * 2.0 - 1.0, uv.y * 2.0 - 1.0, 0.0, 1.0);
    return o;
}

@fragment
fn fs_main(input: VO) -> @location(0) f32 {
    let px = vec2<i32>(input.Position.xy);
    let c = textureLoad(srcTex, px, 0);
    if (ep.channel < 0.5) { return c.r; }
    if (ep.channel < 1.5) { return c.g; }
    if (ep.channel < 2.5) { return c.b; }
    if (ep.channel < 3.5) { return c.a; }
    return dot(c.rgb, vec3<f32>(0.2126, 0.7152, 0.0722));
}
