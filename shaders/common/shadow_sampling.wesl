// Directional shadow-map sampling: normal-offset bias (receiver-side) +
// receiver-plane depth-bias (RPDB) 3x3 PCF. Task T3.
//
// Adapted (NOT a verbatim port) from sampo's src/shaders/common/
// shadow_sampling.wesl, which samples a reversed-Z shadow map with a single
// flat depth bias (0.02). badlands' shadow map is conventional-Z (T2: near
// -> 0, far -> 1, comparison sampler = LessEqual — see shadow_map.hpp) and
// this version derives its bias from geometry instead of a tuned constant:
//   - normal-offset length = 1.5 * T_size / max(NdotL, 0.05), identical to
//     the CPU-side ShadowMath::NormalOffsetLength (shadow_map.hpp) — keep
//     both in sync if this expression ever changes.
//   - a per-PCF-tap receiver-plane depth-bias gradient (Isidoro, "Shadow
//     Mapping: GPU-based Tips and Techniques", GDC 2006), solved from the
//     screen-space derivatives of the shadow UV/depth so the bias follows
//     the receiver surface's actual slope instead of a flat epsilon.
//
// dpdx/dpdy and textureSampleCompare must run in uniform control flow: no
// early return / branch guards them below. The frustum test is folded into
// the return value via `mix` instead (matches the `select`-as-multiplier
// idiom in sampo's shadow_sampling.wesl:26-31).
import package::common::frame::frame;

fn sampleShadowMapPCF(worldPos: vec3<f32>, normal: vec3<f32>, NdotL: f32,
                      shadowMap: texture_depth_2d, shadowSampler: sampler_comparison) -> f32 {
    // 1. Normal-offset bias (receiver-side): push the sampled point off the
    // surface along its normal so the PCF kernel doesn't self-shadow at
    // grazing angles. T_size = frame.shadowParams.x (world size of one
    // shadow-map texel); N_clamp = 0.05.
    let offsetLen = 1.5 * frame.shadowParams.x / max(NdotL, 0.05);
    let biasedWorldPos = worldPos + normal * offsetLen;

    // 2. Project into light clip space. Conventional Z (T2, no reversed-Z
    // remap on the shadow projection): ndc.z in [0,1], near -> 0, far -> 1.
    let clip = frame.lightViewProj * vec4<f32>(biasedWorldPos, 1.0);
    let ndc = clip.xyz / clip.w;
    let uv = vec2<f32>(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    let receiverDepth = ndc.z;

    // 3. Frustum test as a multiplier (not a branch) — keeps the
    // derivatives/compare below in uniform control flow.
    let inFrustum = f32(all(uv >= vec2<f32>(0.0)) && all(uv <= vec2<f32>(1.0)) &&
                        ndc.z >= 0.0 && ndc.z <= 1.0);

    // 4. Receiver-plane depth-bias gradient (RPDB): solve dDepth/dU,
    // dDepth/dV in shadow-UV space from the screen-space derivatives of
    // (uv, receiverDepth), so each PCF tap below gets a bias that tracks the
    // receiver plane's slope instead of a flat epsilon.
    let duvz_dx = vec3<f32>(dpdx(uv), dpdx(receiverDepth));
    let duvz_dy = vec3<f32>(dpdy(uv), dpdy(receiverDepth));
    let det = duvz_dx.x * duvz_dy.y - duvz_dy.x * duvz_dx.y;
    var biasUV = vec2<f32>(
        duvz_dy.y * duvz_dx.z - duvz_dx.y * duvz_dy.z,
        duvz_dx.x * duvz_dy.z - duvz_dy.x * duvz_dx.z);
    biasUV = select(vec2<f32>(0.0), biasUV / det, abs(det) > 1e-8);

    // 5. 3x3 PCF, adaptive per-tap depth (unrolled so textureSampleCompare
    // stays in unambiguous uniform control flow). texel = invShadowRes
    // (frame.shadowParams.y). With the comparison sampler set to LessEqual,
    // textureSampleCompare returns 1.0 (lit) where adaptiveDepth <=
    // storedDepth.
    let texel = frame.shadowParams.y;
    var sum = 0.0;
    for (var j = -1; j <= 1; j++) {
        for (var i = -1; i <= 1; i++) {
            let off = vec2<f32>(f32(i), f32(j)) * texel;
            let sampleUV = uv + off;
            let adaptiveDepth = receiverDepth + dot(off, biasUV);
            sum += textureSampleCompare(shadowMap, shadowSampler, sampleUV, adaptiveDepth);
        }
    }
    let pcf = sum / 9.0;

    // 6. Fully lit outside the shadow frustum; otherwise the PCF result.
    return mix(1.0, pcf, inFrustum);
}
