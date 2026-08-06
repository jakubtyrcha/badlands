// Visibility-buffer raster.
//
// This is the pass that makes geometry fit a render graph at all: it binds NO
// material, writes only {cluster slot, primitive} plus depth, and therefore has
// a completely static resource set no matter how many materials the scene has.
// Material resolution happens afterwards in screen space.
//
// Vertices are PULLED from storage buffers via the vertex id -- there is no
// vertex input layout anywhere in the MVP. One instance renders one selected
// cluster; indices beyond that cluster's own count collapse to a degenerate
// triangle, which is what lets a single indirect draw cover clusters of
// differing sizes.

import lab_common;

ConstantBuffer<LabFrame> frame;
StructuredBuffer<LabVertex> vertices;
StructuredBuffer<uint> indices;
StructuredBuffer<ClusterGpu> clusters;
StructuredBuffer<uint> selected;

struct VsOut
{
    float4 pos : SV_Position;
    nointerpolation uint vis : TEXCOORD0;
};

[shader("vertex")]
VsOut vs_terrain(uint vid : SV_VertexID, uint iid : SV_InstanceID)
{
    VsOut o;
    let cluster_index = selected[iid];
    let c = clusters[cluster_index];

    if (vid >= c.index_count)
    {
        // Degenerate: collapse to a point outside clip space so the triangle
        // has no area. Cheaper than a second draw per cluster size.
        o.pos = float4(0.0, 0.0, -1.0, 0.0);
        o.vis = kVisEmpty;
        return o;
    }

    let v = vertices[indices[c.first_index + vid]];
    o.pos = mul(frame.viewProj, float4(v.position, 1.0));
    // +1 so that 0 stays reserved for "nothing here".
    o.vis = PackVis(iid + 1u, vid / 3u);
    return o;
}

[shader("fragment")]
uint fs_vis(VsOut i) : SV_Target
{
    return i.vis;
}
