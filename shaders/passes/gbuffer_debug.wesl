// G-buffer debug visualization pass (Task S2.B4). Fullscreen blit that
// replaces the tonemap resolve when a GBufferDebugMode is selected (see
// SceneRenderer::SetDebugMode / render_gbuffer_debug.cpp), visualizing a
// single G-buffer channel — or a tonemapped passthrough of the lit HDR
// buffer — full-screen.
//
// Adapted from sampo's src/shaders/debug/debug_gbuffer.wesl (simple
// textureLoad-only visualization, no lighting math): drops sampo's AO/
// directional-shadow/contact-shadow cases (those subsystems aren't ported),
// keeps depth/normals/albedo/roughness, and adds an Hdr passthrough case
// (kHdrView in sampo lives in the tonemap pass instead — folded in here so
// one pass covers the whole debug selector).
import package::common::frame::{frame, getFullscreenTriangleVertex, reconstructLinearZ, decodeOctahedron};
import package::common::color::linear_to_srgb;

@group(0) @binding(1) var depth_texture: texture_depth_2d;
@group(0) @binding(2) var normals_texture: texture_2d<f32>;
@group(0) @binding(3) var albedo_texture: texture_2d<f32>;
@group(0) @binding(4) var material_texture: texture_2d<f32>;  // RGBA8: R=roughness, G=shadow, B=baked AO
@group(0) @binding(5) var hdr_texture: texture_2d<f32>;       // lit HDR accumulation
@group(0) @binding(6) var<uniform> debug_mode: u32;           // 1=Depth 2=Normals 3=Albedo 4=Roughness 5=Hdr 6=Ao
@group(0) @binding(7) var ao_texture: texture_2d<f32>;        // GTAO screen-space AO (R8)

struct VertexOutput {
    @builtin(position) Position: vec4<f32>,
}

@vertex
fn vs_main(@builtin(vertex_index) VertexIndex: u32) -> VertexOutput {
    let fullscreen = getFullscreenTriangleVertex(VertexIndex);
    var output: VertexOutput;
    output.Position = fullscreen.clipPosition;
    return output;
}

@fragment
fn fs_main(input: VertexOutput) -> @location(0) vec4<f32> {
    let texCoord = vec2<i32>(input.Position.xy);

    switch (debug_mode) {
        case 1u: {
            // Depth: linearize the reversed-Z buffer, then a log scale (the
            // near/far range spans several orders of magnitude, so a plain
            // linear (z-near)/(far-near) reads as near-black everywhere but
            // the sky) — white=near, black=far, matching sampo's convention.
            let depth = textureLoad(depth_texture, texCoord, 0);
            let z_linear = reconstructLinearZ(depth);
            let log_depth = log(z_linear / frame.nearPlane) / log(frame.farPlane / frame.nearPlane);
            return vec4<f32>(vec3<f32>(1.0 - clamp(log_depth, 0.0, 1.0)), 1.0);
        }
        case 2u: {
            // Normals: decode octahedron, remap [-1,1] -> [0,1].
            let normalOct = textureLoad(normals_texture, texCoord, 0).xy;
            let normal = decodeOctahedron(normalOct);
            return vec4<f32>(normal * 0.5 + 0.5, 1.0);
        }
        case 3u: {
            // Albedo: raw (unlit) base color.
            let albedo = textureLoad(albedo_texture, texCoord, 0);
            return vec4<f32>(albedo.rgb, 1.0);
        }
        case 4u: {
            // Roughness: material.r, grayscale.
            let roughness = textureLoad(material_texture, texCoord, 0).r;
            return vec4<f32>(vec3<f32>(roughness), 1.0);
        }
        case 5u: {
            // Hdr: convenience passthrough — same kClamp tonemap as
            // passes/tonemapping.wesl's default path.
            let hdr = textureLoad(hdr_texture, texCoord, 0);
            if (frame.outputIsLinear == 1u) {
                return hdr;
            }
            let mapped = clamp(hdr.rgb, vec3<f32>(0.0), vec3<f32>(1.0));
            return vec4<f32>(linear_to_srgb(mapped), hdr.a);
        }
        case 6u: {
            // Ao: GTAO screen-space occlusion field, grayscale (dark at
            // contacts/crevices). 1.0 (white) everywhere when GTAO is off.
            let ao = textureLoad(ao_texture, texCoord, 0).r;
            return vec4<f32>(vec3<f32>(ao), 1.0);
        }
        default: {
            discard;
        }
    }
    return vec4<f32>(0.0);
}
