// GPU-driven terrain cluster selection.
//
// Replaces the CPU `SelectClusters` flat pass, which costs ~1 ms per frame and
// produces ~908 individual DrawIndexed calls on a 512 map. Here the same rule
// runs over every cluster in parallel, survivors are compacted, and ONE
// indirect draw covers the lot.
//
// The rule is unchanged and is what makes the cut crack-free: select the
// cluster whose own projected error is within budget but whose parent's is
// not. Errors are monotone along any leaf->root chain, so that crossing
// happens exactly once -- every point covered once, no ancestor and descendant
// both drawn.

import lab_common;

ConstantBuffer<LabFrame> frame;
StructuredBuffer<ClusterGpu> clusters;

// Compacted output: the cluster index each draw instance should render.
RWStructuredBuffer<uint> selected;
// Standard 20-byte DrawIndexedIndirect args, written straight from the GPU.
RWStructuredBuffer<uint> draw_args;
// draw_args[1] is instanceCount, bumped atomically; kept separate because
// Slang needs the atomic to be its own typed binding.
RWStructuredBuffer<Atomic<uint>> draw_counter;

[shader("compute")]
[numthreads(64, 1, 1)]
void cs_select(uint3 gid : SV_DispatchThreadID)
{
    let i = gid.x;
    let cluster_count = uint(frame.params.w);
    if (i >= cluster_count) { return; }

    let c = clusters[i];
    let tau = frame.params.x;
    let screen_h = frame.params.y;
    let fov = frame.params.z;
    let cam = frame.cameraPos.xyz;

    let own = ProjectedError(c.own_error, c.sphere, cam, fov, screen_h);
    let parent = ProjectedError(c.parent_error, c.parent_sphere, cam, fov, screen_h);

    // Roots carry +inf parent error, so they survive when nothing finer does.
    if (!(own <= tau && parent > tau)) { return; }

    let slot = draw_counter[0].add(1u);
    if (slot >= uint(frame.limits.x)) { return; }  // capacity guard
    selected[slot] = i;
}
