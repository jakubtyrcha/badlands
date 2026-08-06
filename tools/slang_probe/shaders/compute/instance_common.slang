// Slang port of shaders/compute/instance_common.wesl, for the RHI/Slang probe.
//
// Struct-only module, no bindings -- byte layouts must match the C++ mirrors
// (CullConfig 128B, ModelLod 48B, InstanceData 96B). Probe B checks whether
// Slang reflects the same member offsets naga reports today.
module instance_common;

// Mirrors GpuInstanceRenderer's CullConfigData byte-for-byte (128 bytes).
public struct CullConfig
{
    public float4 planes[6];        // 6 inward-facing frustum planes (a,b,c,d)
    public float4 cameraWorldPos;   // xyz = camera world position, w unused
    public uint4  counts;           // x=instanceCount y=numBuckets z=capacity w=numSubmeshes
};

// Per-model LOD chain, one entry per model id (48 bytes). thresholds is 2 float4s
// = 8 slots for kMaxLods-1 = 7 values; the 8th is unused padding.
public struct ModelLod
{
    public float4 thresholds[2];
    public uint   lodCount;
    public uint   pad0;
    public uint   pad1;
    public uint   pad2;
};

// Static per-instance input (96 bytes).
public struct InstanceData
{
    public float4x4 transform;     // world transform
    public float4   boundsSphere;  // xyz = world center, w = radius
    public uint4    modelInfo;     // x = model id (selects the bucket group)
};

// Marks a culled (or out-of-range) instance in perInstanceBucket.
public static const uint SENTINEL = 0xFFFFFFFFu;
