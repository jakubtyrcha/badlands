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
StructuredBuffer<PlaneVertex> vertices;

struct VOut {
    float4 pos : SV_Position;
}

[shader("vertex")]
VOut vs_visbuffer(uint vid : SV_VertexID)
{
    // Pulled by vertex id: the RHI has no vertex buffers by design. In an
    // indexed draw SV_VertexID is the fetched index plus base_vertex, which is
    // exactly the addressing DrawInfo's offsets assume.
    PlaneVertex v = vertices[vid];
    VOut o;
    o.pos = mul(frame.view_proj, float4(v.position - frame.camera_pos.xyz, 1.0));
    return o;
}

[shader("fragment")]
uint fs_visbuffer(VOut i, uint prim : SV_PrimitiveID) : SV_Target
{
    // Draw slot 0: there is one draw. The shift is real code on the real path
    // rather than a placeholder -- the resolve unpacks it and indexes DrawInfo
    // with the result.
    return PackVis(0u, prim);
}
