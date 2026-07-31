// Shared GBuffer encoding utilities
// Used by all shaders that write to the G-Buffer
//
// Ported verbatim from sampo's src/shaders/common/gbuffer_encode.wesl.

// G-Buffer output structure for deferred rendering
// Must match the render target configuration in RenderStateId::kGBuffer
struct GBufferOutput {
    @location(0) normals: vec2<f32>,           // RG16F: octahedron-encoded world normal
    @location(1) albedo: vec4<f32>,            // BGRA8: base color
    @location(2) material: vec4<f32>,          // RGBA8: R=roughness, G=shadow, B=baked AO, A=reserved
}

// Octahedron encoding for unit normal -> RG [-1,1]
// Encodes a unit normal into a 2D octahedron representation
// Reference: "A Survey of Efficient Representations for Independent Unit Vectors"
fn encodeOctahedron(n: vec3<f32>) -> vec2<f32> {
    let absN = abs(n);
    var oct = n.xy / (absN.x + absN.y + absN.z);
    if (n.z < 0.0) {
        oct = (1.0 - abs(oct.yx)) * select(vec2<f32>(-1.0), vec2<f32>(1.0), oct >= vec2<f32>(0.0));
    }
    return oct;
}

// Shading model tag written to material.a. 0.0 (the standard material's
// packNormalMappedGBuffer below, and the G-buffer's {.., 0.0} clear value —
// scene_renderer.cpp) means the standard opaque path; deferred_lighting.wesl
// gates its added foliage-transmission term on `material.a > 0.5`, so this
// must be comfortably above that midpoint threshold with headroom for 8-bit
// RGBA8Unorm quantization.
const kShadingModelFoliage: f32 = 1.0;

// Textureless voxel-foliage GBuffer assembly (Phase 2 of the volumetric-
// foliage feature): unlike packNormalMappedGBuffer, there is no tangent-space
// normal map (voxels have no UV-mapped textures) so `worldNormal` is written
// straight through -- no TBN reconstruction. `material.g` carries the
// translucency strength (instead of the standard path's constant 1.0 "no
// shadow" sentinel -- foliage pixels are never shadow-masked via material.g)
// so deferred_lighting.wesl's evaluateTranslucency call can read it back.
fn packVoxelFoliageGBuffer(worldNormal: vec3<f32>, albedo: vec3<f32>,
                           roughness: f32, ao: f32,
                           translucencyStrength: f32) -> GBufferOutput {
    var output: GBufferOutput;
    output.normals = encodeOctahedron(normalize(worldNormal));
    output.albedo = vec4<f32>(albedo, 1.0);
    // Material: R=roughness, G=translucency strength, B=AO, A=shading model tag.
    output.material = vec4<f32>(roughness, translucencyStrength, ao, kShadingModelFoliage);
    return output;
}

// Shared tangent-space-normal-mapped GBuffer assembly: Gram-Schmidt TBN
// reconstruction + tangent-space normal unpack + octahedron encode + GBuffer
// packing. `albedo` and `normalMap`/`arm` are the caller's own texture
// samples (2D, cubemap, and/or tint-multiplied — sampling differs by caller;
// the assembly below does not), so this is shared by normalmapped.wesl's 2D
// fs_main and instanced_gbuffer.wesl's fs_main.
fn packNormalMappedGBuffer(worldNormal: vec3<f32>, worldTangent: vec3<f32>,
                           albedo: vec3<f32>, normalMap: vec3<f32>, arm: vec4<f32>) -> GBufferOutput {
    // Reconstruct TBN matrix from interpolated vectors
    let N = normalize(worldNormal);
    let T = normalize(worldTangent - N * dot(worldTangent, N)); // Gram-Schmidt
    let B = cross(N, T);
    let TBN = mat3x3<f32>(T, B, N);

    let roughness = arm.g;
    let ao = arm.r;

    // Unpack normal from [0,1] to [-1,1] range. Arrives GL-convention from
    // the loader (see normalmapped.wesl's file-level Textures comment) -- no
    // flip needed.
    let tangentNormal = normalMap * 2.0 - 1.0;
    let outNormal = normalize(TBN * tangentNormal);

    var output: GBufferOutput;
    output.normals = encodeOctahedron(outNormal);
    output.albedo = vec4<f32>(albedo, 1.0);
    // Material: R=roughness, G=shadow (1.0 = no shadow), B=baked AO, A=reserved
    output.material = vec4<f32>(roughness, 1.0, ao, 0.0);
    return output;
}
