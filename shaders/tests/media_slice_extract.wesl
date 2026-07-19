// Test-only: copy one Z-slice's extinction (sigma_t = media.a) of the fog media
// 3D texture (RGBA16Float) to an R32Float 2D target so CpuImage / TextureReadback
// can read it back. Used to verify fog_fill's sim reconstruction path.
struct SliceParams {
    slice: i32,
    pad0: i32,
    pad1: i32,
    pad2: i32,
}

@group(0) @binding(0) var<uniform> params: SliceParams;
@group(0) @binding(1) var mediaIn: texture_3d<f32>;
@group(0) @binding(2) var outTex: texture_storage_2d<r32float, write>;

@compute @workgroup_size(8, 8, 1)
fn main(@builtin(global_invocation_id) gid: vec3<u32>) {
    let dims = textureDimensions(outTex);
    if (gid.x >= dims.x || gid.y >= dims.y) {
        return;
    }
    let v = textureLoad(mediaIn, vec3<i32>(i32(gid.x), i32(gid.y), params.slice), 0);
    textureStore(outTex, vec2<i32>(i32(gid.x), i32(gid.y)),
                 vec4<f32>(v.a, 0.0, 0.0, 0.0));
}
