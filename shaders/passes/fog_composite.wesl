// Volumetric fog composite (Task: fog rendering). Full-resolution fullscreen
// pass: depth-aware bilinear upsample of the (possibly half-res) fog target,
// then blend the premultiplied (in-scatter, transmittance) into the HDR target
// via a fixed-function blend (src=One, dst=SrcAlpha): hdr = accum + hdr*T.
// Depth weighting greatly reduces fog bleeding across silhouettes (paired with
// the raymarch's nearest-depth tEnd); a fancier joint-bilateral filter and
// temporal resolve are later quality follow-ups.
import package::common::frame::{frame, getFullscreenTriangleVertex, reconstructLinearZ};
import package::common::fog_types::{FogUniforms};

@group(1) @binding(0) var<uniform> fog: FogUniforms;
@group(1) @binding(1) var fogTarget: texture_2d<f32>;
@group(1) @binding(2) var compDepthTex: texture_depth_2d;

struct VertexOutput {
    @builtin(position) Position: vec4<f32>,
}

@vertex
fn vs_main(@builtin(vertex_index) idx: u32) -> VertexOutput {
    let v = getFullscreenTriangleVertex(idx);
    var o: VertexOutput;
    o.Position = v.clipPosition;
    return o;
}

@fragment
fn fs_main(input: VertexOutput) -> @location(0) vec4<f32> {
    let hiPixel = vec2<i32>(input.Position.xy);
    let uv = input.Position.xy / frame.screenSize;
    let dHiLin = reconstructLinearZ(textureLoad(compDepthTex, hiPixel, 0));

    let renderSize = vec2<f32>(fog.fogRenderW, fog.fogRenderH);
    let scale = frame.screenSize / renderSize;  // >= 1 (full-res -> 1)
    let lp = uv * renderSize - 0.5;
    let base = floor(lp);
    let fr = lp - base;
    let maxTap = vec2<i32>(renderSize) - vec2<i32>(1, 1);
    let maxHi = vec2<i32>(frame.screenSize) - vec2<i32>(1, 1);

    var sumFog = vec4<f32>(0.0);
    var sumW = 0.0;
    for (var j = 0; j < 2; j = j + 1) {
        for (var i = 0; i < 2; i = i + 1) {
            let tap = clamp(vec2<i32>(base) + vec2<i32>(i, j),
                            vec2<i32>(0, 0), maxTap);
            let fogVal = textureLoad(fogTarget, tap, 0);
            let wx = select(1.0 - fr.x, fr.x, i == 1);
            let wy = select(1.0 - fr.y, fr.y, j == 1);
            let bw = wx * wy;
            // Full-res depth at this low-res texel's centre.
            let hiTexel = clamp(vec2<i32>((vec2<f32>(tap) + 0.5) * scale),
                                vec2<i32>(0, 0), maxHi);
            let dLowLin = reconstructLinearZ(textureLoad(compDepthTex, hiTexel, 0));
            let dw = 1.0 / (0.01 + abs(dLowLin - dHiLin));
            let w = bw * dw;
            sumFog = sumFog + fogVal * w;
            sumW = sumW + w;
        }
    }
    return sumFog / max(sumW, 1e-6);
}
