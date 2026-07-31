// Shared struct + constant definitions for the GPU-driven instanced-rendering
// cull/scan/scatter passes (instance_classify / instance_scan / instance_scatter
// import from here). Struct-only module — no bindings — so all three passes
// agree on the byte layout without one pulling in another's bindings. Mirrors
// GpuInstanceRenderer's CullConfigData (128 bytes) + InstanceInput (96 bytes)
// byte-for-byte; keep the C++ mirror + its static_asserts in sync.

// Mirrors GpuInstanceRenderer's CullConfigData byte-for-byte (128 bytes).
struct CullConfig {
    planes: array<vec4<f32>, 6>,   // 6 inward-facing frustum planes (a,b,c,d)
    cameraWorldPos: vec4<f32>,     // xyz = camera world position, w unused
    counts: vec4<u32>,             // x=instanceCount, y=numBuckets, z=compactedCapacity, w = numSubmeshes
}

// Per-model LOD chain, one entry per model id -- the classify pass indexes it
// by an instance's modelInfo.x. Mirrors GpuInstanceRenderer::ModelLodData
// byte-for-byte (48 bytes). LOD selection is per-MODEL, not global: lodCount
// is the model's RUNTIME level count (1..kMaxLods, where kMaxLods is the
// compile-time cap that also fixes the bucket stride), and only its first
// lodCount-1 thresholds are read. A model with 3 levels and one with 8 can
// therefore coexist in one renderer, each cutting at its own distances.
// thresholds is 2 vec4s = 8 slots for kMaxLods-1 = 7 values; the 8th is
// unused padding. Component-indexed as thresholds[i / 4u][i % 4u].
struct ModelLod {
    thresholds: array<vec4<f32>, 2>,
    lodCount: u32,
    pad0: u32,
    pad1: u32,
    pad2: u32,
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
