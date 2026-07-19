// Shared volumetric-fog cascade sampling (Task: fog rendering). Owns the fog
// params UBO + media texture/sampler bindings (group 1) and the world<->voxel
// toroidal-sampling math, mirroring the C++ engine/rendering/fog_cascade.hpp.
// Imported by shaders/passes/fog.wesl.
import package::common::frame::{frame};
import package::common::fog_types::{FogUniforms};

@group(1) @binding(0) var<uniform> fog: FogUniforms;
// Media: rgb = scattering sigma_s, a = extinction sigma_t. Cascade i occupies
// depth slab [i*resY, (i+1)*resY); X/Z are the toroidal world axes.
@group(1) @binding(1) var fogMedia: texture_3d<f32>;
@group(1) @binding(2) var fogSampler: sampler;

const FOG_PI: f32 = 3.14159265359;

fn fogCascadeFullExtent(i: i32) -> f32 {
    return 2.0 * fog.baseHalfExtent * exp2(f32(i));
}

// Sample one cascade at world position P. Toroidal: texcoord = P.xz /
// fullExtent, with the Repeat sampler wrapping across the seam (texel =
// worldVoxel mod res). The depth axis packs resY Y-slices per cascade; clamp
// into the slab interior to avoid trilinear bleed between cascades.
fn fogSampleCascade(i: i32, P: vec3<f32>) -> vec4<f32> {
    let fullExt = fogCascadeFullExtent(i);
    let uvx = P.x / fullExt;
    let uvz = P.z / fullExt;
    let voxelY = fog.height / fog.resY;
    let sy = clamp((P.y - fog.floorY) / voxelY, 0.5, fog.resY - 0.5);
    let depthTexel = f32(i) * fog.resY + sy;
    let w = depthTexel / (fog.resY * fog.cascadeCount);
    return textureSampleLevel(fogMedia, fogSampler, vec3<f32>(uvx, uvz, w), 0.0);
}

// Media at world P, blended smoothly across cascade boundaries. Zero outside
// the band / beyond the coarsest cascade.
fn fogSampleMedia(P: vec3<f32>) -> vec4<f32> {
    if (P.y < fog.floorY || P.y > fog.floorY + fog.height) {
        return vec4<f32>(0.0);
    }
    let cam = frame.cameraWorldPos.xyz;
    let d = max(abs(P.x - cam.x), abs(P.z - cam.z));
    let count = i32(fog.cascadeCount);
    for (var i = 0; i < count; i = i + 1) {
        let halfExt = fog.baseHalfExtent * exp2(f32(i));
        if (d <= halfExt) {
            let inner = halfExt * 0.8;
            if (d <= inner) {
                return fogSampleCascade(i, P);
            }
            let t = (d - inner) / (halfExt - inner);  // 0 at inner, 1 at edge
            if (i == count - 1) {
                // Coarsest cascade: fade to zero toward the edge. The toroidal
                // window is voxel-snapped while `d` uses the raw camera, so the
                // outermost fraction of a voxel would otherwise wrap (Repeat) to
                // far-away media; fading suppresses that ring and doubles as a
                // max-range falloff.
                return fogSampleCascade(i, P) * (1.0 - t);
            }
            return mix(fogSampleCascade(i, P), fogSampleCascade(i + 1, P), t);
        }
    }
    return vec4<f32>(0.0);
}

// Henyey-Greenstein phase function. cosTheta is dot(rayDir, sunDir) with both
// normalized by the caller.
fn fogPhaseHG(cosTheta: f32, g: f32) -> f32 {
    let g2 = g * g;
    let denom = 1.0 + g2 - 2.0 * g * cosTheta;
    return (1.0 - g2) / (4.0 * FOG_PI * pow(max(denom, 1e-4), 1.5));
}
