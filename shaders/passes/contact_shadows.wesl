// Screen-space contact shadows (SSCS): a short view-space ray-march along
// the sun direction that fills the small Peter-Panning gap the shadow map's
// normal-offset bias (shadow_sampling.wesl) opens near contact points.
// Written into contact_shadow_texture_ (R8Unorm: 1.0 = lit, 0.0 = occluded;
// T2 creates + clears it, this pass fills it every frame, T3's deferred
// pass combines it via shadow = min(shadowMapPCF, contactShadow)).
//
// Ported from sampo's src/shaders/passes/contact_shadows.wesl, adapted
// (Task T5):
//   - all DEBUG_MODE visualization scaffolding stripped — production path
//     only.
//   - ray length is WORLD-SPACE and derived per-pixel (Task T5-fix) via the
//     shared shadowNormalOffsetLength(NdotL, T_size) helper (shaders/common/
//     frame.wesl — also used by shadow_sampling.wesl's normal-offset bias)
//     — the exact normal-offset gap (mirrors the CPU-side
//     ShadowMath::NormalOffsetLength, shadow_map.hpp) the shadow map opened
//     at THIS receiver pixel, using the real per-pixel NdotL. This is
//     bounded above by the grazing worst case E_gap = 30*T_size (i.e.
//     1.5*T_size / N_clamp, N_clamp = 0.05) as NdotL -> N_clamp, so it still
//     never exceeds the max normal-offset distance and keeps this pass from
//     painting long false-shadow streaks on open floor far from any caster
//     — but for typical (non-grazing) pixels it is far shorter than E_gap,
//     which concentrates the fixed 8-step budget on the real gap instead of
//     over-marching ~15x past it, so small casters are no longer straddled
//     between samples. (frame.shadowParams.z is unused: this used to be a
//     precomputed worst-case constant there, but the ray length is now
//     computed per-pixel here instead.)
//
// Depth convention: this pass reads the MAIN camera G-buffer depth, which
// is REVERSED-Z (0 = far, 1 = near) — unlike the shadow map (T2), which is
// conventional-Z. Do not conflate the two; every depth comparison below
// stays reversed-Z, unchanged from sampo.
import package::common::frame::{frame, getFullscreenTriangleVertex,
    reconstructViewSpacePosition, worldSpaceToViewSpaceDirection, isSkyDepth,
    isValidDepth, decodeOctahedron, shadowNormalOffsetLength};

@group(0) @binding(1) var depth_texture: texture_depth_2d;
@group(0) @binding(2) var normals_texture: texture_2d<f32>;

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
fn fs_main(input: VertexOutput) -> @location(0) f32 {
    let texCoord = vec2<i32>(input.Position.xy);
    let screenUV = input.screenUV;

    let depth = textureLoad(depth_texture, texCoord, 0);

    // Skip sky pixels (no geometry to shadow).
    if (isSkyDepth(depth)) {
        return 1.0;
    }

    // Reconstruct view-space position. Metrically correct (T3's
    // reconstructLinearZ fix), so the world-space ray length below maps to
    // the correct view-space distance.
    let posVS = reconstructViewSpacePosition(screenUV, depth);

    // Light direction in view space, pointing TOWARD the sun.
    let lightDirVS = normalize(worldSpaceToViewSpaceDirection(frame.sunDirection.xyz));

    // Receiver normal (world-space, octahedron-encoded G-buffer normal, same
    // decode as deferred_lighting.wesl) + its NdotL, needed to reconstruct
    // the actual per-pixel normal-offset gap below.
    let normal = decodeOctahedron(textureLoad(normals_texture, texCoord, 0).xy);
    let NdotL = max(dot(normal, normalize(frame.sunDirection.xyz)), 0.0);

    // Ray length: the actual per-pixel normal-offset gap, via the shared
    // shadowNormalOffsetLength helper (shaders/common/frame.wesl) — mirrors
    // ShadowMath::NormalOffsetLength (shadow_map.hpp) with the receiver's
    // real NdotL, instead of the fixed grazing worst case E_gap
    // (shadowParams.z is unused — see file header for why).
    let rayLengthWorld = shadowNormalOffsetLength(NdotL, frame.shadowParams.x);
    let numSteps = 8;

    // Transform ray start/end to clip space, then perspective-divide to NDC.
    let rayStartClip = frame.projMatrix * vec4<f32>(posVS, 1.0);
    let rayEndVS = posVS + lightDirVS * rayLengthWorld;
    let rayEndClip = frame.projMatrix * vec4<f32>(rayEndVS, 1.0);

    let rayStartNDC = rayStartClip.xyz / rayStartClip.w;
    let rayEndNDC = rayEndClip.xyz / rayEndClip.w;
    let rayStepNDC = rayEndNDC - rayStartNDC;

    let stepSize = 1.0 / f32(numSteps);

    // Maximum thickness: how far "behind" a surface the ray can be and
    // still count as a hit. Reversed-Z: larger depth = closer. Rejects
    // false hits on thick geometry (e.g. cube side faces).
    let stepDeltaZ = abs(rayStepNDC.z) * stepSize;
    let maxThickness = max(stepDeltaZ * 4.0, rayStartNDC.z * 0.1);

    // Ray march in NDC space.
    var shadow = 1.0;
    for (var i = 1; i <= numSteps; i++) {
        let t = f32(i) * stepSize;
        let sampleNDC = rayStartNDC + rayStepNDC * t;

        // NDC to screen UV: x [-1,1] -> [0,1], y [-1,1] -> [1,0] (Y flipped).
        let sampleUV = vec2<f32>(
            (sampleNDC.x + 1.0) * 0.5,
            (1.0 - sampleNDC.y) * 0.5
        );

        // Bounds check — exit if outside screen.
        if (any(sampleUV < vec2<f32>(0.0)) || any(sampleUV > vec2<f32>(1.0))) {
            break;
        }

        let sampleCoord = vec2<i32>(sampleUV * vec2<f32>(textureDimensions(depth_texture)));
        let sampledDepth = textureLoad(depth_texture, sampleCoord, 0);

        // Reversed-Z: larger depth values are closer. depthDiff < 0 means
        // the ray sample is farther from the camera than the stored
        // surface — i.e. the ray is behind (occluded by) that surface.
        let depthDiff = sampleNDC.z - sampledDepth;

        // Hit: the ray is behind a surface (depthDiff < 0) but not
        // unreasonably far behind it (depthDiff > -maxThickness rejects
        // thick geometry), and the sample isn't sky.
        if (depthDiff < 0.0 && depthDiff > -maxThickness && isValidDepth(sampledDepth)) {
            shadow = 0.0;
            break;
        }
    }

    return shadow;
}
