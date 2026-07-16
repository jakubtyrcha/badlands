// Directional shadow-map sampling: normal-offset bias (receiver-side) +
// receiver-plane depth-bias (RPDB) 3x3 PCF. Task T3 (T3-fix: RPDB gradient
// is now analytic — see step 4 below; T3-fix-2: added a hard/no-PCF debug
// path — see step 4a below).
//
// Adapted (NOT a verbatim port) from sampo's src/shaders/common/
// shadow_sampling.wesl, which samples a reversed-Z shadow map with a single
// flat depth bias (0.02). badlands' shadow map is conventional-Z (T2: near
// -> 0, far -> 1, comparison sampler = LessEqual — see shadow_map.hpp) and
// this version derives its bias from geometry instead of a tuned constant:
//   - normal-offset length = shadowNormalOffsetLength(NdotL, T_size)
//     (shaders/common/frame.wesl — shared with contact_shadows.wesl's SSCS
//     ray length), identical to the CPU-side ShadowMath::NormalOffsetLength
//     (shadow_map.hpp) — keep both in sync if this expression ever changes.
//   - a per-PCF-tap receiver-plane depth-bias gradient (Isidoro, "Shadow
//     Mapping: GPU-based Tips and Techniques", GDC 2006), computed
//     ANALYTICALLY (T3-fix) from the G-buffer normal + frame.lightViewProj
//     instead of screen-space derivatives: dpdx/dpdy of the shadow UV/depth
//     corrupt at object silhouettes (the 2x2 derivative quad straddles a
//     caster/receiver depth discontinuity), wrongly darkening genuinely-lit
//     receiver pixels hugging object edges (T4 Test 1). The directional
//     light is orthographic, so shadow-map (u, v, depth) are affine
//     functions of world position and the receiver plane's exact gradient
//     is a constant derivable from the plane normal + the matrix alone —
//     see step 4 for the derivation.
//
// textureSampleCompare must run in uniform control flow: no early return /
// branch guards it below. The frustum test is folded into the return value
// via `mix` instead (matches the `select`-as-multiplier idiom in sampo's
// shadow_sampling.wesl:26-31). The hard-shadow branch added in step 4a IS a
// branch (an early return) ahead of the PCF path's textureSampleCompare —
// that's still safe: it's gated on frame.shadowParams.w, a per-FRAME
// uniform-buffer value (identical for every invocation this draw), not a
// per-pixel value like the frustum test above, so it can't introduce
// non-uniform control flow around the PCF path's textureSampleCompare.
import package::common::frame::{frame, shadowNormalOffsetLength};

fn sampleShadowMapPCF(worldPos: vec3<f32>, normal: vec3<f32>, NdotL: f32,
                      shadowMap: texture_depth_2d, shadowSampler: sampler_comparison) -> f32 {
    // 1. Normal-offset bias (receiver-side): push the sampled point off the
    // surface along its normal so the PCF kernel doesn't self-shadow at
    // grazing angles. T_size = frame.shadowParams.x (world size of one
    // shadow-map texel); N_clamp = 0.05 (baked into shadowNormalOffsetLength,
    // shared with contact_shadows.wesl's SSCS ray length — see frame.wesl).
    let offsetLen = shadowNormalOffsetLength(NdotL, frame.shadowParams.x);
    let biasedWorldPos = worldPos + normal * offsetLen;

    // 2. Project into light clip space. Conventional Z (T2, no reversed-Z
    // remap on the shadow projection): ndc.z in [0,1], near -> 0, far -> 1.
    let clip = frame.lightViewProj * vec4<f32>(biasedWorldPos, 1.0);
    let ndc = clip.xyz / clip.w;
    let uv = vec2<f32>(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    let receiverDepth = ndc.z;

    // 3. Frustum test as a multiplier (not a branch) — keeps
    // textureSampleCompare below in uniform control flow.
    let inFrustum = f32(all(uv >= vec2<f32>(0.0)) && all(uv <= vec2<f32>(1.0)) &&
                        ndc.z >= 0.0 && ndc.z <= 1.0);

    // 3a. Hard-shadow debug path (T3-fix-2, test-only): a single, nearest,
    // UNFILTERED tap — bypasses the 3x3 PCF loop, the hardware-bilinear
    // comparison sampler, and the RPDB gradient (step 4) entirely. Exists so
    // T4's edge-leak test (Test 1) can validate the raw pre-filtered shadow
    // signal (a crisp edge) instead of the PCF kernel's inherent soft edge
    // at silhouettes, which is a display filter, not a correctness bug (see
    // task-3fix-report.md). Gated on frame.shadowParams.w (0 = PCF/
    // production, >0.5 = hard); SceneRenderer only sets this nonzero when
    // ShadowConfig::hard_shadow_debug is explicitly set, which no app does
    // (production always takes the PCF path below).
    //
    // `receiverDepth` here still carries the normal-offset bias from step 1
    // — that bias is part of the correctness signal Test 1 checks (its
    // Peter-Panning displacement is baked into the test's expected-shadow
    // oracle polygon, not stripped out).
    if (frame.shadowParams.w > 0.5) {
        let dim = vec2<f32>(textureDimensions(shadowMap));
        let texel_i = vec2<i32>(clamp(uv * dim, vec2<f32>(0.0), dim - 1.0));
        let storedDepth = textureLoad(shadowMap, texel_i, 0);
        // Conventional-Z: lit when the receiver is nearer to (or level
        // with) the light than whatever is stored at that texel.
        let lit = select(0.0, 1.0, receiverDepth <= storedDepth);
        return mix(1.0, lit, inFrustum);
    }

    // 4. Receiver-plane depth-bias gradient (RPDB), computed ANALYTICALLY
    // (T3-fix) instead of via screen-space derivatives. The directional
    // light is orthographic (glm::ortho => clip.w == 1 identically), so
    // (u, v, receiverDepth) are affine (linear) functions of world
    // position — the receiver plane's dDepth/dU, dDepth/dV are therefore
    // EXACT constants derived from the plane normal and frame.lightViewProj
    // alone, not derivative estimates that can be corrupted by a
    // neighboring pixel across a silhouette.
    //
    // M is column-major (WGSL mat4x4: M[c][r] = row r, col c). Its rows are
    // the world-space gradients of the (pre-divide) NDC components; since
    // w == 1 everywhere for this ortho matrix, no perspective-divide
    // correction is needed.
    let M = frame.lightViewProj;
    let rowX = vec3<f32>(M[0].x, M[1].x, M[2].x); // grad_world(ndc.x)
    let rowY = vec3<f32>(M[0].y, M[1].y, M[2].y); // grad_world(ndc.y)
    let rowZ = vec3<f32>(M[0].z, M[1].z, M[2].z); // grad_world(ndc.z == depth)

    let gu = 0.5 * rowX;   // grad_world(u), u = 0.5*ndc.x + 0.5
    let gv = -0.5 * rowY;  // grad_world(v), v = 0.5 - 0.5*ndc.y
    let gd = rowZ;         // grad_world(depth)

    // Project each gradient onto the receiver plane's tangent space (drop
    // the component along the surface normal `n`) — a PCF tap only ever
    // slides across the receiver surface itself, so only the tangential
    // part of each gradient is physically meaningful here.
    let n = normal;
    let tu = gu - dot(gu, n) * n;
    let tv = gv - dot(gv, n) * n;
    let td = gd - dot(gd, n) * n;

    // Solve for biasUV = (dDepth/dU, dDepth/dV) ON THE PLANE via the 2x2
    // Gram system: a tangent displacement a*tu + b*tv induces
    // du = [tu.tu, tu.tv]·(a,b), dv = [tu.tv, tv.tv]·(a,b),
    // ddepth = [td.tu, td.tv]·(a,b) -- invert the (symmetric) Gram matrix
    // to solve for biasUV = (ddepth/du, ddepth/dv).
    let guu = dot(tu, tu);
    let guv = dot(tu, tv);
    let gvv = dot(tv, tv);
    let du  = dot(td, tu);
    let dv  = dot(td, tv);
    let det = guu * gvv - guv * guv;
    var biasUV = vec2<f32>(0.0, 0.0);
    // NOTE: this 1e-12 floor is an ABSOLUTE threshold on a quantity that scales
    // ~1/D_max^4 (the ortho matrix rows shrink with coverage): ~1e-8 at
    // D_max=100, ~6e-10 at D_max=200 — safe for the production default
    // (D_max=128) and every test config, and only at very large coverage
    // (D_max >~ 1000 m) would a valid gradient risk being wrongly zeroed. Use a
    // relative test (det > eps*guu*gvv) if such coverages are ever configured.
    if (abs(det) > 1e-12) {
        biasUV = vec2<f32>(du * gvv - dv * guv, dv * guu - du * guv) / det;
    }
    // det ~= 0 only when the plane is edge-on to the light (the tangent
    // projection degenerates) -- biasUV stays 0 (no RPDB) there, which is
    // correct: such a surface is already at grazing incidence (NdotL ~= 0
    // floors the normal-offset bias above regardless).

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
