// Slang port of shaders/compute/instance_classify.wesl, for the RHI/Slang probe.
//
// The compute + atomics case: a uniform buffer, two read-only structured
// buffers, two read-write ones, an atomic increment, and an explicit
// numthreads. Probe B uses it for storage-buffer reflection, read/write access
// classification, and workgroup size; probe A uses it as the small-shader
// compile-time baseline.
//
// WESL declares `array<atomic<u32>>`; Slang's portable spelling is
// RWStructuredBuffer<Atomic<uint>>, which is one of the things probe A checks
// translates cleanly to both Metal and HLSL.

import instance_common;

// Compile-time LOD cap; bucket = modelId*kMaxLods + lod. Must match
// GpuInstanceRenderer::kMaxLods.
static const uint kMaxLods = 8u;

[[vk::binding(0, 0)]] ConstantBuffer<CullConfig> config;
[[vk::binding(1, 0)]] StructuredBuffer<InstanceData> instances;
[[vk::binding(2, 0)]] RWStructuredBuffer<uint> perInstanceBucket;
[[vk::binding(3, 0)]] RWStructuredBuffer<Atomic<uint>> bucketCount;
[[vk::binding(4, 0)]] StructuredBuffer<ModelLod> modelLods;

// A sphere is culled iff it is fully outside ANY single plane's half-space.
bool sphereCulled(float3 center, float radius)
{
    for (uint i = 0u; i < 6u; ++i)
    {
        let p = config.planes[i];
        if (dot(p.xyz, center) + p.w < -radius)
        {
            return true;
        }
    }
    return false;
}

// LOD0 for dist < t0, LOD1 for dist < t1, ... else this MODEL's coarsest LOD.
// Boundary values fall to the COARSER side (strict `<`).
uint selectLod(float dist, ModelLod m)
{
    let count = clamp(m.lodCount, 1u, kMaxLods);
    for (uint i = 0u; i + 1u < count; ++i)
    {
        if (dist < m.thresholds[i / 4u][i % 4u])
        {
            return i;
        }
    }
    return count - 1u;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 gid : SV_DispatchThreadID)
{
    let i = gid.x;
    if (i >= config.counts.x) { return; }

    let inst = instances[i];
    if (sphereCulled(inst.boundsSphere.xyz, inst.boundsSphere.w))
    {
        perInstanceBucket[i] = SENTINEL;
        return;
    }

    let dist = distance(inst.boundsSphere.xyz, config.cameraWorldPos.xyz);

    // Guard the modelId BEFORE the multiply: a garbage modelId large enough to
    // overflow u32 would WRAP into range and defeat the post-multiply guard.
    // This also gates the modelLods[] fetch, which holds exactly numModels entries.
    let numModels = config.counts.y / kMaxLods;
    if (inst.modelInfo.x >= numModels)
    {
        perInstanceBucket[i] = SENTINEL;
        return;
    }

    let lod = selectLod(dist, modelLods[inst.modelInfo.x]);
    let bucket = inst.modelInfo.x * kMaxLods + lod;
    if (bucket >= config.counts.y)
    {
        perInstanceBucket[i] = SENTINEL;
        return;
    }

    perInstanceBucket[i] = bucket;
    bucketCount[bucket].add(1u);
}
