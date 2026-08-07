// The visibility-buffer raster pass.
//
// Writes ONE UINT per pixel and nothing else. No material is bound, no texture
// is sampled, no attribute is interpolated -- everything the surface needs is
// recovered later, in the resolve, from this id plus the depth buffer. That is
// the whole point of a visibility buffer: geometry cost stops depending on
// material complexity.
//
// REVERSED-Z: the depth attachment clears to 0.0 (far) and compares
// GreaterEqual. Clearing to 1.0 makes every fragment fail and the plane
// disappears, which reads as a missing draw rather than as a wrong clear.

module visbuffer;

// Types and packing only. The bindings below are THIS pass's; the resolve
// declares its own, which is why they had to stop living in the same module.
import vis_common;

ConstantBuffer<VisFrame> frame;
StructuredBuffer<MeshVertex> vertices;
StructuredBuffer<DrawInfo> draws;

struct VOut {
    float4 pos : SV_Position;
    // NOINTERPOLATION: an instance id is an index, and interpolating it across
    // a triangle produces fractional slots that round to a neighbour's material
    // along every edge. It is constant per primitive by construction.
    nointerpolation uint slot : TEXCOORD0;
}

[shader("vertex")]
VOut vs_visbuffer(uint vid : SV_VertexID, uint iid : SV_InstanceID)
{
    // Pulled by vertex id: the RHI has no vertex buffers by design. In an
    // indexed draw SV_VertexID is the fetched index plus base_vertex, which is
    // exactly the addressing DrawInfo's offsets assume.
    MeshVertex v = vertices[vid];
    // ONE mesh, N instances, each offset by its own DrawInfo. The resolve adds
    // the SAME offset when it rebuilds the triangle, so the two must read it
    // from the same place or the barycentrics land on a different triangle than
    // the one that was rasterized.
    let world = v.position + draws[iid].offset.xyz;
    VOut o;
    o.pos = mul(frame.view_proj, float4(world - frame.camera_pos.xyz, 1.0));
    o.slot = iid;
    return o;
}

[shader("fragment")]
uint fs_visbuffer(VOut i, uint prim : SV_PrimitiveID) : SV_Target
{
    // The instance id IS the draw slot, which is why the sphere grid needs no
    // per-draw uniform: fourteen spheres are one instanced draw, and the
    // packing already carries eight bits of slot.
    return PackVis(i.slot, prim);
}
