// GPU-driven instanced rendering, Phase D — pass 2 of 3: SCAN (prefix-sum).
// (See .superpowers/sdd/phase-D-brief.md / phase-D-report.md.) Runs AFTER the
// classify pass has final per-bucket counts in `bucketCount`. A single serial
// thread (bucket count = numModels*kMaxLods is tiny) sweeps buckets in order
// computing an exclusive prefix-sum of the counts into `bucketBase`, so each
// bucket owns the contiguous, tightly-packed compacted slice
// [bucketBase[b], +count[b]) — no inter-bucket padding: BindInstanceData binds
// `compacted`/`bucketBase` WHOLE at offset 0 and the vertex shader indexes by
// ELEMENT, never a byte offset, so alignment padding here would buy nothing.
// It also resets each bucket's scatter cursor and publishes the bucket's
// indirect draw instanceCount to EVERY one of its submesh slots (`config.
// counts.w` = numSubmeshes; slot = bucket*numSubmeshes+submesh) — one bucket's
// single compacted slice can drive several draws (distinct geometry/materials
// in different render passes), all sharing that bucket's survivor count. This
// is the crux of the fully GPU-driven design: the CPU never reads these
// offsets back — the draw's vertex shader indexes
// compacted[bucketBase[bucketId] + instance_index] instead.

// CullConfig (144B) is shared with the classify + scatter passes — see
// shaders/compute/instance_common.wesl.
import package::compute::instance_common::{CullConfig};

// Standard 20-byte DrawIndexedIndirect args (one per (bucket, submesh) slot).
// indexCount / firstIndex / baseVertex are CPU-prefilled per slot's geometry;
// the scan publishes instanceCount (= that slot's bucket's survivor count,
// same value for every submesh of a bucket) and firstInstance=0.
struct IndirectArgs {
    indexCount: u32,
    instanceCount: u32,
    firstIndex: u32,
    baseVertex: i32,
    firstInstance: u32,
}

@group(0) @binding(0) var<uniform> config: CullConfig;
// bucketCount is atomic (the classify pass atomicAdd'd into it); read it back
// with atomicLoad here. atomics require the read_write address space.
@group(0) @binding(1) var<storage, read_write> bucketCount: array<atomic<u32>>;
@group(0) @binding(2) var<storage, read_write> bucketBase: array<u32>;
@group(0) @binding(3) var<storage, read_write> writeCursor: array<atomic<u32>>;
@group(0) @binding(4) var<storage, read_write> indirectArgs: array<IndirectArgs>;

@compute @workgroup_size(1)
fn main() {
    let numBuckets = config.counts.y;
    let numSubmeshes = config.counts.w;
    var running: u32 = 0u;
    for (var b = 0u; b < numBuckets; b = b + 1u) {
        // Tight exclusive prefix-sum: each bucket's base is the running total
        // of all prior buckets' counts, no padding. BindInstanceData binds
        // `compacted`/`bucketBase` WHOLE at offset 0 and the vertex shader
        // indexes compacted[bucketBase[bucketId] + instance_index] by ELEMENT
        // (never a byte offset), so alignment here was never load-bearing.
        let base = running;
        bucketBase[b] = base;
        let cnt = atomicLoad(&bucketCount[b]);
        running = base + cnt;
        // Reset the scatter cursor (pass 3 atomicAdds into it from 0).
        atomicStore(&writeCursor[b], 0u);
        // Publish this bucket's indirect draws: instanceCount = survivors,
        // firstInstance = 0 (the base is applied in the vertex shader, not via
        // firstInstance — see the brief's crux), broadcast to every submesh
        // slot of this bucket (slot = b*numSubmeshes+s) — they all draw the
        // same compacted slice.
        for (var s = 0u; s < numSubmeshes; s = s + 1u) {
            let slot = b * numSubmeshes + s;
            indirectArgs[slot].instanceCount = cnt;
            indirectArgs[slot].firstInstance = 0u;
        }
    }
}
