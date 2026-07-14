// IBL pre-filter — RENDER-PATH port of sampo's src/shaders/compute/
// ibl_prefilter.wesl.
//
// badlands' trimmed GpuContext never enabled storage textures, and RGBA16Float
// storage-write needs a device feature it lacks. RGBA16Float IS renderable, so
// instead of a compute shader that `textureStore`s into a storage view, we run
// this as a FRAGMENT shader and render a fullscreen triangle into each cube
// face + mip level view (PrefilteredCubemap::Generate drives the loop). The
// GGX importance-sample math (radicalInverseVdC / hammersley /
// importanceSampleGGX / faceUVToDirection / prefilterEnvMap) is verbatim from
// sampo; only the I/O harness changed:
//   - compute `@builtin(global_invocation_id)` -> fragment `@builtin(position)`
//     (per-texel destination texel center);
//   - `textureStore(output_face, ...)` -> `return` at `@location(0)`.
//
// Each mip = one roughness (roughness = mip/(mips-1)); the target face index +
// roughness + output size arrive via the params uniform.

const PI: f32 = 3.14159265359;

// Sample count varies by mip level (more samples for rough surfaces)
const MAX_SAMPLES: u32 = 1024u;

struct PrefilterParams {
    roughness: f32,       // Roughness for this mip level
    face_index: u32,      // Which cubemap face (0-5)
    output_size: u32,     // Size of the output face at this mip
    _padding: u32,
}

@group(0) @binding(0) var source_cubemap: texture_cube<f32>;
@group(0) @binding(1) var source_sampler: sampler;
@group(0) @binding(2) var<uniform> params: PrefilterParams;

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
    let up = select(vec3<f32>(0.0, 0.0, 1.0), vec3<f32>(1.0, 0.0, 0.0), abs(N.z) < 0.999);
    let tangent = normalize(cross(up, N));
    let bitangent = cross(N, tangent);

    // Transform to world space
    return normalize(tangent * H.x + bitangent * H.y + N * H.z);
}

// Convert face index and UV to cubemap direction.
// Standard cube-map major-axis convention shared by WebGPU/D3D/Vulkan/Metal
// (v = 0 at the TOP of the face, matching @builtin(position).y / output_size).
fn faceUVToDirection(face: u32, uv: vec2<f32>) -> vec3<f32> {
    // Map UV from [0,1] to [-1,1]
    let s = uv.x * 2.0 - 1.0;
    let t = uv.y * 2.0 - 1.0;

    switch (face) {
        case 0u: { return normalize(vec3<f32>( 1.0, -t, -s)); }  // +X
        case 1u: { return normalize(vec3<f32>(-1.0, -t,  s)); }  // -X
        case 2u: { return normalize(vec3<f32>( s,  1.0,  t)); }  // +Y
        case 3u: { return normalize(vec3<f32>( s, -1.0, -t)); }  // -Y
        case 4u: { return normalize(vec3<f32>( s, -t,  1.0)); }  // +Z
        case 5u: { return normalize(vec3<f32>(-s, -t, -1.0)); }  // -Z
        default: { return vec3<f32>(0.0, 1.0, 0.0); }
    }
}

// Pre-filter environment map for a given roughness
fn prefilterEnvMap(N: vec3<f32>, roughness: f32) -> vec3<f32> {
    // For roughness = 0, just sample the environment directly
    if (roughness < 0.001) {
        return textureSampleLevel(source_cubemap, source_sampler, N, 0.0).rgb;
    }

    // Assume V = N (isotropic, no view-dependent effects for pre-filtering)
    let V = N;

    var prefilteredColor = vec3<f32>(0.0);
    var totalWeight = 0.0;

    // Adaptive sample count: fewer samples for smoother surfaces
    let sample_count = select(MAX_SAMPLES / 4u, MAX_SAMPLES, roughness > 0.5);

    for (var i = 0u; i < sample_count; i++) {
        let Xi = hammersley(i, sample_count);
        let H = importanceSampleGGX(Xi, N, roughness);
        let L = normalize(2.0 * dot(V, H) * H - V);

        let NdotL = max(dot(N, L), 0.0);

        if (NdotL > 0.0) {
            // Sample environment in light direction
            let sampleColor = textureSampleLevel(source_cubemap, source_sampler, L, 0.0).rgb;

            // Weight by NdotL (Lambert cosine term)
            prefilteredColor += sampleColor * NdotL;
            totalWeight += NdotL;
        }
    }

    if (totalWeight > 0.0) {
        prefilteredColor /= totalWeight;
    }

    return prefilteredColor;
}

struct VertexOutput {
    @builtin(position) position: vec4<f32>,
}

@vertex
fn vs_main(@builtin(vertex_index) vertex_index: u32) -> VertexOutput {
    // Fullscreen triangle covering the whole face+mip render target.
    var output: VertexOutput;
    let x = f32((vertex_index << 1u) & 2u);
    let y = f32(vertex_index & 2u);
    output.position = vec4<f32>(x * 2.0 - 1.0, y * 2.0 - 1.0, 0.0, 1.0);
    return output;
}

@fragment
fn fs_main(input: VertexOutput) -> @location(0) vec4<f32> {
    // Texel center UV in [0,1], v = 0 at the top of the face (position.y is the
    // fragment's pixel-center y, 0 at the top of the attachment).
    let uv = input.position.xy / f32(params.output_size);

    // World-space direction for this texel (matches hardware cube sampling)
    let N = faceUVToDirection(params.face_index, uv);

    // Pre-filter the environment
    let color = prefilterEnvMap(N, params.roughness);

    return vec4<f32>(color, 1.0);
}
