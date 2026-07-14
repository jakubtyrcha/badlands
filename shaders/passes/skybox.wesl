// Standard cubemap skybox — fills the frame with the environment cube sampled
// along the per-pixel world-space view ray.
//
// Ported/adapted from sampo's src/shaders/passes/skybox.wesl, with two changes:
//  (1) EAC sampling (sampleEACCubemap) -> STANDARD cube sampling
//      (textureSampleLevel(cube, sampler, dir, 0)), matching CubemapBuilder /
//      shaders/ibl/prefilter_render.wesl's convention;
//  (2) the depth read + discard is dropped — this pass fills the WHOLE HDR
//      target (LoadOp::Clear), and the deferred-lighting pass runs AFTER it
//      with LoadOp::Load and discards far/sky pixels, so geometry overwrites
//      the sky exactly where it exists (see scene_renderer.cpp).
//
// The world ray is reconstructed from the frame UBO's pure-rotation view + proj
// (camera at origin), the same convention deferred_lighting.wesl uses for
// world-space reconstruction.
import package::common::frame::{frame, getFullscreenTriangleVertex, getRayDirectionInWorldSpace};

@group(0) @binding(1) var env_cubemap: texture_cube<f32>;
@group(0) @binding(2) var env_sampler: sampler;

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
    // Per-pixel world-space view ray from the frame UBO (camera at origin).
    let dir = getRayDirectionInWorldSpace(input.screenUV);

    // Standard cube sampling at mip 0 (the source cube is single-mip).
    let color = textureSampleLevel(env_cubemap, env_sampler, dir, 0.0);

    return vec4<f32>(color.rgb, 1.0);
}
