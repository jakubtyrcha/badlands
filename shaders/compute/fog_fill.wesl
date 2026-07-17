// Fog media fill (Task: fog rendering). Writes media (scattering sigma_s in rgb,
// extinction sigma_t in a) into a texel sub-box of one cascade of the height-band
// clipmap. Dispatched per (cascade, dirty box): on a camera scroll only the
// newly-exposed columns are refilled; the world-space density is a pure function
// of position, so toroidal partial fills match a full refill.
//
// Media comes from analytic test shapes (fog_shapes.wesl) evaluated per voxel.
import package::common::fog_shapes::{fogShapesDensity};

struct FogFillParams {
    minVoxelX: i32,
    minVoxelZ: i32,
    boxOffX: i32,       // texel offset of this box in the cascade (X), in [0,res)
    boxOffZ: i32,
    boxSizeX: i32,
    boxSizeZ: i32,
    resXZ: i32,
    resY: i32,
    cascadeIndex: i32,
    cascadeCount: i32,
    pad0: i32,
    pad1: i32,
    voxelSizeXZ: f32,
    voxelSizeY: f32,
    floorY: f32,
    height: f32,
}

@group(0) @binding(0) var<uniform> params: FogFillParams;
@group(0) @binding(1) var mediaOut: texture_storage_3d<rgba16float, write>;

// Physical extinction at density 1: "pea-souper" extreme fog, visibility ~10 m
// (Koschmieder: 3.912 / sigma_t ~= 13 m). Physical units, per metre.
const kFogExtinctionAtDensity1: f32 = 0.3;
// White single-scatter albedo (albedo ~= 1) so dense fog takes over the pixel
// colour with the lit fog colour rather than absorbing to black.
const kFogScatterColor: vec3<f32> = vec3<f32>(1.0, 1.0, 1.0);

fn fogPosMod(a: i32, n: i32) -> i32 {
    return ((a % n) + n) % n;
}

@compute @workgroup_size(4, 4, 4)
fn main(@builtin(global_invocation_id) gid: vec3<u32>) {
    // Dispatch is ceil-rounded; skip invocations past the box / band.
    if (i32(gid.x) >= params.boxSizeX || i32(gid.y) >= params.boxSizeZ ||
        i32(gid.z) >= params.resY) {
        return;
    }
    // Boxes never straddle the toroidal seam, so texel = offset + local (no wrap).
    let tx = params.boxOffX + i32(gid.x);
    let tz = params.boxOffZ + i32(gid.y);
    let sy = i32(gid.z);

    // World voxel this texel currently holds (the one inside the window that is
    // congruent to the texel mod res).
    let wvx = params.minVoxelX + fogPosMod(tx - params.minVoxelX, params.resXZ);
    let wvz = params.minVoxelZ + fogPosMod(tz - params.minVoxelZ, params.resXZ);
    let worldX = (f32(wvx) + 0.5) * params.voxelSizeXZ;
    let worldZ = (f32(wvz) + 0.5) * params.voxelSizeXZ;
    let worldY = params.floorY + (f32(sy) + 0.5) * params.voxelSizeY;

    let density = fogShapesDensity(vec3<f32>(worldX, worldY, worldZ));
    let sigma_t = density * kFogExtinctionAtDensity1;
    let sigma_s = kFogScatterColor * sigma_t;

    let globalZ = params.cascadeIndex * params.resY + sy;
    textureStore(mediaOut, vec3<i32>(tx, tz, globalZ),
                 vec4<f32>(sigma_s, sigma_t));
}
