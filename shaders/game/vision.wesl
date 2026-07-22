// Fog-of-war overlay (game post-scene pass; NOT the volumetric fog system).
// A fullscreen modulation run late in the HDR pipeline via the engine's generic
// ScenePostPass hook: reconstruct the world position from the G-buffer depth,
// sample the game-owned vision texture (RG: R=discovered, G=visible) at that
// world position, and modulate the lit colour:
//   * terra-incognita (never discovered) -> solid black
//   * dormant (discovered, not currently seen) -> desaturated + darkened
//   * visible -> passthrough
// Sky pixels (no geometry) pass through unchanged. Geometry whose world
// position falls outside the vision grid is treated as terra-incognita (black).

import package::common::frame::{frame, getFullscreenTriangleVertex, isValidDepth, reconstructWorldSpacePosition};

@group(0) @binding(1) var color_source: texture_2d<f32>;   // HDR snapshot (pre-tonemap)
@group(0) @binding(2) var depth_texture: texture_depth_2d; // G-buffer depth (reversed-Z)
@group(0) @binding(3) var vision_tex: texture_2d<f32>;     // RG8: R=discovered, G=visible
@group(0) @binding(4) var vision_sampler: sampler;         // linear clamp

struct VisionParams {
    // xy = grid origin in world (grid sim-frame min + sim->world offset);
    // zw = world->uv scale (1 / (grid_texels * texel_m)) per axis.
    origin_inv_size: vec4<f32>,
    // x = enabled (0/1). yzw reserved.
    flags: vec4<f32>,
}
@group(0) @binding(5) var<uniform> params: VisionParams;

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
    let src = textureLoad(color_source, coord, 0);
    if (params.flags.x < 0.5) {
        return src;  // fog-of-war disabled
    }

    let depth = textureLoad(depth_texture, coord, 0);
    if (!isValidDepth(depth)) {
        return src;  // sky / no geometry: leave the background untouched
    }

    let world = reconstructWorldSpacePosition(input.screenUV, depth);
    let uv = (world.xz - params.origin_inv_size.xy) * params.origin_inv_size.zw;

    var discovered = 0.0;
    var visible = 0.0;
    if (all(uv >= vec2<f32>(0.0)) && all(uv <= vec2<f32>(1.0))) {
        let s = textureSampleLevel(vision_tex, vision_sampler, uv, 0.0).rg;
        discovered = s.r;
        visible = s.g;
    }

    // Dormant look: desaturate toward luminance, then darken.
    let lum = dot(src.rgb, vec3<f32>(0.2126, 0.7152, 0.0722));
    let dormant_col = mix(vec3<f32>(lum), src.rgb, 0.15) * 0.35;

    // Soft edges: smoothstep the (bilinearly filtered) channels.
    let vis_f = smoothstep(0.15, 0.85, visible);
    let disc_f = smoothstep(0.15, 0.85, discovered);

    var col = mix(dormant_col, src.rgb, vis_f);  // dormant -> visible
    col = mix(vec3<f32>(0.0), col, disc_f);      // terra-incognita (black) -> known
    return vec4<f32>(col, src.a);
}
