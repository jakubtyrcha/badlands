// Tree raster into the SAME visibility buffer as the terrain.
//
// A SEPARATE module from terrain_vis, not a second entry point in it: Slang
// reports every module-level global in every entry point's reflection, so two
// passes sharing a module each appear to declare the other's bindings. One
// module per pass keeps reflection honest and lets the validation layer's
// "declared but not bound" check stay strict.

import lab_common;

ConstantBuffer<LabFrame> frame;
StructuredBuffer<TreeInstance> trees;
StructuredBuffer<LabVertex> tree_vertices;
StructuredBuffer<uint> tree_indices;

struct VsOut
{
    float4 pos : SV_Position;
    nointerpolation uint vis : TEXCOORD0;
};

[shader("vertex")]
VsOut vs_trees(uint vid : SV_VertexID, uint iid : SV_InstanceID)
{
    VsOut o;
    let inst = trees[iid];
    let v = tree_vertices[tree_indices[vid]];
    let world = inst.position_scale.xyz + v.position * inst.position_scale.w;
    o.pos = mul(frame.viewProj, float4(world, 1.0));
    // Trees occupy the high half of the slot space, so the resolve tells them
    // apart from terrain without a second buffer.
    o.vis = PackVis(0x40000u + iid, vid / 3u);
    return o;
}

[shader("fragment")]
uint fs_vis(VsOut i) : SV_Target
{
    return i.vis;
}
