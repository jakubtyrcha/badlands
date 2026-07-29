// GPU-driven instanced rendering, Phase D — pass 3 of 3: SCATTER (compaction).
// (See .superpowers/sdd/phase-D-brief.md / phase-D-report.md.) Runs AFTER the
// scan has published each bucket's base into `bucketBase` and reset each
// `writeCursor` to 0. Per instance i (of counts.x): read its bucket recorded by
// the classify pass; if SENTINEL (culled), skip. Otherwise claim the next slot
// in that bucket's slice with an atomicAdd on the bucket's cursor and write the
// instance's transform into compacted[bucketBase[b] + slot]. Because each
// bucket owns a disjoint prefix-sum slice and its cursor only ever reaches
// count[b] (exactly count[b] instances map to it), a bucket never spills into a
// neighbor — the only guard needed is a global capacity clamp.

// CullConfig (144B), InstanceData (96B), and SENTINEL are shared with the
// classify + scan passes — see shaders/compute/instance_common.wesl.
import package::compute::instance_common::{CullConfig, InstanceData, SENTINEL};

@group(0) @binding(0) var<uniform> config: CullConfig;
@group(0) @binding(1) var<storage, read> instances: array<InstanceData>;
@group(0) @binding(2) var<storage, read> perInstanceBucket: array<u32>;
@group(0) @binding(3) var<storage, read> bucketBase: array<u32>;
@group(0) @binding(4) var<storage, read_write> writeCursor: array<atomic<u32>>;
@group(0) @binding(5) var<storage, read_write> compacted: array<mat4x4<f32>>;

@compute @workgroup_size(64)
fn main(@builtin(global_invocation_id) gid: vec3<u32>) {
    let i = gid.x;
    if (i >= config.counts.x) {
        return;
    }
    let b = perInstanceBucket[i];
    if (b == SENTINEL) {
        return;
    }
    let slot = atomicAdd(&writeCursor[b], 1u);
    let dst = bucketBase[b] + slot;
    // Global capacity clamp: never write past the compacted buffer (sized to
    // the instance capacity, tightly packed — see instance_scan.wesl).
    // In-capacity uploads never trip this; it only guards a mis-sized /
    // over-subscribed run, and clamping (dropping the overflow write) protects
    // neighboring slices.
    if (dst < config.counts.z) {
        compacted[dst] = instances[i].transform;
    }
}
