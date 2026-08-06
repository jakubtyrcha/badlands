// Publishes the indirect args the selector accumulated.
//
// Its own module, not a second entry point in cluster_select: see tree_vis for
// why -- shared module globals leak into every entry point's reflection.

import lab_common;

ConstantBuffer<LabFrame> frame;
RWStructuredBuffer<uint> draw_args;
RWStructuredBuffer<Atomic<uint>> draw_counter;

[shader("compute")]
[numthreads(1, 1, 1)]
void cs_finalize(uint3 gid : SV_DispatchThreadID)
{
    if (gid.x != 0) { return; }
    // indexCount is the fixed per-cluster budget; each instance draws one
    // cluster and degenerates the indices it does not use.
    draw_args[0] = 384u;
    draw_args[1] = draw_counter[0].load();
    draw_args[2] = 0u;  // firstIndex
    draw_args[3] = 0u;  // baseVertex
    draw_args[4] = 0u;  // firstInstance
}
