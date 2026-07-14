// Shared GBuffer encoding utilities
// Used by all shaders that write to the G-Buffer
//
// Ported verbatim from sampo's src/shaders/common/gbuffer_encode.wesl.

// G-Buffer output structure for deferred rendering
// Must match the render target configuration in RenderStateId::kGBuffer
struct GBufferOutput {
    @location(0) normals: vec2<f32>,           // RG16F: octahedron-encoded world normal
    @location(1) albedo: vec4<f32>,            // BGRA8: base color
    @location(2) material: vec4<f32>,          // RGBA8: R=roughness, G=shadow, B/A=reserved
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
