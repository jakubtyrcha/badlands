// Volumetric fog raymarch (Task: fog rendering). Fullscreen pass rendered at the
// fog target's resolution (may be half-res): per pixel, reconstruct the
// world-space view ray, march the media cascades from the near plane to the
// scene depth (or fogMaxDistance for sky), light each step (sun phase + shadow
// shafts + SH ambient), integrate front-to-back, and write the premultiplied
// (in-scatter, transmittance) to the fog target. fog_composite.wesl then
// depth-aware-upsamples this into the HDR target.
import package::common::frame::{
    frame,
    getFullscreenTriangleVertex,
    getRayDirectionInWorldSpace,
    reconstructWorldSpacePosition,
    isValidDepth,
};
import package::common::fog_cascade::{fog, fogSampleMedia, fogPhaseHG};

@group(1) @binding(3) var fogDepthTex: texture_depth_2d;
// Directional shadow map for light shafts (conventional-Z, LessEqual compare;
// covers only ~coverage_dmax metres ahead of the camera — lit outside it).
@group(1) @binding(4) var fogShadowMap: texture_depth_2d;
@group(1) @binding(5) var fogShadowSampler: sampler_comparison;

// Sun visibility at a world position via the shadow map. textureSampleCompare
// Level is used (not the derivative variant) because the raymarch loop is
// non-uniform control flow. Outside the shadow frustum -> lit (1.0).
fn fogSunVisibility(Pw: vec3<f32>) -> f32 {
    let clip = frame.lightViewProj * vec4<f32>(Pw, 1.0);
    if (clip.w <= 0.0) {
        return 1.0;
    }
    let ndc = clip.xyz / clip.w;
    let uv = vec2<f32>(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 ||
        ndc.z < 0.0 || ndc.z > 1.0) {
        return 1.0;
    }
    let bias = 0.001;
    return textureSampleCompareLevel(fogShadowMap, fogShadowSampler, uv,
                                     ndc.z - bias);
}

// Interleaved gradient noise in [0,1), animated by frame index. Used to jitter
// the ray-start offset so the low step count dithers into noise instead of
// banding (temporal resolve is a follow-up; residual noise is accepted).
fn fogIGN(pixel: vec2<f32>, frameIdx: f32) -> f32 {
    let p = pixel + frameIdx * 5.588238;
    return fract(52.9829189 * fract(0.06711056 * p.x + 0.00583715 * p.y));
}

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
    // This pass renders at the fog target's resolution (fogRenderW/H), which
    // may be half the screen. UV is resolution-independent.
    let uv = input.Position.xy / vec2<f32>(fog.fogRenderW, fog.fogRenderH);
    // Nearest (reversed-Z max) depth over the full-res block this froxel covers,
    // so tEnd stops at the CLOSEST surface and fog is not marched past foreground
    // silhouettes (avoids edge halos; the composite's depth-aware upsample then
    // recovers the background side per full-res pixel).
    let scale = max(i32(round(frame.screenSize.x / fog.fogRenderW)), 1);
    // Origin of THIS froxel's own full-res block (px*scale), not the froxel
    // centre (floor(uv*screenSize) = px*scale + scale/2) which would sample the
    // neighbour's block and leak fog one pixel across silhouettes.
    let baseHi = vec2<i32>(input.Position.xy) * scale;
    let maxHi = vec2<i32>(frame.screenSize) - vec2<i32>(1, 1);
    var depth = 0.0;  // reversed-Z: 0 = far; nearest surface = max depth
    for (var by = 0; by < scale; by = by + 1) {
        for (var bx = 0; bx < scale; bx = bx + 1) {
            let p = clamp(baseHi + vec2<i32>(bx, by), vec2<i32>(0, 0), maxHi);
            depth = max(depth, textureLoad(fogDepthTex, p, 0));
        }
    }

    let camPos = frame.cameraWorldPos.xyz;
    let rayDir = getRayDirectionInWorldSpace(uv);

    var tEnd = fog.fogMaxDistance;
    if (isValidDepth(depth)) {
        let scenePos = reconstructWorldSpacePosition(uv, depth);
        tEnd = min(length(scenePos - camPos), fog.fogMaxDistance);
    }
    let tStart = frame.nearPlane;
    if (tEnd <= tStart) {
        return vec4<f32>(0.0, 0.0, 0.0, 1.0);  // no fog span: transmittance 1
    }

    let steps = max(i32(fog.stepCount), 1);
    let dt = (tEnd - tStart) / f32(steps);

    let sunDir = normalize(frame.sunDirection.xyz);
    let phase = fogPhaseHG(dot(rayDir, sunDir), fog.phaseG);
    let sunColor = frame.sunColor.rgb * fog.sunScale;
    // Cheap env approximation: isotropic ambient from the SH DC term (no env
    // scattering, per the design).
    let ambient = frame.ambientSH[0].xyz * fog.ambientScale;

    var offset = 0.5;  // midpoint sampling
    if (fog.jitterEnabled > 0.5) {
        offset = fogIGN(input.Position.xy, fog.frameIndex);
    }

    var accum = vec3<f32>(0.0);
    var transmittance = 1.0;
    var t = tStart + dt * offset;
    for (var k = 0; k < steps; k = k + 1) {
        let P = camPos + rayDir * t;
        let media = fogSampleMedia(P);
        let sigma_s = media.rgb;
        let sigma_t = media.a;
        if (sigma_t > 1e-6) {
            var sunVis = 1.0;
            if (fog.enableShafts > 0.5) {
                sunVis = fogSunVisibility(P);
            }
            let inScatter = sigma_s * (sunVis * sunColor * phase + ambient);
            let Tstep = exp(-sigma_t * dt);
            accum = accum + transmittance * inScatter * (1.0 - Tstep) / max(sigma_t, 1e-6);
            transmittance = transmittance * Tstep;
        }
        t = t + dt;
    }
    return vec4<f32>(accum, transmittance);
}
