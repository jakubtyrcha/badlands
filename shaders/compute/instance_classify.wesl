// GPU-driven instanced rendering, Phase D — pass 1 of 3: CLASSIFY + COUNT.
// (See .superpowers/sdd/phase-D-brief.md / phase-D-report.md.) Generalizes
// Phase C's single-bucket frustum-cull: each surviving instance is routed into
// one `(model, lod)` bucket, where `lod` is a distance-based level-of-detail
// selection against that model's own `modelLods[modelId]` chain. Per instance
// i (of counts.x):
//   * frustum-cull the world-space bounding sphere (reuses Phase C's EXACT
//     `sphereCulled` convention — see below); if culled, mark SENTINEL + return.
//   * else classify: bucket = modelId*kMaxLods + lod(distance-to-camera), record
//     it in perInstanceBucket[i], and atomically bump bucketCount[bucket].
// perInstanceBucket + bucketCount are consumed by the scan (pass 2) and scatter
// (pass 3). Single input set, group-0 bindings, no dynamic offsets.

// CullConfig (128B), ModelLod (48B), InstanceData (96B), and SENTINEL are
// shared with the scan + scatter passes — see
// shaders/compute/instance_common.wesl.
import package::compute::instance_common::{CullConfig, ModelLod, InstanceData, SENTINEL};

// Compile-time LOD CAP. `bucket = modelId*kMaxLods + lod`, so this must match
// GpuInstanceRenderer::kMaxLods (the C++ side sizes bucket buffers as
// numModels*kMaxLods). It bounds the bucket stride only -- how many levels a
// model ACTUALLY has is runtime, per model: modelLods[modelId].lodCount, with
// that model's own kMaxLods-1 distance thresholds alongside it. A model using
// fewer levels than the cap simply leaves its top buckets unconfigured.
const kMaxLods: u32 = 8u;

@group(0) @binding(0) var<uniform> config: CullConfig;
@group(0) @binding(1) var<storage, read> instances: array<InstanceData>;
@group(0) @binding(2) var<storage, read_write> perInstanceBucket: array<u32>;
@group(0) @binding(3) var<storage, read_write> bucketCount: array<atomic<u32>>;
@group(0) @binding(4) var<storage, read> modelLods: array<ModelLod>;

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

// LOD0 for dist < t0, LOD1 for dist < t1, … else this MODEL's coarsest LOD
// (lodCount-1, not the kMaxLods-1 cap). Boundary values fall to the COARSER
// side (strict `<`): dist == t0 -> LOD1. lodCount is clamped to [1, kMaxLods]
// CPU-side, so the loop reads at most kMaxLods-1 thresholds.
fn selectLod(dist: f32, m: ModelLod) -> u32 {
    let count = clamp(m.lodCount, 1u, kMaxLods);
    for (var i = 0u; i + 1u < count; i = i + 1u) {
        if (dist < m.thresholds[i / 4u][i % 4u]) {
            return i;
        }
    }
    return count - 1u;
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
    // Guard the modelId BEFORE the `modelId*kMaxLods` multiply. A garbage
    // modelId large enough that modelId*kMaxLods overflows u32 would WRAP to a
    // small in-range value and defeat the post-multiply `bucket >= numBuckets`
    // guard (e.g. 0x55555556*3 truncates to 2). numBuckets = numModels*kMaxLods,
    // so numModels = numBuckets/kMaxLods; a modelId < numModels keeps
    // modelId*kMaxLods + lod < numBuckets — the multiply can't overflow.
    // This guard ALSO gates the modelLods[] fetch below: that buffer holds
    // exactly numModels entries, so the model's LOD chain must not be read
    // until its id is known in range.
    let numModels = config.counts.y / kMaxLods;
    if (inst.modelInfo.x >= numModels) {
        perInstanceBucket[i] = SENTINEL;
        return;
    }
    let lod = selectLod(dist, modelLods[inst.modelInfo.x]);
    let bucket = inst.modelInfo.x * kMaxLods + lod;
    // Defensive: a modelId >= numModels would index a bucket past the buffers.
    if (bucket >= config.counts.y) {
        perInstanceBucket[i] = SENTINEL;
        return;
    }
    perInstanceBucket[i] = bucket;
    atomicAdd(&bucketCount[bucket], 1u);
}
