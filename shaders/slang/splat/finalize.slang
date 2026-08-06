// Pass 2: turn a count only the GPU knows into dispatch arguments.
import splat_common;

ConstantBuffer<SplatParams> params;
RWStructuredBuffer<uint> counter;
RWStructuredBuffer<uint> args;   // x, y, z

[shader("compute")]
[numthreads(1, 1, 1)]
void cs_finalize() {
    // Clamped to what the buffer actually holds: the counter keeps counting
    // past capacity so the overflow is observable, but dispatching over splats
    // that were never written would read uninitialised memory.
    uint capacity = uint(params.config.y);
    uint n = min(counter[0], capacity);

    // Insurance rather than a load-bearing guess: this grid can produce at most
    // 2 * 32^3 splats, far below any threadgroup-per-dimension limit. A larger
    // producer would need folding into 2-D, which is deliberately out of scope.
    args[0] = (n + 63u) / 64u;
    args[1] = 1u;
    args[2] = 1u;
    // Publish the clamped count so the splat pass agrees with the dispatch.
    counter[1] = n;
}
