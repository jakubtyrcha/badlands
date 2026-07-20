// Fog media composer (Task: map fog generator). Writes media (scattering sigma_s
// in rgb, extinction sigma_t in a) into a texel sub-box of one cascade of the
// height-band clipmap. For each froxel it reconstructs the world position, finds
// the overlapping fog emitters via a world-space broadphase grid, evaluates each
// (fog_emitters.wesl) and takes the MAX of their sigma_t (extinction unions, like the
// analytic fallback shapes; overlapping emitters do not sum to a brighter core).
//
// When useEmitters == 0 (no sources) it falls back to the analytic placeholder
// shapes (fog_shapes.wesl) so the viewer/ai_sandbox still show fog.
import package::common::fog_shapes::{fogShapesDensity};
import package::common::fog_emitters::{GpuEmitter, fogEmitterEval};

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
    // Composer: world-space broadphase grid + animation time.
    bpMinX: f32,
    bpMinZ: f32,
    bpCellSize: f32,
    bpNx: i32,
    bpNz: i32,
    emitterCount: u32,
    useEmitters: i32,
    time: f32,
}

@group(0) @binding(0) var<uniform> params: FogFillParams;
@group(0) @binding(1) var mediaOut: texture_storage_3d<rgba16float, write>;
@group(0) @binding(2) var<storage, read> emitters: array<GpuEmitter>;
@group(0) @binding(3) var<storage, read> bpCells: array<vec2<u32>>;   // (offset,count)
@group(0) @binding(4) var<storage, read> bpIndices: array<u32>;

// Physical extinction at density 1 for the analytic FALLBACK only (pea-souper).
const kFogExtinctionAtDensity1: f32 = 0.3;
// White single-scatter albedo so dense fog takes the lit fog colour, not black.
const kFogScatterColor: vec3<f32> = vec3<f32>(1.0, 1.0, 1.0);

fn fogPosMod(a: i32, n: i32) -> i32 {
    return ((a % n) + n) % n;
}

// Accumulate every emitter overlapping worldPos's broadphase cell.
fn composeEmitters(worldPos: vec3<f32>) -> f32 {
    let bpx = i32(floor((worldPos.x - params.bpMinX) / params.bpCellSize));
    let bpz = i32(floor((worldPos.z - params.bpMinZ) / params.bpCellSize));
    if (params.emitterCount == 0u || bpx < 0 || bpz < 0 ||
        bpx >= params.bpNx || bpz >= params.bpNz) {
        return 0.0;
    }
    let cell = bpCells[u32(bpz * params.bpNx + bpx)];
    let offset = cell.x;
    let count = cell.y;
    var sigma_t = 0.0;
    for (var k: u32 = 0u; k < count; k = k + 1u) {
        let e = emitters[bpIndices[offset + k]];
        // MAX, not sum: overlapping emitters union their density (fogEmitterEval >= 0).
        sigma_t = max(sigma_t, fogEmitterEval(e, worldPos, params.time));
    }
    return sigma_t;
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
    let worldPos = vec3<f32>(worldX, worldY, worldZ);

    var sigma_t: f32;
    if (params.useEmitters != 0) {
        sigma_t = composeEmitters(worldPos);
    } else {
        sigma_t = fogShapesDensity(worldPos) * kFogExtinctionAtDensity1;
    }
    sigma_t = max(sigma_t, 0.0);
    let sigma_s = kFogScatterColor * sigma_t;

    let globalZ = params.cascadeIndex * params.resY + sy;
    textureStore(mediaOut, vec3<i32>(tx, tz, globalZ),
                 vec4<f32>(sigma_s, sigma_t));
}
