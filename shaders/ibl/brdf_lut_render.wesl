// BRDF LUT Generation (Split-Sum Approximation for IBL) - Render Pass Version
//
// Ported verbatim from sampo's src/shaders/compute/brdf_lut_render.wesl
// (already a fullscreen render shader there). Pre-computes the environment BRDF
// integral for different (NdotV, roughness) pairs.
//
// Output: RG16Float texture where:
// - R = scale (multiply with F0)
// - G = bias (add to result)
// - Final Fresnel: F0 * R + G
//
// Has no bindings — the fullscreen triangle + integral are entirely
// vertex-index / UV driven.

const PI: f32 = 3.14159265359;
const SAMPLE_COUNT: u32 = 1024u;

struct VertexOutput {
    @builtin(position) position: vec4<f32>,
    @location(0) uv: vec2<f32>,
}

@vertex
fn vs_main(@builtin(vertex_index) vertex_index: u32) -> VertexOutput {
    // Fullscreen triangle
    var output: VertexOutput;
    let x = f32((vertex_index << 1u) & 2u);
    let y = f32(vertex_index & 2u);
    output.position = vec4<f32>(x * 2.0 - 1.0, y * 2.0 - 1.0, 0.0, 1.0);
    output.uv = vec2<f32>(x, y);
    return output;
}

// Van der Corput sequence for low-discrepancy sampling
fn radicalInverseVdC(bits_in: u32) -> f32 {
    var bits = bits_in;
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return f32(bits) * 2.3283064365386963e-10;
}

// Hammersley 2D quasi-random sequence
fn hammersley(i: u32, N: u32) -> vec2<f32> {
    return vec2<f32>(f32(i) / f32(N), radicalInverseVdC(i));
}

// GGX importance sampling - generates microfacet half-vector
fn importanceSampleGGX(Xi: vec2<f32>, N: vec3<f32>, roughness: f32) -> vec3<f32> {
    let a = roughness * roughness;

    // Spherical coordinates from Hammersley point
    let phi = 2.0 * PI * Xi.x;
    let cosTheta = sqrt((1.0 - Xi.y) / (1.0 + (a * a - 1.0) * Xi.y));
    let sinTheta = sqrt(1.0 - cosTheta * cosTheta);

    // Convert to Cartesian (tangent space)
    let H = vec3<f32>(
        cos(phi) * sinTheta,
        sin(phi) * sinTheta,
        cosTheta
    );

    // Build tangent space basis
    // When N is aligned with Z, use X as up; otherwise use Z as up
    let up = select(vec3<f32>(1.0, 0.0, 0.0), vec3<f32>(0.0, 0.0, 1.0), abs(N.z) < 0.999);
    let tangent = normalize(cross(up, N));
    let bitangent = cross(N, tangent);

    // Transform to world space
    return normalize(tangent * H.x + bitangent * H.y + N * H.z);
}

// Smith geometry function for a single direction
fn geometrySchlickGGX(NdotV: f32, roughness: f32) -> f32 {
    let a = roughness;
    let k = (a * a) / 2.0;
    return NdotV / (NdotV * (1.0 - k) + k);
}

// Smith geometry function (accounts for both view and light direction)
fn geometrySmith(N: vec3<f32>, V: vec3<f32>, L: vec3<f32>, roughness: f32) -> f32 {
    let NdotV = max(dot(N, V), 0.0);
    let NdotL = max(dot(N, L), 0.0);
    return geometrySchlickGGX(NdotV, roughness) * geometrySchlickGGX(NdotL, roughness);
}

// Integrate BRDF for a given (NdotV, roughness) pair
fn integrateBRDF(NdotV: f32, roughness: f32) -> vec2<f32> {
    // View vector (normal aligned with Z-axis)
    let V = vec3<f32>(sqrt(1.0 - NdotV * NdotV), 0.0, NdotV);
    let N = vec3<f32>(0.0, 0.0, 1.0);

    var A = 0.0;
    var B = 0.0;

    for (var i = 0u; i < SAMPLE_COUNT; i++) {
        let Xi = hammersley(i, SAMPLE_COUNT);
        let H = importanceSampleGGX(Xi, N, roughness);
        let L = normalize(2.0 * dot(V, H) * H - V);

        let NdotL = max(L.z, 0.0);
        let NdotH = max(H.z, 0.0);
        let VdotH = max(dot(V, H), 0.0);

        if (NdotL > 0.0) {
            let G = geometrySmith(N, V, L, roughness);
            let G_Vis = (G * VdotH) / (NdotH * NdotV);
            let Fc = pow(1.0 - VdotH, 5.0);

            A += (1.0 - Fc) * G_Vis;
            B += Fc * G_Vis;
        }
    }

    return vec2<f32>(A, B) / f32(SAMPLE_COUNT);
}

@fragment
fn fs_main(input: VertexOutput) -> @location(0) vec2<f32> {
    // UV coordinates: X = NdotV, Y = roughness
    // Note: Y is flipped because WebGPU texture (0,0) is top-left,
    // but clip space (-1,-1) is bottom-left where our UV starts at 0
    let NdotV = max(input.uv.x, 0.001);
    let roughness = max(1.0 - input.uv.y, 0.001);

    return integrateBRDF(NdotV, roughness);
}
