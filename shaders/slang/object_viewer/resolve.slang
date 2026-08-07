// The visibility-buffer resolve: one uint per pixel back into a lit surface.
//
// EVERYTHING comes from the packed id. The raster pass bound no material and
// interpolated no attribute, so this pass refetches the triangle, derives the
// barycentric weights at this pixel, and interpolates uv, normal and tangent
// itself. Barycentrics are not a debug curiosity here -- they are the entire
// mechanism, and without them a visibility buffer yields nothing but an id.
//
// IT DOES NOT READ THE DEPTH BUFFER. World position comes from the interpolated
// triangle, which is more accurate than reconstructing it from depth and needs
// no inverse-viewProj at all. The depth attachment exists for the depth TEST;
// binding it here as well would be a resource this pass does not use.
//
// OUTPUT SPACE: this writes the SCENE target, which is RGBA16Float and LINEAR.
// The lit view therefore emits SCENE-REFERRED HDR -- values above 1 are real and
// survive to the EDR compositor. The tonemap lives in the output pass, not here,
// because a debug view must NOT be tone-curved: "roughness is 0.35" has to stay
// 0.35 on screen.
//
// A debug view emits SrgbToLinear(code value), so that the output pass's encode
// hands back exactly the code value it meant -- the conversion is spelled once,
// where the claim is made.

module resolve;

import vis_common;
import standard_lighting;
import sh_lighting;
import output_transform;

// Mirrors ResolveFrameUniform in resolve_pass.hpp.
public struct ResolveFrame {
    public float4x4 view_proj;
    public float4 camera_pos;     // xyz world
    public float4 sun_direction;  // xyz, normalized, pointing TOWARD the sun
    public float4 sun_color;      // rgb, already scaled by intensity
    public float4 params;         // x = debug view, y = near, z = far
    public float4 ambient_sh[9];  // L2 SH, ENGINE convention (pre-convolved)
}

// Mirrors DebugView in resolve_pass.hpp, and every entry is reachable from
// --debug-view as well as from the UI. A test iterates the enum to prove that:
// a mode only the UI can reach is a mode no headless assertion covers.
public static const uint kViewLit = 0u;
public static const uint kViewTriangleId = 1u;
public static const uint kViewBarycentric = 2u;
public static const uint kViewDepth = 3u;
public static const uint kViewAlbedo = 4u;
public static const uint kViewNormal = 5u;
public static const uint kViewRoughness = 6u;
public static const uint kViewMetallic = 7u;
public static const uint kViewDisplacement = 8u;
public static const uint kViewAo = 9u;

ConstantBuffer<ResolveFrame> frame;
Texture2D<uint> visbuffer;
StructuredBuffer<PlaneVertex> vertices;
StructuredBuffer<uint> indices;
StructuredBuffer<DrawInfo> draws;
Texture2D<float4> albedo_map;
Texture2D<float4> normal_map;
Texture2D<float4> arm_map;        // R = AO, G = roughness, B = metallic
Texture2D<float4> disp_map;
SamplerState material_sampler;

struct VOut {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
}

[shader("vertex")]
VOut vs_fullscreen(uint vid : SV_VertexID)
{
    let uv = float2(float((vid << 1) & 2), float(vid & 2));
    VOut o;
    o.pos = float4(uv * 2.0 - 1.0, 0.0, 1.0);
    o.uv = float2(uv.x, 1.0 - uv.y);
    return o;
}

// --- Barycentrics, and their screen-space gradients -------------------------
//
// The weights AND their derivatives fall out of the same edge coefficients, so
// taking the correct path costs about fifteen extra lines rather than a second
// derivation. That matters: ddx/ddy of a RESOLVED attribute is wrong in every
// quad straddling a triangle edge, and on a 128-triangle plane that is a mip
// seam along every internal edge.
struct Bary {
    float3 lambda;  // the three weights, perspective-correct
    float3 ddx;     // d(lambda)/d(screen x)
    float3 ddy;     // d(lambda)/d(screen y)
}

Bary ComputeBary(float4 p0, float4 p1, float4 p2, float2 pixel_ndc,
                 float2 screen)
{
    Bary o;
    // KNOWN LIMITATION: a vertex behind the eye has w <= 0 and this reciprocal
    // is meaningless -- the resolve refetches the ORIGINAL, unclipped vertices,
    // so a triangle straddling the near plane is not merely imprecise here. The
    // headless near-plane case is what watches for it; the fix, if it fires, is
    // a world-space ray/triangle formulation rather than an epsilon on w.
    let inv_w = float3(1.0 / p0.w, 1.0 / p1.w, 1.0 / p2.w);
    let ndc0 = p0.xy * inv_w.x;
    let ndc1 = p1.xy * inv_w.y;
    let ndc2 = p2.xy * inv_w.z;

    let det = (ndc2.x - ndc1.x) * (ndc0.y - ndc1.y) -
              (ndc0.x - ndc1.x) * (ndc2.y - ndc1.y);
    let inv_det = 1.0 / det;

    o.ddx = float3(ndc1.y - ndc2.y, ndc2.y - ndc0.y, ndc0.y - ndc1.y) *
            inv_det * inv_w;
    o.ddy = float3(ndc2.x - ndc1.x, ndc0.x - ndc2.x, ndc1.x - ndc0.x) *
            inv_det * inv_w;
    var ddx_sum = o.ddx.x + o.ddx.y + o.ddx.z;
    var ddy_sum = o.ddy.x + o.ddy.y + o.ddy.z;

    let delta = pixel_ndc - ndc0;
    let interp_inv_w = inv_w.x + delta.x * ddx_sum + delta.y * ddy_sum;
    let interp_w = 1.0 / interp_inv_w;

    o.lambda.x = interp_w * (inv_w.x + delta.x * o.ddx.x + delta.y * o.ddy.x);
    o.lambda.y = interp_w * (0.0 + delta.x * o.ddx.y + delta.y * o.ddy.y);
    o.lambda.z = interp_w * (0.0 + delta.x * o.ddx.z + delta.y * o.ddy.z);

    // NDC units to PIXEL units. A gradient in NDC is a gradient across the
    // whole screen, so a mip chosen from it would be off by log2(width).
    o.ddx *= (2.0 / screen.x);
    o.ddy *= (2.0 / screen.y);
    ddx_sum *= (2.0 / screen.x);
    ddy_sum *= (2.0 / screen.y);
    // NDC y runs upward and screen y downward.
    o.ddy *= -1.0;
    ddy_sum *= -1.0;

    let w_ddx = 1.0 / (interp_inv_w + ddx_sum);
    let w_ddy = 1.0 / (interp_inv_w + ddy_sum);
    o.ddx = w_ddx * (o.lambda * interp_inv_w + o.ddx) - o.lambda;
    o.ddy = w_ddy * (o.lambda * interp_inv_w + o.ddy) - o.lambda;
    return o;
}

float3 Interp3(float3 a, float3 b, float3 c, float3 l)
{
    return a * l.x + b * l.y + c * l.z;
}
float2 Interp2(float2 a, float2 b, float2 c, float3 l)
{
    return a * l.x + b * l.y + c * l.z;
}

// The triangle-ID hash rhi_lab already uses, so two visibility buffers in this
// repo colour the same id the same way.
float3 HashId(uint id)
{
    return 0.5 + 0.5 * cos(6.2831853 *
                           (float(id) * 0.113 + float3(0.0, 0.33, 0.67)));
}

[shader("fragment")]
float4 fs_resolve(VOut i) : SV_Target
{
    uint w, h;
    visbuffer.GetDimensions(w, h);
    let px = int2(int(i.uv.x * float(w)), int(i.uv.y * float(h)));
    let packed = visbuffer.Load(int3(px, 0));

    // The background, as the linear equivalent of its code value.
    if (packed == kVisEmpty) {
        return float4(SrgbToLinear(float3(0.05, 0.06, 0.09)), 1.0);
    }

    let draw = draws[VisDraw(packed)];
    let prim = VisPrimitive(packed);
    let base = draw.first_index + prim * 3u;
    let i0 = indices[base + 0u] + draw.first_vertex;
    let i1 = indices[base + 1u] + draw.first_vertex;
    let i2 = indices[base + 2u] + draw.first_vertex;
    PlaneVertex v0 = vertices[i0];
    PlaneVertex v1 = vertices[i1];
    PlaneVertex v2 = vertices[i2];

    // Camera-OFFSET, exactly as the raster pass projected them. Using world
    // positions here and camera-relative ones there puts the barycentrics on a
    // different triangle than the one that was rasterized.
    let cam = frame.camera_pos.xyz;
    let p0 = mul(frame.view_proj, float4(v0.position - cam, 1.0));
    let p1 = mul(frame.view_proj, float4(v1.position - cam, 1.0));
    let p2 = mul(frame.view_proj, float4(v2.position - cam, 1.0));

    // Pixel CENTRE. Sampling the corner puts a pixel on the far side of a
    // triangle edge from where it was rasterized, which shows up as a one-pixel
    // fringe of the neighbouring triangle's material.
    let pixel_ndc = float2((float(px.x) + 0.5) / float(w) * 2.0 - 1.0,
                           1.0 - (float(px.y) + 0.5) / float(h) * 2.0);
    let bary = ComputeBary(p0, p1, p2, pixel_ndc, float2(float(w), float(h)));

    let uv = Interp2(v0.uv, v1.uv, v2.uv, bary.lambda);
    let uv_ddx = Interp2(v0.uv, v1.uv, v2.uv, bary.ddx);
    let uv_ddy = Interp2(v0.uv, v1.uv, v2.uv, bary.ddy);
    let world = Interp3(v0.position, v1.position, v2.position, bary.lambda);
    let geo_n = normalize(Interp3(v0.normal, v1.normal, v2.normal, bary.lambda));
    let tan_raw = Interp3(v0.tangent.xyz, v1.tangent.xyz, v2.tangent.xyz,
                          bary.lambda);
    let sign = v0.tangent.w;

    // SampleGrad, not Sample: the gradients are the analytic ones above, and a
    // fullscreen pass's own ddx/ddy would describe the SCREEN quad rather than
    // the surface.
    let albedo_srgb = albedo_map.SampleGrad(material_sampler, uv, uv_ddx, uv_ddy);
    let normal_tex = normal_map.SampleGrad(material_sampler, uv, uv_ddx, uv_ddy);
    let arm = arm_map.SampleGrad(material_sampler, uv, uv_ddx, uv_ddy);
    let disp = disp_map.SampleGrad(material_sampler, uv, uv_ddx, uv_ddy);

    // Gram-Schmidt, then the handedness sign. B = w * cross(N, T), which is
    // what tangent.w exists to carry -- three components cannot express it, and
    // negating T inverts the normal map's U response instead of its V.
    let N0 = geo_n;
    let T = normalize(tan_raw - N0 * dot(N0, tan_raw));
    let B = sign * cross(N0, T);
    // ALWAYS GL-CONVENTION HERE. The loader honours the manifest's
    // normal_format and flips green at load for a DirectX pack, so every map
    // reaching this point is one convention and the shader needs no branch --
    // the same arrangement material_library.cpp uses on the Dawn side.
    let tn = normal_tex.xyz * 2.0 - 1.0;
    let N = normalize(T * tn.x + B * tn.y + N0 * tn.z);

    let V = normalize(cam - world);
    let ao = arm.r;
    let roughness = arm.g;
    let metallic = arm.b;

    let view = uint(frame.params.x);
    // Every debug branch below emits a display-referred CODE VALUE, converted
    // once to linear because the target is linear. The output pass encodes it
    // straight back, so a readback returns the byte the view claims to show.
    if (view == kViewTriangleId) { return float4(SrgbToLinear(HashId(prim)), 1.0); }
    if (view == kViewBarycentric) { return float4(SrgbToLinear(saturate(bary.lambda)), 1.0); }
    if (view == kViewDepth) {
        // View DISTANCE normalized between near and far, white = near. Raw
        // reversed-Z would cluster against 1.0 under perspective and read as a
        // flat white image.
        let near = frame.params.y;
        let far = frame.params.z;
        let t = saturate((length(cam - world) - near) / max(far - near, 1e-4));
        return float4(SrgbToLinear(float3(1.0 - t)), 1.0);
    }
    if (view == kViewAlbedo) {
        // The sampler already decoded sRGB -> linear, and the albedo view wants
        // the SOURCE BYTE on screen -- which is what the linear value already
        // is. No round trip: it is emitted as-is.
        return float4(albedo_srgb.rgb, 1.0);
    }
    if (view == kViewNormal) { return float4(SrgbToLinear(N * 0.5 + 0.5), 1.0); }
    // The scalar channels are LINEAR DATA shown as code values: a roughness
    // texel of 128/255 must read back as 128, which is a claim about a code
    // value rather than an intensity.
    if (view == kViewRoughness) { return float4(SrgbToLinear(float3(roughness)), 1.0); }
    if (view == kViewMetallic) { return float4(SrgbToLinear(float3(metallic)), 1.0); }
    if (view == kViewDisplacement) { return float4(SrgbToLinear(float3(disp.r)), 1.0); }
    if (view == kViewAo) { return float4(SrgbToLinear(float3(ao)), 1.0); }

    // Lit. shadow = 1 and ambientSpecular = 0: no shadow maps and no IBL in
    // this stage, and the parameters stay so the port matches its WESL original
    // line for line.
    let lit = ShadeStandard(albedo_srgb.rgb, N, V, roughness, ao, metallic,
                            1.0, float3(0.0), frame.sun_direction.xyz,
                            frame.sun_color.rgb, frame.ambient_sh);
    // SCENE-REFERRED and unclamped. The tonemap belongs to the output pass --
    // putting it here would clamp the headroom before anything could use it,
    // and would tone-curve the debug views above as well.
    return float4(lit, 1.0);
}
