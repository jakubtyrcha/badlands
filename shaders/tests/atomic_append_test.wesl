// Phase A atomics gate (GPU-driven instanced rendering, primitives spike).
// First shader in this repo to declare `atomic<u32>` in a storage binding —
// exercises whether the WESL->WGSL->naga reflection path accepts a top-level
// atomic storage var. Every dispatched thread appends its global_invocation_id
// to out_indices at a slot claimed via atomicAdd; if every thread appends
// exactly once, counter ends at N and out_indices[0..N) is a permutation of
// 0..N-1 (checked CPU-side after readback).
@group(0) @binding(0) var<storage, read_write> counter: atomic<u32>;
@group(0) @binding(1) var<storage, read_write> out_indices: array<u32>;

@compute @workgroup_size(64)
fn main(@builtin(global_invocation_id) gid: vec3<u32>) {
    let slot = atomicAdd(&counter, 1u);
    out_indices[slot] = gid.x;
}
