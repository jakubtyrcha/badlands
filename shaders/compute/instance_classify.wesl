// GPU-driven instanced rendering, Phase D — pass 1 of 3: CLASSIFY + COUNT.
// (See .superpowers/sdd/phase-D-brief.md / phase-D-report.md.) Generalizes
// Phase C's single-bucket frustum-cull: each surviving instance is routed into
// one `(model, lod)` bucket, where `lod` is a distance-based level-of-detail
// selection against `config.lodThresholds`. Per instance i (of counts.x):
//   * frustum-cull the world-space bounding sphere (reuses Phase C's EXACT
//     `sphereCulled` convention — see below); if culled, mark SENTINEL + return.
//   * else classify: bucket = modelId*kMaxLods + lod(distance-to-camera), record
//     it in perInstanceBucket[i], and atomically bump bucketCount[bucket].
// perInstanceBucket + bucketCount are consumed by the scan (pass 2) and scatter
// (pass 3). Single input set, group-0 bindings, no dynamic offsets.

// Mirrors GpuInstanceRenderer's CullConfigData byte-for-byte (144 bytes).
struct CullConfig {
    planes: array<vec4<f32>, 6>,   // 6 inward-facing frustum planes (a,b,c,d)
    cameraWorldPos: vec4<f32>,     // xyz = camera world position, w unused
    lodThresholds: vec4<f32>,      // x=t0, y=t1 (kMaxLods-1 thresholds); z,w unused
    counts: vec4<u32>,             // x=instanceCount, y=numBuckets, z=compactedCapacity, w unused
}

// Static per-instance input. Mirrors GpuInstanceRenderer::InstanceInput
// byte-for-byte (96 bytes).
struct InstanceData {
    transform: mat4x4<f32>,    // world transform
    boundsSphere: vec4<f32>,   // xyz = world center, w = radius
    modelInfo: vec4<u32>,      // x = model id (selects the bucket group)
}

// Compile-time LOD count. `bucket = modelId*kMaxLods + lod`, so this must match
// GpuInstanceRenderer::kMaxLods (the C++ side sizes bucket buffers as
// numModels*kMaxLods). kMaxLods-1 distance thresholds live in lodThresholds.
const kMaxLods: u32 = 3u;
// Marks a culled (or out-of-range) instance in perInstanceBucket; the scatter
// pass skips these. Distinct from any real bucket id (buckets are < numBuckets).
const SENTINEL: u32 = 0xFFFFFFFFu;

@group(0) @binding(0) var<uniform> config: CullConfig;
@group(0) @binding(1) var<storage, read> instances: array<InstanceData>;
@group(0) @binding(2) var<storage, read_write> perInstanceBucket: array<u32>;
@group(0) @binding(3) var<storage, read_write> bucketCount: array<atomic<u32>>;

// A sphere is culled (fully outside the frustum) iff it is fully outside ANY
// single plane's half-space: dot(plane.xyz, center) + plane.w < -radius. This is
// the sphere-specialized form of Frustum::Intersects' AABB "positive vertex"
// test — identical to Phase C's instance_cull.wesl (see the phase-C report for
// the derivation from engine/rendering/frustum.{hpp,cpp}).
fn sphereCulled(center: vec3<f32>, radius: f32) -> bool {
    for (var i = 0u; i < 6u; i = i + 1u) {
        let p = config.planes[i];
        if (dot(p.xyz, center) + p.w < -radius) {
            return true;
        }
    }
    return false;
}

// LOD0 for dist < t0, LOD1 for dist < t1, else the coarsest LOD (kMaxLods-1).
// Boundary values fall to the COARSER side (strict `<`): dist == t0 -> LOD1.
fn selectLod(dist: f32) -> u32 {
    if (dist < config.lodThresholds.x) { return 0u; }
    if (dist < config.lodThresholds.y) { return 1u; }
    return kMaxLods - 1u;
}

@compute @workgroup_size(64)
fn main(@builtin(global_invocation_id) gid: vec3<u32>) {
    let i = gid.x;
    if (i >= config.counts.x) {
        return;
    }
    let inst = instances[i];
    if (sphereCulled(inst.boundsSphere.xyz, inst.boundsSphere.w)) {
        perInstanceBucket[i] = SENTINEL;
        return;
    }
    let dist = distance(inst.boundsSphere.xyz, config.cameraWorldPos.xyz);
    let lod = selectLod(dist);
    let bucket = inst.modelInfo.x * kMaxLods + lod;
    // Defensive: a modelId >= numModels would index a bucket past the buffers.
    if (bucket >= config.counts.y) {
        perInstanceBucket[i] = SENTINEL;
        return;
    }
    perInstanceBucket[i] = bucket;
    atomicAdd(&bucketCount[bucket], 1u);
}
