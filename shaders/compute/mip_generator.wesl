// Mipmap generation compute shader
// Downsamples a source mip level to a destination mip level using a 2x2 box filter.

// Configurable via @if directives:
// - is_cubemap: Support cubemap array layers
// - use_srgb: Convert input from sRGB to linear before averaging, then back to sRGB
// - filter_box: Use simple box filter (default)
// - filter_linear: (Placeholder for potentially better filtering)

import package::common::color::{srgb_to_linear, linear_to_srgb};

@if(!is_cubemap)
@group(0) @binding(0) var src_texture: texture_2d<f32>;
@if(is_cubemap)
@group(0) @binding(0) var src_texture: texture_2d_array<f32>;

@if(!is_cubemap)
@group(0) @binding(1) var dst_texture: texture_storage_2d<rgba8unorm, write>;
@if(is_cubemap)
@group(0) @binding(1) var dst_texture: texture_storage_2d_array<rgba8unorm, write>;

// Dispatch with (dst_width / 8, dst_height / 8, 1 (or 6 for cubemap))
@compute @workgroup_size(8, 8, 1)
fn main(@builtin(global_invocation_id) id: vec3<u32>) {
    // Determine destination coordinates
    let dst_coords = vec2<i32>(id.xy);
    
    // Determine array layer (0 for 2D, 0-5 for cubemap)
    let layer = i32(id.z);

    // Get dimensions of destination to check bounds
    @if(!is_cubemap)
    let dst_size = textureDimensions(dst_texture);
    @if(is_cubemap)
    let dst_size = textureDimensions(dst_texture).xy;

    if (dst_coords.x >= i32(dst_size.x) || dst_coords.y >= i32(dst_size.y)) {
        return;
    }

    // Source coordinates start at 2 * dst
    let src_origin = dst_coords * 2;

    // Fetch 4 samples (box filter)
    var c00: vec4<f32>;
    var c10: vec4<f32>;
    var c01: vec4<f32>;
    var c11: vec4<f32>;

    @if(!is_cubemap) {
        c00 = textureLoad(src_texture, src_origin + vec2<i32>(0, 0), 0);
        c10 = textureLoad(src_texture, src_origin + vec2<i32>(1, 0), 0);
        c01 = textureLoad(src_texture, src_origin + vec2<i32>(0, 1), 0);
        c11 = textureLoad(src_texture, src_origin + vec2<i32>(1, 1), 0);
    }
    @if(is_cubemap) {
        c00 = textureLoad(src_texture, src_origin + vec2<i32>(0, 0), layer, 0);
        c10 = textureLoad(src_texture, src_origin + vec2<i32>(1, 0), layer, 0);
        c01 = textureLoad(src_texture, src_origin + vec2<i32>(0, 1), layer, 0);
        c11 = textureLoad(src_texture, src_origin + vec2<i32>(1, 1), layer, 0);
    }

    // Optional sRGB conversion
    @if(use_srgb) {
        c00 = vec4<f32>(srgb_to_linear(c00.rgb), c00.a);
        c10 = vec4<f32>(srgb_to_linear(c10.rgb), c10.a);
        c01 = vec4<f32>(srgb_to_linear(c01.rgb), c01.a);
        c11 = vec4<f32>(srgb_to_linear(c11.rgb), c11.a);
    }

    // Average
    var avg = (c00 + c10 + c01 + c11) * 0.25;

    // Convert back if needed
    @if(use_srgb) {
        avg = vec4<f32>(linear_to_srgb(avg.rgb), avg.a);
    }

    // Write to destination
    @if(!is_cubemap)
    textureStore(dst_texture, dst_coords, avg);
    @if(is_cubemap)
    textureStore(dst_texture, dst_coords, layer, avg);
}
