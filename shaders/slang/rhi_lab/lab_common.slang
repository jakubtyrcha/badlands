// Shared types for the rhi_lab MVP. Mirrors the C++ structs byte-for-byte.
module lab_common;

// Per-frame constants. Kept small deliberately -- the MVP does not need the
// engine's 592-byte UniformData, and pulling it in would couple the lab to the
// renderer it exists to replace.
public struct LabFrame
{
    public float4x4 viewProj;
    public float4x4 invViewProj;   // resolve reconstructs world position from depth
    public float4   cameraPos;     // xyz
    public float4   sunDirection;  // xyz, normalized, pointing TOWARD the sun
    public float4   sunColor;      // rgb
    public float4   ambientSH[9];  // L2 SH, rgb in xyz
    public float4   params;        // x = tau_px, y = screen_h_px, z = fov_deg, w = cluster count
    public float4   splatUv;       // world XZ -> splat uv: uv = xz * splatUv.xy + splatUv.zw
    // x = capacity of the compacted `selected` buffer. Passed in rather than
    // queried: StructuredBuffer::getCount() is not available on the Metal
    // target, and a capacity the CPU already knows is not worth a feature gate.
    public float4   limits;
};

// One terrain cluster, as the GPU selector sees it. Exactly the inputs
// SelectClusters uses on the CPU -- the rule is unchanged, only where it runs.
//
// TWO spheres, not one: the parent's projected error must be measured against
// the PARENT GROUP's sphere, which encloses more geometry and sits at a
// different centre. Reusing the cluster's own sphere for both tests drops
// clusters from the cut and tears visible holes in the terrain.
public struct ClusterGpu
{
    public float4 sphere;         // own LOD sphere: xyz centre, w radius
    public float4 parent_sphere;  // parent group's sphere; unused for roots
    public uint   first_index;
    public uint   index_count;
    public float  own_error;      // world metres; 0 for leaves
    public float  parent_error;   // huge for roots, so they are never dropped
};

// Matches TerrainClusterDag's vertex layout byte-for-byte (8 floats:
// kFloatsPerClusterVertex), so the DAG's vertex array uploads with no
// conversion pass at all.
//
// Stored as two float4s rather than the obvious float3/float3/float/float:
// MSL pads float3 to 16 bytes, which would make this struct 40 bytes against
// the CPU's 32 and shear every vertex read. Two float4s are 32 on both sides.
// `color` and `meta` arrive pre-packed as Unorm8x4 and Uint8x4; the MVP does
// not unpack them, since terrain material comes from the world-XZ splat.
public struct LabVertex
{
    public float4 pos_nx;      // xyz = position, w = normal.x
    public float4 nyz_col_meta;// x,y = normal.yz, z = colour packed, w = meta packed

    public property float3 position { get { return pos_nx.xyz; } }
    public property float3 normal {
        get { return float3(pos_nx.w, nyz_col_meta.x, nyz_col_meta.y); }
    }
};

// A tree instance. The second geometry producer, sharing the visibility buffer.
public struct TreeInstance
{
    public float4 position_scale;  // xyz world position, w uniform scale
    public float4 tint;
};

// Projected screen-space error in pixels. Mirrors SelectClusters' `proj`
// lambda exactly, including both special cases: a zero-error cluster projects
// to 0 from anywhere, and a camera INSIDE the sphere projects to infinity so
// the cut is forced to refine past it.
public float ProjectedError(float world_error, float4 sphere, float3 cam_pos,
                            float fov_deg, float screen_h_px)
{
    if (world_error <= 0.0) { return 0.0; }
    let dist = length(cam_pos - sphere.xyz);
    if (dist <= sphere.w) { return 1e30; }
    let k = screen_h_px / (2.0 * tan(radians(fov_deg) * 0.5));
    return world_error * k / max(dist - sphere.w, 1e-4);
}

// Irradiance from L2 spherical harmonics. Small enough to inline here rather
// than depend on the engine's sh_lighting.
public float3 EvaluateSH(float4 sh[9], float3 n)
{
    const float c1 = 0.429043, c2 = 0.511664, c3 = 0.743125;
    const float c4 = 0.886227, c5 = 0.247708;
    float3 L00  = sh[0].xyz, L1m1 = sh[1].xyz, L10 = sh[2].xyz, L11 = sh[3].xyz;
    float3 L2m2 = sh[4].xyz, L2m1 = sh[5].xyz, L20 = sh[6].xyz;
    float3 L21  = sh[7].xyz, L22  = sh[8].xyz;
    return c4 * L00
         + 2.0 * c2 * (L11 * n.x + L1m1 * n.y + L10 * n.z)
         + c1 * (L22 * (n.x * n.x - n.y * n.y) + L20 * n.z * n.z * 3.0)
         - c5 * L20
         + 2.0 * c1 * (L2m2 * n.x * n.y + L21 * n.x * n.z + L2m1 * n.y * n.z);
}

// Visibility-buffer packing: cluster slot in the high bits, primitive in the
// low ones. 12 bits of primitive covers the 128-triangle cluster budget with
// room to spare.
public static const uint kPrimBits = 12u;
public static const uint kPrimMask = (1u << kPrimBits) - 1u;

public uint PackVis(uint slot, uint prim) { return (slot << kPrimBits) | (prim & kPrimMask); }
public uint VisSlot(uint packed) { return packed >> kPrimBits; }
public uint VisPrim(uint packed) { return packed & kPrimMask; }

// 0 is "nothing here" -- the visibility buffer clears to it, so every real
// draw offsets its slot by one.
public static const uint kVisEmpty = 0u;
