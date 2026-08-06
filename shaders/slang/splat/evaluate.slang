// Pass 1: sample an analytic SDF on a FIXED grid and seed a point per surface
// cell. No octree, no subdivision, no LOD -- the smallest thing that is still
// the real technique.
import splat_common;

ConstantBuffer<SplatParams> params;
RWStructuredBuffer<Splat> splats;
RWStructuredBuffer<uint> counter;

void Emit(float3 p, float source) {
    uint index;
    InterlockedAdd(counter[0], 1u, index);
    // The counter still counts past capacity, so the test can tell "the buffer
    // was too small" from "the SDF produced this many".
    if (index < uint(params.config.y)) {
        splats[index].pos_source = float4(p, source);
    }
}

[shader("compute")]
[numthreads(4, 4, 4)]
void cs_evaluate(uint3 tid : SV_DispatchThreadID) {
    uint n = uint(params.config.x);
    if (tid.x >= n || tid.y >= n || tid.z >= n) { return; }

    float cell = params.grid.w;
    float3 p = params.grid.xyz + (float3(tid) + 0.5) * cell;
    float half_cell = cell * 0.5;

    if (params.sphere_a.w > 0.0) {
        float d = length(p - params.sphere_a.xyz) - params.sphere_a.w;
        if (abs(d) <= half_cell) { Emit(p, 0.0); }
    }
    if (params.sphere_b.w > 0.0) {
        float d = length(p - params.sphere_b.xyz) - params.sphere_b.w;
        if (abs(d) <= half_cell) { Emit(p, 1.0); }
    }
}
