// Test-only fog fill (Task: fog rendering graphical tests). Writes the cascade
// media (rgb = scattering sigma_s, a = extinction sigma_t) with a selectable
// analytic pattern. The cascade media IS the interface under test: this shader
// injects the fog, the PRODUCTION raymarch consumes it. Full-grid dispatch per
// cascade (static camera per frame -> no toroidal dirty-region logic), using the
// SAME world<->voxel addressing as the production fog_fill.
//
// mode 0 (UNIFORM):  constant sigma_t inside a world box, white albedo.
// mode 1 (GRADIENT): constant sigma_t inside a world box, sigma_s tinted by a
//                    height gradient (col0 at gradY0 -> col1 at gradY1) so
//                    scattering and extinction are independently authored.
struct FogFillParams {
    minVoxelX: i32,
    minVoxelZ: i32,
    resXZ: i32,
    resY: i32,
    cascadeIndex: i32,
    cascadeCount: i32,
    mode: i32,
    pad0: i32,
    voxelSizeXZ: f32,
    voxelSizeY: f32,
    floorY: f32,
    height: f32,
    bx0: f32,
    by0: f32,
    bz0: f32,
    sigmaT: f32,
    bx1: f32,
    by1: f32,
    bz1: f32,
    gradY0: f32,
    col0R: f32,
    col0G: f32,
    col0B: f32,
    gradY1: f32,
    col1R: f32,
    col1G: f32,
    col1B: f32,
    pad1: f32,
}

@group(0) @binding(0) var<uniform> p: FogFillParams;
@group(0) @binding(1) var mediaOut: texture_storage_3d<rgba16float, write>;

fn pmod(a: i32, n: i32) -> i32 {
    return ((a % n) + n) % n;
}

@compute @workgroup_size(4, 4, 4)
fn main(@builtin(global_invocation_id) gid: vec3<u32>) {
    let tx = i32(gid.x);
    let tz = i32(gid.y);
    let sy = i32(gid.z);
    if (tx >= p.resXZ || tz >= p.resXZ || sy >= p.resY) {
        return;
    }
    let wvx = p.minVoxelX + pmod(tx - p.minVoxelX, p.resXZ);
    let wvz = p.minVoxelZ + pmod(tz - p.minVoxelZ, p.resXZ);
    let wx = (f32(wvx) + 0.5) * p.voxelSizeXZ;
    let wz = (f32(wvz) + 0.5) * p.voxelSizeXZ;
    let wy = p.floorY + (f32(sy) + 0.5) * p.voxelSizeY;

    var sigma_t = 0.0;
    var sigma_s = vec3<f32>(0.0);
    if (wx >= p.bx0 && wx <= p.bx1 && wy >= p.by0 && wy <= p.by1 &&
        wz >= p.bz0 && wz <= p.bz1) {
        sigma_t = p.sigmaT;
        if (p.mode == 1) {
            let t = clamp((wy - p.gradY0) / max(p.gradY1 - p.gradY0, 1e-4), 0.0, 1.0);
            let col = mix(vec3<f32>(p.col0R, p.col0G, p.col0B),
                          vec3<f32>(p.col1R, p.col1G, p.col1B), t);
            sigma_s = col * sigma_t;  // albedo tint (scattering decoupled from extinction)
        } else {
            sigma_s = vec3<f32>(sigma_t, sigma_t, sigma_t);  // white albedo (1)
        }
    }

    let globalZ = p.cascadeIndex * p.resY + sy;
    textureStore(mediaOut, vec3<i32>(tx, tz, globalZ), vec4<f32>(sigma_s, sigma_t));
}
