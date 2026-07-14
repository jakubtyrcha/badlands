// Render-based Mipmap Generation Shader
// Used when StorageBinding is not supported but RenderAttachment is.

// We render a full-screen quad. The vertex shader generates the quad.
// The fragment shader samples the source texture (previous mip level) and averages it.

// Configurable via @if directives (handled by WESL):
// - is_cubemap: Support cubemap array layers (treated as 2D array for rendering)


struct VertexOutput {
    @builtin(position) position: vec4<f32>,
    @location(0) uv: vec2<f32>,
    @location(1) @interpolate(flat) layer: i32, // Passed through for array rendering if needed, though usually we render to one view
};

// We render into a specific mip level view of the destination texture.
// The source is the previous mip level of the temporary (or source) texture.

@if(!is_cubemap)
@group(0) @binding(0) var src_texture: texture_2d<f32>;
@if(is_cubemap)
@group(0) @binding(0) var src_texture: texture_2d_array<f32>;

@group(0) @binding(1) var src_sampler: sampler;

// Push constants or Uniforms for layer index if we are rendering layer by layer?
// Or we can use instance index if we use instanced rendering for layers.
// For simplicity, let's assume we render one layer at a time or use instancing.
// Let's use a uniform buffer for the layer index if handling cubemaps/arrays one by one.
@vertex
fn vs_main(@builtin(vertex_index) vertex_index: u32) -> VertexOutput {
    var out: VertexOutput;
    // Full screen triangle
    // 0: (-1, -1), 1: (3, -1), 2: (-1, 3)
    let x = f32((vertex_index << 1u) & 2u);
    let y = f32(vertex_index & 2u);
    out.position = vec4<f32>(x * 2.0 - 1.0, y * 2.0 - 1.0, 0.0, 1.0);
    out.uv = vec2<f32>(x, 1.0 - y); // Flip Y for UV if needed or just align.
    
    // Let's use standard hardcoded:
    var pos = vec2<f32>(0.0, 0.0);
    if (vertex_index == 0u) { pos = vec2<f32>(-1.0, -1.0); out.uv = vec2<f32>(0.0, 1.0); }
    else if (vertex_index == 1u) { pos = vec2<f32>(3.0, -1.0); out.uv = vec2<f32>(2.0, 1.0); }
    else { pos = vec2<f32>(-1.0, 3.0); out.uv = vec2<f32>(0.0, -1.0); }
    out.position = vec4<f32>(pos, 0.0, 1.0);

    out.layer = 0;
    return out;
}

@fragment
fn fs_main(in: VertexOutput) -> @location(0) vec4<f32> {
    // We are rendering to Mip K. We sample Mip K-1.
    // The vertex shader gives us UVs.
    // We should sample with linear filtering to get the average.
    // Ideally we want exact box filter. 
    // Render attachment means we are drawing onto the pixels of the destination.
    // The "camera" is effectively covering the destination mip.
    
    // If we rely on linear filtering from the sampler:
    // It works but might be slightly inaccurate compared to 4-sample box.
    // But format caps said "filterable".
    
    var color: vec4<f32>;
    
    @if(!is_cubemap) {
        color = textureSample(src_texture, src_sampler, in.uv);
    }
    @if(is_cubemap) {
        // For cubemap/array, we need to sample the correct layer.
        // textureSample for 2d_array takes uv and array_index
        color = textureSample(src_texture, src_sampler, in.uv, in.layer);
    }
    
    return color;
}
