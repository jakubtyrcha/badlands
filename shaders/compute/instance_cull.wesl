// Compute frustum cull + compaction (GPU-driven instanced rendering, Phase
// C — see .superpowers/sdd/phase-C-brief.md / phase-C-report.md). Tests each
// instance's world-space bounding sphere against 6 inward-facing frustum
// planes using the EXACT convention of engine/rendering/frustum.{hpp,cpp}'s
// `Frustum`: a plane is (a,b,c,d); a point p is inside/on the frustum's
// half-space when dot((a,b,c), p) + d >= 0 (see sphereCulled below for the
// sphere-specialized derivation). Survivors' transforms are atomically
// appended (densely, order not guaranteed -- concurrent threads race for
// slots) into `compacted`, and `args.instanceCount` is incremented so one
// indirect indexed draw renders exactly the survivors. Single input set,
// single output bucket -- no LOD selection or multi-bucket routing (a later
// phase's job).

struct FrustumUniform {
    planes: array<vec4<f32>, 6>,
    cameraWorldPos: vec4<f32>,  // xyz = camera world position, w unused
    counts: vec4<u32>,          // x = instance count, yzw unused
}

// Static per-instance input. Mirrors GpuInstanceRenderer::InstanceInput
// byte-for-byte.
struct InstanceData {
    transform: mat4x4<f32>,    // world transform
    boundsSphere: vec4<f32>,   // xyz = world center, w = radius
    modelInfo: vec4<u32>,      // x = model id (unused until a later phase; multi-bucket routing)
}

// Standard 20-byte DrawIndexedIndirect args, with instanceCount as the
// atomic counter the cull increments per survivor.
struct IndirectArgs {
    indexCount: u32,
    instanceCount: atomic<u32>,
    firstIndex: u32,
    baseVertex: i32,
    firstInstance: u32,
}

@group(0) @binding(0) var<uniform> frustum: FrustumUniform;

@group(1) @binding(0) var<storage, read> instances: array<InstanceData>;
@group(1) @binding(1) var<storage, read_write> compacted: array<mat4x4<f32>>;
@group(1) @binding(2) var<storage, read_write> args: IndirectArgs;

// A sphere is culled (fully outside the frustum) iff it is fully outside ANY
// single plane's half-space. The sphere's point of maximum support along a
// plane's (unit) normal is `center + radius * plane.xyz`; the sphere fails
// that plane's inside test (dot(n,p)+d >= 0) at its BEST point when
// dot(plane.xyz, center) + plane.w + radius < 0, i.e.
// dot(plane.xyz, center) + plane.w < -radius. This is the sphere-specialized
// form of Frustum::Intersects' AABB "positive vertex" test (which plugs the
// AABB corner of maximum support into the same dot(n,p)+d >= 0 inside test);
// verified by direct analogy with that CPU code -- see the phase-C report.
fn sphereCulled(center: vec3<f32>, radius: f32) -> bool {
    for (var i = 0u; i < 6u; i = i + 1u) {
        let p = frustum.planes[i];
        if (dot(p.xyz, center) + p.w < -radius) {
            return true;
        }
    }
    return false;
}

@compute @workgroup_size(64)
fn main(@builtin(global_invocation_id) gid: vec3<u32>) {
    let i = gid.x;
    if (i >= frustum.counts.x) {
        return;
    }
    let inst = instances[i];
    if (sphereCulled(inst.boundsSphere.xyz, inst.boundsSphere.w)) {
        return;
    }
    let slot = atomicAdd(&args.instanceCount, 1u);
    compacted[slot] = inst.transform;
}
