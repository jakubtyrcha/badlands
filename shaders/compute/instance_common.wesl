// Shared struct + constant definitions for the GPU-driven instanced-rendering
// cull/scan/scatter passes (instance_classify / instance_scan / instance_scatter
// import from here). Struct-only module — no bindings — so all three passes
// agree on the byte layout without one pulling in another's bindings. Mirrors
// GpuInstanceRenderer's CullConfigData (144 bytes) + InstanceInput (96 bytes)
// byte-for-byte; keep the C++ mirror + its static_asserts in sync.

// Mirrors GpuInstanceRenderer's CullConfigData byte-for-byte (144 bytes).
struct CullConfig {
    planes: array<vec4<f32>, 6>,   // 6 inward-facing frustum planes (a,b,c,d)
    cameraWorldPos: vec4<f32>,     // xyz = camera world position, w unused
    lodThresholds: vec4<f32>,      // x=t0, y=t1 (kMaxLods-1 thresholds); z,w unused
    counts: vec4<u32>,             // x=instanceCount, y=numBuckets, z=compactedCapacity, w = numSubmeshes
}

// Static per-instance input. Mirrors GpuInstanceRenderer::InstanceInput
// byte-for-byte (96 bytes).
struct InstanceData {
    transform: mat4x4<f32>,    // world transform
    boundsSphere: vec4<f32>,   // xyz = world center, w = radius
    modelInfo: vec4<u32>,      // x = model id (selects the bucket group)
}

// Marks a culled (or out-of-range) instance in perInstanceBucket; the scatter
// pass skips these. Distinct from any real bucket id (buckets are < numBuckets).
const SENTINEL: u32 = 0xFFFFFFFFu;
